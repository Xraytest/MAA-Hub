// MaaHub 原生窗口应用实现
#include "app_window.hpp"
#include "task_panels.hpp"

#include <windowsx.h>

namespace hub {

using gui::Input;
using gui::CheckState;

// 最小 CRC32（PNG 用）
static uint32_t crc32_calc(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

MaaHubWindow::MaaHubWindow(HINSTANCE hInst, mj::Value config, bool offscreen)
    : hInst_(hInst), config_(std::move(config)), offscreen_(offscreen) {
    hub_ = std::make_unique<Hub>(config_);
}

MaaHubWindow::~MaaHubWindow() {
    // 关键：不能无限期 join backend_。backend 线程可能正卡在 connect（最长 180s）
    // 或 pull_core（下载大文件）里，此时关窗口会整个 UI 无响应（表现为"死机"）。
    // 先置停止标志，最多等 1.5s；超时则 detach 让进程退出时自行收尾。
    running_ = false;
    if (backend_.joinable()) {
        // 用条件变量/等待方式不可靠，这里用有限等待后 detach
        HANDLE h = backend_.native_handle();
        if (WaitForSingleObject(h, 1500) != WAIT_OBJECT_0) {
            backend_.detach();
        } else {
            backend_.join();
        }
    }
}

bool MaaHubWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MaaHubWindow::wnd_proc;
    wc.hInstance = hInst_;
    wc.lpszClassName = L"MaaHubMainWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    int w = 1280, h = 800;      // MAA 官方最小窗口 800x600
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    DWORD style = offscreen_ ? (WS_POPUP & ~WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
    hwnd_ = CreateWindowExW(
        0, L"MaaHubMainWindow", L"MaaHub — MAA / MaaEnd 统一管理器",
        style,
        (sx - w) / 2, (sy - h) / 2, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst_, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    gui_.init_fonts();
    if (offscreen_) {
        // 移到屏幕外并保持隐藏：窗口始终不可见，仅供离屏渲染
        SetWindowPos(hwnd_, nullptr, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd_, SW_HIDE);
    } else {
        ShowWindow(hwnd_, SW_SHOW);
    }
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, TIMER_REFRESH, 250, nullptr);

    running_ = true;
    backend_ = std::thread([this] { backend_loop(); });
    return true;
}

// 离屏渲染：把指定页面画进内存 DC 并保存为 PNG（窗口保持隐藏）。
// 内置最小 PNG 编码器（zlib stored block + CRC32），零第三方依赖。
bool MaaHubWindow::render_to_file(const std::string& page, const std::string& png_path) {
    page_ = page;
    if (page == "executor" && selected_exec_.empty()) {
        auto ids = hub_->executors();
        if (!ids.empty()) selected_exec_ = ids[0].id;
    }
    const int W = 1280, H = 800;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp || !bits) return false;
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    HDC save_dc = mem_dc_;
    HBITMAP save_bmp = mem_bmp_;
    void* save_bits = mem_bits_;
    SIZE save_size = mem_size_;
    mem_dc_ = mem; mem_bmp_ = bmp; mem_bits_ = bits; mem_size_ = { W, H };

    RECT rc{ 0, 0, W, H };
    on_paint(mem_dc_, rc);

    mem_dc_ = save_dc; mem_bmp_ = save_bmp; mem_bits_ = save_bits; mem_size_ = save_size;
    SelectObject(mem, old_bmp);

    const uint8_t* src = (const uint8_t*)bits;
    std::vector<uint8_t> raw;
    raw.reserve((size_t)H * (W * 3 + 1));
    for (int y = 0; y < H; ++y) {
        raw.push_back(0);
        const uint8_t* row = src + (size_t)y * W * 4;
        for (int x = 0; x < W; ++x) {
            raw.push_back(row[x * 4 + 2]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4]);
        }
    }
    auto be32 = [](uint32_t v, std::vector<uint8_t>& out) {
        out.push_back((v >> 24) & 0xFF); out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 8) & 0xFF);  out.push_back(v & 0xFF);
    };
    auto adler = [](const std::vector<uint8_t>& d) {
        uint32_t a = 1, b = 0;
        for (uint8_t byte : d) { a = (a + byte) % 65521; b = (b + a) % 65521; }
        return (b << 16) | a;
    };
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        bool last = (pos + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back((n >> 8) & 0xFF);
        z.push_back((~n) & 0xFF); z.push_back(((~n) >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    be32(adler(raw), z);

    std::vector<uint8_t> png;
    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 0x1A, 10 };
    png.insert(png.end(), sig, sig + 8);
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        be32((uint32_t)data.size(), png);
        png.insert(png.end(), body.begin(), body.end());
        be32(crc32_calc(body.data(), body.size()), png);
    };
    std::vector<uint8_t> ihdr;
    be32((uint32_t)W, ihdr); be32((uint32_t)H, ihdr);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0);
    ihdr.push_back(0); ihdr.push_back(0);
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    util::write_file(png_path, std::string(png.begin(), png.end()));
    DeleteObject(bmp);
    DeleteDC(mem);
    return true;
}

int MaaHubWindow::run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK MaaHubWindow::wnd_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (MaaHubWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return self ? self->handle_msg(hwnd, m, wp, lp) : DefWindowProcW(hwnd, m, wp, lp);
}

