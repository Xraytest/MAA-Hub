// ============================================================================
// MaaHub 无窗口沙盒渲染验证（Sandbox render harness）
// ----------------------------------------------------------------------------
// 目的：把真实的 MaaHubWindow::on_paint() 画进内存 DC，逐页导出 PNG，
//       用于在**不创建任何窗口**的前提下人工目检按钮文本是否被剪裁。
//
// 手法：
//   * `#define private public` 只为访问 on_paint()/page_*()，仅作用于本 TU
//   * 直接 include app_window.cpp（拿到全部实现的符号，避免重复链接）
//   * Hub 用真实 config；执行器/账号从 build\state 读真实持久化数据
//   * 全程只碰 CreateCompatibleDC，绝不 CreateWindow / ShowWindow
//
// 用法：render_probe.exe [page ...]      page ∈ welcome/executor/accounts/
//                                         cores/planner/events/settings
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#define private public          // 仅本文件：访问 MaaHubWindow 私有绘制函数
#include "app_window.cpp"
#undef private

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

#define LOGF(...) printf(__VA_ARGS__)

using hub::MaaHubWindow;

// ---------------------------------------------------------------- PNG 写出
static void save_png(const std::wstring& path, HDC dc, int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    std::vector<uint8_t> buf((size_t)w * h * 4);
    GetDIBits(dc, (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP), 0, h, buf.data(), &bi, DIB_RGB_COLORS);
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = &buf[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + 2]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]);
        }
    }
    std::vector<uint8_t> png = { 0x89,'P','N','G','\r','\n',0x1A,'\n' };
    uint8_t ihdr[13];
    auto put32 = [&](int off, uint32_t v) {
        ihdr[off] = v >> 24; ihdr[off+1] = v >> 16; ihdr[off+2] = v >> 8; ihdr[off+3] = v;
    };
    put32(0, (uint32_t)w); put32(4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    // zlib stored blocks
    std::vector<uint8_t> z{ 0x78, 0x01 };
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        bool final = (pos + n >= raw.size());
        z.push_back(final ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back((n >> 8) & 0xFF);
        uint16_t inv = (uint16_t)~(uint16_t)n;
        z.push_back(inv & 0xFF); z.push_back((inv >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    uint32_t adler = (s2 << 16) | s1;
    z.push_back(adler >> 24); z.push_back(adler >> 16);
    z.push_back(adler >> 8);  z.push_back(adler);

    auto emit = [&](const char* tag, const std::vector<uint8_t>& d) {
        auto p32 = [&](uint32_t v) {
            png.push_back(v >> 24); png.push_back(v >> 16);
            png.push_back(v >> 8);  png.push_back(v);
        };
        p32((uint32_t)d.size());
        std::vector<uint8_t> td(tag, tag + 4);
        td.insert(td.end(), d.begin(), d.end());
        png.insert(png.end(), td.begin(), td.end());
        uint32_t c = 0xFFFFFFFFu;
        for (uint8_t b : td) {
            c ^= b;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        }
        p32(~c);
    };
    emit("IHDR", std::vector<uint8_t>(ihdr, ihdr + 13));
    emit("IDAT", z);
    emit("IEND", {});
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f) { fwrite(png.data(), 1, png.size(), f); fclose(f); }
}

int main(int argc, char** argv) {
    SetProcessDPIAware();

    // 与 create() 相同的窗口尺寸（只取 client 尺寸）
    int w = 1500, h = 950;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

    // 真实配置 + 真实 Hub（从 build\state 读持久化数据）
    std::string cfg;
    util::read_file(util::exe_dir() + "\\config.json", cfg);
    mj::Value config;
    if (cfg.empty() || !mj::try_parse(cfg, config)) config = mj::Value::object();

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    MaaHubWindow win(hInst, config);
    win.gui_.init_fonts();

    // 灌入一份快照，让各页有数据可画（账号/核心/执行器/计划/事件）
    mj::Value snap = mj::Value::object();
    snap.set("accounts", mj::Value::array());
    snap.set("cores", mj::Value::array());
    snap.set("executors", mj::Value::array());
    snap.set("plans", mj::Value::array());
    snap.set("events", mj::Value::array());
    win.snap_ = snap;
    win.snap_version_ = 1;

    std::vector<std::string> pages;
    if (argc > 1) for (int i = 1; i < argc; ++i) pages.push_back(argv[i]);
    else pages = { "welcome", "accounts", "cores", "planner", "events", "settings" };

    for (auto& pg : pages) {
        win.page_ = pg;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(dc, bmp);

        RECT client{ 0, 0, cw, ch };
        win.on_paint(dc, client);

        std::wstring out = L"page-" + gui::widen(pg) + L".png";
        save_png(out, dc, cw, ch);
        LOGF("rendered %s (%dx%d)\n", pg.c_str(), cw, ch);

        DeleteObject(bmp);
        DeleteDC(dc);
    }
    LOGF("DONE (no window created)\n");
    return 0;
}
