// 导入 MAA / MaaEnd 导出的配置文件
//
// 支持两种格式（按顶层键自动识别）：
// 1) MAA（MaaWpfGui）gui.json：
//    { "Configurations": { "<名>": { "<Task>.<Param>": "值", ... } }, "Current": "<名>", "Global": {...} }
//    任务参数为 "<Task>.<Param>" 扁平字符串键（"True"/"False"/数字串）。
//    新版 MAA 已把任务参数迁出扁平键（ConfigurationKeys.cs L148-149 标注已废弃/已迁移），
//    导出里可能完全没有任务键 —— 此时仅创建执行器，其余键跳过并计数，不视为错误。
//    GUI 级键（GUI.* / VersionUpdate.* / ExternalNotification.* / TimeOut.* /
//    MainFunction.* / MiniGame.* / Infrast.DefaultInfrast 等）不属于任务参数，全部跳过；
//    Timer.* 同样不进任务参数，但会被提取进报告的 "schedules"（见下）。
//    映射目标为 MaaHub 的面板键（task-params.json 存储口径，运行时由
//    normalize_task_params 标准化成核心参数），与 ENG01 实测 ground truth 一致。
// 2) MaaEnd（mxu-MaaEnd.json）：
//    { "version": "1.0", "instances": [ { "name", "tasks": [ { "taskName", "enabled",
//      "enabledByController": { "Win32-Front": bool }, "optionValues": { ... } } ],
//      "schedulePolicies": {...}|[...], "preActions": {...}|[...] } ] }
//    注：schedulePolicies / preActions 实际导出为单个对象（老版本为数组），两者都兼容；
//    schedulePolicies 提取进报告的 "schedules"（enabled:false 也记录），见下。
//    参数按核心键原样写入（MaaEnd 适配器在无路由时把用户参数整体透传）：
//      select   -> caseName 字符串
//      switch   -> value 布尔
//      checkbox -> caseNames 数组（非空），否则布尔 false
//      input    -> values 对象逐键写字符串（含空串，如 "SeizeDeliveryJobsRefreshLimit":""）
//    任务可见性：enabledByController["Win32-Front"]==true 优先，缺失回退 enabled；
//    隐藏任务的参数同样导入（无副作用，任务重新显示时即生效），但不进任务列表。
//
// 任务列表不直接落盘：app_window 的 task_list_ 是内存权威，导入器若直接写
// state\task-lists.json 会被随后的 save_task_list() 覆盖。因此通过报告的
// "task_lists" 字段返回 { exec_id: [任务名...] }，由 GUI 合并进内存态后统一保存。
//
// 定时计划（schedules）同样通过报告的 "schedules" 字段返回，由调用方负责落盘：
//   state\schedules.json = {
//     "version": 1,
//     "entries": [ { "exec_id", "kind", "label",
//                    "enabled", "weekdays": [0..6, 0=周日], "times": ["HH:MM"...],
//                    "last_run_date": "YYYY-MM-DD" } ] }
// 来源两处：
//   MAA gui.json 的 Global.Timer.TimerN{,Config,Hour,Min} -> 每个启用的定时器一条
//     （enabled:false 也记录，字符串 "True"/"2" 经 to_bool/to_int 归一）；
//   MaaEnd 的 schedulePolicies 对象 -> { enabled, weekdays, times }（enabled:false 也记录）。
// MAA 定时器只指定"几点跑配置 N"，任务序列由 Hub 自身 state\task-lists.json 决定；
// Hub 调度器到期时对该执行器跑任务列表（hub.hpp scheduler）。
#pragma once

#include "hub.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hub {
namespace import_config {

// ---------------------------------------------------------------- 小工具

// 标量转文本（MaaEnd input values 的容错写法；数字按 %g，与适配器 scalar_text 一致）
inline std::string scalar_text(const mj::Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.is_number()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", v.as_double(0));
        return buf;
    }
    if (v.is_bool()) return v.as_bool(false) ? "true" : "false";
    return "";
}