LRESULT MaaHubWindow::handle_msg(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEMOVE:
        input_.mouse.x = GET_X_LPARAM(lp);
        input_.mouse.y = GET_Y_LPARAM(lp);
        return 0;
    case WM_LBUTTONDOWN:
        input_.mouse.x = GET_X_LPARAM(lp);
        input_.mouse.y = GET_Y_LPARAM(lp);
        input_.down = true;
        input_.pressed = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        input_.mouse.x = GET_X_LPARAM(lp);
        input_.mouse.y = GET_Y_LPARAM(lp);
        input_.down = false;
        input_.released = true;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_RBUTTONDOWN:
        input_.mouse.x = GET_X_LPARAM(lp);
        input_.mouse.y = GET_Y_LPARAM(lp);
        input_.rdown = true;
        input_.rpressed = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_RBUTTONUP:
        input_.mouse.x = GET_X_LPARAM(lp);
        input_.mouse.y = GET_Y_LPARAM(lp);
        input_.rdown = false;
        input_.rreleased = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        input_.wheel = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CHAR:
        input_.char_input = (wchar_t)wp;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        input_.vk = (int)wp;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH) {
            uint64_t seen = 0;
            {
                std::lock_guard<std::mutex> lk(snap_mtx_);
                seen = snap_version_;
            }
            if (seen != snap_seen_) {
                snap_seen_ = seen;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_APP_SNAP:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!mem_dc_ || mem_size_.cx != rc.right || mem_size_.cy != rc.bottom) {
            if (mem_dc_) { DeleteDC(mem_dc_); DeleteObject(mem_bmp_); }
            HDC screen = GetDC(hwnd);
            mem_dc_ = CreateCompatibleDC(screen);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = rc.right;
            bi.bmiHeader.biHeight = -rc.bottom;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            mem_bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &mem_bits_, nullptr, 0);
            ReleaseDC(hwnd, screen);
            SelectObject(mem_dc_, mem_bmp_);
            mem_size_ = { rc.right, rc.bottom };
        }
        on_paint(mem_dc_, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem_dc_, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        on_paint((HDC)wp, rc);
        return 0;
    }
    case WM_PRINT: {
        HDC target = (HDC)wp;
        if (((UINT)lp & PRF_CHECKVISIBLE) && !IsWindowVisible(hwnd)) return 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC tmp = CreateCompatibleDC(target);
        HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(tmp, bmp);
        on_paint(tmp, rc);
        BitBlt(target, 0, 0, rc.right, rc.bottom, tmp, 0, 0, SRCCOPY);
        SelectObject(tmp, old);
        DeleteObject(bmp);
        DeleteDC(tmp);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

void MaaHubWindow::on_paint(HDC dc, RECT client) {
    // 帧耗时监控：单帧 > 500ms 记录告警（用于定位 UI 卡死）
    double frame_t0 = util::now_sec();
    gui_.set_dc(dc, client);
    gui_.begin(input_);
    gui_.fill(client, gui_.theme.bg);

    RECT sidebar{ 0, 0, sidebar_w_, client.bottom };
    RECT main_area{ sidebar_w_, 0, client.right, client.bottom };
    RECT topbar{ main_area.left, main_area.top, main_area.right, main_area.top + topbar_h_ };
    RECT content{ main_area.left + 20, main_area.top + topbar_h_ + 14,
                  main_area.right - 20, main_area.bottom - 16 };

    draw_sidebar(sidebar);
    draw_topbar(topbar);

    try {
        if (page_ == "executor" && !selected_exec_.empty()) page_executor(content);
        else if (page_ == "accounts") page_accounts(content);
        else if (page_ == "cores") page_cores(content);
        else if (page_ == "planner") page_planner(content);
        else if (page_ == "events") page_events(content);
        else if (page_ == "settings") page_settings(content);
        else page_welcome(content);
    } catch (const std::exception& e) {
        fprintf(stderr, "[paint] EXCEPTION in page %s: %s\n", page_.c_str(), e.what());
    } catch (...) {
        fprintf(stderr, "[paint] UNKNOWN EXCEPTION in page %s\n", page_.c_str());
    }

    gui_.end();
    input_.pressed = false;
    input_.released = false;
    input_.rpressed = false;
    input_.rreleased = false;
    input_.wheel = 0;
    input_.char_input = 0;
    input_.vk = 0;

    double frame_ms = (util::now_sec() - frame_t0) * 1000;
    if (frame_ms > 500) {
        fprintf(stderr, "[ui] slow frame %.0fms (page=%s)\n", frame_ms, page_.c_str());
    }
}

// ================================================================ 侧栏
void MaaHubWindow::draw_sidebar(RECT r) {
    gui_.fill(r, gui_.theme.panel);
    gui_.fill({ r.right - 1, r.top, r.right, r.bottom }, gui_.theme.border);

    // 顶部标题条（Primary 渐变，对应 MAA TitleBrush）
    RECT head{ r.left, r.top, r.right, r.top + 52 };
    gui_.gradient_rect(head, gui_.theme.primary, gui_.theme.primary_dark);
    RECT save0 = gui_.clip();
    gui_.text(r.left + 14, r.top + 10, L"MaaHub", RGB(255, 255, 255), gui_.font_title);
    gui_.text(r.left + 14, r.top + 30, L"统一管理器", RGB(230, 238, 255), gui_.font_small);
    (void)save0;

    // ---- 执行器列表 ----
    int y = r.top + 64;
    gui_.text(r.left + 14, y, L"执行器（实例）", gui_.theme.dim, gui_.font_small);
    // 原来只有 32px 宽，"＋ 新建" 需要 46px，文本会溢出按钮；
    // 放宽到 58px（左边界 180，仍远在标题文字右侧，不会重叠）
    RECT add_btn{ r.right - 70, y - 4, r.right - 12, y + 18 };
    if (gui_.button("new-exec", add_btn, L"＋ 新建", 1)) show_new_exec_ = !show_new_exec_;
    y += 24;

    std::vector<mj::Value> execs;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        if (auto* e = snap_.find("executors"); e && e->is_array()) execs = e->items();
    }
    for (auto& ex : execs) {
        std::string id = ex.get_str("id", "");
        std::string kind = ex.get_str("kind", "");
        std::string label = ex.get_str("label", "");
        std::string conn = ex.get_str("conn", "unknown");
        std::string run = ex.get_str("run", "idle");
        std::string acc = ex.get_str("account_label", "");
        RECT ir{ r.left + 8, y, r.right - 8, y + 40 };
        bool hov = gui_.input().mouse.x >= ir.left && gui_.input().mouse.x < ir.right
                && gui_.input().mouse.y >= ir.top && gui_.input().mouse.y < ir.bottom;
        bool sel = selected_exec_ == id;
        if (sel) gui_.round_rect(ir, gui_.theme.primary_light, gui_.theme.primary, 4);
        else if (hov) gui_.round_rect(ir, gui_.theme.hover, gui_.theme.border, 4);
        else gui_.round_rect(ir, gui_.theme.panel, gui_.theme.border, 8);

        gui_.text(ir.left + 8, ir.top + 5, kind == "maa" ? L"⚔️" : L"🛰️", gui_.theme.text, gui_.font);
        COLORREF dot = conn == "connected" ? gui_.theme.ok :
                       conn == "failed" ? gui_.theme.err :
                       run == "running" ? gui_.theme.primary : gui_.theme.dim;
        gui_.round_rect({ ir.right - 22, ir.top + 8, ir.right - 12, ir.top + 18 }, dot, dot, 5);
        {
            std::wstring sub = gui::widen(kind == "maa" ? "MAA" : "MaaEnd");
            if (!acc.empty()) sub += L" · " + gui::widen(acc);
            else sub += L" · 未绑定账号";
            RECT save = gui_.clip();
            gui_.text_clipped({ ir.left + 34, ir.top + 3, ir.right - 28, ir.top + 20 },
                              gui::widen(label), sel ? gui_.theme.primary_dark : gui_.theme.text,
                              gui_.font);
            gui_.text_clipped({ ir.left + 34, ir.top + 21, ir.right - 28, ir.bottom },
                              sub, gui_.theme.dim, gui_.font_small);
            (void)save;
        }
        if (hov && gui_.input().pressed) {
            selected_exec_ = id;
            page_ = "executor";
        }
        // 14px 宽的按钮只有 11px 的 ✕，太挤；放宽到 18px（不与右侧状态点重叠）
        RECT dr{ ir.right - 42, ir.top + 10, ir.right - 24, ir.top + 30 };
        if (gui_.button(("del-" + id).c_str(), dr, L"✕", 2)) {
            action_delete_executor(id);
        }
        y += 46;
    }
    if (execs.empty() && !show_new_exec_) {
        gui_.text(r.left + 16, y + 4, L"还没有执行器", gui_.theme.dim, gui_.font_small);
        y += 26;
    }

    // ---- 新建执行器表单 ----
    if (show_new_exec_) {
        int fy = y + 4;
        RECT form{ r.left + 8, fy, r.right - 8, fy + 148 };
        gui_.round_rect(form, RGB(250, 251, 253), gui_.theme.border, 8);
        int yy = form.top + 8;
        int ki = gui_.option_strip("new-kind", { form.left + 8, yy, form.right - 8, yy + 24 },
                                   { L"MAA (明日方舟)", L"MaaEnd (终末地)" }, new_kind_);
        if (ki >= 0) new_kind_ = ki;
        yy += 30;
        gui_.text_input("new-label", { form.left + 8, yy, form.right - 8, yy + 24 },
                        new_label_, L"实例备注，如：官服大号");
        yy += 30;
        std::string kind = new_kind_ == 0 ? "maa" : "maaend";
        std::vector<mj::Value> accs;
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            if (auto* a = snap_.find("accounts"); a && a->is_array()) {
                for (auto& x : a->items()) {
                    if (x.get_str("game", "") == kind) accs.push_back(x);
                }
            }
        }
        std::vector<std::wstring> opts = { L"不绑定账号" };
        for (auto& a : accs) opts.push_back(gui::widen(a.get_str("label", "")));
        if (new_acc_idx_ >= (int)opts.size()) new_acc_idx_ = 0;
        RECT ar{ form.left + 8, yy, form.right - 8, yy + 24 };
        std::vector<std::wstring> shown(opts.begin(),
                                        opts.begin() + std::min<size_t>(opts.size(), 3));
        int ai = gui_.option_strip("new-acc", ar, shown, new_acc_idx_);
        if (ai >= 0) new_acc_idx_ = ai;
        yy += 30;
        RECT ok_btn{ form.left + 8, yy, form.right - 8, yy + 26 };
        if (gui_.button("new-ok", ok_btn, L"创建执行器", 1, !new_label_.empty())) {
            action_create_executor();
        }
        y = form.bottom + 8;
    }

    // ---- 全局导航 ----
    struct NavItem { const char* page; const wchar_t* icon; const wchar_t* label; };
    static const NavItem navs[] = {
        { "accounts", L"👥", L"账号管理" },
        { "cores", L"📦", L"核心管理" },
        { "planner", L"🧠", L"智能规划" },
        { "events", L"📡", L"事件日志" },
        { "settings", L"⚙️", L"设置" },
    };
    int nav_y = r.bottom - 40 * 5 - 20;
    for (auto& n : navs) {
        RECT ir{ r.left + 8, nav_y, r.right - 8, nav_y + 36 };
        bool hov = gui_.input().mouse.x >= ir.left && gui_.input().mouse.x < ir.right
                && gui_.input().mouse.y >= ir.top && gui_.input().mouse.y < ir.bottom;
        bool active = page_ == n.page;
        if (active) gui_.round_rect(ir, gui_.theme.primary_light, gui_.theme.primary, 4);
        else if (hov) gui_.round_rect(ir, gui_.theme.hover, gui_.theme.border, 4);
        else gui_.round_rect(ir, gui_.theme.panel, gui_.theme.panel, 8);
        gui_.text(ir.left + 12, ir.top + 9, n.icon, gui_.theme.text, gui_.font);
        gui_.text(ir.left + 40, ir.top + 9, n.label,
                  active ? gui_.theme.primary_dark : gui_.theme.text, gui_.font);
        if (hov && gui_.input().pressed) page_ = n.page;
        nav_y += 40;
    }
}

// ================================================================ 顶栏
void MaaHubWindow::draw_topbar(RECT r) {
    gui_.fill(r, gui_.theme.panel);
    gui_.fill({ r.left, r.bottom - 1, r.right, r.bottom }, gui_.theme.border);

    std::wstring title = L"MaaHub";
    mj::Value cur;
    bool has_cur = false;
    if (page_ == "executor") {
        has_cur = find_executor(selected_exec_, cur);
        title = has_cur ? gui::widen(cur.get_str("label", "")) : L"执行器";
    } else if (page_ == "accounts") title = L"👥 账号管理";
    else if (page_ == "cores") title = L"📦 核心管理";
    else if (page_ == "planner") title = L"🧠 智能规划";
    else if (page_ == "events") title = L"📡 事件日志";
    else if (page_ == "settings") title = L"⚙️ 设置";
    gui_.text(r.left + 4, r.top + 18, title, gui_.theme.text, gui_.font_title);

    bool connected = has_cur && cur.get_str("conn", "") == "connected";
    RECT br{ r.right - 320, r.top + 10, r.right - 240, r.bottom - 10 };
    if (gui_.button("btn-connect", br, connected ? L"断开" : L"连接", 0, has_cur)) {
        if (connected) action_disconnect(selected_exec_);
        else action_connect(selected_exec_);
    }
    RECT lr{ r.right - 228, r.top + 10, r.right - 20, r.bottom - 10 };
    if (gui_.button("btn-linkstart", lr, L"🔗 链接开始！", 1, has_cur)) {
        std::string id = selected_exec_;
        post_action([this, id] {
            mj::Value st = hub_->executor_status(id);
            if (st.get_str("conn", "") != "connected") hub_->connect_executor(id);
            action_run_checked(id);
        }, "链接开始");
    }
    int dl = dl_progress_.load();
    if (dl >= 0) {
        std::wstring t = L"⏳ " + gui::widen(dl_stage_) + L" " + std::to_wstring(dl) + L"%";
        gui_.text(r.right - 560, r.top + 20, t, gui_.theme.warn, gui_.font_small);
    }
}

