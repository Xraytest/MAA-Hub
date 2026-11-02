// MaaHub 零依赖立即模式 GUI 框架（Win32 + GDI 双缓冲）
// 特性：按钮/复选框/文本输入/选项条/滚动区域/卡片面板、CJK 字体、剪裁栈、
//       热激活焦点跟踪、后台线程数据快照友好（渲染只读快照）
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
// GDI+ 需要完整 windows.h，不能定义 WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <algorithm>
#include <codecvt>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "util.hpp"

namespace gui {

// ---------------------------------------------------------------- 主题
// 配色与控件尺寸取自 MAA 官方 GUI（HandyControl Light 主题，见
// MaaAssistantArknights/src/MaaWpfGui/Res/Theme.xaml -> HandyControl Light.xaml）
// Primary #326cf3 / Background #eeeeee / Region #ffffff / Border #e0e0e0
// PrimaryText #212121 / SecondaryText #757575 / ThirdlyText #bdbdbd
// Success #2db84d / Warning #e9af20 / Danger #db3340 / Accent #f8491e
// 控件高 28、圆角 4、内边距 (10,5)、左列宽 190、外框 Margin 20
struct Theme {
    COLORREF bg = RGB(0xEE, 0xEE, 0xEE);            // BackgroundColor
    COLORREF panel = RGB(0xFF, 0xFF, 0xFF);         // RegionColor
    COLORREF border = RGB(0xE0, 0xE0, 0xE0);        // BorderColor
    COLORREF text = RGB(0x21, 0x21, 0x21);          // PrimaryTextColor
    COLORREF dim = RGB(0x75, 0x75, 0x75);           // SecondaryTextColor
    COLORREF faint = RGB(0xBD, 0xBD, 0xBD);         // ThirdlyTextColor
    COLORREF primary = RGB(0x32, 0x6C, 0xF3);       // PrimaryColor
    COLORREF primary_dark = RGB(0x32, 0x6C, 0xF3);  // DarkPrimaryColor
    COLORREF primary_light = RGB(0xF3, 0xFB, 0xFF); // LightPrimaryColor
    COLORREF ok = RGB(0x2D, 0xB8, 0x4D);            // SuccessColor
    COLORREF ok_light = RGB(0xF3, 0xFF, 0xF6);      // LightSuccessColor
    COLORREF warn = RGB(0xE9, 0xAF, 0x20);          // WarningColor
    COLORREF warn_light = RGB(0xFF, 0xFC, 0xF5);    // LightWarningColor
    COLORREF err = RGB(0xDB, 0x33, 0x40);           // DangerColor
    COLORREF err_light = RGB(0xFF, 0xF6, 0xF7);     // LightDangerColor
    COLORREF accent = RGB(0xF8, 0x49, 0x1E);        // AccentColor
    COLORREF hover = RGB(0xF5, 0xF5, 0xF5);         // DarkDefaultColor（悬停底）
    // TaskQueue.TaskItemStatus.*（官方 Light.xaml，Alpha #20 = 12.5%）
    COLORREF status_running = RGB(0x6C, 0x9C, 0xF3);  // InProgress 蓝
    COLORREF status_done = RGB(0x60, 0xD0, 0x60);     // Completed 绿
    COLORREF status_error = RGB(0xFF, 0x66, 0x66);    // Error 红
    COLORREF input_bg = RGB(0xFF, 0xFF, 0xFF);      // DefaultColor
    COLORREF row_alt = RGB(0xEE, 0xEE, 0xEE);       // SecondaryRegionColor
    // 官方背景层级：窗口/左列卡片 = RegionBrushOpacity25(#40FFFFFF)
    // 叠加在系统白底上约等于 #FBFBFB；内容区沿用该色而非纯白
    COLORREF card_bg = RGB(0xFB, 0xFB, 0xFB);      // RegionBrushOpacity25 混合结果
    COLORREF card_bg_soft = RGB(0xF6, 0xF6, 0xF6); // 次级卡片（略深一档）
    // 官方 MaaCheckBoxDefaultStyle：RegionBrushOpacity50（比按钮深一档）
    COLORREF checkbox_bg = RGB(0xF6, 0xF6, 0xF6);
    // 官方 NoSelectedHighlightListBoxItemStyle：
    //   Background=RegionBrushOpacity25(#FBFBFB)，选中态背景不变（无高亮），
    //   IsMouseOver=MouseOverRegionBrushOpacity75(约#FAFAFA)
    COLORREF item_bg = RGB(0xFB, 0xFB, 0xFB);
    COLORREF item_hover = RGB(0xFA, 0xFA, 0xFA);
    COLORREF dark_mask = RGB(0x20, 0x00, 0x00);     // DarkMaskColor（仅用于透明叠加）
};

inline std::wstring widen(const std::string& s) { return util::utf8_to_wide(s); }

// ---------------------------------------------------------------- 输入
struct Input {
    POINT mouse{ 0, 0 };
    bool down = false;        // 左键按住
    bool pressed = false;     // 本帧刚按下
    bool released = false;    // 本帧刚松开
    bool rdown = false;       // 右键按住
    bool rpressed = false;    // 本帧刚按下右键
    bool rreleased = false;   // 本帧刚松开右键
    bool double_click = false;
    int wheel = 0;            // 本帧滚轮增量
    wchar_t char_input = 0;   // WM_CHAR
    int vk = 0;               // WM_KEYDOWN
    bool shift = false;
    bool ctrl = false;
};

// 复选框三态（对应 MAA：关 / 开 / 仅生效一次）
enum class CheckState { Off = 0, On = 1, Once = 2 };

// ---------------------------------------------------------------- GUI 上下文
class Gui {
public:
    Theme theme;
    HFONT font = nullptr;       // 14px
    HFONT font_bold = nullptr;  // 14px 粗体
    HFONT font_small = nullptr; // 12px
    HFONT font_title = nullptr; // 16px 粗体
    HFONT font_big = nullptr;   // 15px 粗体

