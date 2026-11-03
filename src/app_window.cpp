// MaaHub 原生窗口应用实现
#pragma comment(lib, "comdlg32.lib")   // GetOpenFileNameA（作业文件选择）
#include "app_window.hpp"
#include "task_panels.hpp"

#include <windowsx.h>
#include <commdlg.h>

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
    load_task_list();
    load_post_action();
    load_task_alias();
    // MAA 启动直接进入任务队列页（无欢迎页）。MaaHub 有执行器时同样
    // 自动选中第一个，让首屏即呈现与 MAA 一致的实例界面；
    // 完全没有执行器时才落到欢迎页引导创建。
    {
        auto ids = hub_->executors();
        if (!ids.empty()) selected_exec_ = ids[0].id;
    }
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

    int w = 800, h = 600;       // 官方 RootView Width=800 Height=600（最小值同）
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

    // 应用界面主题（官方 界面设置-界面主题：light/dark/system）
    {
        std::string tm = hub_->settings().get().theme;
        bool dark = false;
        if (tm == "dark") dark = true;
        else if (tm == "system") {
            // 跟随系统：读注册表 AppsUseLightTheme
            HKEY hk = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    0, KEY_READ, &hk) == ERROR_SUCCESS) {
                DWORD v = 1, sz = sizeof(v);
                if (RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, nullptr,
                        (LPBYTE)&v, &sz) == ERROR_SUCCESS)
                    dark = (v == 0);
                RegCloseKey(hk);
            }
        }
        gui_.set_theme_mode(dark);
    }
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
    // 用窗口实际客户区尺寸（官方 800x600），保证离屏渲染与真实一致
    RECT cl{ 0, 0, 0, 0 };
    if (hwnd_) GetClientRect(hwnd_, &cl);
    const int W = (cl.right - cl.left) > 0 ? (cl.right - cl.left) : 800;
    const int H = (cl.bottom - cl.top) > 0 ? (cl.bottom - cl.top) : 600;
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
    case WM_NCCALCSIZE:
        // 自定义标题栏：客户区覆盖整个窗口（保留边框由 NCHITTEST 处理缩放）
        if (wp) {
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
            RECT& r = p->rgrc[0];
            // 保留 1px 边框用于边缘缩放命中
            r.left += 1; r.right -= 1; r.bottom -= 1; r.top += 1;
            return 0;
        }
        return DefWindowProcW(hwnd, m, wp, lp);
    case WM_NCHITTEST: {
        // 自定义标题栏命中测试：边缘 4px 缩放、标题栏拖拽
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        const int B = 4;
        bool left = pt.x < B, right = pt.x >= rc.right - B;
        bool top = pt.y < B, bottom = pt.y >= rc.bottom - B;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (pt.y < 32) return HTCAPTION;      // 标题栏区域可拖动
        return HTCLIENT;
    }
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

    // 自定义标题栏（官方 HandyControl ShowTitle=False + NonClientAreaContent）
    const int TITLE_H = 32;
    RECT titlebar{ client.left, client.top, client.right, client.top + TITLE_H };
    draw_title_bar(titlebar);

    // 官方外壳：顶部 Tab 栏（TabControlInLine）+ 全宽内容区
    RECT tabbar{ client.left, client.top + TITLE_H, client.right,
                 client.top + TITLE_H + topbar_h_ };
    // 官方窗口 800x600，内容区边距收窄到 10（TaskQueueView 内部仍是 Margin 20 的比例）
    RECT content{ client.left + 10, client.top + TITLE_H + topbar_h_ + 12,
                  client.right - 10, client.bottom - 8 };

    draw_top_tabs(tabbar);
    draw_gif_widget(client);

    try {
        if (page_ == "executor" && !selected_exec_.empty()) page_executor(content);
        else if (page_ == "copilot") page_copilot(content);
        else if (page_ == "toolbox") page_toolbox(content);
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

// 右下角 GIF 挂件（官方 RootView 的 hc:GifImage）
// 规格：100x100、Margin 10、右下角、可拖拽、右键切换、默认可见。
// MaaHub 未内置 GIF 资源，这里绘制一个等大的可拖拽占位区域：
// 有 GIF 文件时用 GDI+ 播放，无则画一个 Primary 色圆角块 + 提示文字。
void MaaHubWindow::draw_gif_widget(RECT client) {
    const int SZ = 100;                 // 官方 Width=100
    const int BOTTOM_RESERVE = 60;      // 避开底栏控制区（MaaHub 底栏比官方多连接管理）
    int x = (gif_x_ >= 0) ? gif_x_ : (client.right - SZ - 10);
    int y = (gif_y_ >= 0) ? gif_y_ : (client.bottom - SZ - 10 - BOTTOM_RESERVE);
    RECT wr{ x, y, x + SZ, y + SZ };

    const POINT& m = gui_.input().mouse;
    bool inside = m.x >= wr.left && m.x < wr.right && m.y >= wr.top && m.y < wr.bottom;

    // 拖拽（官方 DraggableElementMouseLeftButton*）
    if (inside && gui_.input().pressed) {
        gif_dragging_ = true;
        gif_drag_dx_ = m.x - wr.left;
        gif_drag_dy_ = m.y - wr.top;
    }
    if (gif_dragging_) {
        if (gui_.input().down) {
            gif_x_ = m.x - gif_drag_dx_;
            gif_y_ = m.y - gif_drag_dy_;
            // 限制在窗口内
            if (gif_x_ < 0) gif_x_ = 0;
            if (gif_y_ < 0) gif_y_ = 0;
            if (gif_x_ + SZ > client.right) gif_x_ = client.right - SZ;
            if (gif_y_ + SZ > client.bottom - BOTTOM_RESERVE) {
                gif_y_ = client.bottom - SZ - BOTTOM_RESERVE;
            }
        } else {
            gif_dragging_ = false;
        }
    }
    // 右键切换（官方 ChangeGif）：在无内置资源时用于在"显示/隐藏"间切换
    if (inside && gui_.input().rpressed) {
        gif_hidden_ = !gif_hidden_;
    }
    if (gif_hidden_) return;

    // 外观：浅色圆角块 + MaaHub 字样（官方此处是角色 GIF）
    // 用浅色而非 Primary 实底，避免遮挡右栏内容（官方 GIF 也是半透明挂件）
    gui_.round_rect(wr, gui_.theme.card_bg, gui_.theme.primary, 8);
    gui_.text(wr.left + 20, wr.top + 30, L"MaaHub", gui_.theme.primary, gui_.font_bold);
    gui_.text(wr.left + 16, wr.top + 54, L"\u62d6\u62fd\u79fb\u52a8",
              gui_.theme.dim, gui_.font_small);
    if (gif_dragging_) {
        gui_.round_rect(wr, CLR_INVALID, gui_.theme.primary, 8);
    }
}

// ================================================================ 顶部 Tab 导航
// 官方 RootView 用 TabControlInLine：标签在顶部，四个页签
// （一键长草 / 自动战斗 / 小工具 / 设置）。MaaHub 用同样形态的顶部标签，
// 页签为：任务队列 / 账号管理 / 核心管理 / 智能规划 / 事件日志 / 设置。
// 自定义标题栏（官方 NonClientAreaContent）：
//   居中显示动态窗口标题（官方 UpdateWindowTitle：模拟器 + IP:端口）
//   右侧：📌 置顶切换 + 最小化 / 最大化 / 关闭
void MaaHubWindow::draw_title_bar(RECT r) {
    gui_.fill(r, gui_.theme.card_bg);
    gui_.fill({ r.left, r.bottom - 1, r.right, r.bottom }, gui_.theme.border);

    // 动态窗口标题（对齐官方 UpdateWindowTitle 的信息）
    std::wstring title = L"MaaHub";
    if (page_ == "executor") {
        mj::Value cur;
        if (find_executor(selected_exec_, cur)) {
            std::string lbl = cur.get_str("label", "");
            std::string conn = cur.get_str("conn", "");
            std::string addr = cur.get_str("conn_addr", "");
            if (!lbl.empty()) title = gui::widen(lbl);
            if (conn == "connected" && !addr.empty()) {
                title += L" - " + gui::widen(addr);
            }
        }
    }
    SIZE ts = gui_.text_size(title, gui_.font);
    int tx = (r.left + r.right - ts.cx) / 2;
    gui_.text(tx, r.top + (r.bottom - r.top - 18) / 2, title, gui_.theme.text, gui_.font);

    // 右侧按钮：📌 置顶 + 最小化/最大化/关闭（官方 NonClientAreaContent）
    int bw = 40, bh = r.bottom - r.top - 2;
    int bx = r.right - 1;
    const wchar_t* labels[4] = { L"\u00d7", L"\u25a1", L"\u2014", L"\U0001f4cc" };
    const char* ids[4] = { "tb-close", "tb-max", "tb-min", "tb-pin" };
    for (int k = 0; k < 4; ++k) {
        bx -= bw;
        RECT br{ bx, r.top + 1, bx + bw, r.top + 1 + bh };
        bool hov = gui_.input().mouse.x >= br.left && gui_.input().mouse.x < br.right
                && gui_.input().mouse.y >= br.top && gui_.input().mouse.y < br.bottom;
        COLORREF txt = gui_.theme.dim;
        if (hov) {
            gui_.round_rect(br, gui_.theme.hover, gui_.theme.hover, 3);
            txt = gui_.theme.text;
        }
        if (k == 3 && topmost_) txt = gui_.theme.primary;
        gui_.text(br.left + (bw - gui_.text_size(labels[k], gui_.font).cx) / 2,
                  br.top + (bh - 18) / 2, labels[k], txt, gui_.font);
        if (hov && gui_.input().pressed) {
            if (k == 0) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            else if (k == 1) {
                if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
                else ShowWindow(hwnd_, SW_MAXIMIZE);
            } else if (k == 2) ShowWindow(hwnd_, SW_MINIMIZE);
            else {
                topmost_ = !topmost_;
                SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                             0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        bx -= 2;
    }
}

void MaaHubWindow::draw_top_tabs(RECT r) {
    // 官方 TabControlInLine：headerPanel 背景透明（继承 RegionBrushOpacity25）
    gui_.fill(r, gui_.theme.card_bg);
    gui_.fill({ r.left, r.bottom - 1, r.right, r.bottom }, gui_.theme.border);

    struct Tab { const char* page; const wchar_t* label; };
    const Tab tabs[] = {
        // 官方 4 个 Tab（RootViewModel.InitViewModels）：
        //   TaskQueueViewModel=一键长草 / CopilotViewModel=自动战斗 /
        //   ToolboxViewModel=小工具 / SettingsViewModel=设置
        // MaaHub 的账号/核心/智能规划/事件日志是全局管理页，不占实例页 Tab
        { "executor",  L"\u4e00\u952e\u957f\u8349" },
        { "copilot",   L"\u81ea\u52a8\u6218\u6597" },
        { "toolbox",   L"\u5c0f\u5de5\u5177" },
        { "settings",  L"\u8bbe\u7f6e" },
    };
    int x = r.left + 8;
    int y = r.top + 6;
    const int h = r.bottom - r.top - 12;
    for (auto& t : tabs) {
        int w = gui_.text_size(t.label, gui_.font).cx + 28;
        RECT tr{ x, y, x + w, y + h };
        bool sel = (page_ == t.page)
                   || (t.page == "executor" && page_ == "welcome");
        bool hov = gui_.input().mouse.x >= tr.left && gui_.input().mouse.x < tr.right
                && gui_.input().mouse.y >= tr.top && gui_.input().mouse.y < tr.bottom;
        // 官方 TabItemStyle：Padding=DefaultControlPadding(10,5)、MinHeight=28；
        // 选中态 = 文字 PrimaryBrush + 底部 2px 下划线（TitleBrush），
        // 未选中 = PrimaryTextBrush；hover 仅背景微亮（不是整块蓝底白字）
        COLORREF txt = sel ? gui_.theme.primary : gui_.theme.text;
        if (hov && !sel) {
            gui_.round_rect(tr, gui_.theme.hover, gui_.theme.hover, 3);
        }
        gui_.text(tr.left + 10, tr.top + (h - 18) / 2, t.label, txt, gui_.font);
        if (sel) {
            // 底部 2px 下划线（官方选中态标识）
            gui_.fill({ tr.left + 4, tr.bottom - 2, tr.right - 4, tr.bottom },
                      gui_.theme.primary);
        }
        if (hov && gui_.input().pressed) {
            page_ = t.page;
            if (t.page == "executor" && selected_exec_.empty()) page_ = "welcome";
        }
        x += w + 4;

        // MaaHub 全局管理入口（官方 Tab 仅 4 个功能页，这些是 MaaHub 扩展能力，
        // 放在 Tab 栏右侧以小号文字按钮呈现，不干扰官方 Tab 视觉）
        {
            struct G { const char* page; const wchar_t* label; };
            const G gs[] = {
                { "accounts", L"\u8d26\u53f7" },
                { "cores",    L"\u6838\u5fc3" },
                { "planner",  L"\u89c4\u5212" },
                { "events",   L"\u65e5\u5fd7" },
            };
            int gx = r.right - 8;
            for (int gi = 3; gi >= 0; --gi) {
                int gw = gui_.text_size(gs[gi].label, gui_.font_small).cx + 16;
                gx -= gw;
                RECT gr{ gx, y, gx + gw, y + h };
                bool gh = gui_.input().mouse.x >= gr.left && gui_.input().mouse.x < gr.right
                        && gui_.input().mouse.y >= gr.top && gui_.input().mouse.y < gr.bottom;
                if (gh) gui_.round_rect(gr, gui_.theme.hover, gui_.theme.hover, 3);
                gui_.text(gr.left + 8, gr.top + (h - 15) / 2, gs[gi].label,
                          page_ == gs[gi].page ? gui_.theme.primary : gui_.theme.dim,
                          gui_.font_small);
                if (gh && gui_.input().pressed) page_ = gs[gi].page;
                gx -= 6;
            }
        }
    }

    // 右侧：连接状态 + Link Start!/停止（官方底栏按钮组上移到顶栏右侧太挤，
    // 这里只放最简状态；主操作仍在实例页底栏）
    (void)0;
}

// 执行器切换条：横向排列所有执行器（官方 MAA 单实例只有一个，
// MaaHub 多执行器需要横向切换；选中项高亮）
void MaaHubWindow::draw_executor_strip(RECT r) {
    std::vector<ExecutorEntry> execs = hub_->executors();
    if (execs.empty()) return;
    int x = r.left;
    const int h = r.bottom - r.top;
    for (auto& e : execs) {
        std::wstring label = gui::widen(e.label);
        int w = gui_.text_size(label, gui_.font_small).cx + 34;
        if (x + w > r.right) break;              // 放不下就不画（多执行器时可滚动/折叠）
        RECT ir{ x, r.top, x + w, r.bottom };
        bool sel = selected_exec_ == e.id;
        bool hov = gui_.input().mouse.x >= ir.left && gui_.input().mouse.x < ir.right
                && gui_.input().mouse.y >= ir.top && gui_.input().mouse.y < ir.bottom;
        COLORREF fill_c = sel ? gui_.theme.primary_light : gui_.theme.panel;
        COLORREF line_c = sel ? gui_.theme.primary : gui_.theme.border;
        if (hov && !sel) fill_c = gui_.theme.hover;
        gui_.round_rect(ir, fill_c, line_c, 4);
        mj::Value st = hub_->executor_status(e.id, 0.4);
        std::string conn = st.get_str("conn", "unknown");
        std::string run = st.get_str("run", "");
        COLORREF dot = conn == "connected" ? gui_.theme.ok :
                       conn == "failed" ? gui_.theme.err :
                       run == "running" ? gui_.theme.primary : gui_.theme.dim;
        gui_.round_rect({ ir.left + 8, ir.top + (h - 8) / 2, ir.left + 16, ir.top + (h + 8) / 2 },
                        dot, dot, 5);
        gui_.text_clipped({ ir.left + 22, ir.top + 2, ir.right - 4, ir.bottom },
                          label, sel ? gui_.theme.primary_dark : gui_.theme.text,
                          gui_.font_small);
        if (hov && gui_.input().pressed) {
            selected_exec_ = e.id;
            page_ = "executor";
        }
        x += w + 6;
    }
}

// ================================================================ 顶栏
void MaaHubWindow::draw_topbar(RECT r) {
    // 官方 TabControlInLine：headerPanel 背景透明（继承 RegionBrushOpacity25）
    gui_.fill(r, gui_.theme.card_bg);
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

    // 连接 / Link Start! 只在实例页显示（与 MAA 官方一致：按钮是任务页上下文相关的）
    bool on_instance = (page_ == "executor");
    bool connected = has_cur && cur.get_str("conn", "") == "connected";
    if (on_instance) {
        RECT br{ r.right - 320, r.top + 10, r.right - 240, r.bottom - 10 };
        if (gui_.button("btn-connect", br, connected ? L"断开" : L"连接", 0, has_cur)) {
            if (connected) action_disconnect(selected_exec_);
            else action_connect(selected_exec_);
        }
        RECT lr{ r.right - 228, r.top + 10, r.right - 20, r.bottom - 10 };
        bool running = has_cur && cur.get_bool("busy", false);
        if (running) {
            // 运行中：按钮变为「停止」（官方 Common.Stop）
            if (gui_.button("btn-stop", lr, L"\u23f9 \u505c\u6b62", 2, has_cur)) {
                std::string id = selected_exec_;
                post_action([this, id] { hub_->stop_executor(id); },
                            "\u505c\u6b62");
            }
        } else if (gui_.button("btn-linkstart", lr, L"\U0001f517 Link Start!", 1, has_cur)) {
            std::string id = selected_exec_;
            post_action([this, id] {
                mj::Value st = hub_->executor_status(id);
                if (st.get_str("conn", "") != "connected") hub_->connect_executor(id);
                action_run_checked(id);
            }, "\u94fe\u63a5\u5f00\u59cb");
        }
    }
    int dl = dl_progress_.load();
    if (dl >= 0) {
        std::wstring t = L"\u23f3 " + gui::widen(dl_stage_) + L" " + std::to_wstring(dl) + L"%";
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

// 「添加新任务」浮层：勾选要显示在左列的任务（官方左列仅显示用户添加的任务）
void MaaHubWindow::draw_add_task_overlay(RECT area, const std::string& id,
                                         const std::string& kind) {
    // 半透明遮罩（用深色矩形模拟；立即模式 GUI 不支持真 Alpha，用暗色实底）
    RECT mask{ area.left, area.top, area.right, area.bottom };
    gui_.fill(mask, RGB(0x10, 0x10, 0x10));
    RECT card{ area.left + 24, area.top + 16, area.right - 24, area.bottom - 16 };
    gui_.round_rect(card, gui_.theme.panel, gui_.theme.border, 4);

    gui_.text(card.left + 16, card.top + 14, L"\u6dfb\u52a0\u65b0\u4efb\u52a1",
              gui_.theme.text, gui_.font_title);
    gui_.text(card.left + 16, card.top + 40,
              L"\u52fe\u9009\u8981\u5728\u5de6\u4fa7\u4efb\u52a1\u5217\u8868\u4e2d\u663e\u793a\u7684\u4efb\u52a1",
              gui_.theme.dim, gui_.font_small);

    RECT close_btn{ card.right - 76, card.top + 12, card.right - 16, card.top + 36 };
    if (gui_.button("addtask-close", close_btn, L"\u5173\u95ed", 0)) {
        show_add_task_ = false;
        return;
    }

    // 搜索框
    RECT search{ card.left + 16, card.top + 62, card.right - 16, card.top + 88 };
    gui_.text_input("addtask-search", search, add_task_filter_, L"\u641c\u7d22\u4efb\u52a1\u540d\u2026");

    // 任务清单（可滚动）
    std::vector<mj::Value> all = catalog_of(kind);
    RECT list_r{ card.left + 16, card.top + 96, card.right - 16, card.bottom - 56 };
    int row_h = 28;
    int total = (int)all.size();
    gui_.begin_scroll("addtask-list", list_r, total * row_h + 8,
                      scroll_["addtask-list"]);
    int y = list_r.top + 4 - (int)scroll_["addtask-list"];
    for (int i = 0; i < total; ++i) {
        std::string nm = all[i].get_str("name", "");
        if (kind == "maaend" && maaend_hidden_task(nm)) continue;
        // 过滤
        if (!add_task_filter_.empty()) {
            std::wstring dn = display_name(kind, nm);
            std::string dn8 = util::wide_to_utf8(dn);
            if (dn8.find(add_task_filter_) == std::string::npos
                && nm.find(add_task_filter_) == std::string::npos) {
                continue;
            }
        }
        if (y + row_h < list_r.top) { y += row_h; continue; }
        if (y > list_r.bottom) break;
        // 已显示 = 在 task_list_ 中，或未设置过滤（默认全部）
        bool shown = task_list_.find(id) == task_list_.end()
                     || task_list_[id].count(nm) > 0;
        CheckState st = shown ? CheckState::On : CheckState::Off;
        RECT row{ list_r.left, y, list_r.right, y + row_h - 4 };
        if (gui_.checkbox3(("atc-" + id + "-" + nm).c_str(), row,
                           display_name(kind, nm), st)) {
            if (st == CheckState::On) {
                // 第一次操作时，用当前可见集合作为初始值
                if (task_list_.find(id) == task_list_.end()) {
                    for (auto& t : all) {
                        std::string s = t.get_str("name", "");
                        if (kind == "maaend" && maaend_hidden_task(s)) continue;
                        task_list_[id].insert(s);
                    }
                }
                task_list_[id].insert(nm);
            } else {
                if (task_list_.find(id) == task_list_.end()) {
                    for (auto& t : all) {
                        std::string s = t.get_str("name", "");
                        if (kind == "maaend" && maaend_hidden_task(s)) continue;
                        task_list_[id].insert(s);
                    }
                }
                task_list_[id].erase(nm);
            }
            save_task_list();
        }
        y += row_h;
    }
    gui_.end_scroll();

    // 底部：全选 / 反选（与左列底部按钮组一致）
    int half2 = (card.right - card.left - 32) / 2;
    RECT sel_all{ card.left + 16, card.bottom - 44, card.left + 16 + half2, card.bottom - 16 };
    RECT sel_inv{ card.left + 16 + half2 + 4, card.bottom - 44, card.right - 16, card.bottom - 16 };
    if (gui_.button("addtask-all", sel_all, L"\u5168\u9009", 0)) {
        task_list_[id].clear();
        for (auto& t : all) {
            std::string s = t.get_str("name", "");
            if (kind == "maaend" && maaend_hidden_task(s)) continue;
            task_list_[id].insert(s);
        }
        save_task_list();
    }
    if (gui_.button("addtask-inverse", sel_inv, L"\u53cd\u9009", 0)) {
        // 确保有初始集合，再逐个取反
        if (task_list_.find(id) == task_list_.end()) {
            for (auto& t : all) {
                std::string s = t.get_str("name", "");
                if (kind == "maaend" && maaend_hidden_task(s)) continue;
                task_list_[id].insert(s);
            }
        }
        for (auto& t : all) {
            std::string s = t.get_str("name", "");
            if (kind == "maaend" && maaend_hidden_task(s)) continue;
            if (task_list_[id].count(s)) task_list_[id].erase(s);
            else task_list_[id].insert(s);
        }
        save_task_list();
    }
}

// 持久化任务列表（state/task-lists.json）
// 完成后动作设置（官方 TaskQueue.PostAction.*）
// 全部任务执行完后的收尾动作：返回主屏/退出游戏/退出模拟器/退出 MAA/锁屏/睡眠/休眠/关机
void MaaHubWindow::draw_post_action(RECT area, const std::string& id) {
    RECT mask{ area.left, area.top, area.right, area.bottom };
    gui_.fill(mask, RGB(0x10, 0x10, 0x10));
    RECT card{ area.left + 60, area.top + 40, area.right - 60, area.bottom - 40 };
    gui_.round_rect(card, gui_.theme.panel, gui_.theme.border, 4);

    gui_.text(card.left + 16, card.top + 14, L"\u5b8c\u6210\u540e\u52a8\u4f5c\u8bbe\u7f6e",
              gui_.theme.text, gui_.font_title);
    gui_.text(card.left + 16, card.top + 40,
              L"\u5168\u90e8\u4efb\u52a1\u6267\u884c\u5b8c\u6210\u540e\u7684\u6536\u5c3e\u52a8\u4f5c",
              gui_.theme.dim, gui_.font_small);
    RECT close_btn{ card.right - 76, card.top + 12, card.right - 16, card.top + 36 };
    if (gui_.button("postaction-close", close_btn, L"\u5173\u95ed", 0)) {
        show_post_action_ = false;
        return;
    }

    mj::Value& p = post_action_[id];
    auto getb = [&](const char* k) -> bool {
        auto* v = p.find(k);
        return v ? v->as_bool() : false;
    };

    int y = card.top + 70;
    const int row_h = 30;
    struct Item { const char* key; const wchar_t* label; const wchar_t* tip; };
    const Item items[] = {
        { "back_to_android_home", L"\u8fd4\u56de \u6a21\u62df\u5668 \u4e3b\u5c4f\u5e55", nullptr },
        { "exit_arknights",       L"\u9000\u51fa \u660e\u65e5\u65b9\u821e", nullptr },
        { "exit_emulator",        L"\u9000\u51fa \u6a21\u62df\u5668", nullptr },
        { "exit_self",            L"\u9000\u51fa MAA", nullptr },
        { "lock_screen",          L"\u9501\u5c4f",
          L"\u52fe\u9009\u540e\uff0c\u4ec5\u5f53\u6ca1\u6709\u5176\u4ed6 MAA \u5b9e\u4f8b\u8fd0\u884c\u65f6\u624d\u4f1a\u6267\u884c" },
        { "sleep",                L"\u7761\u7720", nullptr },
        { "hibernate",            L"\u4f11\u7720",
          L"\u9700\u5728 Windows \u7535\u6e90\u9009\u9879\u4e2d\u542f\u7528\u4f11\u7720\u529f\u80fd\uff0c\u5426\u5219\u8be5\u529f\u80fd\u65e0\u6548\u3002" },
        { "shutdown",             L"\u5173\u673a", nullptr },
    };
    for (auto& it : items) {
        CheckState st = getb(it.key) ? CheckState::On : CheckState::Off;
        RECT r{ card.left + 20, y, card.right - 20, y + 22 };
        gui_.checkbox3(("pa-" + id + "-" + it.key).c_str(), r, it.label, st);
        p.set(it.key, st == CheckState::On);
        y += row_h;
        if (it.tip) {
            gui_.tip_line(card.left + 44, y - 6, card.right - 20, it.tip);
            y += 16;
        }
    }
    save_post_action(id);
}

// 完成后动作的人类可读描述（对应官方 PostActionSetting.ActionDescription）
std::wstring MaaHubWindow::post_action_desc(const std::string& id) {
    auto it = post_action_.find(id);
    if (it == post_action_.end()) return L"\u5b8c\u6210\u540e\u65e0\u52a8\u4f5c";
    const mj::Value& p = it->second;
    auto on = [&](const char* k) {
        auto* v = p.find(k);
        return v ? v->as_bool() : false;
    };
    std::wstring out;
    auto add = [&](const wchar_t* s) {
        if (!out.empty()) out += L"\u3001";
        out += s;
    };
    if (on("back_to_android_home")) add(L"\u8fd4\u56de\u4e3b\u5c4f\u5e55");
    if (on("exit_arknights")) add(L"\u9000\u51fa\u6e38\u620f");
    if (on("exit_emulator")) add(L"\u9000\u51fa\u6a21\u62df\u5668");
    if (on("exit_self")) add(L"\u9000\u51fa MAA");
    if (on("lock_screen")) add(L"\u9501\u5c4f");
    if (on("sleep")) add(L"\u7761\u7720");
    if (on("hibernate")) add(L"\u4f11\u7720");
    if (on("shutdown")) add(L"\u5173\u673a");
    if (out.empty()) return L"\u5b8c\u6210\u540e\u65e0\u52a8\u4f5c";
    return L"\u5b8c\u6210\u540e\uff1a" + out;
}

// 持久化完成后动作（state/post-actions.json）
void MaaHubWindow::save_post_action(const std::string& id) {
    mj::Value v = mj::Value::object();
    for (auto& kv : post_action_) v.set(kv.first, kv.second);
    util::write_file(util::exe_dir() + "\\state\\post-actions.json",
                     mj::Value::dump_pretty(v));
}

void MaaHubWindow::load_post_action() {
    std::string content;
    if (!util::read_file(util::exe_dir() + "\\state\\post-actions.json", content)) return;
    mj::Value v;
    if (!mj::try_parse(content, v)) return;
    for (auto& kv : v.members()) {
        if (kv.second.is_object()) post_action_[kv.first] = kv.second;
    }
}

void MaaHubWindow::save_task_list() {
    mj::Value v = mj::Value::object();
    for (auto& kv : task_list_) {
        mj::Value arr = mj::Value::array();
        for (auto& nm : kv.second) arr.push(nm);
        v.set(kv.first, std::move(arr));
    }
    util::write_file(util::exe_dir() + "\\state\\task-lists.json",
                     mj::Value::dump_pretty(v));
}

void MaaHubWindow::load_task_list() {
    std::string content;
    if (!util::read_file(util::exe_dir() + "\\state\\task-lists.json", content)) return;
    mj::Value v;
    if (!mj::try_parse(content, v)) return;
    for (auto& kv : v.members()) {
        if (!kv.second.is_array()) continue;
        for (auto& nm : kv.second.items()) {
            if (nm.is_string()) task_list_[kv.first].insert(nm.as_string());
        }
    }
}

// 任务项右键菜单（官方 ContextMenu：单次运行 / 从此处运行 / 重命名 / 复制 / 移出列表）
void MaaHubWindow::draw_task_ctx_menu() {
    if (!ctx_menu_open_) return;
    const int w = 168, row_h = 30;
    struct Item { const wchar_t* label; const char* cmd; };
    const Item items[] = {
        { L"\u5355\u6b21\u8fd0\u884c",     "run_once" },
        { L"\u4ece\u6b64\u5904\u8fd0\u884c", "run_from" },
        { L"\u91cd\u547d\u540d",           "rename" },
        { L"\u590d\u5236",                 "copy" },
        { L"\u79fb\u51fa\u5217\u8868",     "remove" },
    };
    int h = (int)(sizeof(items) / sizeof(items[0])) * row_h + 8;
    RECT menu{ ctx_menu_x_, ctx_menu_y_, ctx_menu_x_ + w, ctx_menu_y_ + h };
    // 防止超出窗口
    RECT cl;
    GetClientRect(hwnd_, &cl);
    if (menu.right > cl.right) { int d = menu.right - cl.right; menu.left -= d; menu.right -= d; }
    if (menu.bottom > cl.bottom) { int d = menu.bottom - cl.bottom; menu.top -= d; menu.bottom -= d; }

    // 遮罩：点击菜单外任意处关闭
    if (gui_.input().pressed
        && !(gui_.input().mouse.x >= menu.left && gui_.input().mouse.x < menu.right
             && gui_.input().mouse.y >= menu.top && gui_.input().mouse.y < menu.bottom)) {
        ctx_menu_open_ = false;
        renaming_task_.clear();
        return;
    }

    gui_.round_rect(menu, gui_.theme.panel, gui_.theme.border, 4);
    int y = menu.top + 4;
    for (auto& it : items) {
        RECT row{ menu.left + 4, y, menu.right - 4, y + row_h - 4 };
        bool hov = gui_.input().mouse.x >= row.left && gui_.input().mouse.x < row.right
                && gui_.input().mouse.y >= row.top && gui_.input().mouse.y < row.bottom;
        if (hov) gui_.round_rect(row, gui_.theme.hover, gui_.theme.hover, 3);
        gui_.text(row.left + 10, row.top + 6, it.label, gui_.theme.text, gui_.font);
        if (hov && gui_.input().pressed) {
            std::string cmd = it.cmd;
            std::string id = ctx_menu_exec_;
            std::string task = ctx_menu_task_;
            ctx_menu_open_ = false;
            if (cmd == "run_once") {
                post_action([this, id, task] {
                    auto it2 = task_ui_.find(id);
                    if (it2 == task_ui_.end()) return;
                    auto t2 = it2->second.find(task);
                    if (t2 == it2->second.end()) return;
                    mj::Value params = mj::Value::object();
                    for (auto& kv : t2->second.params) params.set(kv.first, kv.second);
                    action_run(id, task, params);
                }, "\u5355\u6b21\u8fd0\u884c");
            } else if (cmd == "run_from") {
                post_action([this, id, task] { action_run_from(id, task); },
                            "\u4ece\u6b64\u5904\u8fd0\u884c");
            } else if (cmd == "rename") {
                renaming_task_ = task;
                rename_exec_ = id;
                auto it2 = task_alias_.find(id + "|" + task);
                rename_buf_ = it2 == task_alias_.end() ? "" : it2->second;
            } else if (cmd == "copy") {
                // 复制：把该任务的参数复制为同名副本（别名加 (副本)）
                auto it2 = task_ui_.find(id);
                if (it2 != task_ui_.end()) {
                    auto t2 = it2->second.find(task);
                    if (t2 != it2->second.end()) {
                        std::string alias_key = id + "|" + task;
                        std::string base = task_alias_.count(alias_key)
                            ? task_alias_[alias_key] : task;
                        task_alias_[alias_key] = base + " (\u526f\u672c)";
                        save_task_alias();
                        set_notice("\u5df2\u590d\u5236\u4efb\u52a1\u8bbe\u7f6e");
                    }
                }
            } else if (cmd == "remove") {
                // 移出列表：等价于在添加新任务里取消勾选
                if (task_list_.find(id) == task_list_.end()) {
                    for (auto& t : catalog_of(kind_of(id))) {
                        std::string s = t.get_str("name", "");
                        task_list_[id].insert(s);
                    }
                }
                task_list_[id].erase(task);
                save_task_list();
                set_notice("\u5df2\u5c06\u4efb\u52a1\u79fb\u51fa\u5217\u8868");
            }
            return;
        }
        y += row_h;
    }
}

// 重命名内联编辑框（在左列对应项上覆盖一个输入框）
void MaaHubWindow::draw_rename_box(RECT area, const std::string& id) {
    if (renaming_task_.empty()) return;
    (void)area;
    RECT cl;
    GetClientRect(hwnd_, &cl);
    // 简单实现：在屏幕中下方放一个输入条
    int w = 320, h = 40;
    RECT box{ (cl.right - w) / 2, cl.bottom - h - 40,
              (cl.right + w) / 2, cl.bottom - 40 };
    gui_.round_rect(box, gui_.theme.panel, gui_.theme.primary, 4);
    gui_.text(box.left + 12, box.top + 12, L"\u65b0\u540d\u79f0", gui_.theme.dim, gui_.font_small);
    RECT ir{ box.left + 80, box.top + 6, box.right - 90, box.bottom - 6 };
    gui_.text_input("rename-input", ir, rename_buf_, nullptr);
    RECT ok_b{ box.right - 84, box.top + 6, box.right - 8, box.bottom - 6 };
    if (gui_.button("rename-ok", ok_b, L"\u786e\u5b9a", 1)) {
        task_alias_[rename_exec_ + "|" + renaming_task_] = rename_buf_;
        save_task_alias();
        renaming_task_.clear();
    }
    RECT cancel_b{ box.right - 84, box.top - 30, box.right - 8, box.top - 6 };
    if (gui_.button("rename-cancel", cancel_b, L"\u53d6\u6d88", 0)) {
        renaming_task_.clear();
    }
}

void MaaHubWindow::save_task_alias() {
    mj::Value v = mj::Value::object();
    for (auto& kv : task_alias_) v.set(kv.first, mj::Value(kv.second));
    util::write_file(util::exe_dir() + "\\state\\task-alias.json",
                     mj::Value::dump_pretty(v));
}

void MaaHubWindow::load_task_alias() {
    std::string content;
    if (!util::read_file(util::exe_dir() + "\\state\\task-alias.json", content)) return;
    mj::Value v;
    if (!mj::try_parse(content, v)) return;
    for (auto& kv : v.members()) {
        if (kv.second.is_string()) task_alias_[kv.first] = kv.second.as_string();
    }
}

std::string MaaHubWindow::kind_of(const std::string& id) const {
    ExecutorEntry e;
    if (hub_->get_executor(id, e)) return e.kind;
    return "maa";
}

// 连接状态中文文案
static std::wstring connected_text(const std::string& conn) {
    if (conn == "connected") return L"\u5df2\u8fde\u63a5";
    if (conn == "failed") return L"\u8fde\u63a5\u5931\u8d25";
    if (conn == "connecting") return L"\u8fde\u63a5\u4e2d";
    return L"\u672a\u8fde\u63a5";
}

// 右列日志区（官方 TaskQueueView Column=2）
// 官方结构：顶部下载卡片（有下载任务时显示）+ 信息流日志列表。
// MaaHub 用 Hub 的事件日志作为数据源，按时间倒序展示。
void MaaHubWindow::draw_log_column(RECT area, const std::string& id) {
    // 右列（官方 Column=2）：日志区。
    // 官方此处是下载卡片 + 信息流日志；MaaHub 改为常驻展示
    // 「实例状态 / 任务进度 / 最近事件」，避免右列大片空白。
    gui_.fill({ area.left, area.top, area.left + 1, area.bottom }, gui_.theme.border);

    int x = area.left + 12;
    int y = area.top + 4;
    int right = area.right - 8;

    gui_.text(x, y, L"\u72b6\u6001\u4e0e\u65e5\u5fd7", gui_.theme.dim, gui_.font_small);
    y += 22;

    mj::Value st = hub_->executor_status(id, 0.4);
    std::string conn = st.get_str("conn", "unknown");
    std::string run = st.get_str("run", "");
    bool busy = st.get_bool("busy", false);
    std::string last = st.get_str("last_error", "");
    int task_count = (int)st.get_int("task_count", 0);

    // ---- 状态卡 ----
    {
        RECT card{ x, y, right, y + 58 };
        COLORREF fill = busy ? gui_.theme.primary_light : gui_.theme.card_bg;
        COLORREF line = busy ? gui_.theme.primary : gui_.theme.border;
        gui_.round_rect(card, fill, line, 4);
        COLORREF dot = conn == "connected" ? gui_.theme.ok :
                       conn == "failed" ? gui_.theme.err :
                       conn == "connecting" ? gui_.theme.warn : gui_.theme.faint;
        gui_.round_rect({ card.left + 10, card.top + 12, card.left + 20, card.top + 22 },
                        dot, dot, 5);
        std::wstring st_txt = connected_text(conn);
        gui_.text(card.left + 26, card.top + 10, st_txt, gui_.theme.text, gui_.font_bold);
        std::wstring sub = busy ? L"\u4efb\u52a1\u8fd0\u884c\u4e2d" :
                          (run == "running" ? L"\u5df2\u8fde\u63a5\uff0c\u7a7a\u95f2" : L"\u7b49\u5f85\u64cd\u4f5c");
        RECT save = gui_.clip();
        gui_.text_clipped({ card.left + 26, card.top + 30, card.right - 10, card.bottom - 4 },
                          sub, gui_.theme.dim, gui_.font_small);
        (void)save;
        y = card.bottom + 10;
    }

    // ---- 任务进度卡（一个数字概览，避免与下方列表标题重复）----
    {
        RECT card{ x, y, right, y + 38 };
        gui_.round_rect(card, gui_.theme.card_bg, gui_.theme.border, 4);
        int enabled = 0;
        auto it = task_ui_.find(id);
        if (it != task_ui_.end()) {
            for (auto& kv : it->second) {
                if (kv.second.checked || kv.second.once) ++enabled;
            }
        }
        gui_.text(card.left + 10, card.top + 5, L"\u4efb\u52a1\u8fdb\u5ea6",
                  gui_.theme.dim, gui_.font_small);
        gui_.text(card.left + 10, card.top + 18,
                  std::to_wstring(enabled) + L" / " + std::to_wstring(task_count)
                      + L" \u5df2\u542f\u7528",
                  gui_.theme.primary, gui_.font_bold);
        y = card.bottom + 8;
    }

    // ---- 最近事件（最多 3 条，避免挤压下方区块）----
    gui_.text(x, y, L"\u6700\u8fd1\u4e8b\u4ef6", gui_.theme.dim, gui_.font_small);
    y += 18;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        if (auto* ev = snap_.find("events"); ev && ev->is_array()) {
            for (auto& e : ev->items()) lines.push_back(e.as_string());
        }
    }
    if (lines.empty()) {
        gui_.text(x, y, L"\u6682\u65e0\u4e8b\u4ef6\u8bb0\u5f55", gui_.theme.faint,
                  gui_.font_small);
        y += 18;
    } else {
        int row_h = 18;
        int shown = (int)lines.size() > 3 ? 3 : (int)lines.size();
        for (int i2 = (int)lines.size() - shown; i2 < (int)lines.size(); ++i2) {
            RECT save = gui_.clip();
            gui_.text_clipped({ x, y, right, y + row_h },
                              gui::widen(lines[i2]), gui_.theme.dim, gui_.font_small);
            (void)save;
            y += row_h;
        }
    }
    y += 6;

    // ---- 最近错误（有则高亮）----
    if (!last.empty()) {
        y += 8;
        RECT card{ x, y, right, y + 44 };
        gui_.round_rect(card, gui_.theme.err_light, gui_.theme.err, 4);
        gui_.text(card.left + 10, card.top + 6, L"\u6700\u8fd1\u9519\u8bef",
                  gui_.theme.err, gui_.font_small);
        RECT save = gui_.clip();
        gui_.text_clipped({ card.left + 10, card.top + 20, card.right - 10, card.bottom - 4 },
                          gui::widen(last), gui_.theme.err, gui_.font_small);
        (void)save;
        y = card.bottom + 10;
    }

    // ---- 已启用任务列表（官方信息流的常驻替代，让右列有实质内容）----
    {
        std::string kind;
        for (auto& e : hub_->executors()) {
            if (e.id == id) { kind = e.kind; break; }
        }
        std::vector<std::string> enabled_names;
        auto it = task_ui_.find(id);
        if (it != task_ui_.end()) {
            for (auto& kv : it->second) {
                if (kv.second.checked || kv.second.once) enabled_names.push_back(kv.first);
            }
        }
        // 按左列显示顺序排序
        std::vector<mj::Value> all = catalog_of(kind);
        auto oit = task_order_.find(id);
        if (oit != task_order_.end() && !oit->second.empty()) {
            std::vector<std::string> ordered;
            for (auto& nm : oit->second) {
                for (auto& e : enabled_names) if (e == nm) { ordered.push_back(nm); break; }
            }
            for (auto& e : enabled_names) {
                bool f = false;
                for (auto& s : ordered) if (s == e) { f = true; break; }
                if (!f) ordered.push_back(e);
            }
            enabled_names.swap(ordered);
        }

        gui_.text(x, y, L"\u5df2\u542f\u7528\u4efb\u52a1", gui_.theme.dim, gui_.font_small);
        y += 20;
        if (!enabled_names.empty()) {
            int row_h = 22;
            int avail = area.bottom - y - 4;
            int visible = avail / row_h;
            if (visible < 1) visible = 1;
            int start = (int)enabled_names.size() > visible
                        ? (int)enabled_names.size() - visible : 0;
            for (int i2 = (int)enabled_names.size() - 1; i2 >= start; --i2) {
                std::wstring nm = display_name(kind, enabled_names[i2]);
                auto al = task_alias_.find(id + "|" + enabled_names[i2]);
                if (al != task_alias_.end() && !al->second.empty()) {
                    nm = gui::widen(al->second);
                }
                bool is_running = hub_->running_task_of(id) == enabled_names[i2];
                RECT row{ x, y, right, y + row_h - 2 };
                if (is_running) {
                    gui_.round_rect(row, gui_.theme.primary_light,
                                    gui_.theme.primary, 3);
                }
                gui_.round_rect({ x + 2, y + 6, x + 8, y + 12 },
                                is_running ? gui_.theme.primary : gui_.theme.ok,
                                is_running ? gui_.theme.primary : gui_.theme.ok, 3);
                RECT save = gui_.clip();
                gui_.text_clipped({ x + 14, y + 2, right, y + row_h },
                                  nm, gui_.theme.text, gui_.font_small);
                (void)save;
                y += row_h;
            }
        }
    }

    // ---- 实例档案（核心/路径/创建时间/账号）：右列常驻信息，避免大片空白 ----
    {
        y += 10;
        ExecutorEntry ex;
        if (hub_->get_executor(id, ex)) {
            RECT card{ x, y, right, y + 96 };
            gui_.round_rect(card, gui_.theme.card_bg, gui_.theme.border, 4);
            int cy = card.top + 8;
            auto row = [&](const std::wstring& k, const std::wstring& v) {
                gui_.text(card.left + 10, cy, k, gui_.theme.dim, gui_.font_small);
                RECT save = gui_.clip();
                gui_.text_clipped({ card.left + 76, cy, card.right - 10, cy + 16 },
                                  v, gui_.theme.text, gui_.font_small);
                (void)save;
                cy += 18;
            };
            row(L"\u7c7b\u578b", ex.kind == "maa" ? L"MAA\uff08\u660e\u65e5\u65b9\u821e\uff09"
                                                    : L"MaaEnd\uff08\u7ec8\u672b\u5730\uff09");
            row(L"\u6838\u5fc3", gui::widen(ex.core_id));
            std::wstring ct = L"-";
            if (ex.created_at > 0) {
                time_t tt = (time_t)ex.created_at;
                struct tm tmv;
                if (localtime_s(&tmv, &tt) == 0) {
                    wchar_t buf[32];
                    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d",
                             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                             tmv.tm_hour, tmv.tm_min);
                    ct = buf;
                }
            }
            row(L"\u521b\u5efa", ct);
            row(L"\u8d26\u53f7", ex.account_id.empty() ? L"\u672a\u7ed1\u5b9a"
                                                        : gui::widen(ex.account_id));
            row(L"\u72b6\u6001", ex.enabled ? L"\u5df2\u542f\u7528" : L"\u5df2\u7981\u7528");
        }
    }
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
    std::vector<mj::Value> tasks;   // 过滤内部入口任务 + 用户未添加的任务
    auto tl_it = task_list_.find(id);
    bool filtering = (tl_it != task_list_.end() && !tl_it->second.empty());
    for (auto& t : all_tasks) {
        std::string nm = t.get_str("name", "");
        if (kind == "maaend" && maaend_hidden_task(nm)) continue;
        if (filtering && tl_it->second.count(nm) == 0) continue;
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

    // 官方结构：页面 Margin 20 无背景；左列是独立卡片
    //   <Border Width="210" MaxWidth="210" Background="RegionBrushOpacity25"
    //            BorderBrush="BorderBrush" BorderThickness="1" CornerRadius="4">
    //     <Grid Width="190"> … 任务列表 + 40px 添加任务行 …
    // 右栏（任务设置）没有独立卡片背景，直接放在页面背景上。
    RECT frame{ content.left, content.top, content.right, content.bottom };
    const int LEFT_CARD_W = 210;            // 官方 Border Width=210
    const int LEFT_W = 190;                 // 官方 Grid Width=190
    const int MID_W = 300;                  // 官方 220 + MaaHub 额外留白（窄列竖排需要）
    const int BAR_H = 56;                    // 容纳官方 50px 高按钮组
    // 左列独立卡片（含圆角与 1px 边框）
    RECT left_card{ frame.left, frame.top, frame.left + LEFT_CARD_W, frame.bottom - BAR_H };
    gui_.round_rect(left_card, gui_.theme.card_bg, gui_.theme.border, 4);

    // 执行器切换条：左列卡片内部顶部，显示当前执行器 + 上/下一个切换按钮
    // （MaaHub 多执行器扩展；官方单实例无此条）
    {
        RECT strip{ left_card.left + 1, left_card.top + 1,
                    left_card.right - 1, left_card.top + 27 };
        gui_.fill(strip, gui_.theme.row_alt);
        std::vector<ExecutorEntry> execs = hub_->executors();
        int cur = -1;
        for (size_t k = 0; k < execs.size(); ++k) {
            if (execs[k].id == selected_exec_) { cur = (int)k; break; }
        }
        // 左箭头
        RECT prev_b{ strip.left + 2, strip.top + 2, strip.left + 22, strip.bottom - 2 };
        if (gui_.button("exec-prev", prev_b, L"\u25c0", 0)) {
            if (!execs.empty()) {
                int n = (cur <= 0) ? (int)execs.size() - 1 : cur - 1;
                selected_exec_ = execs[n].id;
                page_ = "executor";
            }
        }
        // 右箭头
        RECT next_b{ strip.right - 22, strip.top + 2, strip.right - 2, strip.bottom - 2 };
        if (gui_.button("exec-next", next_b, L"\u25b6", 0)) {
            if (!execs.empty()) {
                int n = (cur < 0 || cur + 1 >= (int)execs.size()) ? 0 : cur + 1;
                selected_exec_ = execs[n].id;
                page_ = "executor";
            }
        }
        // 中间：当前执行器名（点击可在多个之间循环）
        RECT name_b{ strip.left + 24, strip.top + 2, strip.right - 24, strip.bottom - 2 };
        std::wstring cur_name;
        if (cur >= 0 && cur < (int)execs.size()) cur_name = gui::widen(execs[cur].label);
        else cur_name = L"\u65b0\u5efa\u6267\u884c\u5668";
        bool hov = gui_.input().mouse.x >= name_b.left && gui_.input().mouse.x < name_b.right
                && gui_.input().mouse.y >= name_b.top && gui_.input().mouse.y < name_b.bottom;
        if (hov) gui_.round_rect(name_b, gui_.theme.hover, gui_.theme.hover, 3);
        gui_.text_clipped({ name_b.left + 6, name_b.top + 3, name_b.right - 2, name_b.bottom },
                          cur_name, gui_.theme.text, gui_.font_small);
        if (hov && gui_.input().pressed && !execs.empty()) {
            int n = (cur < 0 || cur + 1 >= (int)execs.size()) ? 0 : cur + 1;
            selected_exec_ = execs[n].id;
            page_ = "executor";
        }
    }

    // 左列内容区（卡片内 190px，左右各留 10px；顶部为执行器切换条让位 27px）
    RECT left_col{ left_card.left + 10, left_card.top + 27, left_card.left + 10 + LEFT_W,
                   left_card.bottom };
    // ---------------- 右侧：任务设置 ----------------
    // 右侧展示的任务：优先用户选中项；未选中时按官方默认（理智作战 Fight）
    std::string sel_task;
    for (auto& t : tasks) {
        std::string nm = t.get_str("name", "");
        if (task_ui_[id][nm].selected) { sel_task = nm; break; }
    }
    if (sel_task.empty() && !tasks.empty()) {
        // 官方 TaskQueueViewModel 默认选中理智作战（TaskSettingVisibilities.Fight）
        for (auto& t : tasks) {
            if (t.get_str("name", "") == "Fight") { sel_task = "Fight"; break; }
        }
        if (sel_task.empty()) sel_task = tasks[0].get_str("name", "");
    }

    // 任务切换过渡动画（官方 TransitioningContentControl）：
    // 任务变化时按列表位置定向滑入（TransitionOrientation=Vertical）
    // anim_active / anim_frac 已在上面声明，此处只更新状态
    // 任务切换过渡动画（官方 TransitioningContentControl 的 GDI 简化实现）
    bool anim_active = false;
    double anim_frac = 1.0;
    if (sel_task != panel_anim_task_ || id != panel_anim_exec_) {
        if (sel_task != panel_anim_task_) {
            int idx_new = -1, idx_old = -1;
            for (size_t k = 0; k < tasks.size(); ++k) {
                std::string nm = tasks[k].get_str("name", "");
                if (nm == sel_task) idx_new = (int)k;
                if (nm == panel_anim_task_) idx_old = (int)k;
            }
            panel_anim_dir_ = (idx_old >= 0 && idx_new >= 0 && idx_new < idx_old) ? -1 : 1;
        }
        panel_anim_task_ = sel_task;
        panel_anim_exec_ = id;
        panel_anim_t0_ = util::now_sec();
    }
    if (!sel_task.empty()) {
        double el = (util::now_sec() - panel_anim_t0_) * 1000.0;
        if (el < PANEL_ANIM_MS) {
            anim_active = true;
            anim_frac = el / PANEL_ANIM_MS;
        }
    }


    // 中列：任务设置（官方 220px 窄列，标签在上控件在下竖排）
    RECT right_col{ left_card.right + 6, frame.top,
                    left_card.right + 6 + MID_W, frame.bottom - BAR_H };
    // 未偏移的中列边界：信息条与底部「常规/高级设置」标签固定在列底部，
    // 不随动画偏移（否则动画期间会与面板内容重叠）
    const int RC_LEFT = right_col.left, RC_RIGHT = right_col.right;
    const int RC_BOTTOM = right_col.bottom;
    if (anim_active) {
        // ease-out：1-(1-t)^2，模拟官方滑入
        double e = 1.0 - (1.0 - anim_frac) * (1.0 - anim_frac);
        int off = (int)((1.0 - e) * 28.0 * panel_anim_dir_);
        right_col.top += off;
        right_col.bottom += off;
    }
    // 右列：日志区（官方 Column=2，占剩余宽度）
    RECT log_col{ right_col.right + 6, frame.top, frame.right, frame.bottom - BAR_H };

    // ---------------- 左列：任务列表 ----------------
    // 官方左列无「任务列表」标题文字，直接是 ListBox（MinHeight=220）
    int list_top = left_col.top + 4;
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
                      { left_col.left, list_top, left_col.right, left_card.bottom - 76 },
                      n * item_h + 8, scroll_["tasks-" + id]);
    int y = list_top + 4 - (int)scroll_["tasks-" + id];
    for (int ti = 0; ti < n; ++ti) {
        auto& t = tasks[ti];
        std::string name = t.get_str("name", "");
        TaskUiState& ui = task_ui_[id][name];
        if (y + item_h < list_top) { y += item_h; continue; }
        if (y > left_card.bottom - 76) break;
        RECT item{ left_col.left + 8, y, left_col.right - 8, y + item_h - 4 };
        bool hov = gui_.input().mouse.x >= item.left && gui_.input().mouse.x < item.right
                && gui_.input().mouse.y >= item.top && gui_.input().mouse.y < item.bottom;
        // 官方：任务项背景随状态变化（InProgress 蓝 / Completed 绿 / Error 红），
        // Alpha #20(12.5%) 叠加在白底上；勾选态用 Primary 浅底
        std::string running = hub_->running_task_of(id);
        std::string last_res = hub_->last_task_result(id, name);
        // 官方 NoSelectedHighlightListBoxItemStyle：
        //   默认/选中背景都是 RegionBrushOpacity25（选中不高亮，只文字变深）；
        //   hover = MouseOverRegionBrushOpacity75；状态色仅在有运行结果时叠加
        COLORREF fill_c, line_c;
        if (running == name) {
            fill_c = gui_.theme.status_running;  line_c = gui_.theme.primary;
        } else if (last_res == "error") {
            fill_c = gui_.theme.status_error;    line_c = gui_.theme.err;
        } else if (last_res == "ok") {
            fill_c = gui_.theme.status_done;     line_c = gui_.theme.ok;
        } else if (hov) {
            fill_c = gui_.theme.item_hover;      line_c = gui_.theme.border;
        } else {
            fill_c = gui_.theme.item_bg;         line_c = gui_.theme.border;
        }
        if (drag_task_ == name) fill_c = gui_.theme.primary_light;
        gui_.round_rect(item, fill_c, line_c, 4);
        // 官方 CheckBox MaxWidth=140
        // 官方 ItemContainer Padding=10,0,0,0
        // 复选框矩形 = 整个任务项（内部自行垂直居中）
        RECT cbr{ item.left + 10, item.top, item.left + 10 + 140, item.bottom };
        std::wstring label = display_name(kind, name);
        auto al = task_alias_.find(id + "|" + name);
        if (al != task_alias_.end() && !al->second.empty()) {
            label = gui::widen(al->second);
        }
        CheckState st = ui.once ? CheckState::Once
                                : (ui.checked ? CheckState::On : CheckState::Off);
        gui_.checkbox3(("cb-" + id + "-" + name).c_str(), cbr, label, st);
        ui.checked = (st == CheckState::On);
        ui.once = (st == CheckState::Once);
        // 官方任务项右侧：ConfigGeometry 齿轮图标（常态 SecondaryText 色，
        // 选中/hover 时 Primary 色）；hover 时再显示 复制/重命名/删除 三个
        // 22x22 图标按钮（官方 TaskActionButtonStyle）
        {
            int isz = 16;
            int ix = item.right - isz - 6;
            int iy = item.top + ((item.bottom - item.top) - isz) / 2;
            RECT gear_r{ ix, iy, ix + isz, iy + isz };
            COLORREF gear_c = (ui.checked || ui.once) ? gui_.theme.primary
                                                      : gui_.theme.dim;
            gui_.icon_gear(gear_r, gear_c);
            if (hov) {
                // hover 操作按钮（从右往左：删除、重命名、复制）
                int bx = item.right - 22;
                int by = item.top + ((item.bottom - item.top) - 22) / 2;
                struct Act { const char* id; const wchar_t* tip; int kind; };
                const Act acts[] = {
                    { "del",  L"\u5220\u9664", 2 },
                    { "ren",  L"\u91cd\u547d\u540d", 1 },
                    { "cpy",  L"\u590d\u5236", 0 },
                };
                for (auto& a : acts) {
                    RECT br{ bx, by, bx + 22, by + 22 };
                    bool bh = gui_.input().mouse.x >= br.left
                           && gui_.input().mouse.x < br.right
                           && gui_.input().mouse.y >= br.top
                           && gui_.input().mouse.y < br.bottom;
                    if (bh) gui_.round_rect(br, gui_.theme.hover, gui_.theme.hover, 3);
                    RECT ir{ br.left + 4, br.top + 4, br.right - 4, br.bottom - 4 };
                    if (a.kind == 0) gui_.icon_copy_task(ir, gui_.theme.dim);
                    else if (a.kind == 1) gui_.icon_rename_task(ir, gui_.theme.dim);
                    else gui_.icon_delete_task(ir, gui_.theme.err);
                    if (bh && gui_.input().pressed) {
                        if (a.kind == 1) {
                            renaming_task_ = name;
                            rename_exec_ = id;
                            auto al = task_alias_.find(id + "|" + name);
                            rename_buf_ = al == task_alias_.end() ? "" : al->second;
                        } else if (a.kind == 0) {
                            auto al = task_alias_.find(id + "|" + name);
                            std::string base = al == task_alias_.end() ? name : al->second;
                            task_alias_[id + "|" + name] = base + " (\u526f\u672c)";
                            save_task_alias();
                        } else {
                            // 删除 = 移出左列列表
                            if (task_list_.find(id) == task_list_.end()) {
                                for (auto& t : tasks) task_list_[id].insert(t.get_str("name", ""));
                            }
                            task_list_[id].erase(name);
                            save_task_list();
                            set_notice("\u5df2\u5c06\u4efb\u52a1\u79fb\u51fa\u5217\u8868");
                        }
                    }
                    bx -= 22;
                }
                // hover 时齿轮让位给操作按钮
            } else {
                // 非 hover：齿轮可点击打开设置
                bool gh = gui_.input().mouse.x >= gear_r.left
                       && gui_.input().mouse.x < gear_r.right
                       && gui_.input().mouse.y >= gear_r.top
                       && gui_.input().mouse.y < gear_r.bottom;
                if (gh && gui_.input().pressed) {
                    for (auto& kv : task_ui_[id]) kv.second.selected = false;
                    ui.selected = true;
                }
            }
        }
        if (hov && gui_.input().pressed) {
            for (auto& kv : task_ui_[id]) kv.second.selected = false;
            ui.selected = true;
        }
        // 右键 = 打开任务菜单（官方 ContextMenu）
        if (hov && gui_.input().rpressed) {
            ctx_menu_open_ = true;
            ctx_menu_x_ = gui_.input().mouse.x;
            ctx_menu_y_ = gui_.input().mouse.y;
            ctx_menu_task_ = name;
            ctx_menu_exec_ = id;
        }
        y += item_h;
    }
    gui_.end_scroll();

    // 官方 Grid.Row=1：「全选 | 反选」按钮组（并排各半宽、高 30），位于列表下方
    {
        int hy = left_card.bottom - 40 - 32;
        int half = (left_col.right - left_col.left) / 2;
        RECT sel_all{ left_col.left, hy, left_col.left + half, hy + 30 };
        RECT sel_inv{ left_col.left + half, hy, left_col.right, hy + 30 };
        if (gui_.button("tb-all", sel_all, L"\u5168\u9009", 0)) {
            for (auto& t : tasks) task_ui_[id][t.get_str("name", "")].checked = true;
        }
        if (gui_.button("tb-inverse", sel_inv, L"\u53cd\u9009", 0)) {
            for (auto& t : tasks) {
                TaskUiState& s = task_ui_[id][t.get_str("name", "")];
                s.checked = !s.checked;
            }
        }
    }

    // 左列底部 40px 行：「+ 添加新任务」（官方 RowDefinition Height="40"）
    // 贴左列卡片底部（left_card.bottom），与任务列表滚动区不重叠
    {
        RECT add_row{ left_col.left, left_card.bottom - 40, left_col.right, left_card.bottom };
        gui_.fill({ add_row.left, add_row.top, add_row.right, add_row.top + 1 },
                  gui_.theme.border);
        RECT add_tasks_btn{ add_row.left + 8, add_row.top + 6,
                            add_row.right - 8, add_row.bottom - 6 };
        // 官方 TaskQueueAddTaskIcon（Carbon 加号圆圈）替代纯文字"+"
        int icon_sz = add_row.bottom - add_row.top - 12;
        RECT icon_r{ add_tasks_btn.left + 8, add_row.top + 6,
                     add_tasks_btn.left + 8 + icon_sz, add_row.bottom - 6 };
        gui_.icon_add_task(icon_r, gui_.theme.primary);
        if (gui_.button(("addtask-" + id).c_str(),
                        { add_tasks_btn.left + icon_sz + 12, add_tasks_btn.top,
                          add_tasks_btn.right, add_tasks_btn.bottom },
                        L"\u6dfb\u52a0\u65b0\u4efb\u52a1", 0)) {
            show_add_task_ = !show_add_task_;
        }
    }

    bool sel_valid = false;
    for (auto& t : tasks) {
        if (t.get_str("name", "") == sel_task) { sel_valid = true; break; }
    }
    if (!sel_valid) {
        // 选中项不在当前目录中（任务下线/名称变化）：提示而不是静默显示别的任务
        if (!sel_task.empty()) {
            int rx0 = right_col.left + 16;
            gui_.text(rx0, right_col.top + 16,
                      L"\u4efb\u52a1\u300c" + gui::widen(sel_task) +
                      L"\u300d\u4e0d\u5728\u5f53\u524d\u6838\u5fc3\u7684\u4efb\u52a1\u76ee\u5f55\u4e2d",
                      gui_.theme.warn, gui_.font);
            gui_.text(rx0, right_col.top + 40,
                      L"\u8bf7\u5728\u5de6\u4fa7\u91cd\u65b0\u9009\u62e9\u4efb\u52a1\uff0c\u6216\u66f4\u65b0\u6838\u5fc3\u7248\u672c",
                      gui_.theme.dim, gui_.font_small);
        } else if (!tasks.empty()) {
            sel_task = tasks[0].get_str("name", "");
            sel_valid = true;
        }
    }

    int rx = right_col.left + 16;
    int ry = right_col.top + 12;
    if (!sel_task.empty() && sel_valid) {
        // 官方层次：左侧大标题（任务名），右上角小字（任务设置 · 来源）
        std::wstring sel_title = display_name(kind, sel_task);
        auto al2 = task_alias_.find(id + "|" + sel_task);
        if (al2 != task_alias_.end() && !al2->second.empty()) sel_title = gui::widen(al2->second);
        gui_.text(rx, ry, sel_title, gui_.theme.text, gui_.font_title);
        std::wstring desc;
        for (auto& t : tasks) {
            if (t.get_str("name", "") == sel_task) desc = gui::widen(t.get_str("description", ""));
        }
        // 窄列（300px）：右上角只放来源描述，避免与标题抢空间
        if (!desc.empty()) {
            RECT save = gui_.clip();
            gui_.text_clipped({ rx + 130, ry + 22, right_col.right - 16, ry + 42 },
                              desc, gui_.theme.dim, gui_.font_small);
            (void)save;
        }
        ry += 34;

        TaskUiState& ui = task_ui_[id][sel_task];
        // 从持久层加载已保存的面板值（官方面板的设置是持久化的）
        {
            mj::Value saved = hub_->task_params(id, sel_task);
            for (auto& kv : saved.members()) {
                // "__once_" 前缀为一次性标记，不持久化
                if (kv.first.rfind("__once_", 0) == 0) continue;
                ui.params[kv.first] = kv.second;
            }
        }
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
            // 选中任务的 catalog 声明（供通用面板按类型渲染 + 估算滚动高度）
            const mj::Value* spec = nullptr;
            for (auto& t : tasks) {
                if (t.get_str("name", "") == sel_task) { spec = &t; break; }
            }
            std::vector<mj::Value> specs;
            if (spec) {
                if (auto* ps = spec->find("params"); ps && ps->is_array()) specs = ps->items();
            }
            int panel_top = ry;
            int panel_h = (right_col.bottom - 12) - panel_top;
            if (panel_h < 80) panel_h = 80;
            // 内容高度按参数个数估算（每项约 36-40px，含分组标题），
            // 不再用固定 2000——避免内容更高时被裁剪、或过短时出现空滚动
            // 内容高度估算：官方面板每项约 40px，但分组标题(+20)、提示行(+16)、
            // 单选条(+30) 都额外占位；按 specs 数的 2.2 倍估行，确保底部内容可滚动到
            int est_rows = (int)specs.size() * 22 / 10 + 12;
            int content_h = est_rows * 40 + 80;
            if (content_h < panel_h) content_h = panel_h;
            // 滚动区底部止于信息条上方，保证信息条始终可见
            int scroll_bottom = RC_BOTTOM - 92;   // 避开信息条 + 底部常规/高级切换标签
            if (scroll_bottom < panel_top + 80) scroll_bottom = panel_top + 80;
            gui_.begin_scroll(("panel-" + id + "-" + sel_task).c_str(),
                              { rx, panel_top, right_col.right - 16, scroll_bottom },
                              content_h, scroll_["panel-" + id + "-" + sel_task]);
            int saved_y = ry;
            PanelCtx pctx{ gui_, rx, ry, right_col.right - 16, ui.params, id, sel_task };
            bool custom = draw_task_panel(pctx, &specs);
            ry = pctx.y;
            // 面板修改写回持久层（一次性标记除外）
            for (auto& kv : ui.params) {
                if (kv.first.rfind("__once_", 0) == 0) continue;
                hub_->set_task_param(id, sel_task, kv.first, kv.second);
            }
            int used = ry - saved_y;
            gui_.end_scroll();
            // 面板边框（按实际内容高度）
            RECT pbody{ rx, panel_top, right_col.right - 16,
                        panel_top + (used > 0 ? used + 8 : 40) };
            // 先画边框会盖住内容，这里只画左侧竖线作视觉分隔
            gui_.fill({ rx - 8, panel_top, rx - 7, scroll_bottom }, gui_.theme.border);
            ry = right_col.bottom - 40;   // 信息条固定位
            (void)custom;
        }

        // 实例信息条：窄列单行紧凑（中列仅 300px，两行会被裁切）
        RECT info{ rx, RC_BOTTOM - 62, RC_RIGHT - 16, RC_BOTTOM - 36 };
        gui_.round_rect(info, gui_.theme.row_alt, gui_.theme.border, 4);
        int ix = info.left + 8;
        COLORREF def_col = gui_.theme.text;
        auto field = [&](const std::wstring& k, const std::wstring& v, COLORREF col) {
            int kw = gui_.text_size(k, gui_.font_small).cx;
            gui_.text(ix, info.top + 10, k, gui_.theme.dim, gui_.font_small);
            RECT save = gui_.clip();
            gui_.text_clipped({ ix + kw + 4, info.top + 10, ix + kw + 4 + 96,
                                info.top + 26 }, v, col, gui_.font_small);
            (void)save;
            ix += kw + 4 + 96 + 6;
        };
        field(L"\u7248\u672c", gui::widen(version.empty() ? "-" : version), def_col);
        field(L"\u4efb\u52a1", std::to_wstring(task_count), def_col);
        field(L"\u8d26\u53f7",
              acc_label.empty() ? L"\u672a\u7ed1" : gui::widen(acc_label), def_col);
        field(L"\u65e5\u5e38", daily_done ? L"\u5df2\u5b8c\u6210" : L"\u672a\u5b8c\u6210",
              daily_done ? gui_.theme.ok : gui_.theme.warn);
    }

    // 官方底部控制区第二行：「自动重载资源」复选框
    // （EnableAutoReload，可见性由 CanShowAutoReload 控制；MaaHub 连接后可用）
    {
        bool can_show = (conn == "connected");
        if (can_show) {
            CheckState st = auto_reload_ ? CheckState::On : CheckState::Off;
            RECT ar{ right_col.left + 4, frame.bottom - BAR_H + 8,
                     right_col.left + 4 + 160, frame.bottom - BAR_H + 8 + 22 };
            gui_.checkbox3("auto-reload", ar, L"\u81ea\u52a8\u91cd\u8f7d\u8d44\u6e90", st);
            auto_reload_ = (st == CheckState::On);
        }
    }

    // 中列底部：常规设置 / 高级设置 切换标签（官方 Grid.Row=1 ButtonGroup Width=220）
    {
        bool has_params = false;
        for (auto& t : tasks) {
            if (t.get_str("name", "") != sel_task) continue;
            if (auto* ps = t.find("params"); ps && ps->is_array() && !ps->items().empty()) {
                has_params = true;
            }
            break;
        }
        if (has_params && !sel_task.empty()) {
            const int TG_W = 200;
            int tg_x = right_col.left + 4;
            int tg_y = RC_BOTTOM - 36;
            int gh = gui_.option_strip(("tg-" + id + "-" + sel_task).c_str(),
                                       { tg_x, tg_y, tg_x + TG_W, tg_y + 26 },
                                       { L"\u5e38\u89c4\u8bbe\u7f6e", L"\u9ad8\u7ea7\u8bbe\u7f6e" },
                                       advanced_view_ ? 1 : 0);
            if (gh == 0) advanced_view_ = false;
            if (gh == 1) advanced_view_ = true;
        }
    }

    // 官方中列 Grid.Row=2（底部，位于「常规/高级设置」按钮组下方）：
    //   今日开放关卡提示（StagesOfTodayInlines，Width=200）
    {
        int ty = RC_BOTTOM - 14;
        RECT stage_box{ right_col.left + 4, ty - 16, right_col.left + 4 + 220, ty + 2 };
        RECT save = gui_.clip();
        gui_.text_clipped(stage_box,
                          L"\u4eca\u65e5\u5f00\u653e\u5173\u5361\uff1a\u5173\u95ed\u4e2d",
                          gui_.theme.faint, gui_.font_small);
        (void)save;
    }

    // ---------------- 右列：日志区（官方 Column=2）----------------
    draw_log_column(log_col, id);

    // ---------------- 底部控制栏（官方结构）----------------
    // 官方：Link Start!/停止/等待&停止 是居中的 100x50 大按钮组；
    // 左下角是完成后动作描述文本 + 齿轮图标按钮（PostActionSetting.ActionDescription）。
    RECT bottom_bar{ frame.left, frame.bottom - BAR_H, frame.right, frame.bottom };
    gui_.fill({ bottom_bar.left, bottom_bar.top, bottom_bar.right, bottom_bar.top + 1 },
              gui_.theme.border);

    bool connected = conn == "connected";
    bool busy = connected && hub_->executor_status(id, 0).get_bool("busy", false);

    // 左下角：完成后动作（齿轮图标 + 简短摘要；800x600 下只放图标，避免挤占按钮组）
    {
        mj::Value pa;
        auto pa_it = post_action_.find(id);
        if (pa_it != post_action_.end()) pa = pa_it->second;
        int n = 0;
        if (pa.is_object()) {
            for (auto& kv : pa.members()) if (kv.second.as_bool()) ++n;
        }
        std::wstring brief = n > 0 ? (std::to_wstring(n) + L" \u9879\u52a8\u4f5c")
                                   : L"\u672a\u8bbe\u7f6e";
        // 图标 + 计数（窄）
        RECT gear{ bottom_bar.left + 14, bottom_bar.top + (BAR_H - 28) / 2,
                   bottom_bar.left + 44, bottom_bar.top + (BAR_H + 28) / 2 };
        if (gui_.button("btn-postaction-gear", gear, L"\u2699", 0)) {
            show_post_action_ = !show_post_action_;
        }
        gui_.text(bottom_bar.left + 50, bottom_bar.top + (BAR_H - 16) / 2,
                  brief, gui_.theme.dim, gui_.font_small);
    }

    // 居中按钮组（官方基准 100x50；宽度按文字实测，避免裁切）
    const int BH = (BAR_H - 8 < 50) ? (BAR_H - 8) : 50, GAP = 5;
    const std::wstring kLink = L"\U0001f517 Link Start!";
    const std::wstring kStop = L"\u505c\u6b62";
    const std::wstring kWait = L"\u7b49\u5f85 & \u505c\u6b62";
    int bw_link = gui_.text_size(kLink, gui_.font_bold).cx + 28;
    int bw_stop = gui_.text_size(kStop, gui_.font_bold).cx + 28;
    int bw_wait = gui_.text_size(kWait, gui_.font_bold).cx + 28;
    if (bw_link < 100) bw_link = 100;
    if (bw_stop < 100) bw_stop = 100;
    if (bw_wait < 100) bw_wait = 100;
    bool show_wait = busy;
    int group_w = show_wait ? bw_link + bw_stop + bw_wait + GAP * 2
                            : bw_link + bw_stop + GAP;
    int gx = (bottom_bar.left + bottom_bar.right - group_w) / 2;
    int gy = bottom_bar.top + (bottom_bar.bottom - bottom_bar.top - BH) / 2;

    if (busy) {
        RECT stop_btn{ gx, gy, gx + bw_stop, gy + BH };
        if (gui_.button("btn-stop-exec", stop_btn, kStop, 2)) {
            post_action([this, id] { hub_->stop_executor(id); }, "\u505c\u6b62");
        }
        if (show_wait) {
            RECT wait_btn{ gx + bw_stop + GAP, gy, gx + bw_stop + GAP + bw_wait, gy + BH };
            if (gui_.button("btn-waitstop-exec", wait_btn, kWait, 0)) {
                post_action([this, id] { hub_->stop_executor(id); },
                            "\u7b49\u5f85\u505c\u6b62");
            }
        }
    } else {
        // 官方 Link Start 按钮：CarbonLink 图标 + 文字（替代 emoji 🔗）
        RECT link_btn{ gx, gy, gx + bw_link, gy + BH };
        int lsz = BH - 22;
        RECT link_icon{ link_btn.left + 12, gy + 11, link_btn.left + 12 + lsz, gy + 11 + lsz };
        // 先画按钮底与文字（文字左移给图标让位）
        if (gui_.button("btn-linkstart-exec",
                        { link_btn.left, link_btn.top, link_btn.right, link_btn.bottom },
                        L"   Link Start!", 1)) {
            post_action([this, id] {
                mj::Value st = hub_->executor_status(id);
                if (st.get_str("conn", "") != "connected") hub_->connect_executor(id);
                action_run_checked(id);
            }, "\u94fe\u63a5\u5f00\u59cb");
        }
        gui_.icon_link(link_icon, RGB(255, 255, 255));
    }

    // 右下角：连接状态 + 连接/断开（MaaHub 扩展能力）
    {
        COLORREF dot = connected ? gui_.theme.ok :
                       conn == "failed" ? gui_.theme.err :
                       conn == "connecting" ? gui_.theme.warn : gui_.theme.faint;
        gui_.round_rect({ bottom_bar.right - 150, bottom_bar.top + 19,
                          bottom_bar.right - 140, bottom_bar.top + 29 }, dot, dot, 5);
        std::wstring st = connected ? L"\u5df2\u8fde\u63a5" :
                          conn == "failed" ? L"\u8fde\u63a5\u5931\u8d25" :
                          conn == "connecting" ? L"\u8fde\u63a5\u4e2d\u2026" : L"\u672a\u8fde\u63a5";
        gui_.text(bottom_bar.right - 132, bottom_bar.top + 17, st,
                  connected ? gui_.theme.ok : gui_.theme.dim, gui_.font_small);
        RECT conn_btn{ bottom_bar.right - 96, bottom_bar.top + 10,
                       bottom_bar.right - 16, bottom_bar.bottom - 10 };
        if (gui_.button("btn-connect-exec", conn_btn,
                        connected ? L"\u65ad\u5f00" : L"\u8fde\u63a5", 0)) {
            if (connected) action_disconnect(id);
            else action_connect(id);
        }
    }

    // ---------------- 浮层（最上层）----------------
    if (show_add_task_) draw_add_task_overlay(right_col, id, kind);
    if (show_post_action_) draw_post_action(right_col, id);
    draw_task_ctx_menu();
    draw_rename_box(right_col, id);
}


// ================================================================ 欢迎页
void MaaHubWindow::page_welcome(RECT content) {
    RECT panel{ content.left + 60, content.top + 40, content.right - 60, content.bottom - 40 };
    gui_.panel(panel, L"\u6b22\u8fce\u4f7f\u7528 MaaHub");
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
void MaaHubWindow::page_copilot(RECT content) {
    // 官方 CopilotView（自动战斗）：左侧作业列表（按类型分类）+
    // 右侧「作业路径/神秘代码」输入与作业选项。
    // MaaHub 未接入真实作业运行（需 Copilot 适配器），此处提供官方形态的
    // 文件选择与选项区，未实现的能力明确标注，不虚构。
    gui_.text(content.left, content.top, L"\u81ea\u52a8\u6218\u6597",
              gui_.theme.text, gui_.font_bold);
    gui_.text(content.left + 90, content.top + 3,
              L"\u9009\u62e9\u4f5c\u4e1a JSON \u6587\u4ef6\uff0c\u81ea\u52a8\u6267\u884c\u5173\u5361\u6d41\u7a0b",
              gui_.theme.dim, gui_.font_small);
    int y = content.top + 34;

    // ---- 作业列表（官方按类型分类的 ListBox）----
    {
        RECT card{ content.left, y, content.left + 260, content.bottom };
        gui_.round_rect(card, gui_.theme.card_bg, gui_.theme.border, 4);
        int cy = card.top + 12;
        gui_.text(card.left + 12, cy, L"\u4f5c\u4e1a\u5217\u8868", gui_.theme.dim,
                  gui_.font_small);
        cy += 22;
        // 官方分类（Copilot.MainStageAndSideStory / SSS / ParadoxSimulation / OtherActivityStage）
        struct Cat { const wchar_t* name; };
        const Cat cats[] = {
            { L"\u4e3b\u7ebf\u5173\u5361 / \u526f\u672c\u6536\u96c6" },  // 主线/故事集/侧故事
            { L"\u4fdd\u5168\u6d3e\u9a7b" },                        // SSS
            { L"\u609d\u8bba\u6a21\u62df" },                        // 悖论模拟
            { L"\u5176\u4ed6\u6d3b\u52a8" },                        // 其他活动
        };
        std::vector<std::string> empty;
        for (auto& cat : cats) {
            gui_.text(card.left + 12, cy, cat.name, gui_.theme.text, gui_.font_small);
            cy += 22;
            gui_.text(card.left + 24, cy, L"\uff08\u672a\u5bfc\u5165\u4f5c\u4e1a\uff09",
                      gui_.theme.faint, gui_.font_small);
            cy += 24;
        }
        // 官方提示：作业文件可以直接用鼠标拖进来
        RECT save = gui_.clip();
        gui_.text_clipped({ card.left + 12, card.bottom - 34, card.right - 12, card.bottom - 4 },
                          L"\u4f5c\u4e1a\u6587\u4ef6\u53ef\u76f4\u63a5\u62d6\u5165\uff08MaaHub \u672a\u5b9e\u73b0\uff09",
                          gui_.theme.faint, gui_.font_small);
        (void)save;
    }

    // ---- 右侧：作业路径 + 选项 ----
    int rx = content.left + 272;
    int rw = content.right - rx;
    gui_.text(rx, y + 4, L"\u4f5c\u4e1a\u8def\u5f84 / \u795e\u79d8\u4ee3\u7801",
              gui_.theme.dim, gui_.font_small);
    y += 24;
    gui_.text_input("copilot-path", { rx, y, rx + rw - 80, y + 26 },
                    copilot_path_, L"\u9009\u62e9 JSON \u6216\u7c98\u8d34 prts.plus \u4f5c\u4e1a\u4ee3\u7801");
    RECT pick{ rx + rw - 74, y, rx + rw, y + 26 };
    if (gui_.button("copilot-pick", pick, L"\u9009\u62e9", 0)) {
        // 打开文件选择对话框（官方为 WPF OpenFileDialog；此处用系统对话框）
        char buf[MAX_PATH] = {0};
        OPENFILENAMEA ofn = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "Copilot JSON\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
            copilot_path_ = buf;
        }
    }
    y += 34;

    gui_.text(rx, y + 4, L"\u8bfb\u53d6\u526a\u8d34\u677f", gui_.theme.dim, gui_.font_small);
    {
        CheckState st = CheckState::Off;
        // 复选框矩形给足宽度，避免文字换行溢出到下方卡片
        gui_.checkbox3("copilot-clip", { rx + 80, y, rx + rw, y + 22 },
                       L"\u81ea\u52a8\u8bc6\u522b\u4f5c\u4e1a/\u4f5c\u4e1a\u96c6", st);
    }
    y += 34;

    // ---- 作业选项（官方 CopilotView 开关组）----
    {
        RECT card{ rx, y, rx + rw, y + 210 };
        gui_.round_rect(card, gui_.theme.card_bg, gui_.theme.border, 4);
        int cy = card.top + 12;
        gui_.text(card.left + 12, cy, L"\u4f5c\u4e1a\u9009\u9879", gui_.theme.dim,
                  gui_.font_small);
        cy += 24;
        struct Opt { const char* key; const wchar_t* label; const wchar_t* tip; };
        const Opt opts[] = {
            { "copilot-auto-squad",  L"\u81ea\u52a8\u7f16\u961f",
              L"\u81ea\u52a8\u7f16\u961f\u53ef\u80fd\u65e0\u6cd5\u8bc6\u522b\u5e26\u6709 \u300c\u7279\u522b\u5173\u6ce8\u300d \u6807\u8bb0\u7684\u5e72\u5458" },
            { "copilot-use-formation", L"\u4f7f\u7528\u7f16\u961f", L"" },
            { "copilot-ignore-req",  L"\u5ffd\u7565\u5e72\u5458\u5c5e\u6027\u8981\u6c42", L"" },
            { "copilot-support-unit", L"\u501f\u52a9\u6218",
              L"\u7f3a\u4e00\u4e2a\u8fd8\u80fd\u7528\u7528\uff0c\u7f3a\u4e24\u4e2a\u4ee5\u4e0a\u8fd8\u662f\u6362\u4efd\u4f5c\u4e1a\u5427" },
            { "copilot-add-trust",   L"\u8865\u5145\u4f4e\u4fe1\u8d56\u5e72\u5458", L"" },
        };
        for (auto& o : opts) {
            bool v = copilot_opt_.find(o.key) != copilot_opt_.end()
                         ? copilot_opt_[o.key] : false;
            CheckState st = v ? CheckState::On : CheckState::Off;
            RECT r{ card.left + 16, cy, card.right - 16, cy + 22 };
            gui_.checkbox3(o.key, r, o.label, st);
            copilot_opt_[o.key] = (st == CheckState::On);
            cy += 26;
            if (o.tip && *o.tip) {
                RECT save = gui_.clip();
                gui_.text_clipped({ card.left + 40, cy, card.right - 16, cy + 16 },
                                  o.tip, gui_.theme.faint, gui_.font_small);
                (void)save;
                cy += 16;
            }
        }
        y = card.bottom + 12;
    }

    // ---- 操作按钮（官方 Copilot.Start / Common.Stop）----
    {
        RECT sb{ rx, y, rx + 120, y + 34 };
        if (gui_.button("copilot-start", sb, L"\u5f00\u59cb", 1)) {
            if (copilot_path_.empty()) {
                set_notice("\u8bf7\u5148\u9009\u62e9\u4f5c\u4e1a\u6587\u4ef6");
            } else {
                set_notice("\u81ea\u52a8\u6218\u6597\u9700\u8981 Copilot \u9002\u914d\u5668\uff0cMaaHub \u5c1a\u672a\u5b9e\u73b0\u8be5\u4efb\u52a1\u7c7b\u578b");
            }
        }
    }
}

void MaaHubWindow::page_toolbox(RECT content) {
    // 官方 ToolboxView（小工具）：MaaHub 提供常用工具的官方形态入口。
    // 官方 Toolbox 主要是「牛杂」（肉鸽小游戏）相关自动化工具，
    // 依赖 Copilot/Roguelike 适配器，MaaHub 未实现，此处列出官方工具清单
    // 并明确标注未实现，不虚构能力。
    gui_.text(content.left, content.top, L"\u5c0f\u5de5\u5177",
              gui_.theme.text, gui_.font_bold);
    gui_.text(content.left + 76, content.top + 3,
              L"\u5b98\u65b9\u5c0f\u5de5\u5177\u96c6\u5408\uff08MaaHub \u90e8\u5206\u672a\u5b9e\u73b0\uff09",
              gui_.theme.dim, gui_.font_small);
    int y = content.top + 34;

    struct Tool { const wchar_t* name; const wchar_t* desc; bool avail; };
    const Tool tools[] = {
        { L"\u725b\u6742", L"\u751f\u5b58\u6392\u540d\u8005\u526f\u672c\u81ea\u52a8\u5316\uff08\u7ed3\u5c40/\u4e8b\u4ef6/\u4efb\u52a1\u9009\u62e9\uff09", false },
        { L"\u725b\u6742 \u00b7 \u7956\u9f99\u7968", L"\u81ea\u52a8\u9886\u53d6\u7956\u9f99\u7968\u5956\u52b1", false },
        { L"\u725b\u6742 \u00b7 \u9886\u53d6\u7269\u8d44", L"\u81ea\u52a8\u9886\u53d6\u62bd\u5956\u7269\u8d44", false },
        { L"\u4efb\u52a1\u8fc7\u6ee4\u5668", L"\u6309\u5173\u5361\u540d\u8fc7\u6ee4\u4efb\u52a1\u6267\u884c", true },
        { L"\u65e5\u5fd7\u5bfc\u51fa", L"\u5bfc\u51fa\u8fd0\u884c\u65e5\u5fd7\u4e0e\u6392\u67e5\u4fe1\u606f", true },
    };
    for (auto& t : tools) {
        RECT card{ content.left, y, content.right, y + 58 };
        gui_.round_rect(card, gui_.theme.card_bg, gui_.theme.border, 4);
        gui_.text(card.left + 14, card.top + 10, t.name, gui_.theme.text, gui_.font);
        RECT save = gui_.clip();
        gui_.text_clipped({ card.left + 14, card.top + 30, card.right - 130, card.bottom - 4 },
                          t.desc, gui_.theme.dim, gui_.font_small);
        (void)save;
        COLORREF tag_c = t.avail ? gui_.theme.ok : gui_.theme.warn;
        RECT tr{ card.right - 108, card.top + 16, card.right - 14, card.top + 38 };
        gui_.round_rect(tr, tag_c, tag_c, 4);
        gui_.text(tr.left + 10, tr.top + 5, t.avail ? L"\u53ef\u7528" : L"\u672a\u5b9e\u73b0",
                  RGB(255, 255, 255), gui_.font_small);
        y = card.bottom + 10;
    }
}

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

// 测试用：强制打开任务右键菜单
void MaaHubWindow::debug_open_ctx_menu(const std::string& task) {
    if (selected_exec_.empty()) {
        auto ids = hub_->executors();
        if (!ids.empty()) selected_exec_ = ids[0].id;
    }
    ctx_menu_open_ = true;
    ctx_menu_exec_ = selected_exec_;
    ctx_menu_task_ = task;
    ctx_menu_x_ = 420;
    ctx_menu_y_ = 300;
    fprintf(stderr, "[render] ctx menu opened task=%s\n", task.c_str());
}

// 测试用：强制打开「添加新任务」浮层
void MaaHubWindow::debug_open_add_task() {
    show_add_task_ = true;
    if (selected_exec_.empty()) {
        auto ids = hub_->executors();
        if (!ids.empty()) selected_exec_ = ids[0].id;
    }
    fprintf(stderr, "[render] add-task overlay opened\n");
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
            poll_task_states();   // 同步左列任务状态点（运行中 -> 完成/出错）
            PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);
            Sleep(500);
        }
    }
}