// MAA 官方任务显示名（取自 MaaWpfGui/Res/Localizations/zh-cn.xaml 的 TaskQueue.Task.*）
std::wstring MaaHubWindow::display_name(const std::string& kind, const std::string& task) {
    if (kind == "maa") {
        if (task == "StartUp") return L"\u5f00\u59cb\u5524\u9192";
        if (task == "CloseDown") return L"\u5173\u95ed\u6e38\u620f";
        if (task == "Fight") return L"\u7406\u667a\u4f5c\u6218";
        if (task == "Infrast") return L"\u57fa\u5efa\u6362\u73ed";
        if (task == "Recruit") return L"\u81ea\u52a8\u516c\u62db";
        if (task == "Mall") return L"\u4fe1\u7528\u6536\u652f";
        if (task == "Award") return L"\u9886\u53d6\u5956\u52b1";
        if (task == "Roguelike") return L"\u81ea\u52a8\u8089\u9e54";
        if (task == "Copilot") return L"\u81ea\u52a8\u6284\u4f5c\u4e1a";
        if (task == "Depot") return L"\u4ed3\u5e93\u8bc6\u522b";
        if (task == "OperBox") return L"\u5e72\u5458\u8bc6\u522b";
        if (task == "Reclamation") return L"\u751f\u606f\u6f14\u7b97";
        if (task == "Custom") return L"\u81ea\u5b9a\u4efb\u52a1";
    } else {
if (task == "AccountSwitch") return L"🔑自动切换账号";
        if (task == "AdbAutoFightTest") return L"🧪战斗测试";
        if (task == "AeroSalvage") return L"🎈浮空回收";
        if (task == "AndroidOpenGame") return L"🎮打开游戏（安卓端）";
        if (task == "AutoCollect") return L"🧺自动采集";
        if (task == "AutoDeliveryPreferZipline") return L"送货时优先使用滑索";
        if (task == "AutoDeliveryRiskAcknowledgement") return L"我已知悉可能的后果";
        if (task == "AutoEcoFarm") return L"🌾生态农场";
        if (task == "AutoEssence") return L"🎱基质刷取";
        if (task == "AutoSell") return L"💰售卖弹性物资";
        if (task == "AutoStockStaple") return L"🏪购买稳定物资";
        if (task == "AutoStockpile") return L"📦自动囤货";
        if (task == "BakerEntry") return L"💬会话消息嘴替";
        if (task == "BatchAddFriends") return L"👥批量添加好友";
        if (task == "BatchDeleteFriends") return L"🗑️批量删除好友";
        if (task == "BatchUseDetector") return L"🧭批量探测器";
        if (task == "ClaimSimulationRewards") return L"📦领取模拟空间奖励";
        if (task == "CloseGame") return L"❌关闭游戏（安卓端）";
        if (task == "CloseGamePC") return L"❌关闭游戏（PC）";
        if (task == "Crafting") return L"🧪简易制作";
        if (task == "CreditShopping") return L"🛍️信用点购物";
        if (task == "DailyRewards") return L"📅日常奖励领取";
        if (task == "DecomposeWeaponEssence") return L"分解武器基质";
        if (task == "DeliveryJobs") return L"🚚转交委托";
        if (task == "DevTest") return L"🧪开发测试";
        if (task == "DijiangRewards") return L"🎁基建任务";
        if (task == "EnvironmentMonitoring") return L"🌿环境监测";
        if (task == "EssenceFilter") return L"🔒基质筛选锁定";
        if (task == "GearAssembly") return L"🔧装备制造";
        if (task == "GiftOperator") return L"🎁赠送干员礼物";
        if (task == "ImportBluePrints") return L"📐一键导入蓝图";
        if (task == "IntelArchive") return L"📁情报档案库";
        if (task == "ItemTransfer") return L"🐌库存转移";
        if (task == "LaunchGameLinux") return L"启动游戏（Linux）";
        if (task == "LevelUpOperator") return L"📈干员升级";
        if (task == "OutpostTrading") return L"🛒据点交易";
        if (task == "ProtocolSpace") return L"⚔️协议空间";
        if (task == "PullCountCalculator") return L"🧮抽数计算";
        if (task == "PuzzleSolver") return L"🧩解拼图";
        if (task == "ReadAllWiki") return L"📖百科已读";
        if (task == "RealTimeTask") return L"🤖实时开荒辅助";
        if (task == "ReceiveProdManual") return L"🌾简制手册领取";
        if (task == "ResourceRecycleStation") return L"🦉资源回收站";
        if (task == "SanityExtractor") return L"💾萃取理智";
        if (task == "SeizeDeliveryJobs") return L"🏍️抢委托送货";
        if (task == "SeizeDeliveryJobsCommissionSource") return L"委托接收点";
        if (task == "SeizeDeliveryJobsDeliveryPointOriginLodespring") return L"矿脉源区送货点";
        if (task == "SeizeDeliveryJobsDeliveryPointOriginiumSciencePark") return L"源石研究园送货点";
        if (task == "SeizeDeliveryJobsDeliveryPointPowerPlateau") return L"供能高地送货点";
        if (task == "SeizeDeliveryJobsDeliveryPointTestArea") return L"试验园区送货点";
        if (task == "SeizeDeliveryJobsDeliveryPointWulingCity") return L"武陵城送货点";
        if (task == "SeizeDeliveryJobsPostProcessing") return L"抢单结束后的操作";
        if (task == "SeizeDeliveryJobsRefreshLimit") return L"抢单刷新次数上限";
        if (task == "SeizeDeliveryJobsRepeatCount") return L"送货次数";
        if (task == "SeizeDeliveryJobsReward") return L"接取价格不低于：";
        if (task == "SeizeDeliveryJobsSpecifyDeliveryPoint") return L"指定送货点";
        if (task == "SharedZiplineDelete") return L"✂️删除共享滑索";
        if (task == "SimpleProductionBatch") return L"🔨批量简易制作";
        if (task == "StashBackpack") return L"🎒存放背包";
        if (task == "SwitchTeam") return L"🔄切换编队";
        if (task == "TrialOfSwordmancy") return L"🗡️选剑演武";
        if (task == "UmbralMonument") return L"🗿影拓丰碑";
        if (task == "VisitFriends") return L"🤝拜访好友";
        if (task == "WeaponUpgrade") return L"🔫升级武器";
        if (task == "WebEvent202605") return L"🎁自动共贺庆典网页活动";
        if (task == "ZiplineImport") return L"🚡导入/更新滑索坐标";
    }
    return gui::widen(task);
}

// MaaEnd 内部入口任务（官方 GUI 不展示，无 label）
inline bool maaend_hidden_task(const std::string& t) {
    return t == "SellProduct" || t == "SimpleProductionBatchStart";
}

