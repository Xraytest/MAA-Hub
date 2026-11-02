// MAA 官方任务设置面板（1:1 复刻 MaaWpfGui/Views/UserControl/TaskQueue/*.xaml）
// 每个面板对应官方的一个 TaskUserControl，标签取自官方 zh-cn 本地化。
#pragma once
#include <map>
#include <string>
#include <vector>

#include "gui.hpp"
#include "json.hpp"

namespace hub {

using gui::CheckState;

// 面板绘制上下文：把 Gui + 布局矩形 + 参数读写传进去
using ParamMap = std::map<std::string, mj::Value>;

struct PanelCtx {
    gui::Gui& g;
    int x, y, right;      // 内容区左边界、当前 y、右边界
    ParamMap& params;     // 任务参数（读/写）
    const std::string& exec_id;
    const std::string& task;

    // 便捷读取
    bool get_bool(const std::string& k, bool def = false) const {
        auto it = params.find(k);
        if (it == params.end()) return def;
        const mj::Value& v = it->second;
        return (v.is_bool() || v.is_number()) ? v.as_bool() : def;
    }
    void set_bool(const std::string& k, bool v) { params[k] = mj::Value(v); }
    bool get_once(const std::string& k) const {
        auto it = params.find(std::string("__once_") + k);
        return it != params.end() && it->second.as_bool();
    }
    void set_once(const std::string& k, bool v) {
        if (v) params[std::string("__once_") + k] = mj::Value(true);
        else params.erase(std::string("__once_") + k);
    }
    int get_int(const std::string& k, int def = 0) const {
        auto it = params.find(k);
        if (it == params.end()) return def;
        const mj::Value& v = it->second;
        return v.is_number() ? (int)v.as_int() : def;
    }
    void set_int(const std::string& k, int v) { params[k] = mj::Value((int64_t)v); }
    std::string get_str(const std::string& k, const std::string& def = "") const {
        auto it = params.find(k);
        if (it == params.end()) return def;
        const mj::Value& v = it->second;
        return v.is_string() ? v.as_string() : def;
    }
    void set_str(const std::string& k, const std::string& v) { params[k] = mj::Value(v); }