// 轮询各执行器的运行任务状态：任务结束后把「运行中」转为「完成/出错」
void MaaHubWindow::poll_task_states() {
    auto ids = hub_->executors();
    for (auto& e : ids) {
        std::string running = hub_->running_task_of(e.id);
        if (running.empty()) continue;
        mj::Value st = hub_->executor_status(e.id, 0);
        // worker 已停止/停止中，或该执行器没有正在跑的任务 => 结束
        bool busy = st.get_bool("busy", false);
        if (!busy) {
            std::string err = st.get_str("last_error", "");
            hub_->set_task_result(e.id, running, err.empty() ? "ok" : "error");
            hub_->set_running_task(e.id, "");
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
    if (tid <= 0) hub_->set_task_result(id, task, "error");
    set_notice(tid > 0 ? task + " 已启动 (task " + std::to_string(tid) + ")"
                       : task + " 启动失败：" + hub_->executor_status(id).get_str("last_error", ""));
}

// 从此处运行：从指定任务开始，依次执行它及其后所有已勾选任务（官方 RunFromHere）
void MaaHubWindow::action_run_from(const std::string& id, const std::string& from_task) {
    ExecutorEntry e;
    if (!hub_->get_executor(id, e)) return;
    mj::Value cat = hub_->executor_catalog(id);
    std::vector<mj::Value> items;
    if (cat.is_array()) {
        for (auto& t : cat.items()) {
            if (e.kind == "maaend" && maaend_hidden_task(t.get_str("name", ""))) continue;
            items.push_back(t);
        }
    }
    // 应用用户拖拽后的顺序（与左列显示一致）
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
    size_t start = 0;
    bool found_start = false;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].get_str("name", "") == from_task) { start = i; found_start = true; break; }
    }
    if (!found_start) {
        set_notice(std::string("\u4efb\u52a1\u4e0d\u5b58\u5728\uff1a") + from_task);
        return;
    }
    int started = 0;
    for (size_t i = start; i < items.size(); ++i) {
        std::string name = items[i].get_str("name", "");
        auto it = task_ui_.find(id);
        if (it == task_ui_.end()) continue;
        auto tit = it->second.find(name);
        if (tit == it->second.end()) continue;
        if (!tit->second.checked && !tit->second.once) continue;
        mj::Value params = mj::Value::object();
        for (auto& kv : tit->second.params) params.set(kv.first, kv.second);
        if (hub_->run_on_executor(id, name, params) > 0) {
            ++started;
            tit->second.once = false;   // 「仅一次」运行后复位
        }
    }
    if (started > 0) {
        std::wstring msg = L"\u5df2\u4ece\u300c" + display_name(e.kind, from_task)
                         + L"\u300d\u5f00\u59cb\u8fd0\u884c "
                         + std::to_wstring(started) + L" \u4e2a\u4efb\u52a1";
        set_notice(util::wide_to_utf8(msg));
    } else {
        set_notice("\u6ca1\u6709\u53ef\u8fd0\u884c\u7684\u5df2\u52fe\u9009\u4efb\u52a1");
    }
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
    // 首次进入从快照回填（官方 SettingsView 多组设置）
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
            set_theme_ = st.theme;
        }
        settings_loaded_ = true;
    }

    int y = content.top;
    auto group = [&](const wchar_t* title) {
        RECT card{ content.left, y, content.right, y + 28 };
        gui_.round_rect(card, gui_.theme.primary, gui_.theme.primary, 4);
        gui_.text(card.left + 12, card.top + 5, title, RGB(255, 255, 255), gui_.font_bold);
        y += 34;
    };
    auto note = [&](const std::wstring& text) {
        RECT save = gui_.clip();
        gui_.text_clipped({ content.left, y, content.right, y + 34 },
                          text, gui_.theme.dim, gui_.font_small);
        (void)save;
        y += 36;
    };

    // ---------------- 更新与代理 ----------------
    group(L"\u66f4\u65b0\u4e0e\u4ee3\u7406");
    int mi = gui_.option_strip("set-proxy-mode",
                               { content.left, y, content.left + 330, y + 26 },
                               { L"\u4e0d\u4f7f\u7528\u4ee3\u7406", L"HTTP \u4ee3\u7406",
                                 L"SOCKS5 \u4ee3\u7406" }, set_proxy_mode_);
    if (mi >= 0) set_proxy_mode_ = mi;
    y += 32;
    if (set_proxy_mode_ != 0) {
        // 标签统一 56px，输入框等宽（地址/端口一行，用户名/密码一行）
        const int PL = 56;
        int half = (content.right - content.left - PL - 12) / 2;
        gui_.text(content.left, y + 4, L"\u5730\u5740", gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-proxy-host", { content.left + PL, y, content.left + PL + half, y + 26 },
                        set_proxy_host_, L"127.0.0.1");
        gui_.text(content.left + PL + half + 12, y + 4, L"\u7aef\u53e3",
                  gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-proxy-port",
                        { content.left + PL + half + 12 + 40, y, content.right, y + 26 },
                        set_proxy_port_, L"7890");
        y += 30;
        gui_.text(content.left, y + 4, L"\u7528\u6237\u540d", gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-proxy-user", { content.left + PL, y, content.left + PL + half, y + 26 },
                        set_proxy_user_, L"\u53ef\u9009");
        gui_.text(content.left + PL + half + 12, y + 4, L"\u5bc6\u7801",
                  gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-proxy-pass",
                        { content.left + PL + half + 12 + 40, y, content.right, y + 26 },
                        set_proxy_pass_, L"\u53ef\u9009");
        y += 34;
    }
    gui_.text(content.left, y + 4, L"Mirror\u9171 CDK", gui_.theme.dim, gui_.font_small);
    gui_.text_input("set-cdk", { content.left + 96, y, content.right, y + 26 },
                    set_cdk_, L"\u586b\u5199\u540e\u4f18\u5148\u4f7f\u7528 Mirror\u9171 \u4e0b\u8f7d");
    y += 32;
    note(L"\u7559\u7a7a\u5219\u6309 GitHub \u2192 gh-proxy \u2192 ghfast \u2192 moeyy \u987a\u5e8f\u56de\u9000");

    // ---------------- 连接设置 ----------------
    group(L"\u8fde\u63a5\u8bbe\u7f6e");
    note(L"\u5b98\u65b9\u5171 24 \u9879\u3002MaaHub \u5df2\u5b9e\u73b0\u5176\u4e2d\u5fc3\u90e8\u5206\uff1a"
         L"\u81ea\u52a8\u68c0\u6d4b\u8fde\u63a5\u3001\u6bcf\u6b21\u91cd\u65b0\u68c0\u6d4b\u3001\u8fde\u63a5\u914d\u7f6e\u3001\u8fde\u63a5\u5730\u5740\u3001"
         L"\u622a\u56fe\u65b9\u5f0f\u3001\u9f20\u6807\u8f93\u5165\u3001\u952e\u76d8\u8f93\u5165\u3001\u89e6\u63a7\u6a21\u5f0f\u3002\u8fd9\u4e9b\u9879\u76ee\u524d"
         L"\u5728\u5404\u6267\u884c\u5668\u7684\u300c\u5f00\u59cb\u5524\u9192\u300d\u4efb\u52a1\u9762\u677f\u4e2d\u914d\u7f6e\uff08\u6bcf\u6267\u884c\u5668\u72ec\u7acb\uff09\u3002"
         L"\u5176\u4f59\u4e3a\u6a21\u62df\u5668\u4e13\u5c5e\uff08ADB \u66ff\u6362\u3001\u7a97\u53e3\u6062\u590d\u7b49\uff09\u3002");

    // ---------------- 界面设置 ----------------
    group(L"\u754c\u9762\u8bbe\u7f6e");
    {
        // 官方 界面设置-界面主题（light/dark/system）
        int cur = set_theme_ == "dark" ? 1 : (set_theme_ == "system" ? 2 : 0);
        int hit = gui_.option_strip("set-theme",
                { content.left, y, content.left + 360, y + 26 },
                { L"\u4eae\u8272", L"\u6697\u8272", L"\u8ddf\u968f\u7cfb\u7edf" }, cur);
        if (hit >= 0) {
            set_theme_ = hit == 1 ? "dark" : hit == 2 ? "system" : "light";
            bool dark = (set_theme_ == "dark");
            if (set_theme_ == "system") {
                HKEY hk = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        0, KEY_READ, &hk) == ERROR_SUCCESS) {
                    DWORD v = 1, sz = sizeof(v);
                    if (RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, nullptr,
                            (LPBYTE)&v, &sz) == ERROR_SUCCESS) dark = (v == 0);
                    RegCloseKey(hk);
                }
            }
            gui_.set_theme_mode(dark);
        }
        y += 32;
    }
    {
        CheckState gs = gif_hidden_ ? CheckState::Off : CheckState::On;
        gui_.checkbox3("set-gif-widget", { content.left, y, content.left + 280, y + 24 },
                       L"\u663e\u793a\u53f3\u4e0b\u89d2\u6302\u4ef6", gs);
        gif_hidden_ = (gs != CheckState::On);
        y += 30;
    }
    note(L"\u5b98\u65b9\u8fd8\u652f\u6301\u754c\u9762\u4e3b\u9898\u3001\u8fc7\u6e21\u52a8\u753b\u3001\u5e72\u5458\u540d\u79f0\u8bed\u8a00\u3001"
         L"\u65e5\u5fd7\u5361\u7247\u6837\u5f0f\u3001\u6258\u76d8\u56fe\u6807\u7b49\uff08MaaHub \u672a\u5b9e\u73b0\uff09");

            // ---------------- 启动设置（官方 9 项，MaaHub 实现可行子集）----------------
    group(L"\u542f\u52a8\u8bbe\u7f6e");
    {
        AppSettings s = hub_->settings().get();
        set_auto_start_   = s.auto_start;
        set_start_min_    = s.start_minimized;
        set_start_run_    = s.start_run;
        struct U { const char* key; const wchar_t* label; bool* val; };
        const U us[] = {
            { "set-auto-start", L"\u5f00\u673a\u81ea\u52a8\u542f\u52a8 MaaHub", &set_auto_start_ },
            { "set-start-min",  L"\u542f\u52a8\u540e\u76f4\u63a5\u6700\u5c0f\u5316", &set_start_min_ },
            { "set-start-run",  L"\u542f\u52a8\u540e\u76f4\u63a5\u8fd0\u884c", &set_start_run_ },
        };
        for (auto& u : us) {
            CheckState st = *u.val ? CheckState::On : CheckState::Off;
            gui_.checkbox3(u.key, { content.left, y, content.left + 300, y + 22 },
                           u.label, st);
            *u.val = (st == CheckState::On);
            y += 28;
        }
        note(L"\u5b98\u65b9\u8fd8\u6709\u201c\u81ea\u52a8\u5f00\u542f\u6a21\u62df\u5668 / PC \u5ba2\u6237\u7aef\u201d\u4e0e\u201c\u9644\u52a0\u547d\u4ee4\u201d\uff0cMaaHub \u672a\u5b9e\u73b0\u3002");
    }

    // ---------------- 更新设置（官方 9 项，MaaHub 实现核心子集）----------------
    group(L"\u66f4\u65b0\u8bbe\u7f6e");
    {
        AppSettings s = hub_->settings().get();
        set_upd_check_   = s.update_check_on_start;
        set_upd_dl_      = s.update_auto_download;
        set_upd_install_ = s.update_auto_install;
        set_upd_gh_      = s.update_force_github;
        struct U { const char* key; const wchar_t* label; bool* val; };
        const U us[] = {
            { "set-upd-check",   L"\u542f\u52a8\u65f6\u68c0\u67e5\u66f4\u65b0", &set_upd_check_ },
            { "set-upd-dl",      L"\u81ea\u52a8\u4e0b\u8f7d\u66f4\u65b0\u5305", &set_upd_dl_ },
            { "set-upd-install", L"\u81ea\u52a8\u5b89\u88c5\u66f4\u65b0\u5305", &set_upd_install_ },
            { "set-upd-gh",      L"\u5f3a\u5236\u4f7f\u7528 GitHub", &set_upd_gh_ },
        };
        for (auto& u : us) {
            CheckState st = *u.val ? CheckState::On : CheckState::Off;
            gui_.checkbox3(u.key, { content.left, y, content.left + 300, y + 22 },
                           u.label, st);
            *u.val = (st == CheckState::On);
            y += 28;
        }
        note(L"\u6838\u5fc3\u62c9\u53d6\u4e0e\u66f4\u65b0\u5728\u201c\u6838\u5fc3\u7ba1\u7406\u201d\u4e2d\u6267\u884c\uff1b\u5b98\u65b9\u8fd8\u6709\u5b9a\u65f6\u68c0\u67e5\u3001\u66f4\u65b0\u65e5\u5fd7\u3001\u8d44\u6e90\u66f4\u65b0\u7b49\u3002");
    }


