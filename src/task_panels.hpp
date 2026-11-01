// MAA 官方任务设置面板（1:1 复刻 MaaWpfGui/Views/UserControl/TaskQueue/*.xaml）
// 每个面板对应官方的一个 TaskUserControl，标签取自官方 zh-cn 本地化。
#pragma once
#include <map>
#include <string>
#include <vector>

#include "gui.hpp"
#include "json.hpp"

namespace hub {

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
};

// ---- 通用小部件 ----
inline void pc_check(PanelCtx& c, const std::string& key, const std::wstring& label,
                     const std::wstring& tip = L"") {
    bool v = c.get_bool(key);
    RECT r{ c.x, c.y, c.x + 460, c.y + 22 };
    c.g.checkbox((c.exec_id + ":" + c.task + ":" + key).c_str(), r, label, v);
    c.set_bool(key, v);
    c.y += 24;
    if (!tip.empty()) {
        c.g.tip_line(c.x + 24, c.y, c.right, tip);
        c.y += 16;
    }
}

inline void pc_number(PanelCtx& c, const std::string& key, const std::wstring& label,
                      int def = 0, int step = 1, int lo = 0, int hi = 9999) {
    int v = c.get_int(key, def);
    RECT r{ c.x, c.y, c.x + 460, c.y + 26 };
    c.g.number_input((c.exec_id + ":" + c.task + ":" + key).c_str(), r, label, v, step, lo, hi);
    c.set_int(key, v);
    c.y += 32;
}

inline void pc_text(PanelCtx& c, const std::string& key, const std::wstring& label,
                    const std::wstring& hint = L"") {
    std::string v = c.get_str(key);
    RECT r{ c.x, c.y, c.x + 460, c.y + 26 };
    c.g.text_input((c.exec_id + ":" + c.task + ":" + key).c_str(), r, v,
                   hint.empty() ? nullptr : hint.c_str());
    c.set_str(key, v);
    c.y += 32;
}

// 横向单选（选项条）
inline void pc_radio(PanelCtx& c, const std::string& key,
                     const std::vector<std::wstring>& opts, int def = 0) {
    int cur = c.get_int(key, def);
    RECT r{ c.x, c.y, c.x + 460, c.y + 24 };
    int hit = c.g.option_strip((c.exec_id + ":" + c.task + ":" + key).c_str(), r, opts, cur);
    if (hit >= 0) cur = hit;
    c.set_int(key, cur);
    c.y += 30;
}

// ================================================================ 各任务面板
// 每个函数把官方面板的控件按顺序画出；返回 false 表示该任务无专属面板。

// ---- StartUp：开始唤醒 ----
inline bool panel_startup(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"账号与启动");
    c.y += 20;
    pc_check(c, "account_name_set", L"账号切换", L"启用后按执行器绑定的账号自动切换");
    pc_check(c, "start_game_enabled", L"是否启动客户端");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"客户端类型");
    c.y += 20;
    pc_radio(c, "client_type", { L"官服", L"Bilibili", L"txwy", L"YoStarEN",
                                 L"YoStarJP", L"YoStarKR" }, 0);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"连接");
    c.y += 20;
    pc_check(c, "auto_detect", L"自动检测连接");
    return true;
}

// ---- Fight：理智作战 ----
inline bool panel_fight(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"理智与材料");
    c.y += 20;
    pc_number(c, "medicine", L"最大理智药数量", 0, 1, 0, 999);
    pc_number(c, "stone", L"最大吃石头数量", 0, 1, 0, 999);
    pc_check(c, "report_to_penguin", L"上报掉落统计到企鹅物流");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"关卡与次数");
    c.y += 20;
    pc_text(c, "stage", L"关卡", L"如 1-7；留空为 当前/上次");
    pc_number(c, "times", L"作战次数", 5, 1, 1, 9999);
    pc_radio(c, "series", { L"×1", L"×2", L"×3", L"×4", L"×5", L"×6" }, 3);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"行为");
    c.y += 20;
    pc_check(c, "dr_grandet", L"博朗台模式", L"理智为 0 时仍尝试进入关卡，利用宿舍恢复");
    pc_check(c, "hide_series", L"隐藏代理倍率");
    pc_check(c, "allow_stone_save", L"允许使用源石保存状态");
    pc_check(c, "hide_unavailable_stage", L"下拉框中隐藏当日不开放关卡");
        return true;
}

// ---- Infrast：基建换班 ----
inline bool panel_infrast(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"基建模式");
    c.y += 20;
    pc_radio(c, "mode", { L"默认基建", L"自定义基建", L"轮换基建" }, 0);   // 0/1/2 -> 0/10000/20000
    c.y += 6;
    c.g.group_title(c.x, c.y, L"无人机");
    c.y += 20;
    pc_radio(c, "drones_idx", { L"不使用", L"贸易站", L"制造站", L"发电站", L"办公室" }, 0);
    pc_number(c, "dorm_threshold", L"宿舍心情阈值", 30, 5, 0, 100);
    c.y += 6;
    c.g.group_title(c.x, c.y, L"会客室与宿舍");
    c.y += 20;
    pc_check(c, "reception_message_board", L"会客室信息板收取信用");
    pc_check(c, "reception_clue_exchange", L"进行线索交流");
    pc_check(c, "reception_send_clue", L"赠送线索");
    pc_check(c, "dorm_trust", L"宿舍空余位置蹭信赖");
    pc_check(c, "dorm_filter_not_stationed", L"不将已进驻的干员放入宿舍");
    c.y += 6;
    c.g.group_title(c.x, c.y, L"跨设施组合");
    c.y += 20;
    pc_check(c, "use_pinus_sylvestris", L"使用红松骑士团跨设施组合");
    pc_check(c, "use_perception_information", L"使用感知信息跨设施组合");
    pc_check(c, "use_worldly_plight", L"使用人间烟火跨设施组合");
    pc_check(c, "use_abyssal_hunter", L"使用深海猎人跨设施组合");
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

// ---- MaaEnd AccountSwitch：账号切换 ----
inline bool panel_account_switch(PanelCtx& c) {
    c.g.group_title(c.x, c.y, L"账号切换");
    c.y += 20;
    pc_check(c, "enabled", L"启用账号切换", L"使用执行器绑定的账号");
    return true;
}

// 分派：返回 true 表示已用专属面板绘制
inline bool draw_task_panel(PanelCtx& c) {
    if (c.task == "StartUp") return panel_startup(c);
    if (c.task == "Fight") return panel_fight(c);
    if (c.task == "Infrast") return panel_infrast(c);
    if (c.task == "Recruit") return panel_recruit(c);
    if (c.task == "Mall") return panel_mall(c);
    if (c.task == "Award") return panel_award(c);
    if (c.task == "AccountSwitch") return panel_account_switch(c);
    return false;
}

} // namespace hub
