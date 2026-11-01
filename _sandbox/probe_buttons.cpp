// ============================================================================
// MaaHub 无窗口沙盒探针（Sandbox probe）
// ----------------------------------------------------------------------------
// 目的：在不创建任何窗口的前提下，用与 app_window.cpp 完全一致的几何常量
//       复现全部按钮/滚动区绘制点，量化「按钮文本被剪裁」的真实程度。
//
// 做法：
//   1. 用 CreateCompatibleDC 建内存 DC（无窗口、无可见界面）
//   2. 复用 src/gui.hpp 的 Gui（同一套字体/测量/绘制代码）
//   3. 按 on_paint() 的同样公式算出 client / sidebar / topbar / content
//   4. 逐条复现 app_window.cpp 中 26 处 gui_.button(...) 的 RECT 与标签
//   5. 对每处报告：标签宽 vs 按钮宽、被当前 clip 实际裁掉的像素数
//   6. 复现 5 处 begin_scroll 的行剔除边界，报告底部溢出像素
//   7. 额外把布局画进 PNG，便于人工目检
//
// 用法：probe.exe [client_width] [client_height]
// 输出：结果写入 out\probe_report.txt（本程序为 GUI 子系统，无控制台、无窗口）
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX          // windows.h 的 min/max 宏会破坏 std::min(...)
#endif
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#define LOGF(...) printf(__VA_ARGS__)
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "../src/gui.hpp"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using gui::Gui;

// ---------------------------------------------------------------- 几何（与 app_window.cpp 一致）
struct Geom {
    int cw, ch;          // client
    RECT sidebar, topbar, content;
    int sidebar_w = 250, topbar_h = 56;
};

static Geom make_geom(int cw, int ch) {
    Geom g;
    g.cw = cw; g.ch = ch;
    g.sidebar  = { 0, 0, g.sidebar_w, ch };
    g.topbar   = { g.sidebar_w, 0, cw, g.topbar_h };
    g.content  = { g.sidebar_w + 20, g.topbar_h + 14, cw - 20, ch - 16 };
    return g;
}

// ---------------------------------------------------------------- 一条按钮样本
struct BtnSample {
    const char* site;      // app_window.cpp 中的调用点
    int line;              // 行号
    const wchar_t* label;  // 标签（取运行时可能出现的全部变体）
    RECT r;                // 按钮矩形
    RECT clip;             // 该调用点生效的裁剪区
    const char* note;
};

static std::vector<BtnSample> g_samples;
static int g_fail = 0;

static void add(const char* site, int line, const wchar_t* label, RECT r, RECT clip,
                const char* note = "") {
    g_samples.push_back({ site, line, label, r, clip, note });
}

// ---------------------------------------------------------------- 复现各页面按钮
static void collect_sidebar(const Geom& g) {
    RECT r = g.sidebar;
    // L214  new-exec：{r.right-70, y-4, r.right-12, y+18}, y = r.top+68（修复后 58px 宽）
    int y = r.top + 68;
    add("draw_sidebar/new-exec", 214, L"\uFF0B \u65B0\u5EFA",
        { r.right - 70, y - 4, r.right - 12, y + 18 }, g.sidebar, "侧栏标题行右侧");
    // L259  del-<id>：{ir.right-42, ir.top+10, ir.right-24, ir.top+30}（修复后 18px 宽）
    RECT ir{ r.left + 8, y, r.right - 8, y + 40 };
    add("draw_sidebar/del-exec", 259, L"\u2715",
        { ir.right - 42, ir.top + 10, ir.right - 24, ir.top + 30 }, g.sidebar, "执行器条目删除");
    // L302  new-ok
    int fy = y + 4;
    RECT form{ r.left + 8, fy, r.right - 8, fy + 148 };
    int yy = form.top + 8 + 30 + 30;   // kind + label + acc
    add("draw_sidebar/new-ok", 302, L"\u521B\u5EFA\u6267\u884C\u5668",
        { form.left + 8, yy, form.right - 8, yy + 26 }, g.sidebar, "新建执行器表单确认");
}

static void collect_topbar(const Geom& g) {
    RECT r = g.topbar;
    // L354 btn-connect：{r.right-320, r.top+10, r.right-240, r.bottom-10}
    add("draw_topbar/btn-connect", 354, L"\u65AD\u5F00",
        { r.right - 320, r.top + 10, r.right - 240, r.bottom - 10 }, g.topbar, "连接/断开");
    add("draw_topbar/btn-connect", 354, L"\u8FDE\u63A5",
        { r.right - 320, r.top + 10, r.right - 240, r.bottom - 10 }, g.topbar, "连接/断开");
    // L359 btn-linkstart：{r.right-228, r.top+10, r.right-20, r.bottom-10}
    add("draw_topbar/btn-linkstart", 359, L"\U0001F517 \u94FE\u63A5\u5F00\u59CB\uFF01",
        { r.right - 228, r.top + 10, r.right - 20, r.bottom - 10 }, g.topbar, "标志性大按钮");
}