    // 用官方 catalog 声明的 default 预填充尚未设置的参数
    // （官方面板打开时就应显示默认值，而不是空白/0）
    void apply_defaults(const std::vector<mj::Value>* specs) {
        if (!specs) return;
        for (auto& p : *specs) {
            std::string key = p.get_str("name", "");
            if (key.empty() || !p.has("default")) continue;
            if (params.find(key) != params.end()) continue;
            mj::Value d = p["default"];
            // MAA 用 INT_MAX 表示"无限"；归一化为 -1，UI 显示"无限"
            if (d.is_number() && d.as_int() >= 2147483647) {
                params[key] = mj::Value((int64_t)-1);
            } else {
                params[key] = d;
            }
        }
    }
};

// ---- 通用小部件 ----
// 三态勾选：左键开/关，右键「仅生效一次」（官方语义，运行后复位、不写配置）
inline void pc_check(PanelCtx& c, const std::string& key, const std::wstring& label,
                     const std::wstring& tip = L"") {
    CheckState st = c.get_once(key) ? CheckState::Once
                                    : (c.get_bool(key) ? CheckState::On : CheckState::Off);
    // 官方 CheckBox 规格：Height=30 Margin=6
    RECT r{ c.x, c.y + 3, c.right, c.y + 3 + 24 };
    c.g.checkbox3((c.exec_id + ":" + c.task + ":" + key).c_str(), r, label, st);
    c.set_bool(key, st == CheckState::On);
    c.set_once(key, st == CheckState::Once);
    c.y += 30;
    if (!tip.empty()) {
        c.g.tip_line(c.x + 24, c.y, c.right, tip);
        c.y += 16;
    }
}

inline void pc_number(PanelCtx& c, const std::string& key, const std::wstring& label,
                      int def = 0, int step = 1, int lo = 0, int hi = 9999) {
    int v = c.get_int(key, def);
    // 窄列：标签单独一行，控件下一行（官方 220px 列宽就是这样竖排）
    c.g.text(c.x, c.y, label, c.g.theme.dim, c.g.font_small);
    c.y += 20;
    RECT r{ c.x, c.y, c.right, c.y + 26 };
    c.g.number_input((c.exec_id + ":" + c.task + ":" + key).c_str(), r, L"", v, step, lo, hi);
    c.set_int(key, v);
    c.y += 32;
}

inline void pc_text(PanelCtx& c, const std::string& key, const std::wstring& label,
                    const std::wstring& hint = L"") {
    std::string v = c.get_str(key);
    c.g.text(c.x, c.y, label, c.g.theme.dim, c.g.font_small);
    c.y += 20;
    RECT r{ c.x, c.y, c.right, c.y + 26 };
    c.g.text_input((c.exec_id + ":" + c.task + ":" + key).c_str(), r, v,
                   hint.empty() ? nullptr : hint.c_str());
    c.set_str(key, v);
    c.y += 32;
}

// 作战次数：-1 = 无限（MAA 官方语义），显示"无限"
inline void pc_times(PanelCtx& c, const std::string& key, const std::wstring& label) {
    int v = c.get_int(key, -1);
    if (v >= 2147483647) { v = -1; c.set_int(key, -1); }   // 兼容历史脏值
    bool inf = (v < 0);
    // 窄列竖排：标签 / 输入框+± / 无限勾选，各占一行
    c.g.text(c.x, c.y, label, c.g.theme.dim, c.g.font_small);
    c.y += 20;
    int avail = c.right - c.x;
    std::string edit = inf ? std::string() : std::to_string(v);
    if (avail >= 150) {
        // 有足够宽度：输入框 + ± 按钮同行
        RECT ir{ c.x, c.y, c.right - 84, c.y + 26 };
        c.g.text_input((c.exec_id + ":" + c.task + ":" + key).c_str(), ir, edit,
                       inf ? L"\u65e0\u9650" : nullptr);
        RECT minus_b{ c.right - 80, c.y, c.right - 46, c.y + 26 };
        RECT plus_b{ c.right - 42, c.y, c.right - 8, c.y + 26 };
        if (c.g.button((c.exec_id + ":" + c.task + ":" + key + "-m").c_str(),
                       minus_b, L"-", 0)) {
            if (!inf) v = (v - 1 < 0) ? 0 : v - 1;
        }
        if (c.g.button((c.exec_id + ":" + c.task + ":" + key + "-p").c_str(),
                       plus_b, L"+", 0)) {
            if (!inf) v += 1;
        }
        c.y += 30;
    } else {
        // 极窄：只放输入框
        RECT ir{ c.x, c.y, c.right, c.y + 26 };
        c.g.text_input((c.exec_id + ":" + c.task + ":" + key).c_str(), ir, edit,
                       inf ? L"\u65e0\u9650" : nullptr);
        c.y += 30;
    }
    if (!inf) {
        int nv = v;
        try { nv = std::stoi(edit); } catch (...) { nv = v; }
        if (nv < 0) nv = 0;
        v = nv;
    }
    CheckState inf_st = inf ? CheckState::On : CheckState::Off;
    RECT inf_r{ c.x, c.y, c.x + 90, c.y + 22 };
    c.g.checkbox3((c.exec_id + ":" + c.task + ":" + key + "-inf").c_str(),
                  inf_r, L"\u65e0\u9650", inf_st);
    c.set_int(key, inf_st == CheckState::On ? -1 : v);
    c.y += 30;
}


// 横向单选（选项条）
inline void pc_radio(PanelCtx& c, const std::string& key,
                     const std::vector<std::wstring>& opts, int def = 0) {
    int cur = c.get_int(key, def);
    RECT r{ c.x, c.y, c.right, c.y + 24 };
    int hit = c.g.option_strip((c.exec_id + ":" + c.task + ":" + key).c_str(), r, opts, cur);
    if (hit >= 0) cur = hit;
    c.set_int(key, cur);
    c.y += 30;
}

// ================================================================ 各任务面板
// 每个函数把官方面板的控件按顺序画出；返回 false 表示该任务无专属面板。

// ---- StartUp：开始唤醒 ----
inline bool panel_startup(PanelCtx& c) {
    // 官方 StartUpTaskUserControl：12 个控件
    c.g.group_title(c.x, c.y, L"\u8d26\u53f7\u4e0e\u542f\u52a8");
    c.y += 20;
    pc_check(c, "account_name_set", L"\u8d26\u53f7\u5207\u6362",
             L"\u542f\u7528\u540e\u6309\u6267\u884c\u5668\u7ed1\u5b9a\u7684\u8d26\u53f7\u81ea\u52a8\u5207\u6362");
    pc_check(c, "start_game_enabled", L"\u662f\u5426\u542f\u52a8\u5ba2\u6237\u7aef");
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u5ba2\u6237\u7aef\u7c7b\u578b");
    c.y += 20;
    pc_radio(c, "client_type", { L"\u5b98\u670d", L"Bilibili", L"txwy", L"YoStarEN",
                                 L"YoStarJP", L"YoStarKR" }, 0);
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u8fde\u63a5");
    c.y += 20;
    pc_check(c, "auto_detect", L"\u81ea\u52a8\u68c0\u6d4b\u8fde\u63a5");
    pc_check(c, "always_auto_detect", L"\u6bcf\u6b21\u91cd\u65b0\u68c0\u6d4b");
    c.y += 4;
    pc_text(c, "adb_path", L"ADB \u8def\u5f84", L"\u7559\u7a7a\u4f7f\u7528\u9ed8\u8ba4");
    pc_text(c, "connect_address", L"\u8fde\u63a5\u5730\u5740",
            L"\u5982 127.0.0.1:16384\uff1b\u7559\u7a7a\u4e3a\u81ea\u52a8");
    c.y += 4;
    c.g.group_title(c.x, c.y, L"\u6a21\u62df\u5668\u589e\u5f3a");
    c.y += 20;
    pc_check(c, "mumu_extras", L"\u542f\u7528 MuMu \u622a\u56fe\u589e\u5f3a");
    pc_check(c, "mumu_bridge", L"MuMu \u7f51\u7edc\u6865\u63a5\u6a21\u5f0f");
    pc_check(c, "ld_extras", L"\u542f\u7528 LD \u622a\u56fe\u589e\u5f3a");
    pc_check(c, "ld_manual_index", L"\u624b\u52a8\u586b\u5199 \u300c\u96c5\u5149\u6f0f\u6307\u6570\u300d");
    c.y += 4;
    c.g.group_title(c.x, c.y, L"\u622a\u56fe\u4e0e\u8f93\u5165");
    c.y += 20;
    pc_radio(c, "screencap_method", { L"\u9ed8\u8ba4", L"\u540e\u53f0", L"\u76f4\u8fde" }, 0);
    pc_radio(c, "touch_mode", { L"\u9ed8\u8ba4", L"MuMu", L"\u5176\u4ed6" }, 0);
    return true;
}


// ---- Fight：理智作战 ----
inline bool panel_fight(PanelCtx& c) {
    // 官方 FightTaskUserControl：19 个控件，按官方顺序与分组复刻
    c.g.group_title(c.x, c.y, L"\u7406\u667a\u4e0e\u7269\u6599");
    c.y += 20;
    pc_check(c, "use_medicine", L"\u4f7f\u7528\u836f\u5242",
             L"\u8be5\u9009\u9879\u53f3\u952e\u9009\u4e2d\u65f6\u4ec5\u751f\u6548\u4e00\u6b21");
    pc_check(c, "use_stone", L"\u4f7f\u7528\u6e90\u77f3",
             L"\u8be5\u9009\u9879\u4ec5\u751f\u6548\u4e00\u6b21");
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u5173\u5361\u4e0e\u6b21\u6570");
    c.y += 20;
    pc_text(c, "stage", L"\u5173\u5361", L"\u5982 1-7\uff1b\u7559\u7a7a\u4e3a \u5f53\u524d/\u4e0a\u6b21");
    pc_check(c, "use_custom_stage", L"\u624b\u52a8\u8f93\u5165\u5173\u5361\u540d");
    c.y += 4;

    // 指定次数 / 指定材料 二选一（官方 RadioButton 组）
    pc_radio(c, "fight_mode", { L"\u6307\u5b9a\u6b21\u6570", L"\u6307\u5b9a\u6750\u6599" }, 0);
    if (c.get_int("fight_mode", 0) == 0) {
        pc_times(c, "times", L"\u4f5c\u6218\u6b21\u6570");
    } else {
        // 指定材料：刷取数量 / 目标库存
        pc_radio(c, "drop_mode", { L"\u5237\u53d6\u6570\u91cf", L"\u76ee\u6807\u5e93\u5b58" }, 0);
        pc_text(c, "drop_item", L"\u6750\u6599\u540d\u79f0", L"\u5982 \u\u8d28\u6599");
        pc_number(c, "drop_quantity", L"\u6570\u91cf", 0, 1, 0, 999999);
    }
    pc_radio(c, "series", { L"\u00d71", L"\u00d72", L"\u00d73", L"\u00d74", L"\u00d75", L"\u00d76" }, 3);
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u884c\u4e3a");
    c.y += 20;
    pc_check(c, "dr_grandet", L"\u535a\u90ce\u53f0\u6a21\u5f0f",
             L"\u7406\u667a\u4e3a 0 \u65f6\u4ecd\u8bd5\u8fdb\u5165\u5173\u5361\uff0c\u5229\u7528\u5bbf\u820d\u6062\u590d");
    pc_check(c, "use_expire_medicine", L"\u65e0\u9650\u5403 N \u5929\u5185\u8fc7\u671f\u7684\u7406\u667a\u836f");
    pc_number(c, "medicine_expire_days", L"\u8fc7\u671f\u5929\u6570\u9608\u503c", 7, 1, 1, 30);
    pc_check(c, "use_weekly_expire_medicine",
             L"\u6d3b\u52a8\u7ed3\u675f\u524d 48H \u5403\u5f53\u5468\u8fc7\u671f\u7406\u667a\u836f");
    pc_check(c, "hide_series", L"\u9690\u85cf\u4ee3\u7406\u500d\u7387");
    pc_check(c, "allow_stone_save", L"\u5141\u8bb8\u4f7f\u7528\u6e90\u77f3\u4fdd\u5b58\u72b6\u6001");
    pc_check(c, "hide_unavailable_stage", L"\u4e0b\u62c9\u6846\u4e2d\u9690\u85cf\u5f53\u65e5\u4e0d\u5f00\u653e\u5173\u5361");
    c.y += 4;
    // 过期活动关卡重置为（官方 ComboBox）
    c.g.text(c.x, c.y + 4, L"\u8fc7\u671f\u6d3b\u52a8\u5173\u5361\u91cd\u7f6e\u4e3a",
             c.g.theme.dim, c.g.font_small);
    c.y += 22;
    pc_radio(c, "stage_reset_mode", { L"\u4e0d\u91cd\u7f6e", L"\u5f53\u524d/\u4e0a\u6b21", L"\u6307\u5b9a\u5173\u5361" }, 0);
    c.y += 4;
    pc_check(c, "use_alternate_stage", L"\u4f7f\u7528\u5907\u9009\u5173\u5361");
    pc_check(c, "report_to_penguin", L"\u4e0a\u62a5\u6389\u843d\u7edf\u8ba1\u5230\u4f01\u9e45\u7269\u6d41");
    pc_check(c, "use_weekly_plan", L"\u542f\u7528\u5468\u8ba1\u5212");
    return true;
}


// ---- Infrast：基建换班 ----
inline bool panel_infrast(PanelCtx& c) {
    // 官方 InfrastTaskUserControl：8 个控件
    c.g.group_title(c.x, c.y, L"\u57fa\u5efa\u6a21\u5f0f");
    c.y += 20;
    pc_radio(c, "mode", { L"\u9ed8\u8ba4\u57fa\u5efa", L"\u81ea\u5b9a\u4e49\u57fa\u5efa",
                          L"\u8f6e\u6362\u57fa\u5efa" }, 0);
    // 官方 ComboBox「内置配置」：自定义基建时选择的排班方案
    pc_radio(c, "builtin_plan", { L"\u9ed8\u8ba4\u65b9\u6848",
                                  L"\u4e09\u7279\u59d3\u5168\u9760\u5e73\u53f0",
                                  L"\u4e09\u5de5\u4f5c\u5ba4\u65b9\u6848",
                                  L"\u4f20\u68af\u54e5\u65b9\u6848" }, 0);
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u65e0\u4eba\u673a");
    c.y += 20;
    pc_radio(c, "drones_idx", { L"\u4e0d\u4f7f\u7528", L"\u8d38\u6613\u7ad9",
                                L"\u5236\u9020\u7ad9", L"\u53d1\u7535\u7ad9",
                                L"\u529e\u516c\u5ba4" }, 0);
    pc_number(c, "dorm_threshold", L"\u5bbf\u820d\u5fc3\u60c5\u9608\u503c", 30, 5, 0, 100);
    c.y += 6;

    // 官方 ListBox「标签顺序可拖动」：换班设施多选 + 可拖拽排序
    c.g.group_title(c.x, c.y, L"\u6362\u73ed\u8bbe\u65bd");
    c.y += 20;
    c.g.tip_line(c.x, c.y, c.right, L"\u6807\u7b7e\u987a\u5e8f\u53ef\u62d6\u52a8");
    c.y += 18;
    const wchar_t* facs[] = { L"\u63a7\u5236\u4e2d\u67bc", L"\u5236\u9020\u7ad9",
                              L"\u8d38\u6613\u7ad9", L"\u53d1\u7535\u7ad9",
                              L"\u4f1a\u5ba2\u5ba4", L"\u529e\u516c\u5ba4",
                              L"\u8bad\u7ec3\u5ba4", L"\u5de5\u538b\u5e73\u53f0" };
    // 单列排布（右栏宽度有限，两列会溢出到信息条外）
    int half = (c.right - c.x) - 8;
    for (int k = 0; k < 8; ++k) {
        int col = 0;
        int row = k;
        int fx = c.x + col * (half + 12);
        int fy = c.y + row * 24;
        std::string key = std::string("facility_") + std::to_string(k);
        bool v = c.get_bool(key);
        CheckState st = v ? CheckState::On : CheckState::Off;
        RECT fr{ fx, fy, fx + half, fy + 22 };
        c.g.checkbox3((c.exec_id + ":" + c.task + ":" + key).c_str(), fr, facs[k], st);
        c.set_bool(key, st == CheckState::On);
    }
    c.y += 8 * 24 + 6;
    // 官方 全选 / 清空
    RECT all_b{ c.x, c.y, c.x + 100, c.y + 26 };
    RECT clr_b{ c.x + 108, c.y, c.x + 208, c.y + 26 };
    if (c.g.button((c.exec_id + ":infrast-all").c_str(), all_b, L"\u5168\u9009", 0)) {
        for (int k = 0; k < 8; ++k) c.set_bool(std::string("facility_") + std::to_string(k), true);
    }
    if (c.g.button((c.exec_id + ":infrast-clear").c_str(), clr_b, L"\u6e05\u7a7a", 0)) {
        for (int k = 0; k < 8; ++k) c.set_bool(std::string("facility_") + std::to_string(k), false);
    }
    c.y += 34;

    c.g.group_title(c.x, c.y, L"\u4f1a\u5ba2\u5ba4\u4e0e\u5bbf\u820d");
    c.y += 20;
    pc_check(c, "reception_message_board", L"\u4f1a\u5ba2\u5ba4\u4fe1\u606f\u677f\u6536\u53d6\u4fe1\u7528");
    pc_check(c, "reception_clue_exchange", L"\u8fdb\u884c\u7ebf\u7d22\u4ea4\u6d41");
    pc_check(c, "reception_send_clue", L"\u8d60\u9001\u7ebf\u7d22");
    pc_check(c, "dorm_trust", L"\u5bbf\u820d\u7a7a\u4f59\u4f4d\u7f6e\u8e8f\u4fe1\u8d56");
    pc_check(c, "dorm_filter_not_stationed", L"\u4e0d\u5c06\u5df2\u8fdb\u9a7b\u7684\u5e72\u5458\u653e\u5165\u5bbf\u820d");
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u8de8\u8bbe\u65bd\u7ec4\u5408");
    c.y += 20;
    pc_check(c, "use_pinus_sylvestris", L"\u4f7f\u7528\u7ea2\u677e\u9a91\u58eb\u56e2\u8de8\u8bbe\u65bd\u7ec4\u5408");
    pc_check(c, "use_perception_information", L"\u4f7f\u7528\u611f\u77e5\u4fe1\u606f\u8de8\u8bbe\u65bd\u7ec4\u5408");
    pc_check(c, "use_worldly_plight", L"\u4f7f\u7528\u4eba\u95f4\u70df\u706b\u8de8\u8bbe\u65bd\u7ec4\u5408");
    pc_check(c, "use_abyssal_hunter", L"\u4f7f\u7528\u6df1\u6d77\u730e\u4eba\u8de8\u8bbe\u65bd\u7ec4\u5408");
    pc_check(c, "use_weekly_plan", L"\u542f\u7528\u5468\u8ba1\u5212");
    return true;
}


// ---- Recruit：自动公招 ----
inline bool panel_recruit(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"招募次数");
    c.y += 20;
    pc_number(c, "times", L"每次执行时最大招募次数", 4, 1, 1, 999);
    pc_check(c, "refresh", L"自动刷新 3 星 Tags");
    pc_check(c, "force_refresh", L"无招聘许可时继续尝试刷新 Tags");
    pc_number(c, "level3_permit_reserve", L"3 星保留招聘许可", 0, 1, 0, 999);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"自动确认星级");
    c.y += 20;
    pc_check(c, "confirm_3", L"自动确认 3 星");
    pc_check(c, "confirm_4", L"自动确认 4 星");
    pc_check(c, "confirm_5", L"自动确认 5 星");
    pc_check(c, "confirm_6", L"自动确认 6 星");
    pc_check(c, "auto_expedite", L"自动使用加急许可");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"Tag 策略");
    c.y += 20;
    // 官方 hc:ComboBox「公招多选 Tag 的策略」
    c.g.text(c.x, c.y + 4, L"公招多选 Tag 的策略", c.g.theme.dim, c.g.font_small);
    c.y += 22;
    pc_radio(c, "select_strategy", { L"策略 1", L"策略 2", L"策略 3" }, 0);
    // 官方 CheckBox「3 星 Tag 时的 Tag 倾向」
    pc_check(c, "tag_preference_3star", L"3 星 Tag 时的 Tag 倾向");
    pc_text(c, "select_tags", L"点击的 Tag 等级", L"如 5;4;3");
    pc_radio(c, "high_priority", { L"稀有度优先", L"组合优先" }, 0);
    pc_text(c, "preserve_tags", L"保留指定词条", L"多个用分号分隔");
    return true;
}