inline std::string lower_copy(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// PascalCase -> snake_case（ReportToPenguin -> report_to_penguin）
inline std::string snake_case(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (std::isupper((unsigned char)c)) {
            if (i > 0) out.push_back('_');
            out.push_back((char)std::tolower((unsigned char)c));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

inline bool to_bool(const mj::Value& v, bool& out) {
    if (v.is_bool()) { out = v.as_bool(false); return true; }
    if (v.is_string()) {
        std::string s = lower_copy(v.as_string());
        if (s == "true") { out = true; return true; }
        if (s == "false") { out = false; return true; }
    }
    return false;
}

inline bool to_int(const mj::Value& v, int& out) {
    if (v.is_number()) { out = (int)v.as_int(0); return true; }
    if (v.is_string()) {
        const std::string s = v.as_string();
        if (s.empty()) return false;
        char* end = nullptr;
        long val = strtol(s.c_str(), &end, 10);
        if (end && *end == '\0') { out = (int)val; return true; }
    }
    return false;
}

// ---------------------------------------------------------------- 导入上下文

struct Ctx {
    int params_written = 0;
    int skipped_total = 0;
    int schedule_policies = 0;
    int pre_actions = 0;
    mj::Value skipped = mj::Value::array();
    mj::Value errors = mj::Value::array();
    mj::Value task_lists = mj::Value::object();  // exec_id -> [任务名...]（导出顺序）
    mj::Value schedules = mj::Value::array();    // 定时计划条目（见文件头 schedules.json 说明）
    // 安装目录区分标记：同一台机器装多个 MAA/MaaEnd 时，靠它把执行器标签和
    // 定时器 source 区分开，否则 label 相同会复用同一执行器、source 相同会互相覆盖。
    // 空字符串 = 不区分（直接读裸配置文件时的老行为）。
    std::string install_tag;

    void add_skip(const std::string& key) {
        ++skipped_total;
        if ((int)skipped.items().size() < 80) skipped.push(key);
    }
    void add_error(const std::string& msg) { errors.push(msg); }
};

// ---------------------------------------------------------------- MAA 通用键表

// <Task>.<Param> 的 Param 转 snake_case 后查此表（与 maa_adapter build_catalog 对齐；
// use_medicine/use_stone/perform_battles/allow_stone_save 为 MaaHub 面板扩展键，
// ENG01 实测同样存于 task-params.json）。类型：b=bool n=number s=string o=object。
// 数组类参数（facility/drones/confirm/select/blacklist/mode 映射）走特殊转换，不在表内。
inline const std::map<std::string, char>& task_key_types(const std::string& task) {
    static const std::map<std::string, std::map<std::string, char>> table = {
        { "StartUp",     { { "client_type", 's' }, { "account_name", 's' } } },
        // CloseDown.ClientType 不映射：MaaHub 面板的 CloseDown client_type 是布尔开关
        // （与 StartUp 的 radio enum 语义不同），MAA 导出的是枚举字符串，写错类型有害
        { "CloseDown",   {} },
        { "Fight",       { { "stage", 's' }, { "medicine", 'n' }, { "stone", 'n' },
                           { "times", 'n' }, { "series", 'n' }, { "drops", 'o' },
                           { "report_to_penguin", 'b' }, { "use_medicine", 'b' },
                           { "use_stone", 'b' }, { "perform_battles", 'b' },
                           { "allow_stone_save", 'b' } } },
        { "Recruit",     { { "refresh", 'b' }, { "times", 'n' } } },
        { "Mall",        { { "visit_friends", 'b' }, { "shopping", 'b' } } },
        { "Award",       { { "award", 'b' }, { "mail", 'b' }, { "recruit", 'b' } } },
        { "Roguelike",   { { "theme", 's' }, { "mode", 'n' }, { "squad", 's' },
                           { "starts_count", 'n' } } },
        { "Copilot",     { { "filename", 's' }, { "loop_times", 'n' } } },
        { "Reclamation", { { "theme", 's' }, { "mode", 'n' } } },
    };
    static const std::map<std::string, char> empty;
    auto it = table.find(task);
    return it == table.end() ? empty : it->second;
}

// 基建设施/无人机顺序与 hub.hpp normalize_task_params 一致
static const char* kFac[] = { "Control", "Mfg", "Trade", "Power",
                              "Reception", "Office", "Training", "Processing" };
static const char* kDrones[] = { "_NotUse", "Money", "SyntheticJade",
                                 "CombatRecord", "Chip" };

// 特殊键转换：返回 0=非特殊键，1=已写入，2=识别为特殊键但值无效（计跳过）
inline int special_maa(Hub& hub, const std::string& exec, const std::string& task,
                       const std::string& param, const mj::Value& v, Ctx& c) {
    if (task == "Infrast") {
        if (param == "Mode") {
            int m;
            if (!to_int(v, m)) return 2;
            // MAA 0/10000/20000 -> 面板 0/1/2
            hub.set_task_param(exec, task, "mode",
                               mj::Value(m == 10000 ? 1 : m == 20000 ? 2 : 0));
            ++c.params_written;
            return 1;
        }
        if (param == "Facility") {
            if (!v.is_array()) return 2;
            std::set<std::string> on;
            for (auto& e : v.items()) on.insert(lower_copy(scalar_text(e)));
            for (int i = 0; i < 8; ++i) {
                // 必须写全 8 个键：面板端有 touched 标志，只写 true 的会误判
                hub.set_task_param(exec, task,
                                   "facility_" + std::to_string(i),
                                   mj::Value(on.count(lower_copy(kFac[i])) > 0));
                ++c.params_written;
            }
            return 1;
        }
        if (param == "Drones") {
            std::string s = lower_copy(scalar_text(v));
            int idx = -1;
            for (int i = 0; i < 5; ++i) {
                if (s == lower_copy(kDrones[i])) { idx = i; break; }
            }
            // PureGold/OriginStone 面板无人机档没有对应项，落回"不使用"
            if (idx < 0 && (s == "puregold" || s == "originstone")) idx = 0;
            if (idx < 0) return 2;
            hub.set_task_param(exec, task, "drones_idx", mj::Value(idx));
            ++c.params_written;
            return 1;
        }
    } else if (task == "Recruit") {
        if (param == "Confirm") {
            if (!v.is_array()) return 2;
            std::set<std::string> levels;
            for (auto& e : v.items()) levels.insert(scalar_text(e));
            for (int n = 3; n <= 6; ++n) {
                hub.set_task_param(exec, task, "confirm_" + std::to_string(n),
                                   mj::Value(levels.count(std::to_string(n)) > 0));
                ++c.params_written;
            }
            return 1;
        }
        if (param == "Select") {
            if (!v.is_array() || v.items().empty()) return 2;
            std::string tags;
            for (auto& e : v.items()) {
                if (!tags.empty()) tags.push_back(';');
                tags += scalar_text(e);
            }
            if (tags.empty()) return 2;
            hub.set_task_param(exec, task, "select_tags", mj::Value(tags));
            ++c.params_written;
            return 1;
        }
    } else if (task == "Mall") {
        if (param == "Blacklist") {
            if (!v.is_array() || v.items().empty()) return 2;
            std::string bl;
            for (auto& e : v.items()) {
                if (!bl.empty()) bl.push_back(';');
                bl += scalar_text(e);
            }
            if (bl.empty()) return 2;
            hub.set_task_param(exec, task, "blacklist_str", mj::Value(bl));
            ++c.params_written;
            return 1;
        }
    }
    return 0;
}

// 按目录类型转换 MAA 扁平键值
inline bool convert_typed(char type, const mj::Value& src, mj::Value& out) {
    switch (type) {
        case 'b': {
            bool b;
            if (!to_bool(src, b)) return false;
            out = mj::Value(b);
            return true;
        }
        case 'n': {
            int n;
            if (!to_int(src, n)) return false;
            out = mj::Value(n);
            return true;
        }
        case 's': {
            if (src.is_string()) { out = mj::Value(src.as_string()); return true; }
            if (src.is_number()) { out = mj::Value(scalar_text(src)); return true; }
            return false;
        }
        case 'o': {
            if (src.is_object()) { out = src; return true; }
            return false;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------- 执行器定位

// 按种类+标签幂等定位：已有则复用，没有则创建（核心用该种类默认核心）
inline std::string find_or_create_executor(Hub& hub, const std::string& kind,
                                           const std::string& label,
                                           mj::Value& created, mj::Value& reused,
                                           Ctx& c) {
    for (auto& e : hub.executors()) {
        if (e.kind == kind && e.label == label) {
            reused.push(label);
            return e.id;
        }
    }
    std::string id = hub.create_executor(kind, label);
    if (id.empty()) {
        c.add_error("创建执行器 " + label + " 失败：没有可用核心，请先在「核心管理」登记或拉取");
        return "";
    }
    created.push(label);
    return id;
}

// ---------------------------------------------------------------- MaaEnd

inline void import_maaend_option(Hub& hub, const std::string& exec,
                                 const std::string& task, const std::string& key,
                                 const mj::Value& ov, Ctx& c) {
    if (!ov.is_object()) { c.add_skip(task + "." + key + " (非对象选项)"); return; }
    std::string type = ov.get_str("type", "");
    if (type == "select") {
        std::string cn = ov.get_str("caseName", "");
        if (cn.empty()) { c.add_skip(task + "." + key + " (select 无 caseName)"); return; }
        hub.set_task_param(exec, task, key, mj::Value(cn));
        ++c.params_written;
    } else if (type == "switch") {
        auto* val = ov.find("value");
        if (!val || !val->is_bool()) { c.add_skip(task + "." + key + " (switch 无布尔值)"); return; }
        hub.set_task_param(exec, task, key, mj::Value(val->as_bool(false)));
        ++c.params_written;
    } else if (type == "checkbox") {
        auto* cns = ov.find("caseNames");
        if (cns && cns->is_array()) {
            mj::Value arr = mj::Value::array();
            for (auto& e : cns->items()) {
                if (e.is_string()) arr.push(e.as_string());
            }
            if (!arr.items().empty()) {
                // 多 case 复选框部分勾选：写数组，核心端逐 case 匹配
                hub.set_task_param(exec, task, key, arr);
                ++c.params_written;
                return;
            }
        }
        // caseNames 缺失/为空 = 全不选，存布尔 false（ENG01 实测口径）
        hub.set_task_param(exec, task, key, mj::Value(false));
        ++c.params_written;
    } else if (type == "input" || type == "hotkey") {
        auto* vals = ov.find("values");
        if (!vals || !vals->is_object() || vals->members().empty()) {
            c.add_skip(task + "." + key + " (input 无 values)");
            return;
        }
        // values 对象逐键写字符串（含空串：空串=用户显式清空，合法）
        for (auto& pv : vals->members()) {
            hub.set_task_param(exec, task, pv.first, mj::Value(scalar_text(pv.second)));
            ++c.params_written;
        }
    } else {
        c.add_skip(task + "." + key + " (未知类型 " + type + ")");
    }
}

inline void import_maaend(const mj::Value& root, Hub& hub, mj::Value& rep, Ctx& c) {
    rep.set("format", "maaend");
    auto* insts = root.find("instances");
    if (!insts || !insts->is_array() || insts->items().empty()) {
        c.add_error("没有 instances 数组或为空");
        return;
    }
    int tasks_total = 0;
    for (auto& inst : insts->items()) {
        if (!inst.is_object()) { c.add_error("instances 中存在非对象项，已跳过"); continue; }
        std::string name = inst.get_str("name", "");
        if (name.empty()) name = "MaaEnd 实例";
        // 多安装共存：标签加安装目录后缀，避免不同安装的同名实例被复用到一起
        std::string label = c.install_tag.empty() ? name : (name + " @" + c.install_tag);
        mj::Value created = mj::Value::array(), reused = mj::Value::array();
        std::string exec = find_or_create_executor(hub, "maaend", label, created, reused, c);
        for (auto& lb : created.items()) rep["executors_created"].push(lb);
        for (auto& lb : reused.items()) rep["executors_reused"].push(lb);
        if (exec.empty()) continue;

        // schedulePolicies / preActions 实际是单个对象（老版本为数组），两者都兼容
        if (auto* sp = inst.find("schedulePolicies"); sp) {
            const mj::Value* pol = nullptr;
            if (sp->is_object()) pol = sp;
            else if (sp->is_array() && !sp->items().empty() && sp->items()[0].is_object())
                pol = &sp->items()[0];
            if (pol) {
                ++c.schedule_policies;
                mj::Value e = mj::Value::object();
                e.set("exec_id", exec);
                e.set("kind", "maaend");
                e.set("label", label);
                // enabled 兼容 bool 与 "True"/"true" 字符串
                bool en = false;
                if (auto* ev = pol->find("enabled")) to_bool(*ev, en);
                e.set("enabled", en);
                mj::Value wd = mj::Value::array(), tm = mj::Value::array();
                if (auto* w = pol->find("weekdays"); w && w->is_array())
                    for (auto& x : w->items()) wd.push(x.as_int(0));
                if (auto* t = pol->find("times"); t && t->is_array())
                    for (auto& x : t->items()) tm.push(scalar_text(x));
                e.set("weekdays", wd);
                e.set("times", tm);
                e.set("source", c.install_tag.empty()
                                    ? std::string("schedulePolicies")
                                    : (c.install_tag + ":schedulePolicies"));
                c.schedules.push(e);
            }
        }
        if (auto* pa = inst.find("preActions"); pa) {
            if (pa->is_object()) ++c.pre_actions;
            else if (pa->is_array()) c.pre_actions += (int)pa->items().size();
        }

        int included_cnt = 0;
        mj::Value list = mj::Value::array();
        auto* tasks = inst.find("tasks");
        if (tasks && tasks->is_array()) {
            for (auto& t : tasks->items()) {
                if (!t.is_object()) continue;
                std::string tname = t.get_str("taskName", "");
                if (tname.empty()) continue;
                // 参数全部导入（隐藏任务也无副作用）
                if (auto* ov = t.find("optionValues"); ov && ov->is_object()) {
                    for (auto& okv : ov->members()) {
                        import_maaend_option(hub, exec, tname, okv.first, okv.second, c);
                    }
                }
                // 可见性：Win32-Front 控制器优先，缺失回退 enabled
                bool included;
                auto* ctrl = t.find("enabledByController");
                auto* wf = (ctrl && ctrl->is_object()) ? ctrl->find("Win32-Front") : nullptr;
                if (wf && wf->is_bool()) included = wf->as_bool(false);
                else included = t.get_bool("enabled", true);
                if (included) {
                    list.push(tname);
                    ++included_cnt;
                }
            }
        }
        tasks_total += included_cnt;
        // 空列表不进 task_lists：GUI 语义里"无记录=显示全部"，
        // 写空集会被解释为不过滤，与"该实例没有启用任务"难以区分，宁可不动
        if (included_cnt > 0) c.task_lists.set(exec, list);
    }
    rep.set("tasks_imported", tasks_total);
    rep.set("schedulePolicies", c.schedule_policies);
    rep.set("preActions", c.pre_actions);
}

// ---------------------------------------------------------------- MAA

inline void import_maa(const mj::Value& root, Hub& hub, mj::Value& rep, Ctx& c) {
    rep.set("format", "maa");
    auto* cfgs = root.find("Configurations");
    if (!cfgs || !cfgs->is_object() || cfgs->members().empty()) {
        c.add_error("没有 Configurations 对象或为空");
        return;
    }
    std::string cur = root.get_str("Current", "");
    auto* conf = cur.empty() ? nullptr : cfgs->find(cur);
    std::string used_name = cur;
    if (!conf) {
        // Current 缺失/对不上：回退第一份配置
        used_name = cfgs->members().begin()->first;
        conf = cfgs->find(used_name);
    }
    if (!conf || !conf->is_object()) {
        c.add_error("找不到有效的当前配置（Current=" + used_name + "）");
        return;
    }
    std::string label = "MAA " + used_name;
    // 多安装共存：标签加安装目录后缀（如 "MAA Default @MaaAssistantArknights_5"）
    if (!c.install_tag.empty()) label += " @" + c.install_tag;
    mj::Value created = mj::Value::array(), reused = mj::Value::array();
    std::string exec = find_or_create_executor(hub, "maa", label, created, reused, c);
    for (auto& lb : created.items()) rep["executors_created"].push(lb);
    for (auto& lb : reused.items()) rep["executors_reused"].push(lb);
    if (exec.empty()) return;

    std::set<std::string> touched;  // 至少写入过一项参数的任务
    for (auto& kv : conf->members()) {
        const std::string& key = kv.first;
        size_t dot = key.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= key.size()) {
            c.add_skip(key);
            continue;
        }
        std::string task = key.substr(0, dot);
        std::string param = key.substr(dot + 1);

        int sp = special_maa(hub, exec, task, param, kv.second, c);
        if (sp == 1) { touched.insert(task); continue; }
        if (sp == 2) { c.add_skip(key); continue; }

        // 通用：Param 转 snake_case 查目录键表
        const auto& types = task_key_types(task);
        auto it = types.find(snake_case(param));
        if (it == types.end()) { c.add_skip(key); continue; }
        mj::Value out;
        if (!convert_typed(it->second, kv.second, out)) { c.add_skip(key); continue; }
        hub.set_task_param(exec, task, it->first, out);
        ++c.params_written;
        touched.insert(task);
    }
    rep.set("tasks_imported", (int)touched.size());

    // 定时计划提取，两种结构都支持（enabled:false 也记录）：
    //   A) MAA 6.x（gui.new.json）：顶层 Timers.List[] = [{ Id, IsEnabled, Config, Hour, Minute }]
    //      注意 Timer 不在 Global 下，旧写法读不到 → 会把已关闭的定时器误判为启用。
    //   B) MAA 5.x（gui.json）：Global.Timer.TimerN{,Hour,Min,.Config}，纯数字后缀精确匹配。
    // MAA 定时器只指"几点跑哪份配置"，任务序列由 Hub 的 state\task-lists.json 决定。
    bool schedules_from_new = false;
    if (auto* timers = root.find("Timers"); timers && timers->is_object()) {
        if (auto* list = timers->find("List"); list && list->is_array()) {
            for (auto& item : list->items()) {
                if (!item.is_object()) continue;
                mj::Value e = mj::Value::object();
                e.set("exec_id", exec);
                e.set("kind", "maa");
                e.set("label", label);
                bool en = false;
                if (auto* iv = item.find("IsEnabled")) to_bool(*iv, en);
                e.set("enabled", en);
                int hour = 0, min = 0;
                if (auto* h = item.find("Hour")) to_int(*h, hour);
                if (auto* m2 = item.find("Minute")) to_int(*m2, min);
                e.set("hour", hour);
                e.set("min", min);
                e.set("config", item.get_str("Config", used_name));
                int id = (int)c.schedules.items().size();
                if (auto* iv = item.find("Id")) to_int(*iv, id);
                char kbuf[64];
                snprintf(kbuf, sizeof(kbuf), "Timer.Timer%d", id + 1);
                const std::string key = kbuf;
                // 多安装共存：source 加安装目录前缀，否则不同安装的 Timer2 会互相覆盖
                e.set("source", c.install_tag.empty() ? key : (c.install_tag + ":" + key));
                c.schedules.push(e);
            }
            schedules_from_new = true;
        }
    }
    if (!schedules_from_new) {
        if (auto* g = root.find("Global"); g && g->is_object()) {
            for (auto& kv : g->members()) {
                const std::string& key = kv.first;
                if (key.rfind("Timer.Timer", 0) != 0 || key.size() <= 11) continue;
                const std::string suffix = key.substr(11);
                bool numeric = !suffix.empty();
                for (char ch : suffix)
                    if (ch < '0' || ch > '9') { numeric = false; break; }
                if (!numeric) continue;

                mj::Value e = mj::Value::object();
                e.set("exec_id", exec);
                e.set("kind", "maa");
                e.set("label", label);
                bool en = false;
                if (kv.second.is_string() || kv.second.is_bool())
                    to_bool(kv.second, en);
                e.set("enabled", en);
                int hour = 0, min = 0;
                if (auto* h = g->find("Timer.Timer" + suffix + "Hour"))
                    to_int(*h, hour);
                if (auto* m = g->find("Timer.Timer" + suffix + "Min"))
                    to_int(*m, min);
                e.set("hour", hour);
                e.set("min", min);
                e.set("config", g->get_str("Timer.Timer" + suffix + ".Config", used_name));
                // 多安装共存：source 加安装目录前缀，否则不同安装的 Timer2 会互相覆盖
                e.set("source", c.install_tag.empty() ? key : (c.install_tag + ":" + key));
                c.schedules.push(e);
            }
        }
    }
    rep.set("schedules_extracted", (int)c.schedules.items().size());
}

// ---------------------------------------------------------------- 入口

// 编码归一：MAA/MaaEnd 实际导出常见 UTF-16 LE/BE（带 BOM），统一转成 UTF-8 再交给
// 解析器；纯 UTF-8（含带 BOM）原样通过（try_parse 已跳 UTF-8 BOM）。
inline std::string normalize_encoding(const std::string& raw) {
    if (raw.size() >= 2) {
        unsigned char b0 = (unsigned char)raw[0], b1 = (unsigned char)raw[1];
        bool le = (b0 == 0xFF && b1 == 0xFE);
        bool be = (b0 == 0xFE && b1 == 0xFF);
        if (le || be) {
            // 粗查：若后续字节看起来是 ASCII 兼容的 UTF-8（无密集 NUL），按 UTF-8 处理
            // （FFFE+ASCII 更可能是误写，极少见；正常 UTF-16 每字符第二字节为 0）
            size_t nuls = 0;
            size_t scan = std::min<size_t>(raw.size(), 512);
            for (size_t i = 2; i < scan; ++i) if (raw[i] == '\0') ++nuls;
            if (nuls > scan / 4) {
                // 真 UTF-16：宽字符转窄（JSON 导出均为 BMP 内 ASCII/汉字，用 MultiByteToWideChar 组合）
                std::wstring w((raw.size() - 2) / 2, L'\0');
                size_t n = (raw.size() - 2) / 2;
                for (size_t i = 0; i < n; ++i) {
                    unsigned char lo = le ? (unsigned char)raw[2 + i * 2]
                                          : (unsigned char)raw[3 + i * 2];
                    unsigned char hi = le ? (unsigned char)raw[3 + i * 2]
                                          : (unsigned char)raw[2 + i * 2];
                    w[i] = (wchar_t)((hi << 8) | lo);
                }
                // UTF-16 -> UTF-8（自实现，覆盖 BMP + 代理对）
                std::string out;
                out.reserve(n * 3);
                for (size_t i = 0; i < n; ++i) {
                    unsigned int cp = (unsigned int)w[i];
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
                        unsigned int lo2 = (unsigned int)w[i + 1];
                        if (lo2 >= 0xDC00 && lo2 <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo2 - 0xDC00);
                            ++i;
                        }
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xF0 | (cp >> 18));
                        out += (char)(0x80 | ((cp >> 12) & 0x3F));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                }
                return out;
            }
        }
    }
    return raw;
}

// ---------------------------------------------------------------- 路径入口

// 输入既可以是配置文件本身（gui.json / mxu-MaaEnd.json），也可以是 MAA / MaaEnd
// 的安装目录（此时自动定位 config\gui.json 与 config\mxu-MaaEnd.json）。
// 返回 (文件路径, 格式)；找不到时 file 为空并在 error 里说明原因。
inline std::pair<std::string, std::string> resolve_config_path(
        const std::string& input, std::string& error) {
    auto ends_with = [](const std::string& s, const char* suf) {
        size_t n = std::strlen(suf);
        return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suf) == 0;
    };
    std::string p = input;
    while (p.size() > 1 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    if (p.empty()) { error = "路径为空"; return {}; }

    if (util::file_exists(p)) {  // 文件本身
        // MAA 6.x 已把配置迁到 gui.new.json（新结构：顶层 Configurations / Timers /
        // Gui），gui.json 只剩迁移前的历史备份；两者都按 maa 处理，解析器同时兼容
        // 新旧结构（详见 import_maa 的定时计划提取）。
        if (ends_with(p, "gui.new.json")) return { p, "maa" };
        if (ends_with(p, "gui.json")) return { p, "maa" };
        if (ends_with(p, "mxu-maaend.json")) return { p, "maaend" };
        return { p, "" };  // 交给内容自动识别
    }
    if (!util::dir_exists(p)) { error = "路径不存在：" + p; return {}; }

    // 安装目录：MAA 6.x 生效配置是 config\gui.new.json，回退旧版 config\gui.json；
    // MaaEnd 是 config\mxu-MaaEnd.json。顺序即优先级，不要调换。
    const std::pair<const char*, const char*> cand[] = {
        { "config\\gui.new.json", "maa" },
        { "config\\gui.json", "maa" },
        { "config\\mxu-MaaEnd.json", "maaend" },
    };
    for (auto& c2 : cand) {
        std::string f = p + "\\" + c2.first;
        if (util::file_exists(f)) return { f, c2.second };
    }
    error = "目录下没有 config\\gui.new.json / config\\gui.json（MAA）"
            "或 config\\mxu-MaaEnd.json（MaaEnd）：" + p;
    return {};
}

// 前置声明：import_from_path 定义在 import_from_text 之前
inline mj::Value import_from_text(const std::string& text, Hub& hub,
                                  const std::string& kind_hint,
                                  const std::string& install_tag);

// 从 config 文件路径反推安装目录名，用于区分同机多安装：
//   ...\MaaAssistantArknights_5\config\gui.json -> "MaaAssistantArknights_5"
// 反推不出来（盘符根 / 层级不足）时返回空，退回"不区分"的老行为。
inline std::string install_tag_of(const std::string& file) {
    std::string dir = file;
    for (int i = 0; i < 2; ++i) {  // 去掉 \<文件名> 和 \config
        size_t p = dir.find_last_of("\\/");
        if (p == std::string::npos) return "";
        dir = dir.substr(0, p);
    }
    if (dir.size() <= 2) return "";  // "C:" / "C:" 之类，无目录名
    size_t p = dir.find_last_of("\\/");
    std::string name = (p == std::string::npos) ? dir : dir.substr(p + 1);
    if (name.size() > 48) name.resize(48);
    return name;
}

// 从文件或安装目录导入；kind_hint 仅对"内容无法自动识别的文件"生效
inline mj::Value import_from_path(const std::string& path, Hub& hub,
                                  const std::string& kind_hint = "") {
    mj::Value rep = mj::Value::object();
    rep.set("executors_created", mj::Value::array());
    rep.set("executors_reused", mj::Value::array());
    std::string error;
    auto [file, fmt] = resolve_config_path(path, error);
    if (file.empty()) {
        mj::Value errs = mj::Value::array();
        errs.push(error);
        rep.set("errors", errs);
        rep.set("params_written", 0);
        rep.set("keys_skipped", mj::Value::array());
        rep.set("keys_skipped_total", 0);
        rep.set("task_lists", mj::Value::object());
        rep.set("schedules", mj::Value::array());
        return rep;
    }
    std::string text;
    if (!util::read_file(file, text)) {
        mj::Value errs = mj::Value::array();
        errs.push("无法读取文件：" + file);
        rep.set("errors", errs);
        rep.set("task_lists", mj::Value::object());
        rep.set("schedules", mj::Value::array());
        return rep;
    }
    std::string hint = fmt.empty() ? kind_hint : fmt;
    const std::string tag = install_tag_of(file);
    rep = import_from_text(text, hub, hint, tag);
    rep.set("source_file", file);
    rep.set("install_tag", tag);
    return rep;
}

// 解析并导入；返回报告对象（供 GUI 展示与 task_lists 合并）
// kind_hint：CLI 显式指定格式时跳过自动识别（"maa" / "maaend"，其余值按自动）
// install_tag：安装目录名，用于给执行器标签和定时器 source 加区分后缀
inline mj::Value import_from_text(const std::string& text, Hub& hub,
                                  const std::string& kind_hint = "",
                                  const std::string& install_tag = "") {
    mj::Value rep = mj::Value::object();
    rep.set("executors_created", mj::Value::array());
    rep.set("executors_reused", mj::Value::array());
    Ctx c;
    c.install_tag = install_tag;
    rep.set("install_tag", install_tag);
    mj::Value root;
    std::string normalized = normalize_encoding(text);
    if (!mj::try_parse(normalized, root) || !root.is_object()) {
        c.add_error("文件不是有效的 JSON 对象");
    } else {
        auto* insts = root.find("instances");
        auto* cfgs = root.find("Configurations");
        std::string hint = lower_copy(kind_hint);
        if (insts && insts->is_array()) {
            rep.set("format", "maaend");
            import_maaend(root, hub, rep, c);
        } else if (cfgs && cfgs->is_object()) {
            rep.set("format", "maa");
            import_maa(root, hub, rep, c);
        } else if (hint == "maaend") {
            rep.set("format", "maaend");
            import_maaend(root, hub, rep, c);
        } else if (hint == "maa") {
            rep.set("format", "maa");
            import_maa(root, hub, rep, c);
        } else {
            c.add_error("无法识别的配置格式（既非 MaaEnd instances，也非 MAA Configurations）");
        }
    }
    rep.set("params_written", c.params_written);
    rep.set("keys_skipped", c.skipped);
    rep.set("keys_skipped_total", c.skipped_total);
    rep.set("errors", c.errors);
    rep.set("task_lists", c.task_lists);
    rep.set("schedules", c.schedules);
    return rep;
}

// 报告 -> 通知栏文案（含"失败"字样时 GUI 会以错误色显示）
inline std::string report_message(const mj::Value& rep) {
    auto* errs = rep.find("errors");
    if (errs && errs->is_array() && !errs->items().empty()) {
        std::string m = "导入失败：" + errs->items()[0].as_string();
        if (errs->items().size() > 1) {
            m += "（等 " + std::to_string(errs->items().size()) + " 个问题）";
        }
        return m;
    }
    std::string fmt = rep.get_str("format", "");
    std::string m = (fmt == "maaend" ? "MaaEnd" : "MAA");
    m += " 配置导入完成";
    std::string tag = rep.get_str("install_tag", "");
    if (!tag.empty()) m += "（" + tag + "）";
    int created = 0, reused = 0;
    if (auto* a = rep.find("executors_created"); a && a->is_array()) created = (int)a->items().size();
    if (auto* a = rep.find("executors_reused"); a && a->is_array()) reused = (int)a->items().size();
    if (created + reused > 0) {
        m += "：执行器 ";
        if (created) m += "新建 " + std::to_string(created);
        if (created && reused) m += "、";
        if (reused) m += "复用 " + std::to_string(reused);
    }
    int tasks = (int)rep.get_int("tasks_imported", 0);
    int params = (int)rep.get_int("params_written", 0);
    m += "，任务 " + std::to_string(tasks) + " 个，参数 " + std::to_string(params) + " 项";
    int skipped = (int)rep.get_int("keys_skipped_total", 0);
    if (skipped) m += "，跳过未知键 " + std::to_string(skipped) + " 个";
    int scheds = 0;
    if (auto* a = rep.find("schedules"); a && a->is_array()) scheds = (int)a->items().size();
    if (scheds) m += "，定时计划 " + std::to_string(scheds) + " 条";
    return m;
}

} // namespace import_config
} // namespace hub