    void init_fonts() {
        if (font) return;
        // 字号对齐 MAA 官方 WPF 规格：正文 12pt≈16px、辅助 11pt≈15px、
        // 标题 14pt≈19px、大标题 15pt≈20px（96DPI 下 1pt = 1.333px）
        font = make_font(16, false);
        font_bold = make_font(16, true);
        font_small = make_font(15, false);
        font_title = make_font(19, true);
        font_big = make_font(20, true);
    }

    // ------------------------------------------------------------ 帧
    void begin(const Input& in) {
        in_ = in;
        hot_.clear();

        wheel_used_ = false;
    }

    void end() {
        // active：按住中的控件
        if (in_.released || !in_.down) active_.clear();
        if (in_.rreleased || !in_.rdown) r_active_.clear();
    }

    // ------------------------------------------------------------ 图元
    void fill(RECT r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc_, &r, b);
        DeleteObject(b);
    }
    void frame(RECT r, COLORREF c, int thickness = 1) {
        HPEN p = CreatePen(PS_INSIDEFRAME, thickness, c);
        HGDIOBJ old = SelectObject(dc_, p);
        HGDIOBJ oldb = SelectObject(dc_, GetStockObject(NULL_BRUSH));
        Rectangle(dc_, r.left, r.top, r.right, r.bottom);
        SelectObject(dc_, oldb);
        SelectObject(dc_, old);
        DeleteObject(p);
    }
    // 水平渐变填充（对应 MAA TitleBrush/PrimaryBrush 的 LinearGradientBrush）
    void gradient_rect(RECT r, COLORREF c1, COLORREF c2) {
        TRIVERTEX tv[2]{};
        tv[0].x = r.left;  tv[0].y = r.top;
        tv[0].Red = (USHORT)(GetRValue(c1) * 257);
        tv[0].Green = (USHORT)(GetGValue(c1) * 257);
        tv[0].Blue = (USHORT)(GetBValue(c1) * 257);
        tv[1].x = r.right; tv[1].y = r.bottom;
        tv[1].Red = (USHORT)(GetRValue(c2) * 257);
        tv[1].Green = (USHORT)(GetGValue(c2) * 257);
        tv[1].Blue = (USHORT)(GetBValue(c2) * 257);
        GRADIENT_RECT gr{ 0, 1 };
        GradientFill(dc_, tv, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    }

    void round_rect(RECT r, COLORREF fill, COLORREF line, int radius) {
        // GDI+ 抗锯齿圆角矩形（GDI RoundRect 边缘锯齿明显，与 WPF 观感差距大）
        if (r.right <= r.left || r.bottom <= r.top) return;
        int x = r.left, y = r.top;
        int w = r.right - r.left, h = r.bottom - r.top;
        if (radius < 0) radius = 0;
        if (radius * 2 > w) radius = w / 2;
        if (radius * 2 > h) radius = h / 2;
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        GraphicsPath path;
        if (radius <= 0) {
            path.AddRectangle(Rect(x, y, w, h));
        } else {
            int d = radius * 2;
            path.AddArc(x, y, d, d, 180, 90);
            path.AddArc(x + w - d - 1, y, d, d, 270, 90);
            path.AddArc(x + w - d - 1, y + h - d - 1, d, d, 0, 90);
            path.AddArc(x, y + h - d - 1, d, d, 90, 90);
            path.CloseFigure();
        }
        if (fill != CLR_INVALID) {
            SolidBrush br(Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
            g.FillPath(&br, &path);
        }
        if (line != CLR_INVALID) {
            Pen pen(Color(255, GetRValue(line), GetGValue(line), GetBValue(line)), 1.0f);
            g.DrawPath(&pen, &path);
        }
    }
    SIZE text_size(const std::wstring& s, HFONT f) {
        SIZE sz{ 0, 0 };
        HGDIOBJ old = SelectObject(dc_, f);
        GetTextExtentPoint32W(dc_, s.c_str(), (int)s.size(), &sz);
        SelectObject(dc_, old);
        return sz;
    }
    void text(int x, int y, const std::wstring& s, COLORREF c, HFONT f) {
        RECT rc = clip_;
        SetTextColor(dc_, c);
        SetBkMode(dc_, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc_, f);
        // 手动逐行（支持 \n）
        size_t start = 0;
        int ly = y;
        while (start <= s.size()) {
            size_t nl = s.find(L'\n', start);
            std::wstring line = s.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
            ExtTextOutW(dc_, x, ly, ETO_CLIPPED, &rc, line.c_str(), (UINT)line.size(), nullptr);
            if (nl == std::wstring::npos) break;
            start = nl + 1;
            SIZE sz = text_size(L"A", f);
            ly += sz.cy + 2;
        }
        SelectObject(dc_, old);
    }
    void text_clipped(RECT r, const std::wstring& s, COLORREF c, HFONT f, bool ellipsis = true) {
        int avail = r.right - r.left;
        std::wstring t = ellipsis ? this->ellipsize(s, avail, f) : s;
        SIZE sz = text_size(t, f);
        text(r.left, r.top + (r.bottom - r.top - sz.cy) / 2, t, c, f);
    }

    // ------------------------------------------------------------ 文本自适应
    // 按像素宽度截断并补省略号。UTF-16 代理对安全：不会把 emoji 劈成半个。
    std::wstring ellipsize(const std::wstring& s, int avail, HFONT f) {
        if (avail <= 0) return L"";
        if (text_size(s, f).cx <= avail) return s;
        const std::wstring mark = L"\u2026";
        std::wstring t = s;
        while (t.size() > 1) {
            t.pop_back();
            // 不要把代理对劈开（丢掉尾部的高位代理）
            while (!t.empty() && (t.back() & 0xFC00) == 0xD800) t.pop_back();
            if (t.empty()) break;
            if (text_size(t + mark, f).cx <= avail) return t + mark;
        }
        return mark;
    }

    // 把标签塞进 avail 像素：先试 14px，再退 12px，最后 12px + 省略号。
    // 保证返回的 (字体, 文本) 组合一定放得下，调用方无需再判断。
    void fit_label(const std::wstring& s, int avail, HFONT& out_font, std::wstring& out_text) {
        out_font = font;
        out_text = s;
        if (avail <= 0) { out_text = L""; return; }
        if (text_size(s, font).cx <= avail) return;
        out_font = font_small;
        if (text_size(s, font_small).cx <= avail) return;
        out_text = ellipsize(s, avail, font_small);
    }

    // ------------------------------------------------------------ 命中测试
    bool hovered(RECT r) const {
        return PtInRect(&r, in_.mouse) && PtInRect(&clip_, in_.mouse);
    }

    // ------------------------------------------------------------ 滚动区域
    // 注意：滚动通过调用方在绘制时手动减去 scroll 值实现（见 scroll_offset()），
    // 不使用 SetViewportOrigin，保证鼠标命中测试与视觉位置一致。
    void begin_scroll(const char* id, RECT r, int content_h, float& scroll_y) {
        scroll_rect_ = r;
        scroll_content_h_ = content_h;
        scroll_id_ = id;
        scroll_y_ = &scroll_y;
        if (hovered(r) && in_.wheel && !wheel_used_) {
            *scroll_y_ -= in_.wheel * 40.0f;
            wheel_used_ = true;
        }
        int max_scroll = content_h - (r.bottom - r.top);
        if (max_scroll < 0) max_scroll = 0;
        if (*scroll_y_ < 0) *scroll_y_ = 0;
        if (*scroll_y_ > max_scroll) *scroll_y_ = (float)max_scroll;
        push_clip(r);
    }
    void end_scroll() {
        draw_scrollbar();   // 先画滚动条（仍在裁剪区内，位置正确）
        pop_clip();
    }

    // 官方 SVG 图标绘制（Carbon Icons / HandyControl Geometries）
    // 支持 M/L/A/a/h/v/z 子集（官方图标实际用到的指令）
    void draw_icon_path(const std::wstring& path, RECT r, COLORREF color,
                        double viewbox = 32.0) {
        if (path.empty() || r.right <= r.left || r.bottom <= r.top) return;
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath gp;
        double sx = (r.right - r.left) / viewbox;
        double sy = (r.bottom - r.top) / viewbox;
        double cx = 0, cy = 0;          // current point
        double sx0 = 0, sy0 = 0;        // subpath start
        size_t i = 0;
        auto num = [&](size_t& p) -> double {
            while (p < path.size() && (path[p] == L' ' || path[p] == L',')) ++p;
            size_t st = p;
            while (p < path.size() && (iswdigit(path[p]) || path[p] == L'.' ||
                   path[p] == L'-' || path[p] == L'+' || path[p] == L'e')) ++p;
            if (st == p) return 0;
            return _wtof(path.substr(st, p - st).c_str());
        };
        auto skip = [&](size_t& p) { while (p < path.size() && (path[p] == L' ' || path[p] == L',')) ++p; };
        while (i < path.size()) {
            skip(i);
            if (i >= path.size()) break;
            wchar_t cmd = path[i++];
            bool rel = iswlower(cmd);
            wchar_t C = towupper(cmd);
            if (C == L'M') {
                double x = num(i) * sx, y = num(i) * sy;
                if (rel) { x += cx; y += cy; }
                cx = x; cy = y; sx0 = x; sy0 = y;
                gp.StartFigure();
            } else if (C == L'L') {
                double x = num(i) * sx, y = num(i) * sy;
                if (rel) { x += cx; y += cy; }
                gp.AddLine((REAL)(r.left + cx), (REAL)(r.top + cy),
                           (REAL)(r.left + x), (REAL)(r.top + y));
                cx = x; cy = y;
            } else if (C == L'H') {
                double x = num(i) * sx;
                if (rel) x += cx;
                gp.AddLine((REAL)(r.left + cx), (REAL)(r.top + cy),
                           (REAL)(r.left + x), (REAL)(r.top + cy));
                cx = x;
            } else if (C == L'V') {
                double y = num(i) * sy;
                if (rel) y += cy;
                gp.AddLine((REAL)(r.left + cx), (REAL)(r.top + cy),
                           (REAL)(r.left + cx), (REAL)(r.top + y));
                cy = y;
            } else if (C == L'Z') {
                gp.AddLine((REAL)(r.left + cx), (REAL)(r.top + cy),
                           (REAL)(r.left + sx0), (REAL)(r.top + sy0));
                cx = sx0; cy = sy0;
                gp.CloseFigure();
            } else if (C == L'A') {
                double rx = num(i) * sx, ry = num(i) * sy;
                num(i);                                    // x-axis-rotation
                double laf = num(i), swf = num(i);
                double x = num(i) * sx, y = num(i) * sy;
                if (rel) { x += cx; y += cy; }
                // 椭圆弧 -> 用 Bezier 近似（4 段）
                double mx = (cx + x) / 2, my = (cy + y) / 2;
                double dx = (x - cx) / 2, dy = (y - cy) / 2;
                gp.AddBezier((REAL)(r.left + cx), (REAL)(r.top + cy),
                             (REAL)(r.left + mx - dy * 0.5523), (REAL)(r.top + my + dx * 0.5523),
                             (REAL)(r.left + mx + dy * 0.5523), (REAL)(r.top + my - dx * 0.5523),
                             (REAL)(r.left + x), (REAL)(r.top + y));
                cx = x; cy = y;
                (void)rx; (void)ry; (void)laf; (void)swf;
            } else {
                break;   // 不支持的指令，停止
            }
        }
        SolidBrush br(Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        g.FillPath(&br, &gp);
    }

    // 官方图标集（path 取自 MaaWpfGui/Res/Styles/Basic/Geometries.xaml）
    void icon_add_task(RECT r, COLORREF c) {
        draw_icon_path(L"M14.613,10c0,0.23-0.188,0.419-0.419,0.419H10.42v3.774c0,0.23-0.189,0.42-0.42,0.42"
                       L"s-0.419-0.189-0.419-0.42v-3.774H5.806c-0.23,0-0.419-0.189-0.419-0.419"
                       L"s0.189-0.419,0.419-0.419h3.775V5.806c0-0.23,0.189-0.419,0.419-0.419"
                       L"s0.42,0.189,0.42,0.419v3.775h3.774C14.425,9.581,14.613,9.77,14.613,10 "
                       L"M17.969,10c0,4.401-3.567,7.969-7.969,7.969c-4.402,0-7.969-3.567-7.969-7.969"
                       L"c0-4.402,3.567-7.969,7.969-7.969C14.401,2.031,17.969,5.598,17.969,10 "
                       L"M17.13,10c0-3.932-3.198-7.13-7.13-7.13S2.87,6.068,2.87,10c0,3.933,3.198,7.13,7.13,7.13"
                       L"S17.13,13.933,17.13,10", r, c);
    }
    void icon_link(RECT r, COLORREF c) {
        draw_icon_path(L"M29.25 6.76a6 6 0 0 0-8.5 0l1.42 1.42a4 4 0 1 1 5.67 5.67l-8 8a4 4 0 1 1-5.67-5.66"
                       L"l1.41-1.42l-1.41-1.42l-1.42 1.42a6 6 0 0 0 0 8.5A6 6 0 0 0 17 25a6 6 0 0 0 4.27-1.76"
                       L"l8-8a6 6 0 0 0-.02-8.48z M4.19 24.82a4 4 0 0 1 0-5.67l8-8a4 4 0 0 1 5.67 0A3.94 3.94 0 0 1 19 14"
                       L"a4 4 0 0 1-1.17 2.85L15.71 19l1.42 1.42l2.12-2.12a6 6 0 0 0-8.51-8.51l-8 8a6 6 0 0 0 0 8.51"
                       L"A6 6 0 0 0 7 28a6.07 6.07 0 0 0 4.28-1.76l-1.42-1.42a4 4 0 0 1-5.67 0z", r, c);
    }
    void icon_unlink(RECT r, COLORREF c) {
        draw_icon_path(L"M3.584 5.002l1.414-1.415l3.416 3.415L7 8.417z M23.581 24.992l1.415-1.414l3.415 3.416"
                       L"l-1.415 1.414z M11 2h2v4h-2z M2 11h4v2H2z M26 19h4v2h-4z M19 26h2v4h-2z "
                       L"M16.58 21.07l-3.71 3.72a4 4 0 1 1-5.66-5.66l3.72-3.72L9.51 14L5.8 17.72a6 6 0 0 0-.06 8.54"
                       L"A6 6 0 0 0 10 28a6.07 6.07 0 0 0 4.32-1.8L18 22.49z M15.41 10.93l3.72-3.72a4 4 0 1 1 5.66 5.66"
                       L"l-3.72 3.72L22.49 18l3.71-3.72a6 6 0 0 0 .06-8.54A6 6 0 0 0 22 4a6.07 6.07 0 0 0-4.32 1.8L14 9.51z",
                       r, c);
    }
    // 任务操作图标（22x22 按钮内 14x14，官方 TaskActionButtonStyle）
    void icon_copy_task(RECT r, COLORREF c) {
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        int w = r.right - r.left, h = r.bottom - r.top;
        int bw = w * 6 / 10, bh = h * 6 / 10;
        Pen pen(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), 1.6f);
        SolidBrush br(Color(255, GetRValue(theme.card_bg), GetGValue(theme.card_bg),
                            GetBValue(theme.card_bg)));
        g.FillRectangle(&br, r.left, r.top + h - bh - 1, bw, bh);
        g.DrawRectangle(&pen, r.left, r.top + h - bh - 1, bw, bh);
        g.DrawRectangle(&pen, r.left + w - bw - 1, r.top, bw, bh);
    }
    void icon_rename_task(RECT r, COLORREF c) {
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        int w = r.right - r.left, h = r.bottom - r.top;
        SolidBrush br(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
        Point pts[4] = { Point(r.left + 1, r.top + h - 3), Point(r.left + w - 4, r.top + 2),
                         Point(r.left + w - 1, r.top + 5), Point(r.left + 4, r.top + h - 1) };
        g.FillPolygon(&br, pts, 4);
    }
    void icon_delete_task(RECT r, COLORREF c) {
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        int w = r.right - r.left, h = r.bottom - r.top;
        Pen pen(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), 1.6f);
        g.DrawLine(&pen, r.left + 1, r.top + 3, r.left + w - 1, r.top + 3);
        g.DrawRectangle(&pen, r.left + 2, r.top + 5, w - 4, h - 7);
        g.DrawLine(&pen, r.left + w / 2, r.top + 7, r.left + w / 2, r.top + h - 4);
    }

    // 齿轮（ConfigGeometry 的简化等效：外圈 + 中心，官方为 32x32 Carbon 齿轮）
    void icon_gear(RECT r, COLORREF c) {
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        int rad = (r.right - r.left) / 2 - 2;
        SolidBrush br(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
        // 齿（8 个方向的小矩形）
        for (int k = 0; k < 8; ++k) {
            double a = k * 3.14159265 / 4.0;
            int tx = cx + (int)(cos(a) * (rad - 1));
            int ty = cy + (int)(sin(a) * (rad - 1));
            g.FillEllipse(&br, tx - 2, ty - 2, 5, 5);
        }
        g.FillEllipse(&br, cx - rad, cy - rad, rad * 2, rad * 2);
        // 中心挖孔（用背景色）
        SolidBrush hole(Color(255, GetRValue(theme.card_bg), GetGValue(theme.card_bg),
                              GetBValue(theme.card_bg)));
        g.FillEllipse(&hole, cx - rad / 3, cy - rad / 3, rad * 2 / 3, rad * 2 / 3);
    }

    // 官方样式滚动条（HandyControl ScrollBarBaseStyle）：
    //   Width=8、Thumb 背景=PrimaryTextBrush(#212121)、圆角、仅 hover 时显示
    // 在 end_scroll 之后调用，绘制在滚动区右缘（纵向）。
    void draw_scrollbar() {
        if (!scroll_y_) return;
        if (scroll_rect_.right <= scroll_rect_.left) return;
        int content_h = scroll_content_h_;
        int view_h = scroll_rect_.bottom - scroll_rect_.top;
        int max_scroll = content_h - view_h;
        if (max_scroll <= 0) return;
        float frac = *scroll_y_ / (float)max_scroll;
        int thumb_h = view_h * view_h / content_h;
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > view_h) thumb_h = view_h;
        int track_h = view_h - thumb_h;
        int ty = scroll_rect_.top + (int)(track_h * frac);
        int tx = scroll_rect_.right - 8;
        // 轨道（透明底，仅 hover 时微显）
        bool hov = in_.mouse.x >= scroll_rect_.right - 12
                && in_.mouse.x <= scroll_rect_.right
                && in_.mouse.y >= scroll_rect_.top
                && in_.mouse.y <= scroll_rect_.bottom;
        if (hov) {
            fill({ tx, scroll_rect_.top, tx + 4, scroll_rect_.bottom },
                 RGB(0xF0, 0xF0, 0xF0));
        }
        // Thumb：官方 PrimaryTextBrush，圆角 2
        round_rect({ tx, ty, tx + 4, ty + thumb_h },
                   hov ? RGB(0x42, 0x42, 0x42) : RGB(0x9E, 0x9E, 0x9E),
                   CLR_INVALID, 2);
    }

    // 当前滚动偏移（绘制 item 时从 y 中减去）
    int scroll_offset() const { return scroll_y_ ? (int)*scroll_y_ : 0; }

    // 行是否完整落在当前滚动裁剪区内。
    // 列表页原来用「面板底」做 break 阈值，而裁剪区底比面板底高 10px，
    // 导致最后一个可见行（含按钮与文字）被裁掉一半。这里统一按裁剪区判断。
    bool row_fully_visible(int y, int row_h) const {
        if (clip_.bottom <= clip_.top) return false;
        return y >= clip_.top && (y + row_h) <= clip_.bottom;
    }

    // ------------------------------------------------------------ 控件
    // 分组标题（对应 MAA 设置区的 GroupBox / 分隔小标题）
    void group_title(int x, int y, const std::wstring& label) {
        RECT save = clip_;
        text(x, y, label, theme.dim, font_small);
        (void)save;
    }
    void divider(RECT r) {
        fill({ r.left, r.top, r.right, r.top + 1 }, theme.border);
    }

    // style: 0 normal, 1 primary, 2 danger, 3 warn
    bool button(const char* id, RECT r, const std::wstring& label_text, int style = 0,
                bool enabled = true) {
        bool hov = hovered(r) && enabled;
        if (hov) hot_ = id;
        bool click = false;
        if (hov && in_.pressed) active_ = id;
        if (hov && active_ == id && in_.released) click = true;

        // 官方 MaaButtonDefaultStyle：Background=RegionBrushOpacity25(约#FBFBFB)、
        // Foreground=PrimaryTextBrush、基于 ButtonBaseStyle(高28/圆角4/内边距(10,5))
        COLORREF fill = theme.card_bg, border = theme.border, txt = theme.text;
        if (style == 1) { fill = theme.primary; border = theme.primary; txt = RGB(255, 255, 255); }
        else if (style == 2) { fill = RGB(254, 242, 242); border = theme.err; txt = theme.err; }
        else if (style == 3) { fill = RGB(255, 248, 235); border = theme.warn; txt = RGB(180, 120, 10); }
        if (!enabled) { fill = RGB(245, 246, 248); border = theme.border; txt = RGB(160, 168, 178); }
        else if (hov) { fill = (style == 1) ? theme.primary_dark : theme.hover; }

        round_rect(r, fill, active_ == id && hov ? theme.primary : border, 4);
        // 标签自适应 + 裁进按钮自身矩形：
        // 原来直接按按钮宽度居中绘制，标签比按钮宽时既会溢出到相邻控件上，
        // 又会被外层裁剪区（滚动区/面板）拦腰截断 —— 即"按钮文本被剪裁"。
        int bw = r.right - r.left;
        int bh = r.bottom - r.top;
        int pad = bw >= 48 ? 6 : (bw >= 24 ? 3 : 1);
        int avail = bw - pad * 2;
        HFONT f = font;
        std::wstring lbl = label_text;
        fit_label(label_text, avail, f, lbl);
        SIZE sz = text_size(lbl, f);
        int tx = r.left + (bw - sz.cx) / 2;
        int ty = r.top + (bh - sz.cy) / 2;
        push_clip(r);                      // 标签绝不允许画出按钮边界
        text(tx, ty, lbl, txt, f);
        pop_clip();
        return click;
    }

    // 横向选项条（单选，返回点击索引，未点击 -1）已在下方 option_strip

    // 数字输入框：左侧标签 + 中间输入 + 右侧 +/- 步进按钮
    void number_input(const char* id, RECT r, const std::wstring& label,
                      int& value, int step = 1, int min_v = 0, int max_v = 9999) {
        int lw = 150;
        RECT save = clip_;
        text(r.left, r.top + (r.bottom - r.top - 17) / 2, label, theme.text, font);
        (void)save;
        RECT ir{ r.left + lw, r.top, r.right - 76, r.bottom };
        std::string edit = std::to_string(value);
        text_input(id, ir, edit, nullptr);
        int nv = value;
        try { nv = std::stoi(edit); } catch (...) { nv = value; }
        if (nv < min_v) nv = min_v;
        if (nv > max_v) nv = max_v;
        value = nv;
        // 窄列时 ± 按钮会挤压输入框，宽度不足则省略
        int w = r.right - r.left;
        if (w >= 150) {
            RECT minus_b{ r.right - 72, r.top, r.right - 40, r.bottom };
            RECT plus_b{ r.right - 36, r.top, r.right - 4, r.bottom };
            if (button((std::string(id) + "-m").c_str(), minus_b, L"-", 0)) {
                value = (value - step < min_v) ? min_v : value - step;
            }
            if (button((std::string(id) + "-p").c_str(), plus_b, L"+", 0)) {
                value = (value + step > max_v) ? max_v : value + step;
            }
        }
    }

    // 提示行（对应 MAA 的 Tip 文本，三级文字色）
    void tip_line(int x, int y, int right, const std::wstring& text) {
        RECT save = clip_;
        text_clipped({ x, y, right, y + 16 }, text, theme.faint, font_small);
        (void)save;
    }

    // 三态复选框（对应 MAA 官方语义）：
    //   左键 = 开/关切换；右键 = 切换「仅生效一次」（该次运行有效，不写入配置）
    bool checkbox3(const char* id, RECT r, const std::wstring& label, CheckState& st) {
        bool hov = hovered(r);
        if (hov) hot_ = id;
        if (hov && in_.pressed) active_ = id;
        bool changed = false;
        if (hov && active_ == id && in_.released) {
            st = (st == CheckState::On) ? CheckState::Off : CheckState::On;
            changed = true;
        }
        if (hov && in_.rpressed) r_active_ = id;
        if (hov && r_active_ == id && in_.rreleased) {
            st = (st == CheckState::Once) ? CheckState::Off : CheckState::Once;
            changed = true;
        }
        // 方框与文字在传入矩形内垂直居中（官方 VerticalContentAlignment=Center）
        int box = 16;
        int vpad = ((r.bottom - r.top) - box) / 2;
        if (vpad < 0) vpad = 0;
        RECT br{ r.left, r.top + vpad, r.left + box, r.top + vpad + box };
        // 官方复选框底色 RegionBrushOpacity50（浅灰，与按钮形成层次）
        COLORREF fill_c = theme.checkbox_bg, line_c = RGB(180, 188, 200);
        if (st == CheckState::On) { fill_c = theme.primary; line_c = theme.primary; }
        else if (st == CheckState::Once) { fill_c = theme.primary_light; line_c = theme.primary; }
        round_rect(br, fill_c, line_c, 3);
        if (st != CheckState::Off) {
            HPEN p = CreatePen(PS_SOLID, 2, st == CheckState::On ? RGB(255, 255, 255)
                                                                 : theme.primary);
            HGDIOBJ old = SelectObject(dc_, p);
            MoveToEx(dc_, br.left + 3, br.top + 8, nullptr);
            LineTo(dc_, br.left + 6, br.top + 12);
            LineTo(dc_, br.left + 13, br.top + 4);
            SelectObject(dc_, old);
            DeleteObject(p);
            if (st == CheckState::Once) {
                // 「仅一次」加圆点标记
                HPEN d = CreatePen(PS_SOLID, 3, theme.primary);
                HGDIOBJ od = SelectObject(dc_, d);
                Ellipse(dc_, br.left + 5, br.top + 5, br.left + 11, br.top + 11);
                SelectObject(dc_, od);
                DeleteObject(d);
            }
        }
        // 官方 CheckBox 文字 TextWrapping=Wrap：窄列下长标签换行显示，不被裁切
        RECT save = clip_;
        int tx = r.left + box + 8;
        int avail = r.right - tx;
        SIZE full = text_size(label, font);
        int tpad = ((r.bottom - r.top) - 18) / 2;
        if (tpad < 0) tpad = 0;
        if (full.cx <= avail) {
            text(tx, r.top + tpad, label, theme.text, font);
            if (st == CheckState::Once) {
                text(tx + full.cx + 6, r.top + tpad,
                     L"（仅一次）", theme.primary, font_small);
            }
        } else {
            // 按可用宽度逐字换行（最多两行）
            std::wstring line1, line2;
            int used = 0;
            for (wchar_t ch : label) {
                int cw = text_size(std::wstring(1, ch), font).cx;
                if (used + cw > avail) { line2 += ch; }
                else { line1 += ch; used += cw; }
                if (line2.size() > 26) { line2.resize(26); line2 += L"…"; break; }
            }
            text(tx, r.top + tpad - 2, line1, theme.text, font);
            if (!line2.empty()) text(tx, r.top + tpad + 16, line2, theme.text, font);
            if (st == CheckState::Once) {
                text(tx + text_size(line1, font).cx + 6, r.top + tpad,
                     L"（仅一次）", theme.primary, font_small);
            }
        }
        (void)save;
        return changed;
    }

    bool checkbox(const char* id, RECT r, const std::wstring& label, bool& v) {
        // 复选框区域包含文字
        bool hov = hovered(r);
        if (hov) hot_ = id;
        if (hov && in_.pressed) active_ = id;
        bool click = false;
        if (hov && active_ == id && in_.released) {
            v = !v;
            click = true;
        }
        // 方框与文字在传入矩形内垂直居中（官方 VerticalContentAlignment=Center）
        int box = 16;
        int vpad = ((r.bottom - r.top) - box) / 2;
        if (vpad < 0) vpad = 0;
        RECT br{ r.left, r.top + vpad, r.left + box, r.top + vpad + box };
        round_rect(br, v ? theme.primary : RGB(255, 255, 255),
                   v ? theme.primary : RGB(180, 188, 200), 3);
        if (v) {
            // 画勾
            HPEN p = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ old = SelectObject(dc_, p);
            MoveToEx(dc_, br.left + 3, br.top + 8, nullptr);
            LineTo(dc_, br.left + 6, br.top + 12);
            LineTo(dc_, br.left + 13, br.top + 4);
            SelectObject(dc_, old);
            DeleteObject(p);
        }
        // 标签同样要裁进复选框区域：任务名 + 描述可能很长，
        // 不裁的话会一直画到右侧「▶ 立即运行 / ⚙」按钮底下，看起来就是被截断。
        RECT lr{ r.left + box + 8, r.top, r.right, r.bottom };
        int avail = lr.right - lr.left;
        HFONT f = font;
        std::wstring lbl = label;
        fit_label(label, avail, f, lbl);
        SIZE sz = text_size(lbl, f);
        push_clip(lr);
        text(lr.left, r.top + ((r.bottom - r.top) - sz.cy) / 2, lbl, theme.text, f);
        pop_clip();
        return click;
    }

    void text_input(const char* id, RECT r, std::string& value, const wchar_t* hint = nullptr) {
        bool hov = hovered(r);
        if (hov) hot_ = id;
        if (hov && in_.pressed) {
            active_ = id;
            focus_ = id;
        }
        bool focused = focus_ == id;
        // 官方 MaaTextBoxDefaultStyle：Background=RegionBrushOpacity25、圆角 4
        round_rect(r, theme.card_bg, focused ? theme.primary : theme.border, 4);
        std::wstring w = widen(value);
        RECT save = clip_;
        if (w.empty() && hint) {
            text(r.left + 8, r.top + (r.bottom - r.top - 17) / 2, hint, RGB(170, 178, 190), font);
        } else {
            // 水平滚动以露出光标
            int maxw = r.right - r.left - 14;
            SIZE sz = text_size(w, font);
            int x = r.left + 7;
            if (sz.cx > maxw) x = r.left + 7 - (sz.cx - maxw) - 8;
            text(x, r.top + (r.bottom - r.top - 17) / 2, w, theme.text, font);
            if (focused && (GetTickCount() / 530) % 2 == 0) {
                SIZE cs = text_size(w, font);
                int cx = std::min<int>(x + cs.cx + 1, (int)r.right - 3);
                HPEN p = CreatePen(PS_SOLID, 1, theme.text);
                HGDIOBJ old = SelectObject(dc_, p);
                MoveToEx(dc_, cx, r.top + 6, nullptr);
                LineTo(dc_, cx, r.bottom - 6);
                SelectObject(dc_, old);
                DeleteObject(p);
            }
        }
        clip_ = save;
        // 键盘输入
        if (focused) {
            if (in_.char_input) {
                if (in_.char_input >= 32 || in_.char_input == L'\t') {
                    value.push_back((char)in_.char_input);
                } else if (in_.char_input == L'\b' && !value.empty()) {
                    // UTF-8 backspace: drop last complete UTF-8 char
                    size_t n = value.size();
                    while (n > 0 && ((unsigned char)value[n - 1] & 0xC0) == 0x80) --n;
                    if (n > 0) --n;
                    value.resize(n);
                }
            }
            if (in_.vk == VK_RETURN || in_.vk == VK_ESCAPE) focus_.clear();
            if (in_.vk == VK_DELETE) value.clear();
        }
    }

    // 横向选项条：返回点击的索引，未点击返回 -1
    int option_strip(const char* id, RECT r, const std::vector<std::wstring>& opts, int cur) {
        int n = (int)opts.size();
        int gap = 4;
        int w = (r.right - r.left - gap * (n - 1)) / n;
        int clicked = -1;
        for (int i = 0; i < n; ++i) {
            RECT ir{ r.left + i * (w + gap), r.top, r.left + i * (w + gap) + w, r.bottom };
            bool hov = hovered(ir);
            if (hov) hot_ = id + std::to_string(i);
            if (hov && in_.pressed) active_ = id + std::to_string(i);
            bool sel = (i == cur);
            COLORREF fill = sel ? theme.primary_light : RGB(255, 255, 255);
            COLORREF border = sel ? theme.primary : theme.border;
            COLORREF txt = sel ? theme.primary_dark : theme.dim;
            round_rect(ir, fill, border, 5);
            RECT save = clip_;
            text_clipped(ir, opts[i], txt, font_small);
            clip_ = save;
            if (hov && active_ == id + std::to_string(i) && in_.released) clicked = i;
        }
        return clicked;
    }

    // ------------------------------------------------------------ 卡片
    void panel(RECT r, const std::wstring& title = L"") {
        // 官方 RegionBrushOpacity25（约 #FBFBFB）+ CornerRadius=4
        // （原为纯白 #FFFFFF + 圆角 10，形成"大白板"观感）
        round_rect(r, theme.card_bg, theme.border, 4);
        if (!title.empty()) {
            RECT save = clip_;
            text(r.left + 18, r.top + 14, title, theme.text, font_bold);
            clip_ = save;
        }
    }

    // ------------------------------------------------------------ 剪裁栈
    void push_clip(RECT r) {
        RECT inter;
        IntersectRect(&inter, &clip_, &r);
        clip_stack_.push_back(clip_);
        clip_ = inter;
    }
    void pop_clip() {
        if (!clip_stack_.empty()) {
            clip_ = clip_stack_.back();
            clip_stack_.pop_back();
        }
    }
    RECT clip() const { return clip_; }

    // ------------------------------------------------------------ 渲染目标
    void set_dc(HDC dc, RECT client) {
        dc_ = dc;
        clip_ = client;
        // 文字背景透明 + 高质量拉伸（缩放/位图时不出现块状）
        SetBkMode(dc_, TRANSPARENT);
        SetStretchBltMode(dc_, HALFTONE);
        SetBrushOrgEx(dc_, 0, 0, nullptr);
    }

    Input input() const { return in_; }

private:
    static HFONT make_font(int px, bool bold) {
        // ANTIALIASED_QUALITY：字体抗锯齿（WPF 文字平滑的关键；
        // 默认 DEFAULT_QUALITY 在小字号下锯齿明显，是"不像官方"的主因之一）
        return CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEVICE_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                           L"Microsoft YaHei");
    }

    HDC dc_ = nullptr;
    RECT clip_{ 0, 0, 0, 0 };
    std::vector<RECT> clip_stack_;
    Input in_;
    std::string hot_;
    std::string r_active_;   // 右键活动控件
    std::string active_;
    std::string focus_;
    bool wheel_used_ = false;
    // 滚动
    RECT scroll_rect_{};
    int scroll_content_h_ = 0;
    const char* scroll_id_ = nullptr;
    float* scroll_y_ = nullptr;
};

} // namespace gui