// ---- Mall：信用收支 ----
inline bool panel_mall(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"访问与战斗");
    c.y += 20;
    pc_check(c, "visit_friends", L"访问好友");
    pc_check(c, "only_once_a_day", L"一日只执行一次");
    pc_check(c, "credit_fight", L"借助战打 OF-1 赚信用");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"信用交易所");
    c.y += 20;
    pc_check(c, "shopping", L"信用交易所自动购物");
    pc_check(c, "only_buy_discount", L"只购买打折的信用商品");
    pc_check(c, "force_shopping_if_credit_full", L"信用溢出时无视黑名单");
    pc_number(c, "reserve_max_credit", L"信用点低于该值时停止购买", 300, 10, 0, 3000);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"过滤");
    c.y += 20;
    pc_text(c, "buy_first_str", L"优先购买", L"子串即可，分号分隔");
    pc_text(c, "blacklist_str", L"黑名单", L"子串即可，分号分隔");
    return true;
}

// ---- Award：领取奖励 ----
inline bool panel_award(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"领取项");
    c.y += 20;
    pc_check(c, "award", L"领取每日/每周任务奖励");
    pc_check(c, "mail", L"领取所有邮件奖励");
    pc_check(c, "recruit", L"进行限定池赠送的每日免费单抽");
    pc_check(c, "orundum", L"领取幸运墙的每日合成玉奖励");
    pc_check(c, "mining", L"领取限时开采许可的每日合成玉奖励");
    pc_check(c, "special_access", L"领取周年赠送月卡奖励");
    pc_check(c, "sign_in_event", L"领取限时签到活动奖励",
             L"仅支持常见横向版型；其他版型不支持，可能无法正常运作");
    return true;
}

