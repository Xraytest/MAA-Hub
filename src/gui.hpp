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
    // 页面底色用的是 RegionBrush（Light=#FFFFFF / Dark=#1C1C1C），不是 BackgroundColor
    // #eeeeee —— 实测官方浅色截图整页（含标题栏、标签条、列间留白）都是纯白，
    // 卡片只靠 BorderColor #E0E0E0 的 1px 描边区分。#eeeeee 只作为 hover 等叠加层出现。
    COLORREF bg = RGB(0xFF, 0xFF, 0xFF);            // RegionBrush（官方 Light）
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
    COLORREF hover = RGB(0xF3, 0xF3, 0xF3);         // MouseOverRegionBrush（官方 Light 覆盖值）
    // TaskQueue.TaskItemStatus.*：官方为 #20 前缀（12.5% 黑底叠加），
    // 折算为叠在白色根底（RegionBrush）上的实色
    COLORREF status_running = RGB(0xEC, 0xF3, 0xFD);  // #206C9CF3 InProgress
    COLORREF status_done = RGB(0xEB, 0xF9, 0xEB);     // #2060D060 Completed
    COLORREF status_error = RGB(0xFF, 0xEC, 0xEC);    // #20FF6666 Error
    COLORREF input_bg = RGB(0xFF, 0xFF, 0xFF);      // DefaultColor
    COLORREF row_alt = RGB(0xF3, 0xF3, 0xF3);       // SecondaryRegionBrush（Light 覆盖为 #F3F3F3）
    // 官方背景层级：Light 下 RegionBrush=#FFFFFF，RegionBrushOpacity25=#40FFFFFF
    // 叠加在白根底上仍是纯白（Opacity 系列只在设置了背景图时才有可见差异）
    COLORREF card_bg = RGB(0xFF, 0xFF, 0xFF);      // RegionBrushOpacity25 实色
    COLORREF card_bg_soft = RGB(0xFD, 0xFD, 0xFD); // MouseOverRegionBrushOpacity10（日志卡片底）
    // 复选框底色走控件自身 Background（Light 实色白），边框 BorderColor
    COLORREF checkbox_bg = RGB(0xFF, 0xFF, 0xFF);
    // NoSelectedHighlightListBoxItemStyle：默认背景 = 卡片白；
    // IsMouseOver = MouseOverRegionBrushOpacity75(#BFEEEEEE 叠白) = #F2F2F2
    COLORREF item_bg = RGB(0xFF, 0xFF, 0xFF);
    COLORREF item_hover = RGB(0xF2, 0xF2, 0xF2);
    COLORREF trace_text = RGB(0x80, 0x80, 0x80);    // TraceLogBrush（Light=Gray）

    // ---- 亮/暗主题（官方 Res/Themes/Dark.xaml）----
    bool dark = false;

    // 官方 Dark.xaml 权威值：
    //   RegionBrush #1C1C1C、PrimaryTextBrush #E6E6E6、PrimaryBrush #326cf3、
    //   MouseOverRegionBrush #2B2B30、RegionBrushOpacity50 #7F1C1C1C
    // 无 alpha 合成能力，Opacity* 折算为等效实色（卡片略亮于背景，靠边框分层）
    static Theme make_dark() {
        Theme t;
        t.dark = true;
        t.bg             = RGB(0x1C, 0x1C, 0x1C);   // RegionBrush（官方）
        t.panel          = RGB(0x23, 0x23, 0x26);
        t.card_bg        = RGB(0x23, 0x23, 0x26);   // RegionBrushOpacity25 等效
        t.card_bg_soft   = RGB(0x1F, 0x1F, 0x22);
        t.input_bg       = RGB(0x23, 0x23, 0x26);
        t.checkbox_bg    = RGB(0x2E, 0x2E, 0x33);   // RegionBrushOpacity50 等效
        t.item_bg        = RGB(0x23, 0x23, 0x26);
        t.item_hover     = RGB(0x2B, 0x2B, 0x30);   // MouseOverRegionBrush（官方）
        t.status_running = RGB(0x1F, 0x26, 0x37);   // #20326CF3 叠 #1C1C1C
        t.status_done    = RGB(0x2B, 0x36, 0x2B);   // #2090EE90
        t.status_error   = RGB(0x37, 0x21, 0x21);   // #20FF4444
        t.trace_text     = RGB(0xA9, 0xA9, 0xA9);   // TraceLogBrush（Dark=DarkGray）
        t.hover          = RGB(0x2B, 0x2B, 0x30);
        t.row_alt        = RGB(0x1F, 0x1F, 0x22);
        t.border         = RGB(0x3E, 0x3E, 0x42);
        t.text           = RGB(0xE6, 0xE6, 0xE6);   // PrimaryTextBrush（官方）
        t.dim            = RGB(0x9E, 0x9E, 0x9E);   // SecondaryText 等效
        t.faint          = RGB(0x6B, 0x6B, 0x6B);   // ThirdlyText 等效
        t.primary        = RGB(0x32, 0x6C, 0xF3);   // PrimaryBrush（官方，亮暗同色）
        t.primary_dark   = RGB(0x2A, 0x5C, 0xD6);
        t.primary_light  = RGB(0x1E, 0x2A, 0x40);
        t.ok_light       = RGB(0x14, 0x28, 0x1A);
        t.warn_light     = RGB(0x2A, 0x24, 0x12);
        t.err_light      = RGB(0x2A, 0x16, 0x18);
        return t;
    }

    // 切换主题（dark=false 回到默认亮色）
    void set_dark(bool d) { *this = d ? make_dark() : Theme{}; }
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
    HFONT font = nullptr;       // 12px —— 官方 WPF 默认正文
    HFONT font_bold = nullptr;  // 12px 粗体
    HFONT font_small = nullptr; // 11px —— 官方辅助/次级文字
    HFONT font_title = nullptr; // 14px 粗体 —— 官方标题
    HFONT font_big = nullptr;   // 16px 粗体 —— 官方大标题

    void init_fonts() {
        if (font) return;
        // WPF 的 FontSize 单位是 DIP（96DPI 下 = px），不是磅：官方默认正文 12、辅助 11，
        // 标题 14、大标题 16。实测官方截图「使用药剂」4 字 47px、「自动战斗」4 字 46px，
        // 均对应 12px —— 早先按 12pt≈16px 换算是把 DIP 当成了磅，整页文字因此大了一圈。
        font = make_font(12, false);
        font_bold = make_font(12, true);
        font_small = make_font(11, false);
        font_title = make_font(14, true);
        font_big = make_font(16, true);
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
    // 面板里自绘的小控件（列表行删除键等）用它判定「本帧在该格按下」
    bool click_in(RECT r) const { return hovered(r) && in_.pressed; }

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

    // 官方图标集（几何取自 MaaWpfGui/Res/Styles/Basic/Geometries.xaml）
    // TaskQueueAddTaskIcon：细描边圆圈 + 加号
    void icon_add_task(RECT r, COLORREF c) {
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        int w = r.right - r.left, h = r.bottom - r.top;
        int d = w < h ? w : h;
        Pen pen(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), 1.4f);
        g.DrawEllipse(&pen, (REAL)(r.left + (w - d) / 2), (REAL)(r.top + (h - d) / 2),
                      (REAL)(d - 2), (REAL)(d - 2));
        int cx = r.left + w / 2, cy = r.top + h / 2, arm = d / 4;
        g.DrawLine(&pen, cx - arm, cy, cx + arm, cy);
        g.DrawLine(&pen, cx, cy - arm, cx, cy + arm);
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

    // 齿轮（官方 ConfigGeometry 是描边齿轮：外齿圈 + 内孔，这里等效为线框）
    void icon_gear(RECT r, COLORREF c) {
        using namespace Gdiplus;
        Graphics g(dc_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        int rad = (r.right - r.left) / 2 - 1;
        Pen pen(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), 1.5f);
        for (int k = 0; k < 8; ++k) {
            double a = k * 3.14159265 / 4.0;
            double c1 = cos(a), s1 = sin(a);
            g.DrawLine(&pen, (REAL)(cx + c1 * (rad - 2)), (REAL)(cy + s1 * (rad - 2)),
                       (REAL)(cx + c1 * (rad + 1)), (REAL)(cy + s1 * (rad + 1)));
        }
        g.DrawEllipse(&pen, (REAL)(cx - rad + 2), (REAL)(cy - rad + 2),
                      (REAL)((rad - 2) * 2), (REAL)((rad - 2) * 2));
        g.DrawEllipse(&pen, (REAL)(cx - rad / 3), (REAL)(cy - rad / 3),
                      (REAL)(rad * 2 / 3), (REAL)(rad * 2 / 3));
    }

    // hc:SplitButton / ComboBox 右端的下拉箭头
    void icon_chevron(RECT r, COLORREF c, bool up) {
        HPEN p = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ old = SelectObject(dc_, p);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        int dy = up ? -2 : 2;
        MoveToEx(dc_, cx - 3, cy - dy, nullptr);
        LineTo(dc_, cx, cy + dy);
        LineTo(dc_, cx + 3, cy - dy);
        SelectObject(dc_, old);
        DeleteObject(p);
    }
    void icon_chevron_down(RECT r, COLORREF c) { icon_chevron(r, c, false); }

    // 官方 hc:NumericUpDown 右端的菱形（上下箭头共用一条轮廓）
    void icon_diamond(RECT r, COLORREF c) {
        HPEN p = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ old = SelectObject(dc_, p);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        POINT pts[5] = { { cx, cy - 5 }, { cx + 4, cy }, { cx, cy + 5 }, { cx - 4, cy },
                         { cx, cy - 5 } };
        Polyline(dc_, pts, 5);
        SelectObject(dc_, old);
        DeleteObject(p);
    }

    // 官方 controls:TooltipBlock：贴在所在格右端的「?」圆圈（NormalOpacity=0.25）
    void circle_outline(RECT r, COLORREF c) {
        HPEN p = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ oldp = SelectObject(dc_, p);
        HGDIOBJ oldb = SelectObject(dc_, GetStockObject(NULL_BRUSH));
        Ellipse(dc_, r.left, r.top, r.right, r.bottom);
        SelectObject(dc_, oldb);
        SelectObject(dc_, oldp);
        DeleteObject(p);
    }

    void icon_tip(RECT r, COLORREF c) {
        circle_outline({ (r.left + r.right) / 2 - 5, (r.top + r.bottom) / 2 - 5,
                         (r.left + r.right) / 2 + 6, (r.top + r.bottom) / 2 + 6 }, c);
        text((r.left + r.right) / 2 - 3, (r.top + r.bottom) / 2 - 7, L"?", c, font_small);
    }

    // 官方 ComboBox：Height=30、CornerRadius=4、文本左对齐、右侧灰色下拉箭头。
    // 弹出层必须盖住本帧后续绘制的控件，所以 combo() 只登记，帧末由
    // 页面调 flush_combo_popup() 统一绘制并回收点击（选中结果下一帧生效）。
    int combo(const char* id, RECT r, const std::vector<std::wstring>& items, int cur,
              const std::wstring& placeholder = L"", bool item_icon = false) {
        std::string key = id;
        int picked = -1;
        std::string mark = key + "|";
        if (combo_pick_.rfind(mark, 0) == 0) {
            picked = atoi(combo_pick_.c_str() + mark.size());
            combo_pick_.clear();
        }
        bool hov = hovered(r);
        if (hov) hot_ = key;
        if (hov && in_.pressed) active_ = key;
        bool open = combo_open_ == key;
        if (hov && active_ == key && in_.released) {
            combo_open_ = open ? std::string() : key;
            open = !open;
        }
        round_rect(r, theme.card_bg, open ? theme.primary : theme.border, 4);
        std::wstring txt = (cur >= 0 && cur < (int)items.size()) ? items[cur] : placeholder;
        // 官方掉落材料下拉：IsEditable 收起态塞不进图片，用一个不命中的 16px 浮层贴在左端，
        // ComboBox 以 Padding=28 为它让出文本起点；「不选择」项同位画描边圆环占位。
        int tx0 = r.left + (item_icon ? 28 : 8);
        if (item_icon)
            circle_outline({ r.left + 12, (r.top + r.bottom) / 2 - 8,
                             r.left + 28, (r.top + r.bottom) / 2 + 8 }, theme.border);
        RECT tr{ tx0, r.top, r.right - 22, r.bottom };
        push_clip(tr);
        text_clipped(tr, txt, txt.empty() ? theme.trace_text : theme.text, font);
        pop_clip();
        icon_chevron_down({ r.right - 18, r.top, r.right - 4, r.bottom }, theme.dim);
        if (open) {
            combo_pop_.key = key;
            combo_pop_.box = r;
            combo_pop_.items = items;
            combo_pop_.cur = cur;
            combo_pop_.live = true;
        }
        return picked;
    }

    // 帧末调用：绘制展开中的下拉层，并处理「点选项」/「点外部关闭」
    void flush_combo_popup() {
        if (!combo_pop_.live) return;
        combo_pop_.live = false;
        RECT b = combo_pop_.box;
        int n = (int)combo_pop_.items.size();
        const int row = 26;
        RECT list{ b.left, b.bottom + 2, b.right, b.bottom + 2 + n * row };
        fill(list, theme.card_bg);
        round_rect(list, CLR_INVALID, theme.border, 4);
        bool hit = false;
        for (int i = 0; i < n; ++i) {
            RECT ir{ list.left + 1, list.top + i * row, list.right - 1, list.top + (i + 1) * row };
            bool hov = hovered(ir);
            if (hov) {
                hit = true;
                push_clip(ir);
                fill(ir, theme.item_hover);
                pop_clip();
            }
            RECT tr{ ir.left + 8, ir.top, ir.right - 8, ir.bottom };
            push_clip(tr);
            text_clipped(tr, combo_pop_.items[i],
                         i == combo_pop_.cur ? theme.primary : theme.text, font);
            pop_clip();
            if (hov && in_.pressed) {
                combo_pick_ = combo_pop_.key + "|" + std::to_string(i);
                combo_open_.clear();
                return;
            }
        }
        if (in_.pressed && !hit && !PtInRect(&b, in_.mouse)) combo_open_.clear();
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
        // Thumb：官方 PrimaryTextBrush，圆角 2；HandyControl 的 ScrollBarBaseStyle
        // 默认 Opacity=0，只有指针进入滚动区才显形（否则静态界面里像一根多余的竖线）
        if (!hov) return;
        fill({ tx, scroll_rect_.top, tx + 4, scroll_rect_.bottom }, RGB(0xF0, 0xF0, 0xF0));
        round_rect({ tx, ty, tx + 4, ty + thumb_h }, RGB(0x42, 0x42, 0x42), CLR_INVALID, 2);
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

    // 官方 hc:ButtonGroup：整组只有一条外框（CornerRadius=4，仅首尾两端圆角），
    // 相邻按钮之间是 1px 分隔线，而不是各自一个圆角矩形。
    // widths[i] <= 0 表示均分剩余宽度；cells 非空时回填每格矩形。
    // 返回被点击的格下标，未点击返回 -1。
    int button_group(const char* id, RECT r, const std::vector<std::wstring>& labels,
                     const std::vector<int>& widths, int style = 0,
                     std::vector<RECT>* cells = nullptr) {
        int n = (int)labels.size();
        if (n <= 0) return -1;
        std::vector<RECT> cs(n);
        int fixed = 0, flex = 0;
        for (int i = 0; i < n; ++i) {
            int w = i < (int)widths.size() ? widths[i] : 0;
            if (w > 0) { fixed += w; } else { ++flex; }
        }
        int total = r.right - r.left;
        int each = flex > 0 ? (std::max)(1, (total - fixed) / flex) : 0;
        int x = r.left;
        for (int i = 0; i < n; ++i) {
            int w = (i < (int)widths.size() && widths[i] > 0) ? widths[i] : each;
            cs[i] = { x, r.top, x + w, r.bottom };
            x += w;
        }
        if (cells) *cells = cs;

        COLORREF base_fill = style == 1 ? theme.primary : theme.card_bg;
        COLORREF hov_fill = style == 1 ? theme.primary_dark : theme.hover;
        COLORREF txt_c = style == 1 ? RGB(255, 255, 255) : theme.text;

        // 外框先画（含底色），再叠 hover 底色，最后分隔线与文字
        round_rect(r, base_fill, theme.border, 4);

        int clicked = -1;
        for (int i = 0; i < n; ++i) {
            bool hov = hovered(cs[i]);
            std::string iid = std::string(id) + "#" + std::to_string(i);
            if (hov) hot_ = iid;
            if (hov && in_.pressed) active_ = iid;
            if (hov && active_ == iid && in_.released) clicked = i;
            if (!hov) continue;
            push_clip(r);                       // hover 底色绝不越出整组外框
            fill(cs[i], hov_fill);
            pop_clip();
        }
        for (int i = 1; i < n; ++i)
            fill({ cs[i].left, r.top + 1, cs[i].left + 1, r.bottom - 1 }, theme.border);
        for (int i = 0; i < n; ++i) {
            if (cs[i].right <= cs[i].left) continue;
            push_clip(cs[i]);
            text_clipped(cs[i], labels[i], txt_c, font);
            pop_clip();
        }
        return clicked;
    }

    // 官方 hc:NumericUpDown：一个圆角框，右端上下箭头在框内（不是 -/+ 两个按钮）。
    // 标签为空时文本顶满左边界，窄格（面板右列 ~110px）也能正常显示。
    void number_input(const char* id, RECT r, const std::wstring& label,
                      int& value, int step = 1, int min_v = 0, int max_v = 9999) {
        int lw = 0;
        if (!label.empty()) {
            lw = text_size(label, font).cx + 8;
            if (lw > 150) lw = 150;
            text(r.left, r.top + (r.bottom - r.top - 17) / 2, label, theme.text, font);
        }
        const int spin = 16;
        bool narrow = (r.right - r.left) < 60;
        RECT box{ r.left + lw, r.top, r.right, r.bottom };
        std::string edit = std::to_string(value);
        text_input(id, box, edit, nullptr, lw == 0 ? 1 : 0);
        int nv = value;
        try { nv = std::stoi(edit); } catch (...) { nv = value; }
        if (nv < min_v) nv = min_v;
        if (nv > max_v) nv = max_v;
        value = nv;
        if (narrow) return;
        int mid = (r.top + r.bottom) / 2;
        RECT up{ r.right - spin, r.top + 1, r.right - 1, mid };
        RECT dn{ r.right - spin, mid, r.right - 1, r.bottom - 1 };
        std::string uid = std::string(id) + "-up", did = std::string(id) + "-dn";
        bool hup = hovered(up), hdn = hovered(dn);
        if (hup) hot_ = uid;
        if (hdn) hot_ = did;
        // 官方样式：右端只有一个菱形（上下箭头共用轮廓），无分隔线、无 hover 底色
        icon_diamond({ r.right - spin, r.top, r.right - 2, r.bottom },
                     (hup || hdn) ? theme.primary : theme.dim);
        if (hup && in_.pressed) value = (value + step > max_v) ? max_v : value + step;
        if (hdn && in_.pressed) value = (value - step < min_v) ? min_v : value - step;
    }

    // 提示行（对应 MAA 的 Tip 文本，三级文字色）
    void tip_line(int x, int y, int right, const std::wstring& text) {
        RECT save = clip_;
        text_clipped({ x, y, right, y + 16 }, text, theme.faint, font_small);
        (void)save;
    }

    // 三态复选框（对应 MAA 官方语义）：
    //   左键 = 开/关切换；右键 = 切换「仅生效一次」（该次运行有效，不写入配置）
    bool checkbox3(const char* id, RECT r, const std::wstring& label, CheckState& st,
                   int* label_lines = nullptr) {
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
        // 官方：未选中 = 白底 + BorderBrush(#E0E0E0)；选中 = DarkPrimaryBrush 实底 + 白勾。
        // null（仅一次）态复选框本身没有专属配色，视觉上等同未选中。
        COLORREF fill_c = theme.checkbox_bg, line_c = theme.border;
        if (st == CheckState::On) { fill_c = theme.primary; line_c = theme.primary; }
        round_rect(br, fill_c, line_c, 3);
        if (st == CheckState::On) {
            HPEN p = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ old = SelectObject(dc_, p);
            MoveToEx(dc_, br.left + 3, br.top + 8, nullptr);
            LineTo(dc_, br.left + 6, br.top + 12);
            LineTo(dc_, br.left + 13, br.top + 4);
            SelectObject(dc_, old);
            DeleteObject(p);
        }
        // 官方 CheckBox 文字 TextWrapping=Wrap：窄列下长标签换行显示，不被裁切
        // null 态文字色 = TraceLogBrush（Light #808080），这是官方唯一的「仅一次」提示
        COLORREF label_c = (st == CheckState::Once) ? theme.trace_text : theme.text;
        RECT save = clip_;
        int tx = r.left + box + 8;
        int avail = r.right - tx;
        SIZE full = text_size(label, font);
        int tpad = ((r.bottom - r.top) - text_size(L"Ag", font).cy) / 2;
        if (tpad < 0) tpad = 0;
        int lines = 1;
        if (full.cx <= avail) {
            text(tx, r.top + tpad, label, label_c, font);
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
            text(tx, r.top + tpad - 2, line1, label_c, font);
            if (!line2.empty()) {
                text(tx, r.top + tpad + 16, line2, label_c, font);
                lines = 2;
            }
        }
        if (label_lines) *label_lines = lines;
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

    void text_input(const char* id, RECT r, std::string& value, const wchar_t* hint = nullptr,
                    int align = 0) {
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
        int ty = r.top + ((r.bottom - r.top) - text_size(L"Ag", font).cy) / 2;
        if (w.empty() && hint) {
            if (align == 1) {
                SIZE hs = text_size(hint, font);
                text((r.left + r.right - hs.cx) / 2, ty, hint, RGB(170, 178, 190), font);
            } else {
                text(r.left + 8, ty, hint, RGB(170, 178, 190), font);
            }
        } else {
            // 水平滚动以露出光标
            int maxw = r.right - r.left - 14;
            SIZE sz = text_size(w, font);
            int x = (align == 1) ? (r.left + r.right - sz.cx) / 2 : r.left + 7;
            if (sz.cx > maxw) x = r.left + 7 - (sz.cx - maxw) - 8;
            text(x, ty, w, theme.text, font);
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
    // 官方 hc:ButtonGroup（RadioButton 组）：整组一条外框、相邻格共用 1px 分隔线，
    // 选中格 = 白底 + PrimaryBrush 描边 + 蓝字（不是蓝底白字）。
    // vertical=true 对应官方 Orientation="Vertical"（如「刷取数量/目标库存」）；
    // big=true 用正文字号，对应直接放 RadioButton 文本的场合。
    int option_strip(const char* id, RECT r, const std::vector<std::wstring>& opts, int cur,
                     bool vertical = false, bool big = false) {
        int n = (int)opts.size();
        if (n <= 0) return -1;
        int span = vertical ? (r.bottom - r.top) : (r.right - r.left);
        int w = span / n;
        std::vector<RECT> cs(n);
        for (int i = 0; i < n; ++i) {
            int a = r.left + i * w, b = (i == n - 1) ? r.right : r.left + (i + 1) * w;
            int c1 = r.top + i * w, c2 = (i == n - 1) ? r.bottom : r.top + (i + 1) * w;
            cs[i] = vertical ? RECT{ r.left, c1, r.right, c2 } : RECT{ a, r.top, b, r.bottom };
        }

        round_rect(r, theme.card_bg, theme.border, 4);
        int clicked = -1;
        for (int i = 0; i < n; ++i) {
            bool hov = hovered(cs[i]);
            if (hov) hot_ = id + std::to_string(i);
            if (hov && in_.pressed) active_ = id + std::to_string(i);
            if (hov && active_ == id + std::to_string(i) && in_.released) clicked = i;
            if (!hov || i == cur) continue;
            push_clip(r);
            fill(cs[i], theme.hover);
            pop_clip();
        }
        for (int i = 1; i < n; ++i) {
            if (vertical)
                fill({ r.left + 1, cs[i].top, r.right - 1, cs[i].top + 1 }, theme.border);
            else
                fill({ cs[i].left, r.top + 1, cs[i].left + 1, r.bottom - 1 }, theme.border);
        }
        if (cur >= 0 && cur < n) {
            RECT sr = cs[cur];
            sr.left = (cur == 0) ? r.left + 1 : sr.left + 1;
            sr.right = (cur == n - 1) ? r.right - 1 : sr.right - 1;
            sr.top = (cur == 0) ? r.top + 1 : sr.top + 1;
            sr.bottom = (cur == n - 1) ? r.bottom - 1 : sr.bottom - 1;
            round_rect(sr, theme.card_bg, theme.primary, 3);
        }
        for (int i = 0; i < n; ++i) {
            COLORREF txt = (i == cur) ? theme.primary : theme.text;
            HFONT f = big ? font : font_small;
            // 一格放不下的长选项（MaaEnd 的 case 名常常是英文全名）截成省略号，
            // 否则居中的文字会左右溢出、压到邻格文字上
            int avail = (cs[i].right - cs[i].left) - 8;
            std::wstring label = opts[i];
            SIZE sz = text_size(label, f);
            if (sz.cx > avail) {
                label = ellipsize(label, avail, f);
                sz = text_size(label, f);
            }
            // 官方 hc:ButtonGroup 每格内容居中；左对齐会让首字压住 1px 选中边框
            push_clip(cs[i]);
            text(cs[i].left + ((cs[i].right - cs[i].left) - sz.cx) / 2,
                 cs[i].top + ((cs[i].bottom - cs[i].top) - sz.cy) / 2, label, txt, f);
            pop_clip();
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
    // clip_ 只管文字（ExtTextOut 的 ETO_CLIPPED）；GDI+ 的 FillPath/DrawPath
    // 走的是 HDC 自身的裁剪区域，所以必须同步 SelectClipRgn，否则滚动区/面板
    // 里的控件底色会画到裁剪区之外（例如压住列底的「常规/高级设置」条）。
    void push_clip(RECT r) {
        RECT inter;
        IntersectRect(&inter, &clip_, &r);
        clip_stack_.push_back(clip_);
        int save = SaveDC(dc_);
        if (save) IntersectClipRect(dc_, inter.left, inter.top, inter.right, inter.bottom);
        dc_save_.push_back(save);
        clip_ = inter;
    }
    void pop_clip() {
        if (!clip_stack_.empty()) {
            clip_ = clip_stack_.back();
            clip_stack_.pop_back();
            if (!dc_save_.empty()) {
                int save = dc_save_.back();
                dc_save_.pop_back();
                if (save) RestoreDC(dc_, save);
            }
        }
    }
    RECT clip() const { return clip_; }

    // ------------------------------------------------------------ 渲染目标
    // 切换亮/暗主题（官方界面主题设置；影响所有控件配色）
    void set_theme_mode(bool dark) { theme.set_dark(dark); }

    void set_dc(HDC dc, RECT client) {
        dc_ = dc;
        clip_ = client;
        clip_stack_.clear();
        dc_save_.clear();
        // 文字背景透明 + 高质量拉伸（缩放/位图时不出现块状）
        SetBkMode(dc_, TRANSPARENT);
        SetStretchBltMode(dc_, HALFTONE);
        SetBrushOrgEx(dc_, 0, 0, nullptr);
    }

    Input input() const { return in_; }

private:
    static HFONT make_font(int px, bool bold) {
        // CLEARTYPE_QUALITY：系统「字体平滑」开着时按子像素渲染，与 WPF 一致。
        // 之前用 ANTIALIASED_QUALITY 强制灰度抗锯齿，小字号下发虚，是「看不清」的主因。
        // OUT_DEFAULT_PRECIS：OUT_DEVICE_PRECIS 在内存 DC 上可能挑到点阵字体。
        return CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                           L"Microsoft YaHei");
    }

    HDC dc_ = nullptr;
    RECT clip_{ 0, 0, 0, 0 };
    std::vector<RECT> clip_stack_;
    std::vector<int> dc_save_;
    Input in_;
    std::string hot_;
    std::string r_active_;   // 右键活动控件
    std::string active_;
    std::string focus_;
    // 下拉框：展开中的控件、待登记的弹出层、帧末回收的点击
    struct ComboPop {
        std::string key;
        RECT box{};
        std::vector<std::wstring> items;
        int cur = -1;
        bool live = false;
    };
    std::string combo_open_;
    std::string combo_pick_;
    ComboPop combo_pop_;
    bool wheel_used_ = false;
    // 滚动
    RECT scroll_rect_{};
    int scroll_content_h_ = 0;
    const char* scroll_id_ = nullptr;
    float* scroll_y_ = nullptr;
};

} // namespace gui