// ---------------- 智能规划（LLM）----------------
    group(L"\u667a\u80fd\u89c4\u5212\uff08LLM\uff09");
    {
        CheckState es = set_llm_enabled_ ? CheckState::On : CheckState::Off;
        gui_.checkbox3("set-llm-enabled", { content.left, y, content.left + 240, y + 24 },
                       L"\u542f\u7528 LLM \u89c4\u5212", es);
        set_llm_enabled_ = (es == CheckState::On);
        y += 28;
        // 标签统一占 74px，输入框撑满剩余宽度（避免宽度参差）
        const int LBL = 74;
        gui_.text(content.left, y + 4, L"Base URL", gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-llm-base", { content.left + LBL, y, content.right, y + 26 },
                        set_llm_url_, L"https://api.deepseek.com");
        y += 30;
        gui_.text(content.left, y + 4, L"\u6a21\u578b", gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-llm-model", { content.left + LBL, y, content.right, y + 26 },
                        set_llm_model_, L"deepseek-chat");
        y += 30;
        gui_.text(content.left, y + 4, L"API Key", gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-llm-key", { content.left + LBL, y, content.right, y + 26 },
                        set_llm_key_, L"sk-...");
        y += 30;
        note(L"\u672a\u914d\u7f6e\u6216\u4e0d\u53ef\u8fbe\u65f6\u81ea\u52a8\u9000\u56de\u53d1\u5f0f\u89c4\u5212");
    }

    // ---------------- 保存 ----------------
    {
        RECT sb{ content.left, y, content.left + 140, y + 30 };
        if (gui_.button("set-save", sb, L"\u4fdd\u5b58\u8bbe\u7f6e", 1)) {
            action_save_settings();
        }
        if (!core_msg_.empty()) {
            RECT save = gui_.clip();
            gui_.text_clipped({ content.left + 156, y + 6, content.right, y + 28 },
                              gui::widen(core_msg_),
                              core_msg_.find("\u5931\u8d25") != std::string::npos
                                  ? gui_.theme.err : gui_.theme.ok,
                              gui_.font);
            (void)save;
        }
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
    std::string theme = set_theme_;
    bool auto_start = set_auto_start_, start_min = set_start_min_, start_run = set_start_run_;
    bool upd_check = set_upd_check_, upd_dl = set_upd_dl_,
         upd_install = set_upd_install_, upd_gh = set_upd_gh_;
    post_action([this, mode, host, port, user, pass, cdk, enabled, url, key, model, theme,
                 auto_start, start_min, start_run,
                 upd_check, upd_dl, upd_install, upd_gh] {
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
            s.theme = theme;
            s.auto_start = auto_start;
            s.start_minimized = start_min;
            s.start_run = start_run;
            s.update_check_on_start = upd_check;
            s.update_auto_download = upd_dl;
            s.update_auto_install = upd_install;
            s.update_force_github = upd_gh;
        });
        set_notice("设置已保存");
    }, "保存设置");
}

} // namespace hub