// ---- Roguelike：自动肉鸽 ----
inline bool panel_roguelike(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u57fa\u7840");
    c.y += 20;
    pc_radio(c, "theme", { L"\u5c0f\u9e7f", L"\u673a\u94f6", L"\u6c34\u6708",
                           L"\u6b65\u6ed1\u94c1\u5239" }, 0);
    pc_radio(c, "difficulty", { L"\u7b80\u5355", L"\u666e\u901a", L"\u56f0\u96be" }, 0);
    pc_radio(c, "mode", { L"\u7ecf\u5178", L"\u6295\u8d44", L"\u5237\u5f00\u5c40",
                          L"\u9ed1\u5e95\u6cb3" }, 0);
    pc_radio(c, "squad", { L"\u6307\u5c04\u7814\u7a76\u5458", L"\u8f93\u5fc5\u62a2\u536b\u8f66",
                           L"\u4eea\u5f0f\u7814\u7a76\u5458", L"\u9ad8\u7ea7\u7269\u8d44\u56e2" }, 0);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"\u5f00\u5c40");
    c.y += 20;
    pc_text(c, "core_char", L"\u5f00\u5c40\u5e72\u5458", L"\u5e72\u5458\u540d\u79f0");
    pc_check(c, "start_with_elite_two",
             L"\u51f9 \u300c\u5f00\u5c40\u5e72\u5458\u300d\u76f4\u5347\u7cbe\u4e8c");
    pc_check(c, "only_start_with_elite_two",
             L"\u53ea\u51f9 \u300c\u5f00\u5c40\u5e72\u5458\u300d\u76f4\u5347\u7cbe\u4e8c\uff0c\u4e0d\u8fdb\u884c\u4f5c\u6218");
    pc_text(c, "start_with_select_list", L"\u5237\u5f00\u5c40\u671f\u671b\u5956\u52b1",
            L"\u591a\u4e2a\u7528\u5206\u53f7\u5206\u9694");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"\u6295\u8d44\u4e0e\u7ed3\u675f");
    c.y += 20;
    pc_check(c, "investment_enabled", L"\u6295\u8d44\u6e90\u77f3\u9408");
    pc_check(c, "investment_with_more_score",
             L"\u6295\u8d44\u6a21\u5f0f\u542f\u7528\u8d2d\u7269\u3001\u62db\u52df\u3001\u8fdb 2 \u5c42");
    pc_number(c, "start_times_limit",
              L"\u5f00\u59cb\u63a2\u7d22 N \u6b21\u540e\u505c\u6b62\u4efb\u52a1", 0, 1, 0, 999);
    pc_number(c, "gold_times_limit",
              L"\u6295\u8d44 N \u4e2a\u6e90\u77f3\u9408\u540e\u505c\u6b62\u4efb\u52a1", 0, 1, 0, 9999);
    pc_number(c, "stop_on_gold_limit",
              L"\u5907\u7528\u6e90\u77f3\u9408\u4e0a\u9650\u65f6\u505c\u6b62", 0, 100, 0, 99999);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"\u884c\u4e3a");
    c.y += 20;
    pc_check(c, "refresh_trader_with_dice", L"\u5237\u65b0\u5546\u5e97\uff08\u6307\u8def\u9cde\uff09");
    pc_check(c, "use_non_friend_support", L"\u53ef\u4ee5\u4f7f\u7528\u975e\u597d\u53cb\u52a9\u6218");
    pc_check(c, "stop_at_final_boss", L"\u5728\u7b2c\u4e94\u5c42 BOSS \u524d\u6682\u505c");
    pc_check(c, "stop_at_max_level", L"\u6ee1\u7ea7\u540e\u81ea\u52a8\u505c\u6b62");
    pc_check(c, "monthly_squad_auto_iterate", L"\u6708\u5ea6\u5c0f\u961f\u81ea\u52a8\u5207\u6362");
    pc_check(c, "deep_exploration_auto_iterate", L"\u6df1\u5165\u8c03\u67e5\u81ea\u52a8\u5207\u6362");
    return true;
}