// ================================================================ 实例页（MAA 1:1 布局）
// 复刻 MAA 官方 TaskQueueView：左列 190px 任务列表 + 右侧任务设置 + 底部控制栏。
// 配色/圆角(4)/控件高(28)/外边距(20) 对齐 HandyControl Light 主题。
void MaaHubWindow::page_executor(RECT content) {
    mj::Value ex;
    if (!find_executor(selected_exec_, ex)) { page_welcome(content); return; }
    std::string id = ex.get_str("id", "");
    std::string kind = ex.get_str("kind", "");
    std::string conn = ex.get_str("conn", "unknown");
    std::string version = ex.get_str("version", "");
    std::string acc_label = ex.get_str("account_label", "");
    bool daily_done = ex.get_bool("daily_done_today", false);
    int64_t task_count = ex.get_int("task_count", 0);
    std::string core_version = ex.get_str("core_version", "");

    std::vector<mj::Value> all_tasks = catalog_of(kind);
    std::vector<mj::Value> tasks;   // 过滤内部入口任务
    for (auto& t : all_tasks) {
        if (kind == "maaend" && maaend_hidden_task(t.get_str("name", ""))) continue;
        tasks.push_back(t);
    }
    // 应用用户拖拽后的顺序
    auto oit = task_order_.find(id);
    if (oit != task_order_.end() && !oit->second.empty()) {
        std::vector<mj::Value> sorted;
        sorted.reserve(tasks.size());
        for (auto& nm : oit->second) {
            for (auto& t : tasks) {
                if (t.get_str("name", "") == nm) { sorted.push_back(t); break; }
            }
        }
        for (auto& t : tasks) {
            bool found = false;
            for (auto& s : sorted) {
                if (s.get_str("name", "") == t.get_str("name", "")) { found = true; break; }
            }
            if (!found) sorted.push_back(t);
        }
        tasks.swap(sorted);
    }

    // 外框：Margin 20 + 白色 Region 卡片 + 圆角 4
    RECT frame{ content.left, content.top, content.right, content.bottom };
    gui_.round_rect(frame, gui_.theme.panel, gui_.theme.border, 4);

    const int LEFT_W = 190;                 // MAA TaskList Width=190
    const int BAR_H = 44;
    RECT left_col{ frame.left, frame.top, frame.left + LEFT_W, frame.bottom - BAR_H };
    RECT right_col{ frame.left + LEFT_W, frame.top, frame.right, frame.bottom - BAR_H };
    RECT bottom_bar{ frame.left, frame.bottom - BAR_H, frame.right, frame.bottom };

    gui_.fill({ left_col.right, left_col.top, left_col.right + 1, left_col.bottom },
              gui_.theme.border);

    // ---------------- 左列：任务列表 ----------------
    gui_.text(left_col.left + 12, left_col.top + 12, L"\u4efb\u52a1\u5217\u8868",
              gui_.theme.text, gui_.font_bold);
    gui_.text(left_col.left + 12, left_col.top + 32,
              L"\u5de6\u952e\u8bbe\u7f6e \u00b7 \u52fe\u9009\u542f\u7528",
              gui_.theme.faint, gui_.font_small);

    int hy = left_col.top + 52;
    RECT sel_all{ left_col.left + 12, hy, left_col.left + 72, hy + 22 };
    RECT sel_none{ left_col.left + 78, hy, left_col.left + 138, hy + 22 };
    if (gui_.button("tb-all", sel_all, L"\u5168\u9009", 0)) {
        for (auto& t : tasks) task_ui_[id][t.get_str("name", "")].checked = true;
    }
    if (gui_.button("tb-none", sel_none, L"\u5168\u4e0d\u9009", 0)) {
        for (auto& t : tasks) task_ui_[id][t.get_str("name", "")].checked = false;
    }

    int list_top = hy + 30;
    int item_h = 32;
    int n = (int)tasks.size();

    // ---- 拖拽排序状态机（官方左列可拖动调整顺序）----
    // 记录本帧每项的 y 供放置判定
    std::vector<int> item_ys;
    item_ys.reserve(n);
    {
        int yy = list_top + 4 - (int)scroll_["tasks-" + id];
        for (int k = 0; k < n; ++k) { item_ys.push_back(yy); yy += item_h; }
    }
    // 按下左键且在执行器列内 -> 开始拖拽
    if (gui_.input().pressed
        && gui_.input().mouse.x >= left_col.left && gui_.input().mouse.x < left_col.right
        && gui_.input().mouse.y >= list_top) {
        int idx = (gui_.input().mouse.y - (list_top + 4 - (int)scroll_["tasks-" + id])) / item_h;
        if (idx >= 0 && idx < n) {
            drag_task_ = tasks[idx].get_str("name", "");
            drag_y_ = gui_.input().mouse.y;
        }
    }
    if (!drag_task_.empty()) {
        drag_y_ = gui_.input().mouse.y;
        // 计算插入位置
        int base = list_top + 4 - (int)scroll_["tasks-" + id];
        int idx = (drag_y_ - base) / item_h;
        if (idx < 0) idx = 0;
        if (idx > n) idx = n;
        drop_before_ = idx;
        // 松手 -> 提交排序
        if (!gui_.input().down) {
            int from = -1;
            for (int k = 0; k < n; ++k) {
                if (tasks[k].get_str("name", "") == drag_task_) { from = k; break; }
            }
            if (from >= 0 && drop_before_ >= 0 && drop_before_ != from
                && drop_before_ != from + 1) {
                int to = drop_before_;
                if (to > from) --to;
                std::string nm = drag_task_;
                task_order_[id].clear();
                for (auto& t : tasks) {
                    std::string s = t.get_str("name", "");
                    if (s != nm) task_order_[id].push_back(s);
                }
                if (to < 0) to = 0;
                if (to > (int)task_order_[id].size()) to = (int)task_order_[id].size();
                task_order_[id].insert(task_order_[id].begin() + to, nm);
            }
            drag_task_.clear();
            drop_before_ = -1;
        }
    } else {
        drop_before_ = -1;
    }

    gui_.begin_scroll(("tasks-" + id).c_str(),
                      { left_col.left, list_top, left_col.right, left_col.bottom - 6 },
                      n * item_h + 8, scroll_["tasks-" + id]);
    int y = list_top + 4 - (int)scroll_["tasks-" + id];
    for (int ti = 0; ti < n; ++ti) {
        auto& t = tasks[ti];
        std::string name = t.get_str("name", "");
        TaskUiState& ui = task_ui_[id][name];
        if (y + item_h < list_top) { y += item_h; continue; }
        if (y > left_col.bottom) break;
        RECT item{ left_col.left + 8, y, left_col.right - 8, y + item_h - 4 };
        bool hov = gui_.input().mouse.x >= item.left && gui_.input().mouse.x < item.right
                && gui_.input().mouse.y >= item.top && gui_.input().mouse.y < item.bottom;
        COLORREF fill_c = (ui.checked || ui.once) ? gui_.theme.primary_light : gui_.theme.panel;
        COLORREF line_c = (ui.checked || ui.once) ? gui_.theme.primary : gui_.theme.border;
        if (hov && !ui.checked && !ui.once) fill_c = gui_.theme.hover;
        // 拖拽源半透明提示
        if (drag_task_ == name) fill_c = gui_.theme.primary_light;
        gui_.round_rect(item, fill_c, line_c, 4);
        RECT cbr{ item.left + 8, item.top + 5, item.left + 200, item.top + 25 };
        std::wstring label = display_name(kind, name);
        CheckState st = ui.once ? CheckState::Once
                                : (ui.checked ? CheckState::On : CheckState::Off);
        gui_.checkbox3(("cb-" + id + "-" + name).c_str(), cbr, label, st);
        ui.checked = (st == CheckState::On);
        ui.once = (st == CheckState::Once);
        // 序号（拖拽时显示目标插入线）
        gui_.text(item.right - 24, item.top + 7, std::to_wstring(ti + 1),
                  gui_.theme.faint, gui_.font_small);
        if (hov && gui_.input().pressed) {
            for (auto& kv : task_ui_[id]) kv.second.selected = false;
            ui.selected = true;
        }
        y += item_h;
    }
    gui_.end_scroll();

    // ---------------- 右侧：任务设置 ----------------
    std::string sel_task;
    for (auto& t : tasks) {
        std::string nm = t.get_str("name", "");
        if (task_ui_[id][nm].selected) { sel_task = nm; break; }
    }
    if (sel_task.empty() && !tasks.empty()) sel_task = tasks[0].get_str("name", "");

    int rx = right_col.left + 16;
    int ry = right_col.top + 12;
    if (!sel_task.empty()) {
        gui_.text(rx, ry, display_name(kind, sel_task), gui_.theme.text, gui_.font_title);
        std::wstring desc;
        for (auto& t : tasks) {
            if (t.get_str("name", "") == sel_task) desc = gui::widen(t.get_str("description", ""));
        }
        if (!desc.empty()) {
            RECT save = gui_.clip();
            gui_.text_clipped({ rx + 260, ry + 4, right_col.right - 16, ry + 24 },
                              desc, gui_.theme.dim, gui_.font_small);
            (void)save;
        }
        ry += 34;

        TaskUiState& ui = task_ui_[id][sel_task];
        RECT run_btn{ rx, ry, rx + 120, ry + 28 };
        if (gui_.button(("run-" + id + "-" + sel_task).c_str(), run_btn,
                        L"\u25b6 \u5355\u6b21\u8fd0\u884c", 1)) {
            mj::Value params = mj::Value::object();
            for (auto& kv : ui.params) params.set(kv.first, kv.second);
            action_run(id, sel_task, params);
        }
        RECT gear_btn{ rx + 130, ry, rx + 230, ry + 28 };
        if (gui_.button(("gear-" + id + "-" + sel_task).c_str(), gear_btn,
                        ui.open ? L"\u6536\u8d77\u53c2\u6570" : L"\u5c55\u5f00\u53c2\u6570", 0)) {
            ui.open = !ui.open;
        }
        ry += 40;

        if (ui.open) {
            // 官方任务面板（1:1 复刻 MaaWpfGui TaskQueue/*.xaml）
            // 先用滚动区域承载，避免长面板超出可视区
            int panel_top = ry;
            int panel_h = (right_col.bottom - 12) - panel_top;
            if (panel_h < 80) panel_h = 80;
            gui_.begin_scroll(("panel-" + id + "-" + sel_task).c_str(),
                              { rx, panel_top, right_col.right - 16, right_col.bottom - 12 },
                              2000, scroll_["panel-" + id + "-" + sel_task]);
            int saved_y = ry;
            // 找到选中任务的 catalog 声明（供通用面板按类型渲染）
            const mj::Value* spec = nullptr;
            for (auto& t : tasks) {
                if (t.get_str("name", "") == sel_task) { spec = &t; break; }
            }
            std::vector<mj::Value> specs;
            if (spec) {
                if (auto* ps = spec->find("params"); ps && ps->is_array()) specs = ps->items();
            }
            PanelCtx pctx{ gui_, rx, ry, right_col.right - 16, ui.params, id, sel_task };
            bool custom = draw_task_panel(pctx, &specs);
            ry = pctx.y;
            int used = ry - saved_y;
            gui_.end_scroll();
            // 面板边框（按实际内容高度）
            RECT pbody{ rx, panel_top, right_col.right - 16,
                        panel_top + (used > 0 ? used + 8 : 40) };
            // 先画边框会盖住内容，这里只画左侧竖线作视觉分隔
            gui_.fill({ rx - 8, panel_top, rx - 7, pbody.bottom }, gui_.theme.border);
            ry = pbody.bottom + 12;
            (void)custom;
        }

        // 实例信息条
        RECT info{ rx, ry, right_col.right - 16, ry + 30 };
        gui_.round_rect(info, gui_.theme.row_alt, gui_.theme.border, 4);
        int ix = info.left + 12;
        COLORREF def_col = gui_.theme.text;
        auto field = [&](const std::wstring& k, const std::wstring& v, COLORREF col) {
            gui_.text(ix, info.top + 8, k, gui_.theme.dim, gui_.font_small);
            gui_.text(ix, info.top + 18, v, col, gui_.font);
            ix += 130;
        };
        field(L"\u7248\u672c", gui::widen(version.empty() ? "-" : version), def_col);
        field(L"\u6838\u5fc3", gui::widen(core_version.empty() ? "-" : core_version), def_col);
        field(L"\u8d26\u53f7", acc_label.empty() ? L"\u672a\u7ed1\u5b9a" : gui::widen(acc_label), def_col);
        field(L"\u4eca\u65e5\u65e5\u5e38", daily_done ? L"\u5df2\u5b8c\u6210" : L"\u672a\u5b8c\u6210",
              daily_done ? gui_.theme.ok : gui_.theme.warn);
        field(L"\u4efb\u52a1\u6570", std::to_wstring(task_count), def_col);
    } else {
        gui_.text(rx, ry + 8, L"\u8be5\u5b9e\u4f8b\u6ca1\u6709\u53ef\u7528\u4efb\u52a1",
                  gui_.theme.dim, gui_.font);
    }

    // ---------------- 底部控制栏 ----------------
    gui_.fill({ bottom_bar.left, bottom_bar.top, bottom_bar.right, bottom_bar.top + 1 },
              gui_.theme.border);
    bool connected = conn == "connected";
    RECT conn_btn{ bottom_bar.left + 16, bottom_bar.top + 8,
                   bottom_bar.left + 96, bottom_bar.bottom - 8 };
    if (gui_.button("btn-connect-exec", conn_btn, connected ? L"\u65ad\u5f00" : L"\u8fde\u63a5", 0)) {
        if (connected) action_disconnect(id);
        else action_connect(id);
    }
    COLORREF dot = connected ? gui_.theme.ok :
                     conn == "failed" ? gui_.theme.err :
                     conn == "connecting" ? gui_.theme.warn : gui_.theme.faint;
    gui_.round_rect({ bottom_bar.left + 106, bottom_bar.top + 17,
                      bottom_bar.left + 116, bottom_bar.top + 27 }, dot, dot, 5);
    {
        std::wstring st = connected ? L"\u5df2\u8fde\u63a5" :
                          conn == "failed" ? L"\u8fde\u63a5\u5931\u8d25" :
                          conn == "connecting" ? L"\u8fde\u63a5\u4e2d\u2026" : L"\u672a\u8fde\u63a5";
        gui_.text(bottom_bar.left + 124, bottom_bar.top + 15, st,
                  connected ? gui_.theme.ok : gui_.theme.dim, gui_.font_small);
    }
    RECT link_btn{ bottom_bar.right - 150, bottom_bar.top + 7,
                   bottom_bar.right - 16, bottom_bar.bottom - 7 };
    if (gui_.button("btn-linkstart-exec", link_btn, L"\U0001f517 Link Start!", 1)) {
        post_action([this, id] {
            mj::Value st = hub_->executor_status(id);
            if (st.get_str("conn", "") != "connected") hub_->connect_executor(id);
            action_run_checked(id);
        }, "\u94fe\u63a5\u5f00\u59cb");
    }
}

// ================================================================ 欢迎页
void MaaHubWindow::page_welcome(RECT content) {
    RECT panel{ content.left + 60, content.top + 40, content.right - 60, content.bottom - 40 };
    gui_.panel(panel, L"🎮 欢迎使用 MaaHub");
    int y = panel.top + 56;
    const wchar_t* lines[] = {
        L"MaaHub 统一管理 MAA（明日方舟）与 MaaEnd（终末地）实例。",
        L"· 左侧「＋ 新建」创建执行器：一个执行器 = 一个独立的原版软件实例",
        L"· 每个执行器可绑定不同账号，多个执行器可同时运行，实现多账号并行",
        L"· 「核心管理」可拉取 / 更新 MAA 与 MaaEnd 核心（内置多源下载）",
        L"· 「智能规划」由 LLM 依据各实例状态规划自动化行为",
    };
    for (auto* line : lines) {
        gui_.text(panel.left + 24, y, line, gui_.theme.text, gui_.font);
        y += 24;
    }
    y += 16;
    std::vector<mj::Value> execs, cores;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        if (auto* e = snap_.find("executors"); e && e->is_array()) execs = e->items();
        if (auto* c = snap_.find("cores"); c && c->is_array()) cores = c->items();
    }
    int maa_n = 0, end_n = 0;
    for (auto& e : execs) {
        if (e.get_str("kind", "") == "maa") ++maa_n; else ++end_n;
    }
    gui_.text(panel.left + 24, y,
              L"当前执行器：" + std::to_wstring(maa_n) + L" 个 MAA，"
              + std::to_wstring(end_n) + L" 个 MaaEnd；已登记核心 "
              + std::to_wstring(cores.size()) + L" 个",
              gui_.theme.text, gui_.font);
}