static void collect_executor(const Geom& g) {
    RECT c = g.content;
    // L421-423 工具栏
    int ty = c.top + 64 + 12;   // info.bottom + 12
    add("page_executor/tb-run", 424, L"\u25B6 \u8FD0\u884C\u52FE\u9009",
        { c.left, ty, c.left + 130, ty + 30 }, c, "运行勾选");
    add("page_executor/tb-all", 425, L"\u5168\u9009",
        { c.left + 140, ty, c.left + 210, ty + 30 }, c, "全选");
    add("page_executor/tb-none", 428, L"\u5168\u4E0D\u9009",
        { c.left + 220, ty, c.left + 290, ty + 30 }, c, "全不选");
    // L459-466 任务行（item.right = content.right）
    RECT item{ c.left, ty, c.right, ty + 46 - 6 };
    add("page_executor/run-task", 460, L"\u25B6 \u7ACB\u5373\u8FD0\u884C",
        { item.right - 190, item.top + 8, item.right - 50, item.top + 32 }, c, "任务行立即运行");
    add("page_executor/gear", 466, L"\u2699",
        { item.right - 42, item.top + 8, item.right - 14, item.top + 32 }, c, "任务行齿轮");
}

static void collect_accounts(const Geom& g) {
    RECT c = g.content;
    RECT form{ c.left, c.top, c.right, c.top + 46 };
    int fx = form.left + 16 + 200 + 190 + 300 + 200;   // label+game+ident+match
    add("page_accounts/acc-add", 563, L"\uFF0B \u6DFB\u52A0\u8D26\u53F7",
        { fx, form.top + 10, fx + 110, form.top + 36 }, c, "添加账号");
    RECT panel{ c.left, c.top + 58, c.right, c.bottom };
    int cols7 = panel.left + 900;
    int ry = panel.top + 38 + 22 + 6;
    add("page_accounts/acc-toggle", 626, L"\u505C\u7528",
        { cols7, ry + 2, cols7 + 52, ry + 26 }, c, "账号行 停用/启用");
    add("page_accounts/acc-switch", 634, L"\u5207\u6362",
        { cols7 + 60, ry + 2, cols7 + 112, ry + 26 }, c, "账号行 切换");
    add("page_accounts/acc-del", 637, L"\u5220\u9664",
        { cols7 + 120, ry + 2, cols7 + 172, ry + 26 }, c, "账号行 删除");
}

static void collect_cores(const Geom& g) {
    RECT c = g.content;
    RECT bar{ c.left, c.top, c.right, c.top + 64 };
    int bx = bar.left + 14;
    add("page_cores/core-check", 659, L"\U0001F504 \u68C0\u67E5\u66F4\u65B0",
        { bx, bar.top + 8, bx + 120, bar.top + 32 }, c, "检查更新");
    bx += 130;
    add("page_cores/core-pull-maa", 663, L"\u2B07 \u62C9\u53D6 MAA \u6838\u5FC3",
        { bx, bar.top + 8, bx + 180, bar.top + 32 }, c, "拉取 MAA 核心");
    bx += 190;
    add("page_cores/core-pull-end", 668, L"\u2B07 \u62C9\u53D6 MaaEnd \u6838\u5FC3",
        { bx, bar.top + 8, bx + 180, bar.top + 32 }, c, "拉取 MaaEnd 核心");
    // L707 core-pull（更新检查面板内）
    int y = bar.bottom + 12;
    RECT up{ c.left, y, c.right, y + 96 };
    int uy = up.top + 38 + 28;
    add("page_cores/core-pull", 707, L"\u2B07 \u62C9\u53D6\u5E76\u5B89\u88C5",
        { up.left + 18, uy, up.left + 200, uy + 26 }, c, "拉取并安装");
    // 行内按钮
    RECT panel{ c.left, y, c.right, c.bottom };
    int cols6 = panel.left + 790, cols7 = panel.left + 890;
    int ry = panel.top + 38 + 22 + 6;
    add("page_cores/core-ck", 775, L"\u68C0\u67E5",
        { cols6, ry + 2, cols6 + 70, ry + 26 }, c, "核心行 检查");
    add("page_cores/core-def", 781, L"\u8BBE\u9ED8\u8BA4",
        { cols7, ry + 2, cols7 + 60, ry + 26 }, c, "核心行 设默认");
    add("page_cores/core-dl", 787, L"\u5220\u9664",
        { cols7 + 68, ry + 2, cols7 + 128, ry + 26 }, c, "核心行 删除");
}

