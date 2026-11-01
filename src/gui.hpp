// MaaHub 零依赖立即模式 GUI 框架（Win32 + GDI 双缓冲）
// 特性：按钮/复选框/文本输入/选项条/滚动区域/卡片面板、CJK 字体、剪裁栈、
//       热激活焦点跟踪、后台线程数据快照友好（渲染只读快照）
#pragma once
#include <windows.h>
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

    void round_rect(RECT r, COLORREF fill_c, COLORREF border_c, int radius) {
        HBRUSH b = CreateSolidBrush(fill_c);
        HPEN p = CreatePen(PS_INSIDEFRAME, 1, border_c);
        HGDIOBJ ob = SelectObject(dc_, b);
        HGDIOBJ op = SelectObject(dc_, p);
        RoundRect(dc_, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(dc_, ob);
        SelectObject(dc_, op);
        DeleteObject(b);
        DeleteObject(p);
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
    void end_scroll() { pop_clip(); }

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

        COLORREF fill = theme.panel, border = theme.border, txt = theme.text;
        if (style == 1) { fill = theme.primary; border = theme.primary; txt = RGB(255, 255, 255); }
        else if (style == 2) { fill = RGB(254, 242, 242); border = theme.err; txt = theme.err; }
        else if (style == 3) { fill = RGB(255, 248, 235); border = theme.warn; txt = RGB(180, 120, 10); }
        if (!enabled) { fill = RGB(245, 246, 248); border = theme.border; txt = RGB(160, 168, 178); }
        else if (hov) { fill = (style == 1) ? theme.primary_dark : theme.hover; }

        round_rect(r, fill, active_ == id && hov ? theme.primary : border, 6);
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
        RECT minus_b{ r.right - 72, r.top, r.right - 40, r.bottom };
        RECT plus_b{ r.right - 36, r.top, r.right - 4, r.bottom };
        if (button((std::string(id) + "-m").c_str(), minus_b, L"-", 0)) {
            value = (value - step < min_v) ? min_v : value - step;
        }
        if (button((std::string(id) + "-p").c_str(), plus_b, L"+", 0)) {
            value = (value + step > max_v) ? max_v : value + step;
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
        int box = 16;
        RECT br{ r.left, r.top + 1, r.left + box, r.top + 1 + box };
        COLORREF fill_c = theme.panel, line_c = RGB(180, 188, 200);
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
        RECT save = clip_;
        text(r.left + box + 8, r.top, label, theme.text, font);
        if (st == CheckState::Once) {
            text(r.left + box + 8 + text_size(label, font).cx + 6, r.top,
                 L"（仅一次）", theme.primary, font_small);
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
        int box = 16;
        RECT br{ r.left, r.top + 1, r.left + box, r.top + 1 + box };
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
        round_rect(r, theme.input_bg, focused ? theme.primary : theme.border, 5);
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
        round_rect(r, theme.panel, theme.border, 10);
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
    }

    Input input() const { return in_; }

private:
    static HFONT make_font(int px, bool bold) {
        return CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
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
    const char* scroll_id_ = nullptr;
    float* scroll_y_ = nullptr;
};

} // namespace gui