// ---- MaaEnd AccountSwitch ----
inline bool panel_account_switch(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"账号切换");
    c.y += 20;
    pc_check(c, "enabled", L"启用账号切换", L"使用执行器绑定的账号");
    return true;
}

// 分派：返回 true 表示已用专属面板绘制
// ================================================================ MaaEnd 任务面板
// 选项名与标签取自本机 MaaEnd cores 的 tasks/*.json + locales/*/zh_cn.json

// ---- 日常奖励领取 ----
inline bool panel_end_daily(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u9886\u53d6\u9879");
    c.y += 20;
    pc_check(c, "DailyTaskRewards", L"\u6bcf\u65e5\u4efb\u52a1\u5956\u52b1");
    pc_check(c, "DailyEmailRewards", L"\u90ae\u4ef6/\u6682\u5b58\u533a\u5956\u52b1");
    pc_check(c, "DailyClaimDeliveryJobsRewards", L"\u8f6c\u4ea4\u59d4\u6258\u5956\u52b1");
    pc_check(c, "DailyEventRewards", L"\u6d3b\u52a8\u5956\u52b1");
    pc_check(c, "DailyProtocolPassRewards", L"\u901a\u884c\u8bc1\u5956\u52b1");
    pc_check(c, "DailyActivityFullWarning", L"\u6d3b\u8dc3\u5ea6\u672a\u6ee1\u65f6\u544a\u8b66");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"\u6bcf\u65e5\u4efb\u52a1");
    c.y += 20;
    pc_check(c, "DailyTaskUpgradeWeapon", L"\u5347\u7ea7\u6b66\u5668");
    pc_check(c, "DailyTaskAssemble", L"\u5236\u9020\u88c5\u5907");
    pc_check(c, "DailyTaskLevelUpOperator", L"\u5e72\u5458\u5347\u7ea7");
    pc_number(c, "LevelUpOperatorMaxLevelThreshold", L"\u7b49\u7ea7\u8bbe\u7f6e", 0, 1, 0, 90);
    pc_check(c, "DailyTaskCreation", L"\u7b80\u6613\u5236\u4f5c");
    return true;
}