static void collect_planner(const Geom& g) {
    RECT c = g.content;
    add("page_planner/btn-propose", 813, L"\u751F\u6210\u8BA1\u5212",
        { c.right - 160, c.top, c.right, c.top + 30 }, c, "生成计划");
    int y = c.top + 42;
    RECT panel{ c.left, y, c.right, y + 300 };
    add("page_planner/plan-exec", 878, L"\u2705 \u786E\u8BA4\u5E76\u6267\u884C",
        { panel.left + 18, panel.bottom - 38, panel.left + 138, panel.bottom - 10 }, c, "确认并执行");
    add("page_planner/plan-reject", 882, L"\u2716 \u62D2\u7EDD",
        { panel.left + 146, panel.bottom - 38, panel.left + 216, panel.bottom - 10 }, c, "拒绝");
    add("page_planner/plan-stop", 885, L"\u23F9 \u505C\u6B62\u6267\u884C",
        { panel.left + 224, panel.bottom - 38, panel.left + 314, panel.bottom - 10 }, c, "停止执行");
}

static void collect_settings(const Geom& g) {
    RECT c = g.content;
    int y = c.top + 150 + 14 + 96 + 14 + 150 + 18;
    add("page_settings/set-save", 1331, L"\U0001F4BE \u4FDD\u5B58\u8BBE\u7F6E",
        { c.left, y, c.left + 160, y + 34 }, c, "保存设置");
}

// ---------------------------------------------------------------- 滚动区竖向剪裁分析
struct ScrollCase {
    const char* name;
    int line;
    RECT clip;          // begin_scroll 传入的裁剪矩形
    int first_row_y;    // 第一行的 y（已减 scroll）
    int row_h;
    int rows;           // 行数
    int cull_limit;     // 代码里的 break 条件阈值
    int row_content_bottom_dy;  // 行内最底部内容相对行首的偏移
    const wchar_t* what;
};

static void analyze_scroll(const ScrollCase& sc, bool fixed_logic) {
    // fixed_logic=false 复现旧代码：if (ry > cull_limit) break;
    // fixed_logic=true  复现新代码：if (!row_fully_visible(ry, row_h)) break;
    int last = -1;
    for (int i = 0; i < sc.rows; ++i) {
        int ry = sc.first_row_y + i * sc.row_h;
        if (ry + sc.row_h < sc.clip.top) continue;
        if (fixed_logic) {
            if (ry < sc.clip.top || ry + sc.row_h > sc.clip.bottom) break;
        } else {
            if (ry > sc.cull_limit) break;
        }
        last = ry;
    }
    if (last < 0) { LOGF("    (无可绘制行)\n"); return; }
    int content_bottom = last + sc.row_content_bottom_dy;
    int clip_bottom = sc.clip.bottom;
    int over = content_bottom - clip_bottom;
    LOGF("    末行 y=%d 行内容底=%d  clip 底=%d  底部溢出=%+d px  %s%s\n",
           last, content_bottom, clip_bottom, over,
           over > 0 ? "  <== 被裁" : "",
           fixed_logic ? "   [新逻辑]" : "   [旧逻辑·仅作对照]");
    if (over > 0 && fixed_logic) ++g_fail;   // 只按修复后的逻辑判定缺陷
}