// ================================================================ 账号管理
void MaaHubWindow::page_accounts(RECT content) {
    mj::Value snap;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        snap = snap_;
    }
    RECT form{ content.left, content.top, content.right, content.top + 46 };
    gui_.panel(form);
    int fx = form.left + 16;
    gui_.text_input("acc-label", { fx, form.top + 10, fx + 190, form.top + 36 },
                    form_label_, L"备注名，如：官服大号");
    fx += 200;
    int gi = gui_.option_strip("acc-form-game", { fx, form.top + 10, fx + 180, form.top + 36 },
                               { L"明日方舟 (MAA)", L"终末地 (MaaEnd)" }, acc_form_game_);
    if (gi >= 0) acc_form_game_ = gi;
    fx += 190;
    gui_.text_input("acc-ident", { fx, form.top + 10, fx + 290, form.top + 36 }, form_ident_,
                    L"账号标识：MAA 登录名片段 / MaaEnd 邮箱或后四位");
    fx += 300;
    int mi = gui_.option_strip("acc-form-match", { fx, form.top + 10, fx + 190, form.top + 36 },
                               { L"后四位匹配", L"邮箱匹配" }, acc_form_match_);
    if (mi >= 0) acc_form_match_ = mi;
    fx += 200;
    RECT ab{ fx, form.top + 10, fx + 110, form.top + 36 };
    if (gui_.button("acc-add", ab, L"＋ 添加账号", 1, !form_label_.empty())) {
        std::string label = form_label_, ident = form_ident_;
        std::string game = acc_form_game_ == 0 ? "maa" : "maaend";
        std::string match = acc_form_match_ == 0 ? "LastFourDigits" : "Email";
        post_action([this, label, game, ident, match] {
            hub_->accounts().add(label, game, ident, match, "Official", "");
            set_notice("已添加账号 " + label);
        }, "添加账号");
        form_label_.clear();
        form_ident_.clear();
    }

    RECT panel{ content.left, content.top + 58, content.right, content.bottom };
    gui_.panel(panel, L"👥 账号列表");
    int ty = panel.top + 38;
    int cols[] = { panel.left + 16, panel.left + 190, panel.left + 300, panel.left + 470,
                   panel.left + 590, panel.left + 670, panel.left + 790, panel.left + 900 };
    const wchar_t* heads[] = { L"备注", L"游戏", L"标识", L"匹配方式", L"状态", L"最近运行",
                               L"今日日常", L"操作" };
    for (int i = 0; i < 8; ++i) gui_.text(cols[i], ty, heads[i], gui_.theme.dim, gui_.font_small);
    ty += 22;
    gui_.fill({ panel.left + 12, ty - 4, panel.right - 12, ty - 3 }, gui_.theme.border);

    std::vector<mj::Value> accs;
    if (auto* a = snap.find("accounts"); a && a->is_array()) accs = a->items();
    std::string today = util::today_str();
    int row_h = 34;
    gui_.begin_scroll("acc-scroll", { panel.left + 10, ty, panel.right - 10, panel.bottom - 10 },
                      (int)accs.size() * row_h + 10, scroll_["acc"]);
    int ry = ty + 6 - (int)scroll_["acc"];
    if (accs.empty()) gui_.text(panel.left + 16, ry + 6, L"尚未添加账号", gui_.theme.dim, gui_.font_small);
    for (auto& a : accs) {
        if (ry + row_h < ty) { ry += row_h; continue; }
        if (!gui_.row_fully_visible(ry, row_h)) break;   // 按裁剪区底剔除，别裁半行
        std::string id = a.get_str("id", "");
        std::string game = a.get_str("game", "");
        bool enabled = a.get_bool("enabled", true);
        bool is_active = a.get_bool("is_active", false);
        bool done = a.get_str("daily_done_date", "") == today
                    || a.get_bool("daily_done_today", false);
        double last_run = 0;
        if (auto* st = a.find("state")) last_run = st->get_int("last_run_at", 0);

        gui_.text(cols[0], ry + 6, gui::widen(a.get_str("label", "")) + (is_active ? L" ✓" : L""),
                  gui_.theme.text, gui_.font_small);
        gui_.text(cols[1], ry + 6, gui::widen(game == "maa" ? "明日方舟" : "终末地"),
                  gui_.theme.dim, gui_.font_small);
        {
            RECT save = gui_.clip();
            gui_.text_clipped({ cols[2], ry + 6, cols[3] - 10, ry + 26 },
                              gui::widen(a.get_str("identifier", "")), gui_.theme.text, gui_.font_small);
            (void)save;
        }
        gui_.text(cols[3], ry + 6, gui::widen(a.get_str("match_mode", "")), gui_.theme.dim, gui_.font_small);
        gui_.text(cols[4], ry + 6, enabled ? L"启用" : L"停用",
                  enabled ? gui_.theme.ok : gui_.theme.err, gui_.font_small);
        gui_.text(cols[5], ry + 6, last_run > 0 ? gui::widen(util::now_str()) : L"—",
                  gui_.theme.dim, gui_.font_small);
        gui_.text(cols[6], ry + 6, done ? L"✅" : L"⏳",
                  done ? gui_.theme.ok : gui_.theme.warn, gui_.font_small);
        RECT b1{ cols[7], ry + 2, cols[7] + 52, ry + 26 };
        RECT b2{ cols[7] + 60, ry + 2, cols[7] + 112, ry + 26 };
        RECT b3{ cols[7] + 120, ry + 2, cols[7] + 172, ry + 26 };
        if (gui_.button(("acc-tg-" + id).c_str(), b1, enabled ? L"停用" : L"启用", 0)) {
            bool newv = !enabled;
            post_action([this, id, newv] {
                mj::Value patch = mj::Value::object();
                patch.set("enabled", newv);
                hub_->accounts().update(id, patch);
            }, "更新账号");
        }
        if (gui_.button(("acc-sw-" + id).c_str(), b2, L"切换", 0)) {
            post_action([this, id, game] { hub_->accounts().set_active(game, id); }, "切换账号");
        }
        if (gui_.button(("acc-dl-" + id).c_str(), b3, L"删除", 2)) {
            post_action([this, id] {
                hub_->accounts().remove(id);
                set_notice("已删除账号");
            }, "删除账号");
        }
        ry += row_h;
        gui_.fill({ panel.left + 12, ry - 4, panel.right - 12, ry - 5 }, RGB(245, 247, 250));
    }
    gui_.end_scroll();
}

// ================================================================ 核心管理
void MaaHubWindow::page_cores(RECT content) {
    mj::Value snap;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        snap = snap_;
    }
    RECT bar{ content.left, content.top, content.right, content.top + 64 };
    gui_.panel(bar);
    int bx = bar.left + 14;
    if (gui_.button("core-check", { bx, bar.top + 8, bx + 120, bar.top + 32 }, L"🔄 检查更新", 1)) {
        action_check_update("");
    }
    bx += 130;
    if (gui_.button("core-pull-maa", { bx, bar.top + 8, bx + 180, bar.top + 32 },
                    L"⬇ 拉取 MAA 核心", 1, dl_progress_.load() < 0)) {
        action_pull_latest("maa");
    }
    bx += 190;
    if (gui_.button("core-pull-end", { bx, bar.top + 8, bx + 180, bar.top + 32 },
                    L"⬇ 拉取 MaaEnd 核心", 1, dl_progress_.load() < 0)) {
        action_pull_latest("maaend");
    }
    // 内部目录提示 + 共享核心说明
    {
        std::wstring root = L"核心目录：" + gui::widen(hub_->cores().cores_root());
        RECT save = gui_.clip();
        gui_.text_clipped({ bar.left + 14, bar.top + 40, bar.right - 14, bar.top + 60 },
                          root, gui_.theme.dim, gui_.font_small);
        (void)save;
    }
    if (!core_msg_.empty()) {
        RECT save = gui_.clip();
        gui_.text_clipped({ bar.right - 520, bar.top + 6, bar.right - 10, bar.top + 22 },
                          gui::widen(core_msg_),
                          core_msg_.find("失败") != std::string::npos ? gui_.theme.err : gui_.theme.ok,
                          gui_.font_small);
        (void)save;
    }

    int y = bar.bottom + 12;
    if (has_update_info_) {
        RECT up{ content.left, y, content.right, y + 96 };
        gui_.panel(up, L"🔄 更新检查");
        int uy = up.top + 38;
        gui_.text(up.left + 18, uy, L"当前版本:", gui_.theme.dim, gui_.font_small);
        gui_.text(up.left + 96, uy,
                  gui::widen(update_info_.current.empty() ? "-" : update_info_.current),
                  gui_.theme.text, gui_.font);
        gui_.text(up.left + 240, uy, L"最新版本:", gui_.theme.dim, gui_.font_small);
        gui_.text(up.left + 316, uy,
                  gui::widen(update_info_.latest.empty() ? "-" : update_info_.latest),
                  update_info_.has_update ? gui_.theme.ok : gui_.theme.dim, gui_.font);
        gui_.text(up.left + 450, uy, L"来源:", gui_.theme.dim, gui_.font_small);
        gui_.text(up.left + 498, uy, gui::widen(update_info_.source_name), gui_.theme.dim, gui_.font_small);
        uy += 28;
        if (update_info_.ok && update_info_.has_update && !update_info_.download_url.empty()) {
            RECT pb{ up.left + 18, uy, up.left + 200, uy + 26 };
            if (gui_.button("core-pull", pb, L"⬇ 拉取并安装", 1, dl_progress_.load() < 0)) {
                std::string url = update_info_.download_url;
                std::string kind = update_info_.kind;
                post_action([this, kind, url] { action_pull_core(kind, url); }, "拉取核心");
            }
            RECT save = gui_.clip();
            gui_.text_clipped({ up.left + 210, uy + 5, up.right - 16, uy + 23 },
                              L"下载地址: " + gui::widen(update_info_.download_url),
                              gui_.theme.dim, gui_.font_small);
            (void)save;
        } else if (update_info_.ok) {
            gui_.text(up.left + 18, uy, L"已是最新版本", gui_.theme.ok, gui_.font);
        } else {
            RECT save = gui_.clip();
            gui_.text_clipped({ up.left + 18, uy, up.right - 16, uy + 20 },
                              gui::widen(update_info_.error), gui_.theme.err, gui_.font_small);
            (void)save;
        }
        y = up.bottom + 12;
    }

    RECT panel{ content.left, y, content.right, content.bottom };
    gui_.panel(panel, L"📦 已登记核心");
    int ty = panel.top + 38;
    int cols[] = { panel.left + 16, panel.left + 64, panel.left + 240, panel.left + 420,
                   panel.left + 560, panel.left + 700, panel.left + 790, panel.left + 890 };
    const wchar_t* heads[] = { L"类型", L"名称", L"版本", L"来源", L"路径", L"默认", L"更新", L"操作" };
    for (int i = 0; i < 8; ++i) gui_.text(cols[i], ty, heads[i], gui_.theme.dim, gui_.font_small);
    ty += 22;
    gui_.fill({ panel.left + 12, ty - 4, panel.right - 12, ty - 3 }, gui_.theme.border);

    std::vector<mj::Value> cores;
    if (auto* c = snap.find("cores"); c && c->is_array()) cores = c->items();
    int row_h = 34;
    gui_.begin_scroll("core-scroll", { panel.left + 10, ty, panel.right - 10, panel.bottom - 10 },
                      (int)cores.size() * row_h + 10, scroll_["core"]);
    int ry = ty + 6 - (int)scroll_["core"];
    if (cores.empty()) {
        gui_.text(panel.left + 16, ry + 6,
                  L"尚未安装核心 — 请点击上方「拉取 MAA 核心 / 拉取 MaaEnd 核心」",
                  gui_.theme.warn, gui_.font);
        ry += 26;
        gui_.text(panel.left + 16, ry + 6,
                  L"核心由 MaaHub 内部管理（存放在上面的核心目录），不依赖系统已有安装",
                  gui_.theme.dim, gui_.font_small);
    }
    for (auto& c : cores) {
        if (ry + row_h < ty) { ry += row_h; continue; }
        if (!gui_.row_fully_visible(ry, row_h)) break;   // 按裁剪区底剔除，别裁半行
        std::string id = c.get_str("id", "");
        std::string kind = c.get_str("kind", "");
        bool is_def = c.get_bool("is_default", false);
        gui_.text(cols[0], ry + 6, kind == "maa" ? L"MAA" : L"MaaEnd", gui_.theme.primary, gui_.font_small);
        {
            RECT save = gui_.clip();
            gui_.text_clipped({ cols[1], ry + 6, cols[2] - 10, ry + 26 },
                              gui::widen(c.get_str("name", "")), gui_.theme.text, gui_.font_small);
            (void)save;
        }
        gui_.text(cols[2], ry + 6, gui::widen(c.get_str("version", "")), gui_.theme.text, gui_.font_small);
        gui_.text(cols[3], ry + 6, gui::widen(c.get_str("source", "")), gui_.theme.dim, gui_.font_small);
        {
            RECT save = gui_.clip();
            gui_.text_clipped({ cols[4], ry + 6, cols[5] - 10, ry + 26 },
                              gui::widen(c.get_str("path", "")), gui_.theme.dim, gui_.font_small);
            (void)save;
        }
        gui_.text(cols[5], ry + 6, is_def ? L"✓ 默认" : L"", gui_.theme.ok, gui_.font_small);
        if (gui_.button(("core-ck-" + id).c_str(), { cols[6], ry + 2, cols[6] + 70, ry + 26 },
                        L"检查", 0)) {
            action_check_update(id);
        }
        RECT b2{ cols[7], ry + 2, cols[7] + 60, ry + 26 };
        RECT b3{ cols[7] + 68, ry + 2, cols[7] + 128, ry + 26 };
        if (gui_.button(("core-def-" + id).c_str(), b2, L"设默认", 0, !is_def)) {
            post_action([this, id] {
                hub_->cores().set_default(id);
                set_notice("已设置默认核心");
            }, "设置默认核心");
        }
        if (gui_.button(("core-dl-" + id).c_str(), b3, L"删除", 2)) {
            post_action([this, id] {
                hub_->cores().remove(id);
                set_notice("已移除核心登记");
            }, "删除核心");
        }
        ry += row_h;
        gui_.fill({ panel.left + 12, ry - 4, panel.right - 12, ry - 5 }, RGB(245, 247, 250));
    }
    gui_.end_scroll();
}