// ---- 自动采集 ----
inline bool panel_end_collect(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u91c7\u96c6\u533a\u57df");
    c.y += 20;
    pc_check(c, "AutoCollectValleyIV", L"\u56db\u53f7\u8c37\u5730");
    pc_check(c, "AutoCollectValleyIVRareRoutes", L"\u7a00\u6709\u91c7\u96c6\u7269");
    pc_check(c, "AutoCollectValleyIVCommonRoutes", L"\u4e00\u822c\u91c7\u96c6\u7269");
    pc_check(c, "AutoCollectWuling", L"\u6b66\u9675");
    pc_check(c, "AutoCollectWulingRareRoutes", L"\u7a00\u6709\u91c7\u96c6\u7269");
    pc_check(c, "AutoCollectWulingCommonRoutes", L"\u4e00\u822c\u91c7\u96c6\u7269");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"\u6a21\u5f0f");
    c.y += 20;
    pc_radio(c, "AutoCollectMode", { L"\u59cb\u7ec8", L"\u76ee\u6807\u5e93\u5b58" }, 0);
    pc_number(c, "AutoCollectTargetInventory", L"\u76ee\u6807\u5e93\u5b58", 0, 1, 0, 99999);
    pc_check(c, "AutoCollectStashBackpackSubTask", L"\u4efb\u52a1\u524d\u540e\u5b58\u653e\u80cc\u5305");
    return true;
}

// ---- 协议空间 ----
inline bool panel_end_protocol(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u6218\u6597");
    c.y += 20;
    pc_check(c, "AutoFightSetting", L"\u81ea\u52a8\u6218\u6597\u8bbe\u7f6e");
    pc_radio(c, "ProtocolSpaceTeamChoose",
             { L"\u9ed8\u8ba4", L"\u961f\u4f0d 1", L"\u961f\u4f0d 2", L"\u961f\u4f0d 3" }, 0);
    pc_radio(c, "ProtocolSpaceMode", { L"\u6309\u6b21\u6570", L"\u76ee\u6807\u5e93\u5b58" }, 0);
    pc_number(c, "ProtocolSpaceSuccessCount",
              L"\u884c\u52a8\u6210\u529f\u6b21\u6570\u4e0a\u9650", 0, 1, 0, 999);
    pc_number(c, "ProtocolSpaceFailedCount",
              L"\u884c\u52a8\u5931\u8d25\u6b21\u6570\u4e0a\u9650", 0, 1, 0, 999);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"\u5176\u4ed6");
    c.y += 20;
    pc_number(c, "SupplyPlanLimits", L"\u57f9\u517b\u9053\u5177\u76ee\u6807", 0, 1, 0, 99999);
    return true;
}

// ---- 赠送干员礼物 ----
inline bool panel_end_gift(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u8d60\u9001");
    c.y += 20;
    pc_text(c, "SelectOperator", L"\u8d60\u9001\u5e72\u5458", L"\u5e72\u5458\u540d\u79f0");
    pc_number(c, "GiftCount", L"\u8d60\u9001\u6570\u91cf", 1, 1, 1, 999);
    pc_check(c, "OnlyReceiveGift", L"\u53ea\u6536\u793c\u7269");
    pc_check(c, "AcceptAllGifts", L"\u63a5\u53d7\u5168\u90e8\u793c\u7269");
    return true;
}