// ---------------------------------------------------------------- PNG 输出（无窗口）
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
    // 注意：IHDR 由下面的 emit("IHDR", ...) 统一打包，这里不能提前插入
    // zlib：stored deflate blocks（无需链接 zlib）
    size_t pos = 0;
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
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
    uint32_t adler = 1, s1 = 1, s2 = 0;
    for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    adler = (s2 << 16) | s1;
    z.push_back(adler >> 24); z.push_back(adler >> 16); z.push_back(adler >> 8); z.push_back(adler);

    auto emit = [&](const char* tag, const std::vector<uint8_t>& d) {
        uint32_t n = (uint32_t)d.size();
        auto p32 = [&](uint32_t v) {
            png.push_back(v >> 24); png.push_back(v >> 16);
            png.push_back(v >> 8);  png.push_back(v);
        };
        p32(n);
        std::vector<uint8_t> td(tag, tag + 4);
        td.insert(td.end(), d.begin(), d.end());
        png.insert(png.end(), td.begin(), td.end());
        // CRC32
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

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    SetProcessDPIAware();
    LOGF("MARK: main entered\n");

    // 与 MaaHubWindow::create() 完全相同的窗口尺寸推导（只取 client 尺寸）
    int w = 1500, h = 950;
    LOGF("MARK: before AdjustWindowRect\n");
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    LOGF("MARK: after AdjustWindowRect\n");
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (argc > 2) { cw = atoi(argv[1]); ch = atoi(argv[2]); }
    Geom g = make_geom(cw, ch);

    LOGF("== MaaHub 按钮文本剪裁探针（无窗口沙盒） ==\n");
    LOGF("client = %dx%d  sidebar_w=%d topbar_h=%d\n\n", cw, ch, g.sidebar_w, g.topbar_h);

    // ---- 内存 DC（不创建任何窗口）----
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    if (!dc) { LOGF("FATAL: CreateCompatibleDC failed\n"); return 2; }
    ReleaseDC(nullptr, screen);

    Gui gui;
    gui.init_fonts();
    RECT client{ 0, 0, cw, ch };
    gui.set_dc(dc, client);          // 必须先绑定 DC，否则 text_size 全部返回 0

    collect_sidebar(g);
    collect_topbar(g);
    collect_executor(g);
    collect_accounts(g);
    collect_cores(g);
    collect_planner(g);
    collect_settings(g);

    LOGF("---- 按钮文本适配（label 宽 vs 按钮宽）----\n");
    LOGF("%-34s %5s %6s %6s %7s  %s\n", "调用点", "行", "按钮宽", "文本宽", "差值", "结论");
    int overflow_cnt = 0;
    for (auto& s : g_samples) {
        int bw = s.r.right - s.r.left;
        SIZE sz = gui.text_size(s.label, gui.font);
        int diff = bw - sz.cx;
        const char* verdict = diff >= 0 ? "ok" : "OVERFLOW";
        if (diff < 0) ++overflow_cnt;
        LOGF("%-34s %5d %6d %6d %+7d  %-9s %s\n", s.site, s.line, bw, sz.cx, diff,
               verdict, s.note);
    }
    LOGF("\n按钮横向溢出总数: %d / %zu\n\n", overflow_cnt, g_samples.size());

    // ---- 实际绘制（验证 clip 实际裁掉多少）----
    LOGF("---- 实际绘制时的 clip 剪裁量 ----\n");
    for (auto& s : g_samples) {
        gui.set_dc(dc, client);
        // 复现 button() 的居中计算
        SIZE sz = gui.text_size(s.label, gui.font);
        int bw = s.r.right - s.r.left;
        int bh = s.r.bottom - s.r.top;
        int tx = s.r.left + (bw - sz.cx) / 2;
        int ty = s.r.top + (bh - sz.cy) / 2;
        // text() 用 ETO_CLIPPED + clip_；此处手动模拟 clip 与文字矩形的交集
        RECT inter;
        IntersectRect(&inter, &s.clip, &s.r);
        int vis_w = std::max<int>(0, std::min<int>(tx + sz.cx, (int)inter.right) - std::max<int>(tx, (int)inter.left));
        int vis_h = std::max<int>(0, std::min<int>(ty + sz.cy, (int)inter.bottom) - std::max<int>(ty, (int)inter.top));
        int clipped = (sz.cx * sz.cy) - (vis_w * vis_h);
        if (clipped > 0 || bw - sz.cx < 0) {
            LOGF("  %-34s 文本 %dx%d 可见 %dx%d  丢失 %d px^2 %s\n",
                   s.site, sz.cx, sz.cy, vis_w, vis_h, clipped,
                   (bw - sz.cx < 0) ? "(文本超出按钮边界)" : "");
            ++g_fail;
        }
    }
    if (!g_fail) LOGF("  (无)\n");

    // ---- 滚动区竖向剔除边界 ----
    LOGF("\n---- 滚动区底部行剔除边界（行内容底 vs clip 底）----\n");
    LOGF("(用 60 行确保列表真的滚到底，从而触发剔除边界)\n");
    LOGF("(每组第一行=修复前旧逻辑，第二行=修复后新逻辑)\n");
    {
        // page_executor: clip={content.left,list_top,content.right,content.bottom}
        //   cull: if (y > content.bottom) break;  行内按钮底 = y+32
        int list_top = g.content.top + 64 + 12 + 40;
        ScrollCase sc{ "page_executor/tasks", 451,
                       { g.content.left, list_top, g.content.right, g.content.bottom },
                       list_top + 6, 46, 60, g.content.bottom, 32, L"任务行(含按钮)" };
        analyze_scroll(sc, false);
        analyze_scroll(sc, true);
    }
    {
        RECT panel{ g.content.left, g.content.top + 58, g.content.right, g.content.bottom };
        int ty = panel.top + 38 + 22;
        ScrollCase sc{ "page_accounts/acc-scroll", 596,
                       { panel.left + 10, ty, panel.right - 10, panel.bottom - 10 },
                       ty + 6, 34, 60, panel.bottom, 26, L"账号行(含按钮)" };
        analyze_scroll(sc, false);
        analyze_scroll(sc, true);
    }
    {
        RECT panel{ g.content.left, g.content.top + 64 + 12 + 96 + 12, g.content.right, g.content.bottom };
        int ty = panel.top + 38 + 22;
        ScrollCase sc{ "page_cores/core-scroll", 754,
                       { panel.left + 10, ty, panel.right - 10, panel.bottom - 10 },
                       ty + 6, 34, 60, panel.bottom, 26, L"核心行(含按钮)" };
        analyze_scroll(sc, false);
        analyze_scroll(sc, true);
    }
    {
        RECT hpanel{ g.content.left, g.content.top + 42 + 300 + 12, g.content.right, g.content.bottom };
        int ty = hpanel.top + 36 + 22;
        ScrollCase sc{ "page_planner/plans-scroll", 907,
                       { hpanel.left + 10, ty, hpanel.right - 10, hpanel.bottom - 10 },
                       ty + 4, 26, 60, hpanel.bottom, 22, L"计划历史行" };
        analyze_scroll(sc, false);
        analyze_scroll(sc, true);
    }
    {
        RECT panel{ g.content.left, g.content.top, g.content.right, g.content.bottom };
        ScrollCase sc{ "page_events/ev-scroll", 943,
                       { panel.left + 10, panel.top + 40, panel.right - 10, panel.bottom - 10 },
                       panel.top + 44, 26, 60, panel.bottom, 22, L"事件行" };
        analyze_scroll(sc, false);
        analyze_scroll(sc, true);
    }

    // ---- 不同窗口尺寸下的横向溢出扫描 ----
    LOGF("\n---- 窗口尺寸扫描（哪些按钮在小窗口下文本溢出）----\n");
    const int sizes[][2] = {
        {1522, 1006}, {1400, 900}, {1200, 800}, {1000, 700}, {860, 620}, {720, 560}
    };
    for (auto& sz2 : sizes) {
        Geom g2 = make_geom(sz2[0], sz2[1]);
        std::vector<BtnSample> keep = g_samples;
        g_samples.clear();
        collect_sidebar(g2); collect_topbar(g2); collect_executor(g2);
        collect_accounts(g2); collect_cores(g2); collect_planner(g2); collect_settings(g2);
        int bad = 0;
        std::string names;
        for (auto& s : g_samples) {
            int bw = s.r.right - s.r.left;
            SIZE t = gui.text_size(s.label, gui.font);
            if (bw - t.cx < 0) { ++bad; names += " "; names += s.site; }
        }
        LOGF("  client %4dx%-4d : 溢出 %2d 项%s\n", sz2[0], sz2[1], bad,
               bad ? names.c_str() : "");
        g_samples = keep;
    }

    LOGF("\n==== 汇总：%d 处缺陷 ====\n", g_fail);

    // ---- 目检用离屏渲染 ----
    int pw = 1100, ph = 700;
    HDC pdc = CreateCompatibleDC(dc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(pdc, bmp);
    Gui g2;
    g2.init_fonts();
    g2.set_dc(pdc, { 0, 0, pw, ph });
    g2.fill({ 0, 0, pw, ph }, g2.theme.bg);
    int yy = 20;
    for (auto& s : g_samples) {
        // 等比缩放到 1100 宽画布，仅用于目检
        int bw = (s.r.right - s.r.left);
        RECT dr{ 20, yy, 20 + std::min(bw, 460), yy + 26 };
        g2.button(s.site, dr, s.label, 1);
        g2.text(500, yy + 5, std::wstring(L"[") + gui::widen(s.site) + L"]  w=" +
                std::to_wstring(bw), g2.theme.dim, g2.font_small);
        yy += 34;
        if (yy > ph - 40) break;
    }
    save_png(L"probe_buttons.png", pdc, pw, ph);
    LOGF("已写出 probe_buttons.png（离屏渲染，未创建窗口）\n");

    DeleteObject(bmp);
    DeleteDC(pdc);
    DeleteDC(dc);
    return g_fail ? 1 : 0;
}