// ================================================================ 规划页
void MaaHubWindow::page_planner(RECT content) {
    mj::Value snap;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        snap = snap_;
    }
    RECT ir{ content.left, content.top, content.right - 300, content.top + 30 };
    gui_.text_input("goal-input", ir, goal_, L"输入目标，例如：清理所有执行器的今日日常");
    RECT pr{ content.right - 290, content.top, content.right - 170, content.top + 30 };
    static const std::vector<std::wstring> popts = { L"默认", L"LLM", L"启发式" };
    int clicked = gui_.option_strip("planner-sel", pr, popts, planner_sel_);
    if (clicked >= 0) planner_sel_ = clicked;
    RECT br{ content.right - 160, content.top, content.right, content.top + 30 };
    if (gui_.button("btn-propose", br, L"生成计划", 1)) {
        std::string pl = planner_sel_ == 1 ? "llm" : planner_sel_ == 2 ? "heuristic" : "";
        std::string g = goal_;
        post_action([this, g, pl] { action_propose(g, pl); }, "生成计划");
    }

    int y = content.top + 42;
    mj::Value plan;
    if (!last_plan_json_.empty() && mj::try_parse(last_plan_json_, plan)) {
        std::string status = plan.get_str("status", "proposed");
        std::string pid = plan.get_str("id", "");
        RECT panel{ content.left, y, content.right, y + 300 };
        gui_.panel(panel, L"🧠 计划");
        int ty = panel.top + 36;
        gui_.text(panel.left + 18, ty, L"规划器: " + gui::widen(plan.get_str("planner", "")),
                  gui_.theme.dim, gui_.font_small);
        gui_.text(panel.left + 110, ty, L"状态: " + gui::widen(status), gui_.theme.dim, gui_.font_small);
        gui_.text(panel.left + 200, ty, L"#" + gui::widen(pid), gui_.theme.dim, gui_.font_small);
        ty += 20;
        gui_.text(panel.left + 18, ty, gui::widen(plan.get_str("rationale", "")), gui_.theme.text, gui_.font);
        ty += 24;
        if (auto* ws = plan.find("warnings"); ws && ws->is_array() && !ws->items().empty()) {
            std::wstring w = L"⚠ ";
            for (auto& x : ws->items()) w += gui::widen(x.as_string()) + L" | ";
            RECT save = gui_.clip();
            gui_.text_clipped({ panel.left + 18, ty, panel.right - 18, ty + 18 },
                              w.substr(0, 400), gui_.theme.warn, gui_.font_small);
            (void)save;
            ty += 22;
        }
        int cols[] = { panel.left + 18, panel.left + 60, panel.left + 230, panel.left + 420,
                       panel.left + 600 };
        const wchar_t* heads[] = { L"#", L"执行器", L"任务", L"参数", L"理由" };
        for (int i = 0; i < 5; ++i) gui_.text(cols[i], ty, heads[i], gui_.theme.dim, gui_.font_small);
        ty += 20;
        gui_.fill({ panel.left + 14, ty - 4, panel.right - 14, ty - 3 }, gui_.theme.border);
        if (auto* steps = plan.find("steps"); steps && steps->is_array()) {
            int i = 0;
            for (auto& st : steps->items()) {
                ++i;
                gui_.text(cols[0], ty + 3, std::to_wstring(i), gui_.theme.dim, gui_.font_small);
                gui_.text(cols[1], ty + 3, gui::widen(st.get_str("exec_label", "")),
                          gui_.theme.text, gui_.font_small);
                gui_.text(cols[2], ty + 3, gui::widen(st.get_str("task", "")),
                          gui_.theme.primary, gui_.font_small);
                {
                    RECT save = gui_.clip();
                    gui_.text_clipped({ cols[3], ty + 3, cols[4] - 10, ty + 21 },
                                      gui::widen(mj::Value::dump(st["params"])),
                                      gui_.theme.dim, gui_.font_small);
                    (void)save;
                }
                {
                    RECT save = gui_.clip();
                    gui_.text_clipped({ cols[4], ty + 3, panel.right - 18, ty + 21 },
                                      gui::widen(st.get_str("reason", "")),
                                      gui_.theme.dim, gui_.font_small);
                    (void)save;
                }
                ty += 22;
            }
        }
        RECT a1{ panel.left + 18, panel.bottom - 38, panel.left + 138, panel.bottom - 10 };
        RECT a2{ panel.left + 146, panel.bottom - 38, panel.left + 216, panel.bottom - 10 };
        RECT a3{ panel.left + 224, panel.bottom - 38, panel.left + 314, panel.bottom - 10 };
        if (gui_.button("plan-exec", a1, L"✅ 确认并执行", 1,
                        status == "proposed" || status == "confirmed")) {
            post_action([this, pid] { action_execute(pid); }, "执行计划");
        }
        if (gui_.button("plan-reject", a2, L"✖ 拒绝", 2, status == "proposed")) {
            post_action([this, pid] { action_reject(pid); }, "拒绝计划");
        }
        if (gui_.button("plan-stop", a3, L"⏹ 停止执行", 3)) {
            post_action([this] { action_stop_exec(); }, "停止执行");
        }
        y = panel.bottom + 12;
    }

    RECT hpanel{ content.left, y, content.right, content.bottom };
    gui_.panel(hpanel, L"🕘 计划历史");
    int ty = hpanel.top + 36;
    int cols[] = { hpanel.left + 16, hpanel.left + 200, hpanel.left + 300, hpanel.left + 800,
                   hpanel.left + 880 };
    const wchar_t* heads[] = { L"时间", L"规划器", L"目标", L"步骤", L"状态" };
    for (int i = 0; i < 5; ++i) gui_.text(cols[i], ty, heads[i], gui_.theme.dim, gui_.font_small);
    ty += 22;
    gui_.fill({ hpanel.left + 12, ty - 4, hpanel.right - 12, ty - 3 }, gui_.theme.border);
    std::vector<mj::Value> plans;
    if (auto* p = snap.find("plans"); p && p->is_array()) plans = p->items();
    int row_h = 26;
    gui_.begin_scroll("plans-scroll", { hpanel.left + 10, ty, hpanel.right - 10, hpanel.bottom - 10 },
                      (int)plans.size() * row_h + 10, scroll_["plans"]);
    int ry = ty + 4 - (int)scroll_["plans"];
    for (auto& p : plans) {
        if (ry + row_h < ty) { ry += row_h; continue; }
        if (!gui_.row_fully_visible(ry, row_h)) break;   // 按裁剪区底剔除，别裁半行
        gui_.text(cols[0], ry + 4, gui::widen(util::now_str()), gui_.theme.dim, gui_.font_small);
        gui_.text(cols[1], ry + 4, gui::widen(p.get_str("planner", "")), gui_.theme.text, gui_.font_small);
        {
            RECT save = gui_.clip();
            gui_.text_clipped({ cols[2], ry + 4, cols[3] - 10, ry + 22 },
                              gui::widen(p.get_str("goal", "")), gui_.theme.dim, gui_.font_small);
            (void)save;
        }
        int n = 0;
        if (auto* st = p.find("steps")) n = (int)st->items().size();
        gui_.text(cols[3], ry + 4, std::to_wstring(n), gui_.theme.dim, gui_.font_small);
        gui_.text(cols[4], ry + 4, gui::widen(p.get_str("status", "")), gui_.theme.dim, gui_.font_small);
        ry += row_h;
    }
    gui_.end_scroll();
}