// ---- 拜访好友 ----
inline bool panel_end_friends(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u62dc\u8bbf\u89c4\u5219");
    c.y += 20;
    pc_check(c, "VisitFriendsRemark", L"\u4f18\u5148\u62dc\u8bbf\u5e26\u5907\u6ce8\u7684\u597d\u53cb");
    pc_check(c, "VisitFriendsPriorityRemarkEnable",
             L"\u4f18\u5148\u62dc\u8bbf\u6307\u5b9a\u597d\u53cb\uff08\u4f18\u5148\u7ea7\u66f4\u9ad8\uff09");
    pc_text(c, "VisitFriendsPriorityRemark", L"\u4f18\u5148\u62dc\u8bbf\u7684\u597d\u53cb",
            L"\u591a\u4e2a\u7528\u5206\u53f7\u5206\u9694");
    pc_check(c, "VisitFriendsPrioritySearchEnable",
             L"\u901a\u8fc7\u641c\u7d22\u6846\u5b9a\u4f4d\u4f18\u5148\u597d\u53cb");
    return true;
}

// ---- Depot：仓库识别（库存保持）----
inline bool panel_depot(PanelCtx& c) {
    // 官方 DepotMaintainTaskUserControl：14 个控件
    c.g.group_title(c.x, c.y, L"\u4efb\u52a1\u884c\u4e3a");
    c.y += 20;
    pc_check(c, "update_before_start", L"\u4efb\u52a1\u5f00\u59cb\u524d\u66f4\u65b0\u5e93\u5b58\u6570\u636e");
    pc_check(c, "skip_during_activity", L"\u6d3b\u52a8\u671f\u95f4\u8df3\u8fc7");
    pc_check(c, "skip_during_resource_collection",
             L"\u8d44\u6e90\u6536\u96c6\u9650\u65f6\u5168\u5929\u5f00\u653e\u671f\u95f4\u8df3\u8fc7");
    pc_check(c, "only_first_insufficient_plan", L"\u4ec5\u6267\u884c\u7b2c\u4e00\u4e2a\u5e93\u5b58\u4e0d\u8db3\u7684\u8ba1\u5212");
    pc_check(c, "sort_by_progress", L"\u6309\u5e93\u5b58\u8fbe\u6210\u7387\u6392\u5e8f\u6267\u884c");
    pc_check(c, "count_dual_chip_as_chip_pack", L"\u82af\u7247\u7ec4\u8ba1\u5165\u53cc\u82af\u7247\u6298\u7b97");
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u5173\u5361");
    c.y += 20;
    pc_text(c, "stage", L"\u5173\u5361", L"\u7559\u7a7a\u4e3a \u5f53\u524d/\u4e0a\u6b21");
    pc_check(c, "use_custom_stage", L"\u624b\u52a8\u8f93\u5165\u5173\u5361\u540d");
    pc_check(c, "auto_series", L"AUTO \u4ee3\u7406\u500d\u7387");
    c.y += 6;

    c.g.group_title(c.x, c.y, L"\u5236\u84b4\u4e0e\u7269\u6599");
    c.y += 20;
    pc_check(c, "use_medicine", L"\u542f\u7528 \u300c\u4f7f\u7528\u836f\u5242\u300d",
             L"\u8be5\u9009\u9879\u53f3\u952e\u9009\u4e2d\u65f6\u4ec5\u751f\u6548\u4e00\u6b21");
    pc_check(c, "use_stone", L"\u542f\u7528 \u300c\u4f7f\u7528\u6e90\u77f3\u300d",
             L"\u8be5\u9009\u9879\u4ec5\u751f\u6548\u4e00\u6b21");
    pc_check(c, "use_48h_expire_medicine", L"\u4f7f\u7528 48 \u5c0f\u65f6\u5185\u8fc7\u671f\u7684\u7406\u667a\u836f");
    return true;
}


// ---- OperBox：干员识别 ----
inline bool panel_operbox(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u8bc6\u522b\u8303\u56f4");
    c.y += 20;
    pc_check(c, "update_all", L"\u5168\u91cf\u8bc6\u522b");
    pc_check(c, "update_elite", L"\u7ecbe\u82f1\u5316");
    pc_check(c, "update_skill", L"\u6280\u80fd\u7b49\u7ea7");
    pc_check(c, "update_module", L"\u6a21\u7ec4");
    return true;
}

// ---- Reclamation：生息演算 ----
inline bool panel_reclamation(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u6f14\u7b97");
    c.y += 20;
    pc_text(c, "theme", L"\u4e3b\u9898", L"\u5982 Tales");
    pc_number(c, "mode", L"\u6a21\u5f0f", 0, 1, 0, 9);
    return true;
}

// ---- Copilot：自动抄作业 ----
inline bool panel_copilot(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u4f5c\u4e1a");
    c.y += 20;
    pc_text(c, "copilot_file", L"\u4f5c\u4e1a\u6587\u4ef6", L"JSON \u6587\u4ef6\u8def\u5f84");
    pc_check(c, "loop", L"\u5faa\u73af\u6267\u884c");
    return true;
}

// ---- Custom：自定任务 ----
inline bool panel_custom(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u4efb\u52a1\u540d\u5217\u8868");
    c.y += 20;
    pc_text(c, "task_names", L"\u4efb\u52a1\u540d", L"\u591a\u4e2a\u7528\u9017\u53f7\u5206\u9694");
    return true;
}

