// MaaHub 原生窗口应用实现
#pragma comment(lib, "comdlg32.lib")   // GetOpenFileNameA（作业文件选择）
#pragma comment(lib, "ole32.lib")      // CoCreateInstance/CoTaskMemFree（导入路径选择）
#include "app_window.hpp"
#include "task_panels.hpp"

#include <windowsx.h>
#include <commdlg.h>
#include <shobjidl.h>   // IFileDialog（导入安装路径选择）

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
    load_task_checks();
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

    // 官方 RootView Width=800 Height=600：执行器区保持 800 不变，左侧栏额外占 RAIL_W
    int w = 800 + RAIL_W, h = 600;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    RECT rc{ 0, 0, w, h };
    DWORD style = offscreen_ ? (WS_POPUP & ~WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
    if (offscreen_) {
        // WS_POPUP 没有非客户区，外框即客户区。若这里也 AdjustWindowRect，
        // 离屏画布会比真实窗口的 800x600 客户区多出标题栏那 16x39，
        // 底部对齐的元素在真窗口里挤成重叠、在渲染图上却看不出来。
        if (dump_w_ > 0 && dump_h_ > 0) { w = dump_w_; h = dump_h_; rc.right = w; rc.bottom = h; }
    } else {
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    }
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
    case WM_APP_IMPORT: {
        // backend 线程完成配置导入：取走 task_lists 并合并进内存权威 task_list_，
        // 然后统一落盘（不直写 state/task-lists.json，避免与内存态互相覆盖）
        mj::Value lists;
        {
            std::lock_guard<std::mutex> lk(import_mtx_);
            lists = std::move(import_task_lists_);
            import_task_lists_ = mj::Value::object();
        }
        bool changed = false;
        if (lists.is_object()) {
            for (auto& kv : lists.members()) {
                if (!kv.second.is_array()) continue;
                auto& dst = task_list_[kv.first];
                for (auto& t : kv.second.items()) {
                    if (t.is_string() && !t.as_string().empty()) {
                        dst.insert(t.as_string());
                        changed = true;
                    }
                }
            }
        }
        if (changed) {
            save_task_list();
            refresh_snapshot();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
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
    sync_window_title();

    // 外壳：原生标题栏（窗口标题 = 动态实例信息）+ 左侧栏 + 右侧执行器区
    // 执行器 = 一个独立的 MAA/MaaEnd 实例；非执行器内容全部收进左侧栏，
    // 右侧整块留给官方 TaskQueueView 的三列布局
    RECT rail{ client.left, client.top, client.left + RAIL_W, client.bottom };
    RECT body{ rail.right, client.top, client.right, client.bottom };
    RECT tabbar{ body.left, body.top, body.right, body.top + topbar_h_ };
    // 官方窗口 800x600，内容区边距收窄到 10（TaskQueueView 内部仍是 Margin 20 的比例）
    RECT content{ body.left + 10, body.top + topbar_h_ + 12,
                  body.right - 10, body.bottom - 8 };

    draw_left_rail(rail);
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
        else page_executor(content);
    } catch (const std::exception& e) {
        fprintf(stderr, "[paint] EXCEPTION in page %s: %s\n", page_.c_str(), e.what());
    } catch (...) {
        fprintf(stderr, "[paint] UNKNOWN EXCEPTION in page %s\n", page_.c_str());
    }

    draw_new_exec_overlay();   // 左栏「+ 新建」的弹窗，盖在所有页面之上

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
// 右侧再挂全局管理入口（账号 / 核心 / 规划 / 日志）。
// 窗口标题对齐官方 UpdateWindowTitle（模拟器 + IP:端口），由原生标题栏显示。
void MaaHubWindow::sync_window_title() {
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
    if (title == win_title_) return;   // SetWindowTextW 会重绘非客户区，仅变化时调用
    win_title_ = title;
    if (hwnd_) SetWindowTextW(hwnd_, title.c_str());
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
        // 以下两页尚未做完，暂时隐藏入口（页面实现与路由保留，做完再放开）：
        //   copilot = 自动战斗：作业列表为空，未接入 Copilot 适配器
        //   toolbox = 小工具：5 个工具里 3 个（牛杂系列）未实现
        // { "copilot",   L"\u81ea\u52a8\u6218\u6597" },
        // { "toolbox",   L"\u5c0f\u5de5\u5177" },
    };
    int y = r.top + 6;
    const int h = r.bottom - r.top - 12;
    // 官方 TabControlInLine 的 headerPanel 是 UniformGrid Rows=1：等分整行，标签居中
    const int ntab = (int)(sizeof(tabs) / sizeof(tabs[0]));
    const int PIN_W = 56;   // 右侧只留给「置顶」
    int slot = ((r.right - PIN_W) - r.left) / (ntab > 0 ? ntab : 1);
    int x = r.left;
    for (auto& t : tabs) {
        int w = slot;
        RECT tr{ x, y, x + w, y + h };
        bool sel = (page_ == t.page);
        bool hov = gui_.input().mouse.x >= tr.left && gui_.input().mouse.x < tr.right
                && gui_.input().mouse.y >= tr.top && gui_.input().mouse.y < tr.bottom;
        // 官方 TabItemStyle：Padding=DefaultControlPadding(10,5)、MinHeight=28；
        // 选中态 = 文字 PrimaryBrush + 底部 2px 下划线（TitleBrush），
        // 未选中 = PrimaryTextBrush；hover 仅背景微亮（不是整块蓝底白字）
        COLORREF txt = sel ? gui_.theme.primary : gui_.theme.text;
        if (hov && !sel) {
            gui_.round_rect(tr, gui_.theme.hover, gui_.theme.hover, 3);
        }
        int lw = gui_.text_size(t.label, gui_.font).cx;
        gui_.text(tr.left + (w - lw) / 2, tr.top + (h - 18) / 2, t.label, txt, gui_.font);
        if (sel) {
            // 底部 2px 下划线（官方选中态标识），宽度随标签
            int ux0 = tr.left + (w - lw) / 2;
            gui_.fill({ ux0, tr.bottom - 2, ux0 + lw, tr.bottom },
                      gui_.theme.primary);
        }
        if (hov && gui_.input().pressed) {
            page_ = t.page;
        }
        x += w;
    }

    // 置顶切换：原来挂在自绘标题栏的 📌 上，标题栏去掉后挪到标签栏最右侧
    {
        const wchar_t* pin = L"置顶";
        int pw = gui_.text_size(pin, gui_.font_small).cx + 16;
        RECT pr{ r.right - 8 - pw, y, r.right - 8, y + h };
        bool ph = gui_.hovered(pr);
        if (ph) gui_.round_rect(pr, gui_.theme.hover, gui_.theme.hover, 3);
        gui_.text(pr.left + 8, pr.top + (h - 15) / 2, pin,
                  topmost_ ? gui_.theme.primary : gui_.theme.dim, gui_.font_small);
        if (ph && gui_.input().pressed) {
            topmost_ = !topmost_;
            SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

// ================================================================ 左侧栏
// 执行器 = 一个独立的 MAA/MaaEnd 实例。账号/核心/规划/日志/设置这些不属于执行器
// 的内容全部收在这里，右侧整块留给官方 TaskQueueView 的三列布局。
void MaaHubWindow::draw_left_rail(RECT r) {
    gui_.fill(r, gui_.theme.row_alt);
    gui_.fill({ r.right - 1, r.top, r.right, r.bottom }, gui_.theme.border);

    int x0 = r.left + 8, x1 = r.right - 8;
    int y = r.top + 10;
    gui_.text(x0 + 4, y, L"执行器", gui_.theme.dim, gui_.font_small);
    y += 20;

    std::vector<ExecutorEntry> execs = hub_->executors();
    if (!execs.empty()) {
        draw_executor_list({ x0, y, x1, y + (int)execs.size() * EXEC_ROW_H });
        y += (int)execs.size() * EXEC_ROW_H + 2;
    }
    RECT nb{ x0, y, x1, y + EXEC_ROW_H };
    bool nh = gui_.hovered(nb);
    if (nh) gui_.round_rect(nb, gui_.theme.hover, gui_.theme.hover, 4);
    gui_.text(nb.left + 10, nb.top + 5, L"＋ 新建执行器",
              nh ? gui_.theme.primary : gui_.theme.dim, gui_.font_small);
    if (nh && gui_.input().pressed) {
        show_new_exec_ = true;
        new_label_.clear();
    }
    y += EXEC_ROW_H + 8;

    gui_.fill({ x0 + 4, y, x1 - 4, y + 1 }, gui_.theme.border);
    y += 12;

    struct G { const char* page; const wchar_t* label; };
    const G gs[] = {
        { "accounts", L"账号管理" },
        { "cores",    L"核心管理" },
        { "planner",  L"智能规划" },
        { "events",   L"事件日志" },
        { "settings", L"设置"     },
    };
    for (auto& g : gs) {
        RECT gr{ x0, y, x1, y + EXEC_ROW_H };
        bool sel = page_ == g.page;
        bool hov = gui_.hovered(gr);
        if (sel) gui_.round_rect(gr, gui_.theme.primary_light, gui_.theme.primary, 4);
        else if (hov) gui_.round_rect(gr, gui_.theme.hover, gui_.theme.hover, 4);
        gui_.text(gr.left + 10, gr.top + 5, g.label,
                  sel ? gui_.theme.primary_dark : gui_.theme.text, gui_.font_small);
        if (hov && gui_.input().pressed) page_ = g.page;
        y += EXEC_ROW_H + 2;
    }
}

// 执行器列表：常驻左侧栏，点选即切换当前实例。
void MaaHubWindow::draw_executor_list(RECT r) {
    std::vector<ExecutorEntry> execs = hub_->executors();
    const int RH = EXEC_ROW_H;               // 行高（与官方任务项 24.5 同量级）
    for (size_t k = 0; k < execs.size(); ++k) {
        const ExecutorEntry& e = execs[k];
        int top = r.top + (int)k * RH;
        if (top + RH - 3 > r.bottom) break;  // 放不下即止（单列实例数远小于可视高度）
        RECT ir{ r.left, top, r.right, top + RH - 3 };
        bool sel = selected_exec_ == e.id;
        bool hov = gui_.hovered(ir);
        // 侧栏里靠底色区分选中，未选中不描边，否则一列方框很碎
        gui_.round_rect(ir, sel ? gui_.theme.primary_light : (hov ? gui_.theme.hover : CLR_INVALID),
                        sel ? gui_.theme.primary : CLR_INVALID, 4);
        mj::Value st = hub_->executor_status(e.id, 0.4);
        std::string conn = st.get_str("conn", "unknown");
        std::string run = st.get_str("run", "");
        COLORREF dot = conn == "connected" ? gui_.theme.ok :
                       conn == "failed" ? gui_.theme.err :
                       run == "running" ? gui_.theme.primary : gui_.theme.dim;
    int dh = 8;
        gui_.round_rect({ ir.left + 9, ir.top + (ir.bottom - ir.top - dh) / 2,
                          ir.left + 9 + dh, ir.top + (ir.bottom - ir.top + dh) / 2 },
                        dot, dot, 5);
        gui_.text_clipped({ ir.left + 24, ir.top + 2, ir.right - 8, ir.bottom },
                          gui::widen(e.label),
                          sel ? gui_.theme.primary_dark : gui_.theme.text, gui_.font_small);
        if (hov && gui_.input().pressed) {
            selected_exec_ = e.id;
            page_ = "executor";
        }
    }
}

// 新建执行器：一个执行器 = 一份独立的核心 + 账号 + 任务配置
void MaaHubWindow::draw_new_exec_overlay() {
    if (!show_new_exec_) return;
    RECT cl;
    GetClientRect(hwnd_, &cl);
    gui_.fill(cl, RGB(0x10, 0x10, 0x10));

    const int W = 320, H = 196;
    RECT card{ (cl.right - W) / 2, (cl.bottom - H) / 2, (cl.right + W) / 2, (cl.bottom + H) / 2 };
    gui_.round_rect(card, gui_.theme.panel, gui_.theme.border, 6);
    gui_.text(card.left + 16, card.top + 14, L"新建执行器", gui_.theme.text, gui_.font_title);

    int y = card.top + 44;
    gui_.text(card.left + 16, y + 4, L"类型", gui_.theme.dim, gui_.font_small);
    RECT kind_r{ card.left + 64, y, card.right - 16, y + 24 };
    int k = gui_.option_strip("newexec-kind", kind_r,
                              { L"MAA 官服", L"MaaEnd 终末地" }, new_kind_);
    if (k >= 0) new_kind_ = k;
    y += 32;

    gui_.text(card.left + 16, y + 4, L"名称", gui_.theme.dim, gui_.font_small);
    RECT name{ card.left + 64, y, card.right - 16, y + 24 };
    gui_.text_input("newexec-label", name, new_label_, L"留空自动命名");
    y += 32;

    // 账号按类型过滤；0 = 不绑定
    std::string kind_key = new_kind_ == 0 ? "maa" : "maaend";
    std::vector<std::wstring> accs{ L"不绑定" };
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        if (auto* a = snap_.find("accounts"); a && a->is_array()) {
            for (auto& x : a->items()) {
                if (x.get_str("game", "") != kind_key) continue;
                accs.push_back(gui::widen(x.get_str("label", "")));
            }
        }
    }
    if (new_acc_idx_ >= (int)accs.size()) new_acc_idx_ = 0;
    gui_.text(card.left + 16, y + 4, L"账号", gui_.theme.dim, gui_.font_small);
    RECT acc{ card.left + 64, y, card.right - 16, y + 24 };
    int pick = gui_.combo("newexec-acc", acc, accs, new_acc_idx_);
    if (pick >= 0) new_acc_idx_ = pick;

    RECT ok_b{ card.right - 96, card.bottom - 40, card.right - 16, card.bottom - 14 };
    RECT cancel_b{ card.right - 184, card.bottom - 40, card.right - 104, card.bottom - 14 };
    gui_.flush_combo_popup();
    if (gui_.button("newexec-cancel", cancel_b, L"取消", 0)) show_new_exec_ = false;
    if (gui_.button("newexec-ok", ok_b, L"创建", 1)) action_create_executor();
}


// MaaEnd 任务名 -> 官方界面文案：直接取 interface 里 $task.X.label 解析出的 label。
// 硬编码表覆盖不全（SellProduct 的文案挂在 task.OutpostTrading.label 上），
// 所以以核心自带的 locales 为准，表只作兜底。
const std::map<std::string, std::wstring>& MaaHubWindow::end_labels() {
    std::map<std::string, std::wstring> fresh;
    std::string sig;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        if (auto* e = snap_.find("executors"); e && e->is_array()) {
            for (auto& x : e->items()) {
                if (x.get_str("kind", "") != "maaend") continue;
                auto* c = x.find("catalog");
                if (!c || !c->is_array()) break;
                for (auto& t : c->items()) {
                    std::string nm = t.get_str("name", "");
                    if (nm.empty()) continue;
                    sig += nm; sig += '|';
                    std::string lbl = t.get_str("label", "");
                    if (!lbl.empty()) fresh[nm] = gui::widen(lbl);
                }
                break;
            }
        }
    }
    if (!sig.empty() && sig != end_label_sig_) {
        end_label_sig_ = sig;
        end_label_ = std::move(fresh);
    }
    return end_label_;
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
        auto& lm = end_labels();
        auto li = lm.find(task);
        if (li != lm.end()) return li->second;
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
                        task_list_[id].insert(s);
                    }
                }
                task_list_[id].insert(nm);
            } else {
                if (task_list_.find(id) == task_list_.end()) {
                    for (auto& t : all) {
                        std::string s = t.get_str("name", "");
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
            task_list_[id].insert(s);
        }
        save_task_list();
    }
    if (gui_.button("addtask-inverse", sel_inv, L"\u53cd\u9009", 0)) {
        // 确保有初始集合，再逐个取反
        if (task_list_.find(id) == task_list_.end()) {
            for (auto& t : all) {
                std::string s = t.get_str("name", "");
                task_list_[id].insert(s);
            }
        }
        for (auto& t : all) {
            std::string s = t.get_str("name", "");
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
        // 按 catalog 目录序落盘（StartUp 恒第一），不让 std::set 字典序进文件：
        // 调度器/CLI 按文件数组原序执行，字母序会把 StartUp 排最后导致 02:00 链失败。
        mj::Value arr = mj::Value::array();
        std::vector<mj::Value> cat = catalog_of(kind_of(kv.first));
        for (auto& t : cat) {
            std::string nm = t.get_str("name", "");
            if (!nm.empty() && kv.second.count(nm)) arr.push(nm);
        }
        if (arr.items().empty()) {          // 目录不可用（快照未就绪）兜底原序
            for (auto& nm : kv.second) arr.push(nm);
        }
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

// 左列勾选状态：官方 MAA 会记住队列勾选，重启后恢复上次的运行组合
void MaaHubWindow::save_task_checks() {
    mj::Value v = mj::Value::object();
    for (auto& kv : task_checks_) {
        mj::Value arr = mj::Value::array();
        for (auto& nm : kv.second) arr.push(nm);
        v.set(kv.first, std::move(arr));
    }
    util::write_file(util::exe_dir() + "\\state\\task-checks.json",
                     mj::Value::dump_pretty(v));
}

// 依据当前可见任务重建勾选记录并落盘
void MaaHubWindow::sync_task_checks(const std::string& id,
                                    const std::vector<mj::Value>& tasks) {
    auto& cs = task_checks_[id];
    cs.clear();
    auto it = task_ui_.find(id);
    if (it == task_ui_.end()) { save_task_checks(); return; }
    for (auto& t : tasks) {
        std::string nm = t.get_str("name", "");
        auto jt = it->second.find(nm);
        if (jt != it->second.end() && jt->second.checked) cs.insert(nm);
    }
    save_task_checks();
}

void MaaHubWindow::load_task_checks() {
    std::string content;
    if (!util::read_file(util::exe_dir() + "\\state\\task-checks.json", content)) return;
    mj::Value v;
    if (!mj::try_parse(content, v)) return;
    for (auto& kv : v.members()) {
        if (!kv.second.is_array()) continue;
        for (auto& nm : kv.second.items()) {
            if (nm.is_string()) task_checks_[kv.first].insert(nm.as_string());
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
    // 官方右列（Grid.Column="2"，Margin="10,10,0,10"）= 纯日志流：
    //   每条日志一个卡片 Border Margin="0,0,0,2.5" Padding="5,0,1,5"
    //     Background=MouseOverRegionBrushOpacity10 BorderBrush=BorderBrush
    //     BorderThickness=1 CornerRadius=4
    //   卡片内两列：时间 Width=60 / Foreground=TraceLogBrush，内容 TextWrapping=Wrap
    // 官方按时间正序追加并自动滚到底，这里同样从底部向上排最近若干条。
    int x = area.left;
    int right = area.right - 1;
    int content_x = x + 65;                 // 5（左边距）+ 60（时间列）
    int maxw = right - content_x - 3;
    if (maxw < 40) maxw = 40;

    // 逐字按可用宽度折行（官方面对窄列时 Wrap 而不是裁切），超过 3 行加省略号
    auto wrap = [&](const std::wstring& s, int max_rows) {
        std::vector<std::wstring> rows;
        std::wstring cur;
        int used = 0;
        for (wchar_t ch : s) {
            int cw = gui_.text_size(std::wstring(1, ch), gui_.font_small).cx;
            if (used + cw > maxw && !cur.empty()) {
                rows.push_back(cur);
                cur.clear();
                used = 0;
                if ((int)rows.size() >= max_rows) { rows.back() += L"\u2026"; return rows; }
            }
            cur += ch;
            used += cw;
        }
        if (!cur.empty()) rows.push_back(cur);
        if ((int)rows.size() > max_rows) { rows.resize(max_rows); rows.back() += L"\u2026"; }
        return rows;
    };

    struct Row { std::wstring time, text; COLORREF col; };
    std::vector<Row> src;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        if (auto* ev = snap_.find("events"); ev && ev->is_array()) {
            for (auto& e : ev->items()) {
                if (e.get_str("exec_id", "") != id) continue;
                Row r;
                int64_t ts = e.get_int("ts", 0);
                r.time = L"--:--:--";
                if (ts > 0) {
                    time_t tt = (time_t)ts;
                    struct tm tmv;
                    if (localtime_s(&tmv, &tt) == 0) {
                        wchar_t buf[16];
                        swprintf(buf, 16, L"%02d:%02d:%02d",
                                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
                        r.time = buf;
                    }
                }
                std::string what = e.get_str("what", "");
                std::string raw = e.get_str("raw", "");
                std::string task = e.get_str("task", "");
                std::string body = raw.empty()
                    ? (what + (task.empty() ? "" : (" / " + task))) : raw;
                r.text = gui::widen(body.empty() ? what : body);
                std::string cat = e.get_str("category", "");
                r.col = cat == "error" ? gui_.theme.err
                      : (cat == "warning" || cat == "connection") ? gui_.theme.warn
                                                                  : gui_.theme.text;
                src.push_back(std::move(r));
            }
        }
    }
    if (src.empty()) {
        gui_.text(x + 4, area.top + 4, L"\u6682\u65e0\u65e5\u5fd7",
                  gui_.theme.faint, gui_.font_small);
        return;
    }

    int y = area.bottom;
    for (int i = (int)src.size() - 1; i >= 0; --i) {
        std::vector<std::wstring> lines = wrap(src[i].text, 3);
        int h = 10 + 16 * (int)lines.size();
        if (h < 26) h = 26;
        int top = y - h - 3;                // 官方 Margin="0,0,0,2.5"
        if (top < area.top) break;
        RECT card{ x, top, right, y - 3 };
        gui_.round_rect(card, gui_.theme.card_bg_soft, gui_.theme.border, 4);
        gui_.text(card.left + 5, card.top + 5, src[i].time,
                  gui_.theme.trace_text, gui_.font_small);
        int ty = card.top + 5;
        for (auto& s : lines) {
            gui_.text(content_x, ty, s, src[i].col, gui_.font_small);
            ty += 16;
        }
        y = top;
    }
}



// ================================================================ 实例页（MAA 1:1 布局）
// 复刻 MAA 官方 TaskQueueView：左列 190px 任务列表 + 右侧任务设置 + 底部控制栏。
// 配色/圆角(4)/控件高(28)/外边距(20) 对齐 HandyControl Light 主题。
void MaaHubWindow::page_executor(RECT content) {
    mj::Value ex;
    if (!find_executor(selected_exec_, ex)) {
        gui_.text(content.left + 24, content.top + 40,
                  L"左侧「执行器」里选择一个实例，或点「＋ 新建执行器」创建。",
                  gui_.theme.dim, gui_.font);
        return;
    }
    std::string id = ex.get_str("id", "");
    std::string kind = ex.get_str("kind", "");
    std::string conn = ex.get_str("conn", "unknown");
    std::string version = ex.get_str("version", "");
    std::string acc_label = ex.get_str("account_label", "");
    bool daily_done = ex.get_bool("daily_done_today", false);
    int64_t task_count = ex.get_int("task_count", 0);
    std::string core_version = ex.get_str("core_version", "");

    std::vector<mj::Value> all_tasks = catalog_of(kind);
    // 首次打开 MaaEnd 实例：默认任务列表取官方核心预设「全套日常」(DailyFull)。
    // 只在 state/task-lists.json 里该实例还没有记录时初始化一次，之后完全交给用户。
    if (kind == "maaend" && !task_ui_seeded_[id]) {
        task_ui_seeded_[id] = true;
        if (task_list_.find(id) == task_list_.end()) {
            std::set<std::string> picked;
            for (auto& t : all_tasks) {
                std::string nm = t.get_str("name", "");
                auto* ps = t.find("presets");
                if (!ps || !ps->is_array()) continue;
                for (auto& p : ps->items()) {
                    if (p.is_string() && p.as_string() == "DailyFull") {
                        picked.insert(nm);
                        break;
                    }
                }
            }
            if (!picked.empty()) {
                task_list_[id] = picked;
                save_task_list();
            }
        }
    }
    std::vector<mj::Value> tasks;   // 过滤内部入口任务 + 用户未添加的任务
    auto tl_it = task_list_.find(id);
    bool filtering = (tl_it != task_list_.end() && !tl_it->second.empty());
    for (auto& t : all_tasks) {
        std::string nm = t.get_str("name", "");
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

    // 官方 TaskQueueView：<Grid Margin="20"> + 三列 Width="*"（各占 1/3）
    //   col0 = 210 宽任务列表卡片 +「完成后」+ LinkStart/停止/等待并停止
    //   col1 = 任务设置面板（控件列 220 宽、居中）→ 常规/高级 → 今日关卡提示
    //   col2 = 纯日志流（下载卡片 + 信息流）
    // 只有左列卡片有独立背景（RegionBrushOpacity25 + 1px BorderBrush + CornerRadius 4），
    // 中/右列直接放在页面背景上。
    RECT frame{ content.left + 10, content.top + 8, content.right - 10, content.bottom - 12 };
    const int COL_W = (frame.right - frame.left) / 3;
    RECT col0{ frame.left, frame.top, frame.left + COL_W, frame.bottom };
    RECT col1{ col0.right, frame.top, col0.right + COL_W, frame.bottom };
    RECT col2{ col1.right, frame.top, frame.right, frame.bottom };
    const bool connected = conn == "connected";

    // col0 的 Grid Margin="10"，行 = *（列表卡片）/ Auto（完成后）/ Auto（按钮组）
    int LEFT_CARD_W = COL_W - 20 < 210 ? COL_W - 20 : 210;   // 官方 Border Width=210
    int LEFT_W = LEFT_CARD_W - 20 < 190 ? LEFT_CARD_W - 20 : 190;  // 官方内部 Grid Width=190
    const int ADD_ROW_H = 40;               // 卡片底部固定行：菜单 + 全选 + 反选/清空
    const int POST_H = 78;                  // 「完成后」标题 + Divider + 说明
    const int START_H = 60;                 // 官方按钮 Width=100 Height=50 Margin=5
    const int RELOAD_H = 22;                // 自动重载资源勾选行
    int c0x = col0.left + 10 + (COL_W - 20 - LEFT_CARD_W) / 2;
    if (c0x < col0.left + 10) c0x = col0.left + 10;
    // 执行器切换在左侧栏，这里回到官方单实例布局
    RECT left_card{ c0x, col0.top + 10, c0x + LEFT_CARD_W,
                    col0.bottom - 10 - POST_H - START_H - (connected ? RELOAD_H : 0) };
    if (left_card.bottom - left_card.top < 160) left_card.bottom = left_card.top + 160;
    gui_.round_rect(left_card, gui_.theme.card_bg, gui_.theme.border, 4);

    // 左列内容区（卡片内 190px，左右各留 10px）
    RECT left_col{ left_card.left + 10, left_card.top, left_card.left + 10 + LEFT_W,
                   left_card.bottom };
    if (left_col.right > left_card.right - 10) left_col.right = left_card.right - 10;
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


    // col1：任务设置。官方 220 控件列（+10 右边距）在列内居中，
    // 行 = *（面板，MinHeight 360）/ Auto（常规·高级 ButtonGroup 220x28）/ Auto（今日提示 200）
    const int PANEL_W = COL_W - 10 < 220 ? COL_W - 10 : 220;
    const int TG_H = 28;                    // 官方 DefaultControlHeight
    const int TIP_H = 46;                   // 今日关卡提示（Width=200，最多两行）
    int c1x = col1.left + (COL_W - (PANEL_W + 10)) / 2;
    if (c1x < col1.left + 2) c1x = col1.left + 2;
    if (c1x + PANEL_W > col1.right - 2) c1x = col1.right - 2 - PANEL_W;
    const int TG_TOP = col1.bottom - 10 - TIP_H - TG_H - 6;
    const int TIP_TOP = col1.bottom - 10 - TIP_H;
    RECT right_col{ c1x, col1.top + 10, c1x + PANEL_W, TG_TOP - 8 };
    // 未偏移的中列边界：底部按钮组与提示固定在列底部，不随动画偏移
    const int RC_LEFT = right_col.left, RC_RIGHT = right_col.right;
    const int RC_BOTTOM = right_col.bottom;
    if (right_col.bottom - right_col.top < 160) right_col.bottom = right_col.top + 160;
    if (anim_active) {
        // ease-out：1-(1-t)^2，模拟官方滑入
        double e = 1.0 - (1.0 - anim_frac) * (1.0 - anim_frac);
        int off = (int)((1.0 - e) * 28.0 * panel_anim_dir_);
        right_col.top += off;
        right_col.bottom += off;
    }
    // col2：日志流（官方 Grid.Column="2" Margin="10,10,0,10"）
    RECT log_col{ col2.left + 10, col2.top + 10, col2.right, col2.bottom - 10 };

    // ---------------- 左列：任务列表 ----------------
    // 官方左列无「任务列表」标题文字，直接是 ListBox（MinHeight=220）
    int list_top = left_card.top + 10;
    int n = (int)tasks.size();
    // 官方：ItemContainer MinHeight=24.5；Items.Count>9 时
    //   MaxHeight = ActualHeight/(Items.Count+1.5)（上限 24.5）
    int list_h = (left_card.bottom - ADD_ROW_H) - list_top;
    int item_h = 25;
    if (n > 9) {
        int fit = (int)((double)list_h / (n + 1.5));
        if (fit < item_h) item_h = fit < 18 ? 18 : fit;
    }

    // ---- 拖拽排序状态机（官方左列可拖动调整顺序）----
    // 记录本帧每项的 y 供放置判定
    std::vector<int> item_ys;
    item_ys.reserve(n);
    {
        int yy = list_top - (int)scroll_["tasks-" + id];
        for (int k = 0; k < n; ++k) { item_ys.push_back(yy); yy += item_h; }
    }
    // 按下左键且在执行器列内 -> 开始拖拽
    if (gui_.input().pressed
        && gui_.input().mouse.x >= left_col.left && gui_.input().mouse.x < left_col.right
        && gui_.input().mouse.y >= list_top) {
        int idx = (gui_.input().mouse.y - (list_top - (int)scroll_["tasks-" + id])) / item_h;
        if (idx >= 0 && idx < n) {
            drag_task_ = tasks[idx].get_str("name", "");
            drag_y_ = gui_.input().mouse.y;
        }
    }
    if (!drag_task_.empty()) {
        drag_y_ = gui_.input().mouse.y;
        // 计算插入位置
        int base = list_top - (int)scroll_["tasks-" + id];
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
                      { left_col.left, list_top, left_col.right, left_card.bottom - ADD_ROW_H },
                      n * item_h + 8, scroll_["tasks-" + id]);
    int y = list_top - (int)scroll_["tasks-" + id];
    for (int ti = 0; ti < n; ++ti) {
        auto& t = tasks[ti];
        std::string name = t.get_str("name", "");
        TaskUiState& ui = task_ui_[id][name];
        if (task_ui_init_[id].insert(name).second) {
            // 没有勾选记录 = 首次使用该执行器，官方行为是新任务默认勾选
            auto cit = task_checks_.find(id);
            ui.checked = cit == task_checks_.end() || cit->second.count(name) > 0;
        }
        if (y + item_h < list_top) { y += item_h; continue; }
        if (y > left_card.bottom - ADD_ROW_H) break;
        RECT item{ left_col.left, y, left_col.right, y + item_h - 1 };
        bool hov = gui_.input().mouse.x >= item.left && gui_.input().mouse.x < item.right
                && gui_.input().mouse.y >= item.top && gui_.input().mouse.y < item.bottom;
        // 官方 NoSelectedHighlightListBoxItemStyle：
        //   默认与选中背景都是 RegionBrushOpacity25（选中不高亮，只文字变深），
        //   hover = MouseOverRegionBrushOpacity75，且没有边框线（BorderThickness=0）；
        //   状态色仅在本/上次运行有结果时叠加（InProgress 蓝 / Completed 绿 / Error 红，
        //   均为 #20 Alpha 叠在白底上）。当前显示设置的任务只由齿轮变 Primary 表示。
        std::string running = hub_->running_task_of(id);
        std::string last_res = hub_->last_task_result(id, name);
        COLORREF fill_c;
        if (running == name) fill_c = gui_.theme.status_running;
        else if (last_res == "error") fill_c = gui_.theme.status_error;
        else if (last_res == "ok") fill_c = gui_.theme.status_done;
        else if (hov) fill_c = gui_.theme.item_hover;
        else fill_c = gui_.theme.item_bg;
        if (drag_task_ == name) fill_c = gui_.theme.primary_light;
        gui_.round_rect(item, fill_c, fill_c, 4);
        // 官方 CheckBox MaxWidth=140
        // 官方 ItemContainer Padding=10,0,0,0
        // 复选框矩形 = 整个任务项（内部自行垂直居中）
        RECT cbr{ item.left + 2, item.top, item.left + 2 + 164, item.bottom };
        std::wstring label = display_name(kind, name);
        auto al = task_alias_.find(id + "|" + name);
        if (al != task_alias_.end() && !al->second.empty()) {
            label = gui::widen(al->second);
        }
        CheckState st = ui.once ? CheckState::Once
                                : (ui.checked ? CheckState::On : CheckState::Off);
        gui_.checkbox3(("cb-" + id + "-" + name).c_str(), cbr, label, st);
        bool prev_checked = ui.checked;
        ui.checked = (st == CheckState::On);
        ui.once = (st == CheckState::Once);
        if (ui.checked != prev_checked) sync_task_checks(id, tasks);
        // 官方任务项右侧：ConfigGeometry 齿轮图标（常态 SecondaryText 色，
        // 选中/hover 时 Primary 色）；hover 时再显示 复制/重命名/删除 三个
        // 22x22 图标按钮（官方 TaskActionButtonStyle）
        {
            int isz = 16;
            int ix = item.right - isz - 6;
            int iy = item.top + ((item.bottom - item.top) - isz) / 2;
            RECT gear_r{ ix, iy, ix + isz, iy + isz };
            COLORREF gear_c = (name == sel_task) ? gui_.theme.primary : gui_.theme.dim;
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

    // 官方左列底部 40px 行（Grid.Row=1，RowDefinition Height="40"）：
    //   MenuButton 30x30（TaskQueueAddTaskIcon）| 全选 | 反选/清空，三段同高相邻
    {
        int ry = left_card.bottom - ADD_ROW_H + (ADD_ROW_H - 30) / 2;
        int lx = left_col.left;
        int rw = left_col.right - left_col.left;
        std::vector<RECT> cells;
        int hit = gui_.button_group(
            ("taskgrp-" + id).c_str(), { lx, ry, lx + rw, ry + 30 },
            { L"", L"\u5168\u9009", L"\u53cd\u9009" }, { 30, 0, 0 }, 0, &cells);
        if (hit == 0) {
            show_add_task_ = !show_add_task_;
        } else if (hit == 1) {
            for (auto& t : tasks) task_ui_[id][t.get_str("name", "")].checked = true;
        } else if (hit == 2) {
            for (auto& t : tasks) {
                TaskUiState& s = task_ui_[id][t.get_str("name", "")];
                s.checked = !s.checked;
            }
        }
        if (hit == 1 || hit == 2) sync_task_checks(id, tasks);
        if (!cells.empty())
            gui_.icon_add_task({ cells[0].left + 8, cells[0].top + 8,
                                 cells[0].left + 22, cells[0].bottom - 8 },
                               gui_.theme.primary);
        // 官方末格是 SplitButton：右侧再给一个下拉箭头（切换 反选/清空）
        if (cells.size() == 3) {
            RECT& last = cells[2];
            gui_.icon_chevron_down({ last.right - 16, last.top, last.right - 4, last.bottom },
                                    gui_.theme.dim);
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

    int rx = right_col.left;
    if (!sel_task.empty() && sel_valid) {
        TaskUiState& ui = task_ui_[id][sel_task];
        ui.open = true;     // 官方没有「收起参数」开关：选中任务即展示其设置面板
        // 从持久层加载已保存的面板值（官方面板的设置是持久化的）
        {
            mj::Value saved = hub_->task_params(id, sel_task);
            for (auto& kv : saved.members()) {
                // "__once_" 前缀为一次性标记，不持久化
                if (kv.first.rfind("__once_", 0) == 0) continue;
                ui.params[kv.first] = kv.second;
            }
        }
        // 官方：ScrollViewer Grid.Row=0 → SimpleStackPanel Width=220，
        // 面板直接坐在页面背景上——无大标题、无独立卡片、无左侧竖线
        const mj::Value* spec = nullptr;
        for (auto& t : tasks) {
            if (t.get_str("name", "") == sel_task) { spec = &t; break; }
        }
        std::vector<mj::Value> specs;
        if (spec) {
            if (auto* ps = spec->find("params"); ps && ps->is_array()) specs = ps->items();
        }
        int panel_h = RC_BOTTOM - right_col.top;
        if (panel_h < 80) panel_h = 80;
        // 内容高度：用上一帧实测值（首帧先按视口高，画完立刻校正）。
        // 原来按 specs 条数×40 估，估多了会把滚动条拉出一屏空白，估少了够不到底部。
        std::string skey = "panel-" + id + "-" + sel_task;
        float& psy = scroll_[skey];
        int& measured = content_h_[skey];
        int content_h = measured > 0 ? measured : panel_h;
        gui_.begin_scroll(skey.c_str(),
                          { rx, right_col.top, right_col.right, RC_BOTTOM },
                          content_h, psy);
        // 滚动偏移必须真的减进内容起点，否则滚轮只动滚动条、内容不动
        PanelCtx pctx{ gui_, rx, right_col.top - (int)psy, right_col.right,
                       ui.params, id, sel_task, advanced_view_ };
        draw_task_panel(pctx, &specs);
        measured = pctx.y - right_col.top + 16;
        if (measured < panel_h) measured = panel_h;
        // 面板修改写回持久层（一次性标记除外）
        for (auto& kv : ui.params) {
            if (kv.first.rfind("__once_", 0) == 0) continue;
            hub_->set_task_param(id, sel_task, kv.first, kv.second);
        }
        gui_.end_scroll();
    }

    // 中列 Grid.Row=1：常规设置 / 高级设置
    // （官方 ButtonGroup Width=220、控件高 28，在列内居中；列底之上）
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
            int gh = gui_.option_strip(("tg-" + id + "-" + sel_task).c_str(),
                                       { RC_LEFT, TG_TOP, RC_LEFT + PANEL_W, TG_TOP + TG_H },
                                       { L"\u5e38\u89c4\u8bbe\u7f6e", L"\u9ad8\u7ea7\u8bbe\u7f6e" },
                                       advanced_view_ ? 1 : 0, false, true);
            if (gh == 0) advanced_view_ = false;
            if (gh == 1) advanced_view_ = true;
        }
    }

    // 中列 Grid.Row=2（列底，VerticalAlignment=Bottom）：
    //   今日开放关卡提示 ScrollViewer Width=200 Margin="0,10,10,10"
    {
        (void)RC_RIGHT;
        int tx = col1.left + (COL_W - 200) / 2;
        if (tx < col1.left + 2) tx = col1.left + 2;
        // 官方首行是加粗的「今日关卡小提示:」，右侧挂 TooltipBlock（? 图标，半透明）
        gui_.text(tx, TIP_TOP + 4, L"\u4eca\u65e5\u5173\u5361\u5c0f\u63d0\u793a:",
                  gui_.theme.text, gui_.font_bold);
        RECT hint_r{ tx + 186, TIP_TOP + 4, tx + 200, TIP_TOP + 18 };
        gui_.round_rect(hint_r, gui_.theme.card_bg, gui_.theme.faint, 7);
        gui_.text(hint_r.left + 4, hint_r.top + 1, L"?", gui_.theme.faint, gui_.font_small);
        RECT stage_box{ tx + 12, TIP_TOP + 24, tx + 200, TIP_TOP + TIP_H - 4 };
        gui_.text_clipped(stage_box,
                          L"\u4eca\u65e5\u5f00\u653e\u5173\u5361\uff1a\u5173\u95ed\u4e2d",
                          gui_.theme.trace_text, gui_.font_small);
    }

    // ---------------- col0 底部（官方 Grid.Row=1 / Row=2）----------------
    //   Row1 = 「完成后」：ActionTitle + Divider + ActionDescription，右侧齿轮 RadioButton
    //   Row2 = Link Start! / 停止（官方同一时刻只可见一个，Width=100 Height=50 Margin=5，居中）
    //          其下 = 自动重载资源（CanShowAutoReload 时可见）
    bool busy = connected && hub_->executor_status(id, 0).get_bool("busy", false);

    // Grid.Row=1：完成后（官方 StackPanel Width=210 = 内层 185 文本 + 右侧齿轮）
    {
        int py = left_card.bottom;
        int txt_w = 185;                    // 官方内层 StackPanel Width=185
        if (txt_w > LEFT_CARD_W - 25) txt_w = LEFT_CARD_W - 25;
        std::wstring post_title = L"\u5b8c\u6210\u540e";
        int tw = gui_.text_size(post_title, gui_.font).cx;
        gui_.text(c0x + 20 + (txt_w - 20 - tw) / 2, py + 15, post_title,
                  gui_.theme.text, gui_.font);
        gui_.divider({ c0x + 10, py + 39, c0x + txt_w - 10, py + 40 });
        gui_.text_clipped({ c0x + (txt_w - 160) / 2, py + 47,
                            c0x + (txt_w + 160) / 2, py + POST_H },
                          post_action_desc(id), gui_.theme.dim, gui_.font_small);
        // 齿轮 RadioButton：Background=Transparent、BorderThickness=0、
        // Foreground=SecondaryTextBrush，高亮色 = Primary（打开完成后设置浮层）
        RECT gear{ c0x + txt_w + 4, py + (POST_H - 20) / 2,
                   c0x + txt_w + 24, py + (POST_H + 20) / 2 };
        bool gear_hov = gui_.hovered(gear);
        gui_.icon_gear(gear, (show_post_action_ || gear_hov) ? gui_.theme.primary
                                                             : gui_.theme.dim);
        if (gear_hov && gui_.input().pressed) show_post_action_ = !show_post_action_;
    }

    // Grid.Row=2：Link Start! / 停止 —— 官方可见性互斥，同一时刻只画一个 100x50
    {
        const int BW = 100, BH = 50;
        int by = left_card.bottom + POST_H;
        int bx = c0x + (LEFT_CARD_W - BW) / 2;
        RECT main_btn{ bx, by + 5, bx + BW, by + 5 + BH };
        if (busy) {
            if (gui_.button("btn-stop-exec", main_btn, L"\u505c\u6b62", 0)) {
                post_action([this, id] { hub_->stop_executor(id); }, "\u505c\u6b62");
            }
        } else {
            // 官方 LinkStart = MaaButtonDefaultStyle（RegionBrushOpacity25 白底 +
            // PrimaryTextBrush 前景）、IconElement.Geometry=x:Null —— 不是蓝色按钮
            if (gui_.button("btn-linkstart-exec", main_btn, L"Link Start!", 0)) {
                post_action([this, id] {
                    mj::Value st = hub_->executor_status(id);
                    if (st.get_str("conn", "") != "connected") hub_->connect_executor(id);
                    action_run_checked(id);
                }, "\u94fe\u63a5\u5f00\u59cb");
            }
        }
        // 自动重载资源（该行 row=1，仅在 CanShowAutoReload 时出现）
        if (connected) {
            std::wstring ar_label = L"\u81ea\u52a8\u91cd\u8f7d\u8d44\u6e90";
            int ar_w = 24 + gui_.text_size(ar_label, gui_.font).cx;
            int ar_x = c0x + (LEFT_CARD_W - ar_w) / 2;
            CheckState st = auto_reload_ ? CheckState::On : CheckState::Off;
            gui_.checkbox3("auto-reload",
                           { ar_x, by + START_H, ar_x + ar_w, by + START_H + RELOAD_H },
                           ar_label, st);
            auto_reload_ = (st == CheckState::On);
        }
    }

    // ---------------- col2：日志流（官方 Grid.Column="2"）----------------
    // 顶部两行压缩实例信息（连接/核心 · 任务数/账号）+ 连接或断开（MaaHub 多实例扩展）
    {
        RECT head{ log_col.left, log_col.top, log_col.right, log_col.top + 36 };
        COLORREF dot = connected ? gui_.theme.ok :
                       conn == "failed" ? gui_.theme.err :
                       conn == "connecting" ? gui_.theme.warn : gui_.theme.faint;
        gui_.round_rect({ head.left + 1, head.top + 10, head.left + 11, head.top + 20 },
                        dot, dot, 5);
        std::wstring acc_txt = acc_label.empty() ? std::wstring(L"\u672a\u7ed1")
                                                 : gui::widen(acc_label);
        std::wstring l1 = connected_text(conn) + L" \xb7 \u6838\u5fc3 "
            + gui::widen(core_version.empty() ? "-" : core_version);
        std::wstring l2 = L"\u4efb\u52a1 " + std::to_wstring(task_count)
            + L" \xb7 \u8d26\u53f7 " + acc_txt;
        RECT conn_btn{ head.right - 60, head.top + 7, head.right - 4, head.top + 27 };
        if (gui_.button("btn-connect-exec", conn_btn,
                        connected ? L"\u65ad\u5f00" : L"\u8fde\u63a5", 0)) {
            if (connected) action_disconnect(id);
            else action_connect(id);
        }
        int tw = conn_btn.left - 8 - (head.left + 18);
        gui_.text(head.left + 18, head.top + 2, gui_.ellipsize(l1, tw, gui_.font_small),
                  gui_.theme.trace_text, gui_.font_small);
        gui_.text(head.left + 18, head.top + 19, gui_.ellipsize(l2, tw, gui_.font_small),
                  gui_.theme.trace_text, gui_.font_small);
        RECT feed{ log_col.left, head.bottom + 2, log_col.right, log_col.bottom };
        draw_log_column(feed, id);
    }

    // ---------------- 浮层（最上层）----------------
    gui_.flush_combo_popup();   // 下拉层要盖住本帧后续控件，但仍在模态浮层之下
    if (show_add_task_) draw_add_task_overlay(frame, id, kind);
    if (show_post_action_) draw_post_action(frame, id);
    draw_task_ctx_menu();
    draw_rename_box(frame, id);
}


// ================================================================ 账号管理
void MaaHubWindow::page_accounts(RECT content) {
    mj::Value snap;
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        snap = snap_;
    }
    // 表单两行：窄窗口下「＋ 添加账号」不能被挤出右缘
    RECT form{ content.left, content.top, content.right, content.top + 80 };
    gui_.panel(form);
    int fx = form.left + 16;
    gui_.text_input("acc-label", { fx, form.top + 10, fx + 190, form.top + 36 },
                    form_label_, L"备注名，如：官服大号");
    gui_.text_input("acc-ident", { fx + 200, form.top + 10, form.right - 16, form.top + 36 },
                    form_ident_, L"账号标识：MAA 登录名片段 / MaaEnd 邮箱或后四位");
    int fy = form.top + 44;
    int gi = gui_.option_strip("acc-form-game", { fx, fy, fx + 180, fy + 26 },
                               { L"明日方舟 (MAA)", L"终末地 (MaaEnd)" }, acc_form_game_);
    if (gi >= 0) acc_form_game_ = gi;
    fx += 190;
    int mi = gui_.option_strip("acc-form-match", { fx, fy, fx + 190, fy + 26 },
                               { L"后四位匹配", L"邮箱匹配" }, acc_form_match_);
    if (mi >= 0) acc_form_match_ = mi;
    fx += 200;
    RECT ab{ fx, fy, fx + 110, fy + 26 };
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

    RECT panel{ content.left, content.top + 92, content.right, content.bottom };
    gui_.panel(panel, L"👥 账号列表");
    int ty = panel.top + 38;
    int pw = panel.right - panel.left;
    int cols[] = { panel.left + pw * 2 / 100,  panel.left + pw * 12 / 100,
                   panel.left + pw * 19 / 100, panel.left + pw * 38 / 100,
                   panel.left + pw * 49 / 100, panel.left + pw * 55 / 100,
                   panel.left + pw * 64 / 100, panel.left + pw * 73 / 100 };
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
        post_action([this] { action_check_update(""); }, "检查更新");
    }
    bx += 130;
    if (gui_.button("core-pull-maa", { bx, bar.top + 8, bx + 180, bar.top + 32 },
                    L"⬇ 更新 MAA 核心", 1, dl_progress_.load() < 0)) {
        post_action([this] { action_auto_update("maa"); }, "更新 MAA");
    }
    bx += 190;
    if (gui_.button("core-pull-end", { bx, bar.top + 8, bx + 180, bar.top + 32 },
                    L"⬇ 更新 MaaEnd 核心", 1, dl_progress_.load() < 0)) {
        post_action([this] { action_auto_update("maaend"); }, "更新 MaaEnd");
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
    bool has_upd = false;
    UpdateInfo upd;
    {
        std::lock_guard<std::mutex> lk(upd_mtx_);
        has_upd = has_update_info_;
        upd = update_info_;
    }
    if (has_upd) {
        RECT up{ content.left, y, content.right, y + 150 };
        gui_.panel(up, L"🔄 更新检查");
        int uy = up.top + 32;
        gui_.text(up.left + 18, uy + 2, L"当前:", gui_.theme.dim, gui_.font_small);
        gui_.text(up.left + 64, uy, gui::widen(upd.current.empty() ? "-" : upd.current),
                  gui_.theme.text, gui_.font);
        gui_.text(up.left + 320, uy + 2, L"上游最新:", gui_.theme.dim, gui_.font_small);
        gui_.text(up.left + 396, uy, gui::widen(upd.latest.empty() ? "-" : upd.latest),
                  upd.has_update ? gui_.theme.ok : gui_.theme.dim, gui_.font);
        gui_.text(up.left + 540, uy + 2, L"来源:", gui_.theme.dim, gui_.font_small);
        gui_.text(up.left + 588, uy + 2, gui::widen(upd.source_name), gui_.theme.dim, gui_.font_small);
        if (upd.asset_size > 0) {
            wchar_t sz[64];
            swprintf_s(sz, L"%.1f MB", (double)upd.asset_size / (1024.0 * 1024.0));
            gui_.text(up.left + 660, uy + 2, sz, gui_.theme.dim, gui_.font_small);
        }
        // 当前 tag 单独一行（小字），不与下面的动作行重叠
        if (!upd.current_tag.empty()) {
            gui_.text(up.left + 64, uy + 20, L"tag " + gui::widen(upd.current_tag),
                      gui_.theme.dim, gui_.font_small);
        }
        int ay = up.top + 74;
        if (upd.ok && upd.has_update && !upd.download_url.empty()) {
            RECT pb{ up.left + 18, ay, up.left + 200, ay + 26 };
            if (gui_.button("core-pull", pb, L"⬇ 下载并安装", 1, dl_progress_.load() < 0)) {
                std::string kind = upd.kind;
                post_action([this, kind] { action_auto_update(kind); }, "下载并安装");
            }
            RECT save = gui_.clip();
            gui_.text_clipped({ up.left + 210, ay + 5, up.right - 16, ay + 23 },
                              L"资产: " + gui::widen(upd.asset_name)
                              + (upd.asset_sha256.empty() ? L"（上游无 sha256）"
                                                          : L"（sha256 校验）"),
                              gui_.theme.dim, gui_.font_small);
            (void)save;
        } else if (upd.ok) {
            gui_.text(up.left + 18, ay + 4, L"已是最新版本", gui_.theme.ok, gui_.font);
        } else {
            RECT save = gui_.clip();
            gui_.text_clipped({ up.left + 18, ay + 4, up.right - 16, ay + 24 },
                              gui::widen(upd.error), gui_.theme.err, gui_.font_small);
            (void)save;
        }
        if (upd.tag_mismatch) {
            RECT save2 = gui_.clip();
            gui_.text_clipped({ up.left + 18, up.top + 104, up.right - 16, up.top + 122 },
                              L"⚠ 当前核心未记录来源 tag，版本口径与上游不同（探测值 "
                              + gui::widen(upd.current) + L"），下载后按包内真实版本比对",
                              gui_.theme.warn, gui_.font_small);
            (void)save2;
        }
        // 下载/解压进度
        int dp = dl_progress_.load();
        if (dp >= 0) {
            std::wstring line = gui::widen(dl_stage_) + L"  " + std::to_wstring(dp) + L"%";
            gui_.text(up.left + 18, up.bottom - 26, line.c_str(), gui_.theme.primary, gui_.font_small);
        }
        y = up.bottom + 12;
    }

    RECT panel{ content.left, y, content.right, content.bottom };
    gui_.panel(panel, L"📦 已登记核心");
    int ty = panel.top + 38;
    // 列宽按面板实际宽度排：右侧三个按钮组右对齐，其余列向左均分，
    // 避免在默认窗口宽度下「操作」列被挤出可视区（旧布局固定偏移会溢出）
    const int row_gap = 6, bw_ck = 66, bw_def = 58, bw_del = 52;
    const int btn_r = panel.right - 12;
    const int ck_x = btn_r - bw_del - row_gap - bw_def - row_gap - bw_ck;
    const int def_x = btn_r - bw_del - row_gap - bw_def;
    const int del_x = btn_r - bw_del;
    const int c_type = panel.left + 16;
    const int c_name = c_type + 48;
    const int c_ver = c_name + 152;
    const int c_src = c_ver + 96;
    const int c_path = c_src + 76;
    const int c_def = ck_x - 72;
    gui_.text(c_type, ty, L"类型", gui_.theme.dim, gui_.font_small);
    gui_.text(c_name, ty, L"名称", gui_.theme.dim, gui_.font_small);
    gui_.text(c_ver, ty, L"版本", gui_.theme.dim, gui_.font_small);
    gui_.text(c_src, ty, L"来源", gui_.theme.dim, gui_.font_small);
    gui_.text(c_path, ty, L"路径", gui_.theme.dim, gui_.font_small);
    gui_.text(c_def, ty, L"默认", gui_.theme.dim, gui_.font_small);
    gui_.text(ck_x, ty, L"操作", gui_.theme.dim, gui_.font_small);
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
        gui_.text(c_type, ry + 6, kind == "maa" ? L"MAA" : L"MaaEnd", gui_.theme.primary, gui_.font_small);
        {
            RECT save = gui_.clip();
            gui_.text_clipped({ c_name, ry + 6, c_ver - 10, ry + 26 },
                              gui::widen(c.get_str("name", "")), gui_.theme.text, gui_.font_small);
            (void)save;
        }
        gui_.text(c_ver, ry + 6, gui::widen(c.get_str("version", "")), gui_.theme.text, gui_.font_small);
        gui_.text(c_src, ry + 6, gui::widen(c.get_str("source", "")), gui_.theme.dim, gui_.font_small);
        {
            RECT save = gui_.clip();
            gui_.text_clipped({ c_path, ry + 6, c_def - 10, ry + 26 },
                              gui::widen(c.get_str("path", "")), gui_.theme.dim, gui_.font_small);
            (void)save;
        }
        gui_.text(c_def, ry + 6, is_def ? L"✓ 默认" : L"", gui_.theme.ok, gui_.font_small);
        if (gui_.button(("core-ck-" + id).c_str(), { ck_x, ry + 2, ck_x + bw_ck, ry + 26 },
                        L"检查", 0)) {
            post_action([this, id] { action_check_update(id); }, "检查更新");
        }
        RECT b2{ def_x, ry + 2, def_x + bw_def, ry + 26 };
        RECT b3{ del_x, ry + 2, del_x + bw_del, ry + 26 };
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
    int pw = hpanel.right - hpanel.left;
    int cols[] = { hpanel.left + pw * 2 / 100, hpanel.left + pw * 22 / 100,
                   hpanel.left + pw * 38 / 100, hpanel.left + pw * 62 / 100,
                   hpanel.left + pw * 86 / 100 };
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

// 测试用：强制打开「新建执行器」浮层
void MaaHubWindow::debug_open_new_exec() {
    show_new_exec_ = true;
    new_kind_ = 0;
    new_acc_idx_ = 0;
    fprintf(stderr, "[render] new-exec overlay opened\n");
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
        // ---- 自动更新：启动检查 + 定时检查（对齐 MAA CheckOnStartup / CheckOnSchedule）
        // 二者都在 backend 线程执行，不占 UI 线程。
        {
            AppSettings st = hub_->app_settings();
            double now = util::now_sec();
            bool due = false;
            if (!startup_checked_) {
                startup_checked_ = true;
                if (st.update_check_on_start) due = true;
            } else if (st.update_check_on_schedule && last_update_check_ > 0
                       && now - last_update_check_ > 6 * 3600.0) {
                due = true;
            }
            if (due && !update_checking_.exchange(true)) {
                std::string auto_kind = "maaend";
                CoreInfo c = hub_->cores().default_core(auto_kind);
                if (c.id.empty()) {
                    auto_kind = "maa";
                    c = hub_->cores().default_core("maa");
                }
                UpdateInfo ui = CoreManager::check_update(auto_kind, c.version, c.release_tag, &st);
                ui.kind = auto_kind;
                {
                    std::lock_guard<std::mutex> lk(upd_mtx_);
                    update_info_ = ui;
                    has_update_info_ = true;
                }
                last_update_check_ = now;
                update_checking_ = false;
                if (ui.ok && ui.has_update) {
                    if (st.update_auto_download && !ui.download_url.empty()) {
                        set_notice("发现新版本 " + ui.latest + "，自动下载中…");
                        dl_progress_ = 0;
                        dl_stage_ = "下载";
                        std::string installed;
                        bool unchanged = false;
                        std::string id = CoreManager::pull_core(
                            auto_kind, ui.download_url, "", &dl_progress_, &dl_stage_, &st,
                            ui.latest_tag, st.update_auto_install, ui.asset_sha256,
                            &installed, &unchanged);
                        dl_progress_ = -1;
                        if (!id.empty()) {
                            std::lock_guard<std::mutex> lk(upd_mtx_);
                            update_info_.has_update = false;
                        }
                        set_notice(id.empty() ? ("自动更新失败：" + dl_stage_)
                                              : ("已自动更新到 " + installed));
                    } else {
                        set_notice("发现新版本 " + ui.latest + "（自动下载未开启）");
                    }
                    PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);
                }
            }
        }
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
    if (label.empty()) {
        // 留空时按同类实例数顺延编号，避免出现两个同名执行器
        int n = 1;
        for (auto& e : hub_->executors()) {
            if (e.kind == kind) ++n;
        }
        label = (kind == "maa" ? "MAA " : "MaaEnd ") + std::to_string(n);
    }
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
        page_ = "executor";
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

// 注意：以下更新类 action 全部在 backend 线程执行（UI 侧用 post_action 入队），
// 检查最长 5×20s、下载 ~300MB 都不会阻塞界面。
void MaaHubWindow::action_check_update(const std::string& core_id) {
    std::string kind = "maa";
    std::string current, current_tag;
    auto pick = [&](const CoreInfo& c) {
        if (c.id.empty()) return false;
        kind = c.kind;
        current = c.version;
        current_tag = c.release_tag;
        return true;
    };
    if (!core_id.empty()) {
        CoreInfo c;
        if (hub_->cores().get(core_id, c)) pick(c);
    } else {
        // 未指定时看已登记的默认核心（MaaEnd 优先，主用）
        CoreInfo ce = hub_->cores().default_core("maaend");
        CoreInfo cm = hub_->cores().default_core("maa");
        if (!pick(ce)) pick(cm);
    }
    AppSettings st = hub_->app_settings();
    UpdateInfo ui = CoreManager::check_update(kind, current, current_tag, &st);
    ui.kind = kind;
    {
        std::lock_guard<std::mutex> lk(upd_mtx_);
        update_info_ = ui;
        has_update_info_ = true;
    }
    last_update_check_ = util::now_sec();
    if (ui.ok) {
        std::string msg = ui.has_update ? ("发现新版本 " + ui.latest)
                                        : ("已是最新版本 " + ui.latest);
        if (ui.tag_mismatch) msg += "（版本口径不同：当前 " + current + "）";
        set_notice(msg);
    } else {
        set_notice("更新检查失败：" + ui.error);
    }
}

void MaaHubWindow::action_pull_core(const std::string& kind, const std::string& url) {
    UpdateInfo ui;
    {
        std::lock_guard<std::mutex> lk(upd_mtx_);
        ui = update_info_;
    }
    std::string real_url = url.empty() ? ui.download_url : url;
    if (real_url.empty()) {
        set_notice("没有可用的下载地址");
        return;
    }
    dl_progress_ = 0;
    dl_stage_ = "下载";
    AppSettings st = hub_->app_settings();
    std::string installed;
    bool unchanged = false;
    std::string id = CoreManager::pull_core(kind, real_url, "", &dl_progress_, &dl_stage_, &st,
                                            ui.latest_tag, /*set_default=*/true,
                                            ui.asset_sha256, &installed, &unchanged);
    dl_progress_ = -1;
    if (id.empty()) {
        set_notice("更新失败：" + dl_stage_ + "（请检查网络/镜像源）");
        return;
    }
    CoreInfo c;
    hub_->cores().get(id, c);
    {
        std::lock_guard<std::mutex> lk(upd_mtx_);
        update_info_.has_update = false;
    }
    set_notice(unchanged ? ("已是最新核心（" + c.version + "），已设为默认")
                         : ("已安装 " + c.name + " " + c.version + " 并设为默认"));
}

void MaaHubWindow::action_pull_latest(const std::string& kind) {
    action_auto_update(kind);
}

// 一键更新：检查 → 下载 → 校验 sha256 → 安装 → 设为默认（对齐 MAA 的自动更新链路）
void MaaHubWindow::action_auto_update(const std::string& kind) {
    CoreInfo c = hub_->cores().default_core(kind);
    AppSettings st = hub_->app_settings();
    UpdateInfo ui = CoreManager::check_update(kind, c.version, c.release_tag, &st);
    ui.kind = kind;
    {
        std::lock_guard<std::mutex> lk(upd_mtx_);
        update_info_ = ui;
        has_update_info_ = true;
    }
    last_update_check_ = util::now_sec();
    if (!ui.ok) {
        set_notice("更新检查失败：" + ui.error);
        return;
    }
    if (!ui.has_update) {
        set_notice((kind == "maa" ? std::string("MAA ") : std::string("MaaEnd "))
                   + "已是最新版本 " + ui.latest);
        return;
    }
    if (ui.download_url.empty()) {
        set_notice("未找到可用的下载地址");
        return;
    }
    set_notice("开始下载 " + ui.latest + " …");
    dl_progress_ = 0;
    dl_stage_ = "下载";
    std::string installed;
    bool unchanged = false;
    std::string id = CoreManager::pull_core(kind, ui.download_url, "", &dl_progress_, &dl_stage_,
                                            &st, ui.latest_tag, /*set_default=*/true,
                                            ui.asset_sha256, &installed, &unchanged);
    dl_progress_ = -1;
    if (id.empty()) {
        set_notice("更新失败：" + dl_stage_);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(upd_mtx_);
        update_info_.has_update = false;
    }
    set_notice(unchanged ? ("已是最新核心（" + installed + "），已设为默认")
                         : ("已更新到 " + installed + " 并设为默认"));
}

// ================================================================ 设置页
void MaaHubWindow::page_settings(RECT content) {
    // 首次进入从快照回填（官方 SettingsView 多组设置）。
    // 快照还没到（后端线程未产出第一份 snapshot）时保持 settings_loaded_=false，
    // 下一帧重试——避免用空快照回填把默认值冲掉。
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
            // 启动 / 更新复选框也只在回填时赋值一次；放在每帧会覆盖用户点击
            set_auto_start_   = st.auto_start;
            set_start_min_    = st.start_minimized;
            set_start_run_    = st.start_run;
            set_upd_check_    = st.update_check_on_start;
            set_upd_sched_    = st.update_check_on_schedule;
            set_upd_dl_       = st.update_auto_download;
            set_upd_install_  = st.update_auto_install;
            set_upd_channel_  = st.update_version_type == "beta" ? 1
                              : st.update_version_type == "nightly" ? 2 : 0;
            set_upd_source_   = st.update_source == "github" ? 1
                              : st.update_source == "mirrorchyan" ? 2 : 0;
            set_gh_token_     = st.github_token;
            settings_loaded_ = true;
        }
    }

    int y = content.top;
    auto group = [&](const wchar_t* title) {
        // 官方设置页是 HandyControl TitleElement：小节标题为普通加粗文字 + 分隔线，
        // 不是整块主色高亮条
        gui_.text(content.left, y + 2, title, gui_.theme.text, gui_.font_bold);
        y += 22;
        gui_.divider({ content.left, y, content.right, y + 1 });
        y += 12;
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
        // 值已在一次性回填里赋好；这里每帧重赋会覆盖用户勾选
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

    // ---------------- 更新设置（对齐 MAA 官方 Update 组）----------------
    group(L"\u66f4\u65b0\u8bbe\u7f6e");
    {
        // 值已在一次性回填里赋好；这里每帧重赋会覆盖用户勾选
        struct U { const char* key; const wchar_t* label; bool* val; };
        const U us[] = {
            { "set-upd-check",   L"\u542f\u52a8\u65f6\u68c0\u67e5\u66f4\u65b0", &set_upd_check_ },
            { "set-upd-sched",   L"\u5b9a\u65f6\u68c0\u67e5\uff08\u8fd0\u884c\u671f\u95f4\u6bcf 6 \u5c0f\u65f6\uff09", &set_upd_sched_ },
            { "set-upd-dl",      L"\u81ea\u52a8\u4e0b\u8f7d\u66f4\u65b0\u5305", &set_upd_dl_ },
            { "set-upd-install", L"\u81ea\u52a8\u5b89\u88c5\uff08\u4e0b\u8f7d\u540e\u8bbe\u4e3a\u9ed8\u8ba4\u6838\u5fc3\uff09", &set_upd_install_ },
        };
        for (auto& u : us) {
            CheckState st = *u.val ? CheckState::On : CheckState::Off;
            gui_.checkbox3(u.key, { content.left, y, content.left + 380, y + 22 },
                           u.label, st);
            *u.val = (st == CheckState::On);
            y += 28;
        }
        gui_.text(content.left, y + 4, L"\u66f4\u65b0\u6e20\u9053", gui_.theme.dim, gui_.font_small);
        gui_.option_strip("set-upd-channel", { content.left + 74, y, content.left + 374, y + 26 },
                          { L"\u6b63\u5f0f\u7248", L"\u6d4b\u8bd5\u7248", L"Nightly" }, set_upd_channel_);
        y += 30;
        gui_.text(content.left, y + 4, L"\u66f4\u65b0\u6e90", gui_.theme.dim, gui_.font_small);
        gui_.option_strip("set-upd-source", { content.left + 74, y, content.left + 374, y + 26 },
                          { L"\u81ea\u52a8", L"GitHub", L"Mirror\u9171" }, set_upd_source_);
        y += 30;
        gui_.text(content.left, y + 4, L"GitHub Token", gui_.theme.dim, gui_.font_small);
        gui_.text_input("set-gh-token", { content.left + 110, y, content.right, y + 26 },
                        set_gh_token_, L"ghp_...\uff08\u53ef\u9009\uff0c\u63d0\u9ad8 API \u9650\u989d\uff09");
        y += 30;
        note(L"\u66f4\u65b0\u94fe\u8def\uff1a\u68c0\u67e5 \u2192 \u4e0b\u8f7d \u2192 sha256 \u6821\u9a8c \u2192 \u89e3\u5305 \u2192 \u767b\u8bb0 \u2192 \u8bbe\u4e3a\u9ed8\u8ba4\u6838\u5fc3\u3002");
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
        RECT ib{ content.left + 156, y, content.left + 296, y + 30 };
        if (gui_.button("set-import", ib, L"\u5bfc\u5165\u914d\u7f6e...", 0)) {
            action_import_config();
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
    bool upd_check = set_upd_check_, upd_sched = set_upd_sched_, upd_dl = set_upd_dl_,
         upd_install = set_upd_install_;
    int upd_channel = set_upd_channel_, upd_source = set_upd_source_;
    std::string gh_token = set_gh_token_;
    post_action([this, mode, host, port, user, pass, cdk, enabled, url, key, model, theme,
                 auto_start, start_min, start_run,
                 upd_check, upd_sched, upd_dl, upd_install, upd_channel, upd_source, gh_token] {
        // update 返回真实落盘结果；LLM 配置保存后立即生效
        bool ok = hub_->settings().update([&](AppSettings& s) {
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
            s.update_check_on_schedule = upd_sched;
            s.update_auto_download = upd_dl;
            s.update_auto_install = upd_install;
            s.update_version_type = upd_channel == 1 ? "beta"
                                  : upd_channel == 2 ? "nightly" : "stable";
            s.update_source = upd_source == 1 ? "github"
                            : upd_source == 2 ? "mirrorchyan" : "auto";
            s.github_token = gh_token;
        });
        if (ok) {
            hub_->reload_llm_config();
            set_notice("设置已保存");
        } else {
            // 写盘失败通常因为 state 目录被占用/只读（杀软、同步盘）
            set_notice("设置保存失败：无法写入 " + util::exe_dir() + "\\state\\settings.json");
        }
    }, "保存设置");
}

void MaaHubWindow::action_import_config() {
    // 安装路径选择器（Vista IFileDialog FOS_PICKFOLDERS；也允许直接选配置文件）
    std::string path;
    {
        IFileDialog* dlg = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dlg));
        if (SUCCEEDED(hr)) {
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            // 只选文件夹；FOS_FILEMUSTEXIST 与 PICKFOLDERS 组合会禁止单选文件，去掉
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
            // 标题：选择 MAA / MaaEnd 安装目录（或直接选 gui.json / mxu-MaaEnd.json）
            dlg->SetTitle(L"\u9009\u62e9 MAA / MaaEnd \u5b89\u88c5\u76ee\u5f55"
                          L"\uff08\u6216\u76f4\u63a5\u9009 gui.json / mxu-MaaEnd.json\uff09");
            if (!import_path_.empty()) {
                std::wstring wpath = util::utf8_to_wide(import_path_);
                IShellItem* si = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(wpath.c_str(), nullptr,
                                                          IID_PPV_ARGS(&si)))) {
                    dlg->SetDefaultFolder(si);
                    si->Release();
                }
            }
            hr = dlg->Show(hwnd_);
            if (SUCCEEDED(hr)) {
                IShellItem* res = nullptr;
                if (SUCCEEDED(dlg->GetResult(&res))) {
                    PWSTR wpath = nullptr;
                    if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                        path = util::wide_to_utf8(wpath);
                        CoTaskMemFree(wpath);
                    }
                    res->Release();
                }
            }
            dlg->Release();
        }
        if (path.empty()) return;   // 用户取消或对话框失败
        import_path_ = path;        // 记住本次位置
    }

    HWND hwnd = hwnd_;
    post_action([this, hwnd, path] {
        // 路径解析（安装目录 -> config\xxx.json）+ 解析/写库全在 backend 线程
        mj::Value rep = import_config::import_from_path(path, *hub_);
        hub_->flush_task_params();   // 批量写入绕过 800ms 节流，防退出丢尾

        // 任务列表不直写盘：暂存到 import_task_lists_，由 UI 线程在
        // WM_APP_IMPORT 里合并进内存权威 task_list_ 后统一 save_task_list()
        {
            std::lock_guard<std::mutex> lk(import_mtx_);
            import_task_lists_ = rep.find("task_lists")
                                     ? *rep.find("task_lists")
                                     : mj::Value::object();
        }
        // 定时计划合并进 Hub 调度器（按 exec_id+source 去重，保留 last_run_date）
        if (auto* sch = rep.find("schedules"); sch && sch->is_array()
            && !sch->items().empty()) {
            hub_->merge_schedules(*sch);
        }
        PostMessageW(hwnd, WM_APP_IMPORT, 0, 0);

        set_notice(import_config::report_message(rep));
    }, "\u5bfc\u5165\u914d\u7f6e");
}

} // namespace hub