// ================================================================ 事件页
void MaaHubWindow::page_events(RECT content) {
    mj::Value snap;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        snap = snap_;
    }
    RECT panel{ content.left, content.top, content.right, content.bottom };
    gui_.panel(panel, L"📡 实时事件（全部执行器）");
    int row_h = 26;
    std::vector<mj::Value> evs;
    if (auto* e = snap.find("events"); e && e->is_array()) evs = e->items();
    gui_.begin_scroll("ev-scroll", { panel.left + 10, panel.top + 40, panel.right - 10, panel.bottom - 10 },
                      (int)evs.size() * row_h + 10, scroll_["ev"]);
    int ry = panel.top + 44 - (int)scroll_["ev"];
    if (evs.empty()) gui_.text(panel.left + 16, ry + 8, L"暂无事件", gui_.theme.dim, gui_.font_small);
    for (auto& e : evs) {
        if (ry + row_h < panel.top + 40) { ry += row_h; continue; }
        if (!gui_.row_fully_visible(ry, row_h)) break;   // 按裁剪区底剔除，别裁半行
        std::string cat = e.get_str("category", "");
        COLORREF c = cat == "error" ? gui_.theme.err :
                     cat == "connection" ? gui_.theme.warn : gui_.theme.dim;
        std::wstring el = e.get_str("exec_label", "").empty()
            ? gui::widen(e.get_str("exec_id", "")) : gui::widen(e.get_str("exec_label", ""));
        gui_.text(panel.left + 16, ry + 4, el, gui_.theme.primary, gui_.font_small);
        gui_.text(panel.left + 170, ry + 4, gui::widen(e.get_str("what", "")), c, gui_.font_small);
        {
            RECT save = gui_.clip();
            gui_.text_clipped({ panel.left + 420, ry + 4, panel.right - 16, ry + 24 },
                              gui::widen(e.get_str("raw", "")), gui_.theme.dim, gui_.font_small);
            (void)save;
        }
        ry += row_h;
    }
    gui_.end_scroll();
}

// ================================================================ 数据辅助
// 返回拷贝而非指针：快照会被后台线程整体替换，裸指针会悬空
bool MaaHubWindow::find_executor(const std::string& id, mj::Value& out) const {
    std::lock_guard<std::mutex> lk(snap_mtx_);
    if (auto* e = snap_.find("executors"); e && e->is_array()) {
        for (auto& x : e->items()) {
            if (x.get_str("id", "") == id) { out = x; return true; }
        }
    }
    return false;
}

std::vector<mj::Value> MaaHubWindow::executors_of(const std::string& kind) const {
    std::vector<mj::Value> out;
    std::lock_guard<std::mutex> lk(snap_mtx_);
    if (auto* e = snap_.find("executors"); e && e->is_array()) {
        for (auto& x : e->items()) {
            if (kind.empty() || x.get_str("kind", "") == kind) out.push_back(x);
        }
    }
    return out;
}

std::vector<mj::Value> MaaHubWindow::catalog_of(const std::string& kind) const {
    // 鍙蹇収缂撳瓨锛岀粷涓嶅湪 UI 绾跨▼璋冪敤 worker锛堥伩鍏嶉樆濉炴覆鏌擄級
    std::lock_guard<std::mutex> lk(snap_mtx_);
    if (auto* e = snap_.find("executors"); e && e->is_array()) {
        for (auto& x : e->items()) {
            if (x.get_str("kind", "") != kind) continue;
            if (auto* c = x.find("catalog"); c && c->is_array() && !c->items().empty()) {
                return c->items();
            }
        }
    }
    return {};
}

// ================================================================ 后台线程
// 即时快照：只读持久化文件，不碰 worker 管道（毫秒级），用于启动首帧
void MaaHubWindow::refresh_snapshot_fast() {
    mj::Value v = hub_->snapshot_fast();
    std::lock_guard<std::mutex> lk(snap_mtx_);
    snap_ = std::move(v);
    ++snap_version_;
}

// 测试用：选中指定任务并展开其参数面板（按任务类型挑选执行器）
void MaaHubWindow::debug_select_task(const std::string& task) {
    // MaaEnd 专属任务 -> 选 maaend 执行器；否则选 maa 执行器
    static const char* kEndOnly[] = { "AccountSwitch", "DailyRewards", "CreditShopping",
                                      "AutoCollect", "ProtocolSpace", "GiftOperator",
                                      "VisitFriends", "BatchAddFriends", "ReadAllWiki" };
    // 先按已知 MaaEnd 任务判断；未知任务则优先 MaaEnd（其任务覆盖面更大）
    bool want_end = true;
    for (auto* t : kEndOnly) {
        if (task == t) { want_end = true; break; }
    }
    static const char* kMaaOnly[] = { "StartUp", "CloseDown", "Fight", "Infrast",
                                      "Recruit", "Mall", "Award", "Roguelike",
                                      "Copilot", "Depot", "OperBox", "Reclamation",
                                      "Custom" };
    for (auto* t : kMaaOnly) {
        if (task == t) { want_end = false; break; }
    }
    auto ids = hub_->executors();
    std::string pick, fallback;
    for (auto& e : ids) {
        if ((e.kind == "maaend") == want_end) { pick = e.id; break; }
        if (fallback.empty()) fallback = e.id;
    }
    if (pick.empty()) pick = fallback;
    if (pick.empty() && !ids.empty()) pick = ids[0].id;
    if (pick.empty()) return;
    selected_exec_ = pick;
    auto& m = task_ui_[selected_exec_];
    for (auto& kv : m) kv.second.selected = false;
    TaskUiState& ui = m[task];
    ui.selected = true;
    ui.open = true;
    fprintf(stderr, "[render] selected exec=%s task=%s open=%d\n",
            selected_exec_.c_str(), task.c_str(), (int)ui.open);
}

void MaaHubWindow::refresh_snapshot() {
    mj::Value s = hub_->snapshot();
    std::lock_guard<std::mutex> lk(snap_mtx_);
    snap_ = std::move(s);
    ++snap_version_;
}

void MaaHubWindow::backend_loop() {
    // 渐进式启动：先立刻产出「即时快照」（只读持久化，毫秒级），
    // 让左侧执行器立即可见；worker 启动/核心加载在后台并行进行，就绪后增量刷新。
    double t0 = util::now_sec();
    hub_->ensure_default_executors();
    refresh_snapshot_fast();                       // ← 即时帧（不等 worker）
    double t1 = util::now_sec();
    PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);        // 立刻让 UI 画出来
    hub_->start_all();                             // 并行拉起 worker + 加载核心
    double t2 = util::now_sec();
    hub_->refresh_catalogs();
    refresh_snapshot();
    double t3 = util::now_sec();
    PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);
    fprintf(stderr, "[startup] first_paint=%.0fms start_all=%.0fms full_ready=%.0fms\n",
            (t1 - t0) * 1000, (t2 - t1) * 1000, (t3 - t0) * 1000);
    while (running_) {
        std::pair<std::function<void()>, std::string> job;
        bool have_job = false;
        {
            std::lock_guard<std::mutex> lk(action_mtx_);
            if (!actions_.empty()) {
                job = std::move(actions_.front());
                actions_.pop_front();
                have_job = true;
            }
        }
        if (have_job) {
            try {
                job.first();
            } catch (const std::exception& e) {
                set_notice(std::string("操作失败: ") + e.what());
            } catch (...) {
                set_notice("操作失败");
            }
            // 长动作（connect/拉取）执行期间若已请求退出，立即跳出不再刷新
            if (!running_) return;
            refresh_snapshot();
            PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);
        } else {
            refresh_snapshot();
            PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);
            Sleep(500);
        }
    }
}

void MaaHubWindow::post_action(std::function<void()> fn, const std::string& busy_text) {
    std::lock_guard<std::mutex> lk(action_mtx_);
    // 队列上限：连点时丢弃后续（保留最新），避免动作堆积把 backend 线程堵死
    if (actions_.size() >= 8) {
        actions_.pop_front();
    }
    actions_.push_back({ std::move(fn), busy_text });
}

void MaaHubWindow::set_notice(const std::string& s) {
    core_msg_ = s;
    core_msg_ts_ = util::now_sec();
    std::lock_guard<std::mutex> lk(snap_mtx_);
    ++snap_version_;
}

// ================================================================ 动作
void MaaHubWindow::action_create_executor() {
    std::string kind = new_kind_ == 0 ? "maa" : "maaend";
    std::string account_id;
    if (new_acc_idx_ > 0) {
        std::vector<mj::Value> list;
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            if (auto* a = snap_.find("accounts"); a && a->is_array()) {
                for (auto& x : a->items()) {
                    if (x.get_str("game", "") == kind) list.push_back(x);
                }
            }
        }
        if (new_acc_idx_ - 1 < (int)list.size()) {
            account_id = list[new_acc_idx_ - 1].get_str("id", "");
        }
    }
    std::string label = new_label_;
    std::string id = hub_->create_executor(kind, label, account_id);
    if (id.empty()) {
        set_notice("创建失败：没有可用核心，请先在「核心管理」中登记或拉取");
        return;
    }
    selected_exec_ = id;
    page_ = "executor";
    show_new_exec_ = false;
    new_label_.clear();
    new_acc_idx_ = 0;
    set_notice("已创建执行器 " + label);
}

void MaaHubWindow::action_delete_executor(const std::string& id) {
    hub_->remove_executor(id);
    if (selected_exec_ == id) {
        selected_exec_.clear();
        page_ = "welcome";
    }
    set_notice("已删除执行器");
}

void MaaHubWindow::action_connect(const std::string& id) {
    bool ok = hub_->connect_executor(id);
    set_notice(ok ? "已连接" : "连接失败：" + hub_->executor_status(id).get_str("last_error", ""));
}

void MaaHubWindow::action_disconnect(const std::string& id) {
    hub_->disconnect_executor(id);
    set_notice("已断开");
}

void MaaHubWindow::action_stop(const std::string& id) {
    hub_->stop_executor(id);
    set_notice("已请求停止");
}

void MaaHubWindow::action_run(const std::string& id, const std::string& task,
                              const mj::Value& params) {
    int64_t tid = hub_->run_on_executor(id, task, params);
    set_notice(tid > 0 ? task + " 已启动 (task " + std::to_string(tid) + ")"
                       : task + " 启动失败：" + hub_->executor_status(id).get_str("last_error", ""));
}