// ---- CloseDown：关闭游戏 ----
inline bool panel_closedown(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"\u5173\u95ed");
    c.y += 20;
    pc_check(c, "client_type", L"\u6309\u5ba2\u6237\u7aef\u7c7b\u578b\u5173\u95ed");
    return true;
}

// ---- 通用参数编辑器（未定制任务的回退）----
// 按 catalog 声明的类型渲染对应控件，与官方面板视觉一致：
//   bool -> 三态勾选（支持右键仅一次）；number -> 数字步进；
//   array -> 分号分隔文本框；其他 -> 文本框
// 标签用官方 description（MAA catalog / MaaEnd locale 均已中文化）
inline bool panel_generic(PanelCtx& c, const std::vector<mj::Value>* specs) {
    c.g.group_title(c.x, c.y, L"\u9ad8\u7ea7\u8bbe\u7f6e");
    c.y += 20;
    if (!specs || specs->empty()) {
        c.g.tip_line(c.x, c.y, c.right, L"\u8be5\u4efb\u52a1\u65e0\u53ef\u914d\u7f6e\u53c2\u6570");
        c.y += 20;
        return true;
    }
    for (auto& p : *specs) {
        std::string key = p.get_str("name", "");
        if (key.empty()) continue;
        std::string type = p.get_str("type", "string");
        std::string desc = p.get_str("description", "");
        std::wstring label = desc.empty() ? gui::widen(key) : gui::widen(desc);
        bool required = p.get_bool("required", false);
        std::wstring tip;
        if (required) tip = L"\u5fc5\u586b\u9879";
        if (p.has("default")) {
            std::wstring d = L"\u9ed8\u8ba4 " + gui::widen(mj::Value::dump(p["default"]));
            tip = tip.empty() ? d : (tip + L" \u00b7 " + d);
        }
        if (type == "bool") {
            pc_check(c, key, label, tip);
        } else if (type == "select" && p.has("cases")) {
            // 官方枚举：用单选条呈现（cases 已本地化）
            std::vector<std::wstring> opts;
            for (auto& cs : p["cases"].items()) {
                if (cs.is_string()) opts.push_back(gui::widen(cs.as_string()));
            }
            if (!opts.empty()) {
                // 当前值按 cases 索引匹配（兼容存枚举名或中文名）
                std::string cur = c.get_str(key);
                int sel = 0;
                for (size_t i = 0; i < opts.size(); ++i) {
                    std::string w = util::wide_to_utf8(opts[i]);
                    if (w == cur) { sel = (int)i; break; }
                }
                pc_radio(c, key, opts, sel);
                // 回写为选中项的中文名（与 cases 展示一致）
                c.set_str(key, util::wide_to_utf8(opts[sel]));
                if (!tip.empty()) { c.g.tip_line(c.x + 24, c.y, c.right, tip); c.y += 16; }
            }
        } else if (type == "number") {
            pc_number(c, key, label, (int)p.get_int("default", 0), 1, 0, 999999);
            if (!tip.empty()) { c.g.tip_line(c.x + 24, c.y, c.right, tip); c.y += 16; }
        } else if (type == "array") {
            std::string cur;
            auto it = c.params.find(key);
            if (it != c.params.end() && it->second.is_array()) {
                for (auto& e : it->second.items()) {
                    if (!cur.empty()) cur += ";";
                    cur += e.as_string();
                }
            }
            RECT r{ c.x, c.y, c.x + 460, c.y + 26 };
            c.g.text_input((c.exec_id + ":" + c.task + ":" + key).c_str(), r, cur,
                           L"\u591a\u4e2a\u7528\u5206\u53f7\u5206\u9694");
            mj::Value arr = mj::Value::array();
            std::string part;
            for (char ch : cur + ";") {
                if (ch == ';') {
                    if (!part.empty()) { arr.push(part); part.clear(); }
                } else if (ch != ' ') {
                    part += ch;
                }
            }
            c.params[key] = arr;
            c.y += 32;
            if (!tip.empty()) { c.g.tip_line(c.x + 24, c.y, c.right, tip); c.y += 16; }
        } else {
            pc_text(c, key, label, tip);
            if (!tip.empty()) { c.g.tip_line(c.x + 24, c.y, c.right, tip); c.y += 16; }
        }
    }
    return true;
}

inline bool draw_task_panel(PanelCtx& c, const std::vector<mj::Value>* specs = nullptr) {
    // 先用官方 default 预填充，保证面板打开即显示默认值
    c.apply_defaults(specs);
    if (c.task == "StartUp") return panel_startup(c);
    if (c.task == "Fight") return panel_fight(c);
    if (c.task == "Infrast") return panel_infrast(c);
    if (c.task == "Recruit") return panel_recruit(c);
    if (c.task == "Mall") return panel_mall(c);
    if (c.task == "Award") return panel_award(c);
    if (c.task == "Roguelike") return panel_roguelike(c);
    if (c.task == "AccountSwitch") return panel_account_switch(c);
    if (c.task == "Depot") return panel_depot(c);
    if (c.task == "OperBox") return panel_operbox(c);
    if (c.task == "Reclamation") return panel_reclamation(c);
    if (c.task == "Copilot") return panel_copilot(c);
    if (c.task == "Custom") return panel_custom(c);
    if (c.task == "CloseDown") return panel_closedown(c);
    if (c.task == "DailyRewards") return panel_end_daily(c);
    if (c.task == "AutoCollect") return panel_end_collect(c);
    if (c.task == "ProtocolSpace") return panel_end_protocol(c);
    if (c.task == "GiftOperator") return panel_end_gift(c);
    if (c.task == "VisitFriends") return panel_end_friends(c);
    // 未定制任务：按 catalog 类型渲染通用面板（与官方视觉一致）
    return panel_generic(c, specs);
}

} // namespace hub
