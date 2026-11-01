// ============================================================================
// MaaHub 无窗口沙盒：滚动区底部剪裁验证
// ----------------------------------------------------------------------------
// 目的：给账号/核心/计划/事件列表灌入足够多的行，让列表真的滚到底，
//       然后用真实 on_paint() 离屏渲染，检查最后一行是否被裁掉一半。
//       全程不创建任何窗口。
//
// 判据：渲染结果里，最底部可见行应完整可见；若某行的按钮/文字在面板
//       底边被截断，即为缺陷。这里用「行底是否超过裁剪区底」做数值判定，
//       同时导出 PNG 供目检。
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#define private public
#include "app_window.cpp"
#undef private

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

#define LOGF(...) printf(__VA_ARGS__)

using hub::MaaHubWindow;

static void save_png(const std::wstring& path, HDC dc, int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    std::vector<uint8_t> buf((size_t)w * h * 4);
    GetDIBits(dc, (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP), 0, h, buf.data(), &bi, DIB_RGB_COLORS);
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = &buf[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + 2]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4]);
        }
    }
    std::vector<uint8_t> png = { 0x89,'P','N','G','\r','\n',0x1A,'\n' };
    uint8_t ihdr[13];
    auto put32 = [&](int off, uint32_t v) {
        ihdr[off] = v >> 24; ihdr[off+1] = v >> 16; ihdr[off+2] = v >> 8; ihdr[off+3] = v;
    };
    put32(0, (uint32_t)w); put32(4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
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
    z.push_back(adler >> 24); z.push_back(adler >> 16); z.push_back(adler >> 8); z.push_back(adler);
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

// 造 N 条账号
static mj::Value make_accounts(int n) {
    mj::Value arr = mj::Value::array();
    for (int i = 0; i < n; ++i) {
        mj::Value a = mj::Value::object();
        a.set("id", "acc-" + std::to_string(i));
        a.set("label", "账号" + std::to_string(i));
        a.set("game", i % 2 ? "maaend" : "maa");
        a.set("identifier", "138****" + std::to_string(1000 + i));
        a.set("match_mode", "LastFourDigits");
        a.set("enabled", true);
        a.set("is_active", i == 0);
        a.set("daily_done_date", "");
        mj::Value st = mj::Value::object();
        st.set("last_run_at", (int64_t)0);
        st.set("total_runs", i);
        a.set("state", std::move(st));
        arr.push(std::move(a));
    }
    return arr;
}

// 造一个执行器 + MAA 风格任务目录（含长描述，用于验证复选框标签裁剪）
static mj::Value make_executor_with_tasks(int n_tasks) {
    mj::Value ex = mj::Value::object();
    ex.set("id", "exec-A");
    ex.set("kind", "maa");
    ex.set("label", "官服大号");
    ex.set("conn", "connected");
    ex.set("run", "idle");
    ex.set("version", "6.13.0");
    ex.set("account_label", "官服大号");
    ex.set("core_name", "MAA");
    ex.set("core_version", "6.13.0");
    ex.set("daily_done_today", false);
    ex.set("task_count", n_tasks);
    ex.set("last_error", "");
    mj::Value cat = mj::Value::array();
    const char* names[] = { "StartUp", "Fight", "Infrast", "Recruit", "Mall",
                            "Award", "Roguelike", "ReclamationAlgorithm" };
    for (int i = 0; i < n_tasks; ++i) {
        mj::Value t = mj::Value::object();
        t.set("name", names[i % 8] + std::string(i / 8 ? std::to_string(i / 8) : ""));
        t.set("description", "这是一段很长的任务描述文本，用来验证复选框标签是否会被右侧按钮裁掉一部分文字内容");
        // 让前两个任务带参数，验证展开面板
        if (i < 2) {
            mj::Value ps = mj::Value::array();
            for (int k = 0; k < 3; ++k) {
                mj::Value p = mj::Value::object();
                p.set("name", "param_" + std::to_string(k));
                p.set("type", "string");
                p.set("description", "参数说明");
                p.set("required", k == 0);
                ps.push(std::move(p));
            }
            t.set("params", std::move(ps));
        }
        cat.push(std::move(t));
    }
    ex.set("catalog", std::move(cat));
    mj::Value arr = mj::Value::array();
    arr.push(std::move(ex));
    return arr;
}

int main() {
    SetProcessDPIAware();
    int w = 1500, h = 950;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    MaaHubWindow win(hInst, mj::Value::object());
    win.gui_.init_fonts();

    mj::Value snap = mj::Value::object();
    snap.set("accounts", make_accounts(40));
    snap.set("cores", mj::Value::array());
    snap.set("executors", make_executor_with_tasks(24));
    snap.set("plans", mj::Value::array());
    snap.set("events", mj::Value::array());
    win.snap_ = snap;
    win.snap_version_ = 1;
    win.page_ = "accounts";
    win.selected_exec_ = "exec-A";

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
    save_png(L"scroll-accounts.png", dc, cw, ch);
    LOGF("rendered scroll-accounts.png (%dx%d), 40 accounts\n", cw, ch);

    // ---- 执行器任务页：全部展开（含参数面板），验证底部剔除 ----
    for (auto& kv : win.task_ui_["exec-A"]) kv.second.open = true;
    win.page_ = "executor";
    win.on_paint(dc, client);
    save_png(L"scroll-tasks-open.png", dc, cw, ch);
    LOGF("rendered scroll-tasks-open.png (%dx%d), 24 tasks all expanded\n", cw, ch);

    // ---- 执行器任务页：全部收起，验证按钮与复选框标签 ----
    for (auto& kv : win.task_ui_["exec-A"]) kv.second.open = false;
    win.on_paint(dc, client);
    save_png(L"scroll-tasks.png", dc, cw, ch);
    LOGF("rendered scroll-tasks.png (%dx%d), 24 tasks collapsed\n", cw, ch);

    DeleteObject(bmp);
    DeleteDC(dc);
    LOGF("DONE (no window created)\n");
    return 0;
}