void MaaHubWindow::action_run_checked(const std::string& id) {
    ExecutorEntry e;
    if (!hub_->get_executor(id, e)) return;
    mj::Value cat = hub_->executor_catalog(id);
    // 按用户拖拽后的顺序执行（与左列显示一致）
    std::vector<mj::Value> items;
    if (cat.is_array()) {
        for (auto& t : cat.items()) {
            if (e.kind == "maaend" && maaend_hidden_task(t.get_str("name", ""))) continue;
            items.push_back(t);
        }
    }
    auto oit = task_order_.find(id);
    if (oit != task_order_.end() && !oit->second.empty()) {
        std::vector<mj::Value> sorted;
        for (auto& nm : oit->second) {
            for (auto& t : items) {
                if (t.get_str("name", "") == nm) { sorted.push_back(t); break; }
            }
        }
        for (auto& t : items) {
            bool found = false;
            for (auto& s : sorted) {
                if (s.get_str("name", "") == t.get_str("name", "")) { found = true; break; }
            }
            if (!found) sorted.push_back(t);
        }
        items.swap(sorted);
    }
    int started = 0;
    {
        for (auto& t : items) {
            std::string name = t.get_str("name", "");
            auto it = task_ui_.find(id);
            if (it == task_ui_.end()) continue;
            auto tit = it->second.find(name);
            if (tit == it->second.end()) continue;
            if (!tit->second.checked && !tit->second.once) continue;
            mj::Value params = mj::Value::object();
            for (auto& kv : tit->second.params) params.set(kv.first, kv.second);
            if (hub_->run_on_executor(id, name, params) > 0) {
                ++started;
                // 「仅一次」运行后复位，且不写入配置
                tit->second.once = false;
            }
        }
    }
    set_notice(started > 0 ? "已启动 " + std::to_string(started) + " 个任务"
                           : "没有勾选的任务（或启动失败）");
}

void MaaHubWindow::action_propose(const std::string& goal, const std::string& planner) {
    mj::Value p = hub_->propose_plan(goal, planner);
    std::lock_guard<std::mutex> lk(snap_mtx_);
    last_plan_json_ = mj::Value::dump(p);
    ++snap_version_;
}

void MaaHubWindow::action_execute(const std::string& plan_id) {
    bool ok = hub_->execute_plan(plan_id);
    set_notice(ok ? "计划已开始执行" : "计划不存在或无法执行");
}

void MaaHubWindow::action_reject(const std::string& plan_id) {
    hub_->update_plan_status(plan_id, "rejected");
    set_notice("计划已拒绝");
}

void MaaHubWindow::action_stop_exec() {
    hub_->stop_execution();
    set_notice("已请求停止执行");
}

void MaaHubWindow::action_check_update(const std::string& core_id) {
    std::string kind = "maa";
    std::string current;
    if (!core_id.empty()) {
        CoreInfo c;
        if (hub_->cores().get(core_id, c)) {
            kind = c.kind;
            current = c.version;
        }
    } else {
        CoreInfo c = hub_->cores().default_core("maa");
        if (!c.id.empty()) current = c.version;
    }
    AppSettings st = hub_->app_settings();
    UpdateInfo ui = CoreManager::check_update(kind, current, st.mirrorchyan_cdk, &st);
    ui.kind = kind;
    update_info_ = ui;
    has_update_info_ = true;
    if (ui.ok) {
        set_notice(ui.has_update ? "发现新版本 " + ui.latest : "已是最新版本 " + ui.latest);
    } else {
        set_notice("更新检查失败：" + ui.error);
    }
}

void MaaHubWindow::action_pull_core(const std::string& kind, const std::string& url) {
    dl_progress_ = 0;
    dl_stage_ = "下载";
    AppSettings st = hub_->app_settings();
    std::string id = CoreManager::pull_core(kind, url, "", &dl_progress_, &dl_stage_, &st);
    dl_progress_ = -1;
    if (id.empty()) {
        set_notice("拉取失败：下载或解压出错（请检查网络/镜像源）");
    } else {
        CoreInfo c;
        hub_->cores().get(id, c);
        set_notice("已拉取新核心：" + c.name + " " + c.version);
    }
}

void MaaHubWindow::action_pull_latest(const std::string& kind) {
    // 检查更新 -> 有新版则直接拉取安装到内部 cores 目录
    AppSettings st = hub_->app_settings();
    std::string current;
    {
        CoreInfo c = hub_->cores().default_core(kind);
        if (!c.id.empty()) current = c.version;
    }
    UpdateInfo ui = CoreManager::check_update(kind, current, st.mirrorchyan_cdk, &st);
    ui.kind = kind;
    update_info_ = ui;
    has_update_info_ = true;
    if (!ui.ok) {
        set_notice("更新检查失败：" + ui.error);
        return;
    }
    if (!ui.has_update) {
        set_notice("已是最新版本 " + ui.latest);
        return;
    }
    if (ui.download_url.empty()) {
        set_notice("未找到可用的下载地址");
        return;
    }
    set_notice("开始拉取 " + ui.latest);
    action_pull_core(kind, ui.download_url);
}

// ================================================================ 设置页
void MaaHubWindow::page_settings(RECT content) {
    // 首次进入从快照回填
    if (!settings_loaded_) {
        mj::Value snap;
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            snap = snap_;
        }
        if (auto* s = snap.find("settings")) {
            AppSettings st = AppSettings::from_json(*s);
            set_proxy_mode_ = st.proxy_mode == "http" ? 1 :
                              st.proxy_mode == "socks5" ? 2 : 0;
            set_proxy_host_ = st.proxy_host;
            set_proxy_port_ = std::to_string(st.proxy_port);
            set_proxy_user_ = st.proxy_user;
            set_proxy_pass_ = st.proxy_pass;
            set_cdk_ = st.mirrorchyan_cdk;
            set_llm_enabled_ = st.llm_enabled;
            set_llm_url_ = st.llm_base_url;
            set_llm_key_ = st.llm_api_key;
            set_llm_model_ = st.llm_model;
        }
        settings_loaded_ = true;
    }

    int y = content.top;

    // ---- 更新代理 ----
    RECT p1{ content.left, y, content.right, y + 150 };
    gui_.panel(p1, L"🌐 更新代理（用于更新检查与核心拉取）");
    int yy = p1.top + 40;
    int mi = gui_.option_strip("set-proxy-mode",
                               { p1.left + 18, yy, p1.left + 330, yy + 26 },
                               { L"不使用代理", L"HTTP 代理", L"SOCKS5 代理" }, set_proxy_mode_);
    if (mi >= 0) set_proxy_mode_ = mi;
    yy += 34;
    gui_.text(p1.left + 18, yy + 4, L"地址", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-proxy-host", { p1.left + 56, yy, p1.left + 300, yy + 26 },
                    set_proxy_host_, L"127.0.0.1");
    gui_.text(p1.left + 316, yy + 4, L"端口", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-proxy-port", { p1.left + 352, yy, p1.left + 452, yy + 26 },
                    set_proxy_port_, L"7890");
    yy += 34;
    gui_.text(p1.left + 18, yy + 4, L"用户名", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-proxy-user", { p1.left + 66, yy, p1.left + 280, yy + 26 },
                    set_proxy_user_, L"可选");
    gui_.text(p1.left + 296, yy + 4, L"密码", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-proxy-pass", { p1.left + 334, yy, p1.left + 548, yy + 26 },
                    set_proxy_pass_, L"可选");
    {
        RECT save = gui_.clip();
        gui_.text_clipped({ p1.left + 570, yy + 4, p1.right - 16, yy + 24 },
                          L"HTTP 代理形如 127.0.0.1:7890（Clash/v2ray 混合端口）",
                          gui_.theme.dim, gui_.font_small);
        (void)save;
    }
    y = p1.bottom + 14;

    // ---- 镜像源 ----
    RECT p2{ content.left, y, content.right, y + 96 };
    gui_.panel(p2, L"📡 镜像源");
    yy = p2.top + 38;
    gui_.text(p2.left + 18, yy + 4, L"Mirror酱 CDK", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-cdk", { p2.left + 110, yy, p2.left + 420, yy + 26 },
                    set_cdk_, L"可选，填写后优先使用 Mirror酱 下载");
    {
        RECT save = gui_.clip();
        gui_.text_clipped({ p2.left + 440, yy + 4, p2.right - 16, yy + 24 },
                          L"留空则按 GitHub → gh-proxy → ghfast → moeyy 顺序回退",
                          gui_.theme.dim, gui_.font_small);
        (void)save;
    }
    y = p2.bottom + 14;

    // ---- LLM ----
    RECT p3{ content.left, y, content.right, y + 150 };
    gui_.panel(p3, L"🧠 LLM 规划（OpenAI 兼容接口）");
    yy = p3.top + 38;
    gui_.checkbox("set-llm-enabled", { p3.left + 18, yy, p3.left + 200, yy + 24 },
                  L"启用 LLM 规划", set_llm_enabled_);
    yy += 32;
    gui_.text(p3.left + 18, yy + 4, L"Base URL", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-llm-url", { p3.left + 96, yy, p3.left + 420, yy + 26 },
                    set_llm_url_, L"https://api.deepseek.com");
    gui_.text(p3.left + 436, yy + 4, L"模型", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-llm-model", { p3.left + 480, yy, p3.left + 680, yy + 26 },
                    set_llm_model_, L"deepseek-chat");
    yy += 32;
    gui_.text(p3.left + 18, yy + 4, L"API Key", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-llm-key", { p3.left + 96, yy, p3.left + 480, yy + 26 },
                    set_llm_key_, L"sk-...");
    {
        RECT save = gui_.clip();
        gui_.text_clipped({ p3.left + 500, yy + 4, p3.right - 16, yy + 24 },
                          L"未配置或不可达时自动回退启发式规划",
                          gui_.theme.dim, gui_.font_small);
        (void)save;
    }
    y = p3.bottom + 18;

    // ---- 保存 ----
    RECT sb{ content.left, y, content.left + 160, y + 34 };
    if (gui_.button("set-save", sb, L"💾 保存设置", 1)) {
        action_save_settings();
    }
    if (!core_msg_.empty()) {
        RECT save = gui_.clip();
        gui_.text_clipped({ content.left + 180, y + 8, content.right, y + 30 },
                          gui::widen(core_msg_),
                          core_msg_.find("失败") != std::string::npos
                              ? gui_.theme.err : gui_.theme.ok,
                          gui_.font);
        (void)save;
    }
}

void MaaHubWindow::action_save_settings() {
    int port = 7890;
    try { port = std::stoi(set_proxy_port_); } catch (...) { port = 7890; }
    std::string mode = set_proxy_mode_ == 1 ? "http" : set_proxy_mode_ == 2 ? "socks5" : "none";
    bool enabled = set_llm_enabled_;
    std::string url = set_llm_url_, key = set_llm_key_, model = set_llm_model_;
    std::string host = set_proxy_host_, user = set_proxy_user_, pass = set_proxy_pass_;
    std::string cdk = set_cdk_;
    post_action([this, mode, host, port, user, pass, cdk, enabled, url, key, model] {
        hub_->settings().update([&](AppSettings& s) {
            s.proxy_mode = mode;
            s.proxy_host = host;
            s.proxy_port = port;
            s.proxy_user = user;
            s.proxy_pass = pass;
            s.mirrorchyan_cdk = cdk;
            s.llm_enabled = enabled;
            s.llm_base_url = url;
            s.llm_api_key = key;
            s.llm_model = model;
        });
        set_notice("设置已保存");
    }, "保存设置");
}

} // namespace hub
