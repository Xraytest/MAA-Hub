// Hub：统一管理器（执行器模型）
// 执行器 (Executor) = 一个独立的 MAA/MaaEnd 实例：
//   - 独立 worker 子进程（各自加载自己的核心 DLL，互不冲突）
//   - 绑定一个核心 (core) 与一个账号 (account)
//   - 多个执行器可同时运行，实现多账号并行
#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "accounts.hpp"
#include "adapters/adapter.hpp"
#include "core_manager.hpp"
#include "json.hpp"
#include "planner.hpp"
#include "settings.hpp"
#include "util.hpp"
#include "worker_client.hpp"

namespace hub {

struct HubEvent {
    std::string id_;
    double ts = 0;
    std::string exec_id;
    std::string exec_label;
    std::string app;          // maa | maaend
    std::string source;
    std::string category;
    std::string what;
    std::string task;
    int64_t task_id = 0;
    mj::Value details = mj::Value::object();
    std::string raw;

    mj::Value to_json() const {
        mj::Value v = mj::Value::object();
        v.set("id", id_);
        v.set("ts", ts);
        v.set("exec_id", exec_id);
        v.set("exec_label", exec_label);
        v.set("app", app);
        v.set("source", source);
        v.set("category", category);
        v.set("what", what);
        v.set("task", task);
        v.set("task_id", task_id ? mj::Value(task_id) : mj::Value(nullptr));
        v.set("details", details);
        v.set("raw", raw);
        return v;
    }
};

// 业务数据收集键（与 Python 版一致）
inline const std::map<std::string, std::string>& data_keys() {
    static const std::map<std::string, std::string> k = {
        { "StageDrops", "stage_drops" },
        { "DepotInfo", "depot" },
        { "OperBoxInfo", "oper_box" },
        { "ProductOfFacility", "infrast_products" },
        { "RecruitResult", "recruit_results" },
        { "StageInfo", "stage_info" },
    };
    return k;
}

struct ExecutorEntry {
    std::string id;
    std::string kind;          // maa | maaend
    std::string label;
    std::string account_id;
    std::string core_id;
    bool enabled = true;
    double created_at = 0;
    mj::Value config = mj::Value::object();   // worker 配置

    // 运行时（不持久化）
    std::shared_ptr<WorkerClient> worker;
    mj::Value catalog = mj::Value::array();      // 目录缓存
    double catalog_ts = 0;                       // 目录缓存时间戳
    mj::Value status_cache = mj::Value::object();// 状态缓存
    double status_ts = 0;                        // 状态缓存时间戳

    mj::Value to_json() const {
        mj::Value v = mj::Value::object();
        v.set("id", id);
        v.set("kind", kind);
        v.set("label", label);
        v.set("account_id", account_id);
        v.set("core_id", core_id);
        v.set("enabled", enabled);
        v.set("created_at", created_at);
        return v;
    }
    static ExecutorEntry from_json(const mj::Value& v) {
        ExecutorEntry e;
        e.id = v.get_str("id", "");
        e.kind = v.get_str("kind", "");
        e.label = v.get_str("label", "");
        e.account_id = v.get_str("account_id", "");
        e.core_id = v.get_str("core_id", "");
        e.enabled = v.get_bool("enabled", true);
        e.created_at = v.get_int("created_at", 0);
        return e;
    }
};

class Hub {
public:
    explicit Hub(mj::Value config)
        : config_(std::move(config)) {
        std::string state_dir = util::exe_dir() + "\\state";
        util::ensure_dir(state_dir);
        accounts_ = std::make_unique<AccountStore>(state_dir);
        cores_ = std::make_unique<CoreManager>(state_dir,
                                               CoreManager::default_cores_root());
        settings_ = std::make_unique<SettingsStore>(state_dir);
        load_executors();
        load_llm_config();
    }

    ~Hub() {
        // 通知执行线程退出，再并行关停 worker
        stopping_ = true;
        stop_execution();
        std::vector<std::thread> stops;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : executors_) {
                auto w = kv.second.worker;
                if (!w) continue;
                stops.emplace_back([w] { w->shutdown(); });
            }
        }
        for (auto& t : stops) t.join();
        if (save_thread_.joinable()) save_thread_.join();
    }

    // ================================================================ 执行器
    std::vector<ExecutorEntry> executors() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<ExecutorEntry> out;
        for (auto& kv : executors_) out.push_back(kv.second);
        return out;
    }

    bool get_executor(const std::string& id, ExecutorEntry& out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = executors_.find(id);
        if (it == executors_.end()) return false;
        out = it->second;
        return true;
    }

    // 创建执行器：kind + label + account + core（core 为空用默认核心）
    std::string create_executor(const std::string& kind, const std::string& label,
                                const std::string& account_id = "",
                                const std::string& core_id = "") {
        CoreInfo core;
        if (!core_id.empty()) {
            if (!cores_->get(core_id, core)) return "";
        } else {
            core = cores_->default_core(kind);
        }
        if (core.id.empty()) return "";

        ExecutorEntry e;
        e.id = "exec-" + random_hex(6);
        e.kind = kind;
        e.label = label.empty() ? (kind == "maa" ? "MAA 实例" : "MaaEnd 实例") : label;
        e.account_id = account_id;
        e.core_id = core.id;
        e.created_at = util::now_sec();
        e.config = build_worker_config(e);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            executors_[e.id] = e;
        }
        // 不在这里 spawn：由 start_all() 统一并行启动，避免串行等待拖慢启动
        save_executors();
        return e.id;
    }

    bool remove_executor(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = executors_.find(id);
        if (it == executors_.end()) return false;
        if (it->second.worker) it->second.worker->shutdown();
        executors_.erase(it);
        save_executors();
        return true;
    }

    bool update_executor(const std::string& id, const mj::Value& patch) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = executors_.find(id);
        if (it == executors_.end()) return false;
        if (patch.has("label")) it->second.label = patch.get_str("label", it->second.label);
        if (patch.has("account_id")) it->second.account_id = patch.get_str("account_id", "");
        if (patch.has("enabled")) it->second.enabled = patch.get_bool("enabled", true);
        save_executors();
        return true;
    }

    // ================================================================ 执行器操作
    bool connect_executor(const std::string& id) {
        auto w = worker_of(id);
        if (!w) return false;
        mj::Value r;
        // 连接超时 60s（原 180s 太长：期间 backend 线程被独占，
        // 用户其他点击全部排队，表现为界面"死机"）
        return w->call("connect", mj::Value::object(), 60000, r);
    }

    void disconnect_executor(const std::string& id) {
        auto w = worker_of(id);
        if (!w) return;
        mj::Value r;
        w->call("disconnect", mj::Value::object(), 30000, r);
    }

    bool stop_executor(const std::string& id) {
        auto w = worker_of(id);
        if (!w) return false;
        mj::Value r;
        return w->call("stop", mj::Value::object(), 60000, r);
    }

    mj::Value executor_status(const std::string& id, double ttl_sec = 0.4) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it != executors_.end() && it->second.status_cache.is_object()
                && util::now_sec() - it->second.status_ts < ttl_sec) {
                return it->second.status_cache;
            }
        }
        auto w = worker_of(id);
        if (!w) return mj::Value::object();
        mj::Value r;
        if (w->call("status", mj::Value::object(), 15000, r)) {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it != executors_.end()) {
                it->second.status_cache = r;
                it->second.status_ts = util::now_sec();
            }
            return r;
        }
        return mj::Value::object();
    }

    // 任务目录：带 TTL 缓存（目录极少变化，避免每次快照都做大 JSON 管道往返）
    mj::Value executor_catalog(const std::string& id, double ttl_sec = 30.0) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it != executors_.end() && it->second.catalog.is_array()
                && !it->second.catalog.items().empty()
                && util::now_sec() - it->second.catalog_ts < ttl_sec) {
                return it->second.catalog;
            }
        }
        auto w = worker_of(id);
        if (!w) return mj::Value::array();
        mj::Value r;
        if (w->call("tasks", mj::Value::object(), 15000, r) && r.is_array()) {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it != executors_.end()) {
                it->second.catalog = r;
                it->second.catalog_ts = util::now_sec();
            }
            return r;
        }
        return mj::Value::array();
    }

    // 把 UI 面板的辅助键转换为 MaaCore/MaaEnd 真实参数名
    // （面板用更友好的键，下发前在此归一化）
    static mj::Value normalize_task_params(const std::string& kind,
                                           const std::string& task,
                                           const mj::Value& in) {
        mj::Value out = in;
        if (kind != "maa") return out;
        auto num = [&](const std::string& k, int def) -> int64_t {
            auto* v = in.find(k);
            return (v && v->is_number()) ? v->as_int() : def;
        };
        auto boolean = [&](const std::string& k, bool def) -> bool {
            auto* v = in.find(k);
            return (v && (v->is_bool() || v->is_number())) ? v->as_bool() : def;
        };
        if (task == "Infrast") {
            // mode: 0默认 / 10000自定义 / 20000轮换
            int m = (int)num("mode", 0);
            out.set("mode", (int64_t)(m == 1 ? 10000 : m == 2 ? 20000 : 0));
            // drones: 索引 -> MAA 字符串
            static const char* kDrones[] = { "_NotUse", "Money", "SyntheticJade",
                                             "CombatRecord", "Chip" };
            int d = (int)num("drones_idx", 0);
            if (d < 0 || d > 4) d = 0;
            out.set("drones", std::string(kDrones[d]));
        }
        if (task == "Recruit") {
            // confirm_3..6 -> confirm 数组；select_tags -> select 数组
            mj::Value confirm = mj::Value::array();
            mj::Value select = mj::Value::array();
            for (int lv = 3; lv <= 6; ++lv) {
                std::string k = "confirm_" + std::to_string(lv);
                if (boolean(k, false)) confirm.push((int64_t)lv);
            }
            std::string tags = in.get_str("select_tags", "");
            std::string cur;
            for (char ch : tags + ";") {
                if (ch == ';') {
                    if (!cur.empty()) {
                        try { select.push((int64_t)std::stoi(cur)); } catch (...) {}
                        cur.clear();
                    }
                } else if (ch != ' ') {
                    cur += ch;
                }
            }
            out.set("confirm", std::move(confirm));
            out.set("select", std::move(select));
        }
        if (task == "Mall") {
            // blacklist_str -> blacklist 数组；buy_first 字符串 -> 数组
            std::string bl = in.get_str("blacklist_str", "");
            if (!bl.empty()) {
                mj::Value arr = mj::Value::array();
                std::string cur;
                for (char ch : bl + ";") {
                    if (ch == ';') {
                        if (!cur.empty()) { arr.push(cur); cur.clear(); }
                    } else if (ch != ' ') {
                        cur += ch;
                    }
                }
                out.set("blacklist", std::move(arr));
            }
        }
        if (task == "StartUp") {
            // account_name_set 只是面板开关，真实参数 account_name 由账号注入提供
        }
        return out;
    }

    int64_t run_on_executor(const std::string& id, const std::string& task,
                            const mj::Value& params) {
        auto w = worker_of(id);
        if (!w) return 0;
        ExecutorEntry e;
        get_executor(id, e);
        Account acc;
        bool has_acc = false;
        if (!e.account_id.empty()) has_acc = accounts_->get(e.account_id, acc);
        static const std::vector<std::string> aware_tasks = { "StartUp", "CloseDown", "AccountSwitch" };
        bool is_aware = std::find(aware_tasks.begin(), aware_tasks.end(), task) != aware_tasks.end();
        mj::Value normalized = normalize_task_params(e.kind, task, params);
        mj::Value final_params = (is_aware || has_acc)
            ? AccountStore::apply_to_task(e.kind, task, normalized, has_acc ? &acc : nullptr)
            : normalized;
        mj::Value req = mj::Value::object();
        req.set("task", task);
        req.set("params", final_params);
        mj::Value r;
        if (!w->call("run", req, 60000, r)) return 0;
        int64_t tid = r.get_int("task_id", 0);
        if (has_acc && tid > 0) accounts_->mark_run(e.account_id);
        return tid;
    }

    // ================================================================ 快照
    // 即时快照：只读内存/持久化文件，不做任何 worker 管道往返（毫秒级）。
    // 用于启动首帧——让左侧执行器立刻可见，运行时状态稍后由完整快照补齐。
    mj::Value snapshot_fast() {
        mj::Value v = mj::Value::object();
        std::string today = util::today_str();
        mj::Value execs = mj::Value::array();
        for (auto& e : executors()) {
            mj::Value j = e.to_json();
            j.set("conn", "unknown");
            j.set("run", "idle");
            j.set("version", "");
            j.set("last_error", "");
            j.set("task_count", 0);
            CoreInfo core;
            if (!cores_->get(e.core_id, core) || core.path.empty()) {
                core = cores_->default_core(e.kind);
            }
            if (!core.id.empty()) {
                j.set("core_name", core.name);
                j.set("core_version", core.version);
            }
            Account acc;
            if (!e.account_id.empty() && accounts_->get(e.account_id, acc)) {
                j.set("account_label", acc.label);
                j.set("daily_done_today", acc.daily_done_date == today);
            } else {
                j.set("account_label", "");
                j.set("daily_done_today", false);
            }
            execs.push(std::move(j));
        }
        v.set("executors", std::move(execs));
        mj::Value accs = mj::Value::array();
        for (auto& a : accounts_->list()) {
            mj::Value j = a.to_json();
            Account act;
            j.set("is_active", accounts_->active(a.game, act) && act.id == a.id);
            j.set("daily_done_today", a.daily_done_date == today);
            accs.push(std::move(j));
        }
        v.set("accounts", std::move(accs));
        mj::Value cores = mj::Value::array();
        for (auto& c : cores_->list()) cores.push(c.to_json());
        v.set("cores", std::move(cores));
        v.set("plans", mj::Value::array());
        v.set("events", mj::Value::array());
        v.set("data", mj::Value::object());
        v.set("settings", settings_->get().to_json());
        return v;
    }

    mj::Value snapshot() {
        mj::Value v = mj::Value::object();
        mj::Value execs = mj::Value::array();
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : executors_) ids.push_back(kv.first);
        }
        std::string today = util::today_str();
        for (auto& id : ids) {
            ExecutorEntry e;
            if (!get_executor(id, e)) continue;
            mj::Value j = e.to_json();
            mj::Value st = executor_status(id);
            j.set("conn", st.get_str("conn", "unknown"));
            j.set("run", st.get_str("run", "idle"));
            j.set("version", st.get_str("version", ""));
            j.set("last_error", st.get_str("last_error", ""));
            j.set("task_count", st.get_int("task_count", 0));
            if (st.has("controller")) j.set("controller", st["controller"]);
            Account acc;
            if (!e.account_id.empty() && accounts_->get(e.account_id, acc)) {
                j.set("account_label", acc.label);
                j.set("daily_done_today", acc.daily_done_date == today);
            } else {
                j.set("account_label", "");
                j.set("daily_done_today", false);
            }
            CoreInfo core;
            if (!cores_->get(e.core_id, core) || core.path.empty()) {
                core = cores_->default_core(e.kind);
            }
            if (!core.id.empty()) {
                j.set("core_name", core.name);
                j.set("core_version", core.version);
            }
            // 浠诲姟鐩綍锛堝悗鍙扮嚎绋嬫煡璇紝UI 鍙蹇収锛岀粷涓嶉樆濉炴覆鏌擄級
            j.set("catalog", executor_catalog(id));
            execs.push(std::move(j));
        }
        v.set("executors", std::move(execs));
        mj::Value accs = mj::Value::array();
        for (auto& a : accounts_->list()) {
            mj::Value j = a.to_json();
            Account act;
            j.set("is_active", accounts_->active(a.game, act) && act.id == a.id);
            j.set("daily_done_today", a.daily_done_date == today);
            accs.push(std::move(j));
        }
        v.set("accounts", std::move(accs));
        mj::Value cores = mj::Value::array();
        for (auto& c : cores_->list()) cores.push(c.to_json());
        v.set("cores", std::move(cores));
        v.set("plans", plans(50));
        mj::Value evs = mj::Value::array();
        for (auto& e : recent_events(200)) evs.push(e->to_json());
        v.set("events", std::move(evs));
        mj::Value data = mj::Value::object();
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            for (auto& kv : data_) data.set(kv.first, kv.second);
        }
        v.set("data", std::move(data));
        v.set("settings", settings_->get().to_json());
        return v;
    }

    // ================================================================ 事件
    void on_worker_event(const std::string& exec_id, const AdapterEvent& ev) {
        auto he = std::make_shared<HubEvent>();
        static std::atomic<int64_t> seq{ 0 };
        he->id_ = std::to_string(++seq);
        he->ts = util::now_sec();
        he->exec_id = exec_id;
        ExecutorEntry e;
        if (get_executor(exec_id, e)) he->exec_label = e.label;
        he->app = ev.app;
        he->source = ev.source;
        he->category = ev.category;
        he->what = ev.what;
        he->task = ev.task;
        he->task_id = ev.task_id;
        he->details = ev.details;
        he->raw = ev.raw;
        {
            std::lock_guard<std::mutex> lk(ev_mtx_);
            events_.push_back(he);
            if (events_.size() > 500) events_.erase(events_.begin(), events_.begin() + 250);
        }
        auto it = data_keys().find(ev.what);
        if (it != data_keys().end()) {
            {
                std::lock_guard<std::mutex> lk(data_mtx_);
                data_[exec_id][it->second] = ev.details;
            }
            schedule_save();
        }
        for (auto& cb : subscribers_) cb(*he);
    }

    void subscribe(std::function<void(const HubEvent&)> cb) {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        subscribers_.push_back(std::move(cb));
    }

    std::vector<std::shared_ptr<HubEvent>> recent_events(int limit = 100,
                                                         const std::string& exec_id = "") {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        std::vector<std::shared_ptr<HubEvent>> out;
        for (auto& e : events_) {
            if (!exec_id.empty() && e->exec_id != exec_id) continue;
            out.push_back(e);
        }
        if ((int)out.size() > limit) out.erase(out.begin(), out.end() - limit);
        return out;
    }

    // ================================================================ 计划
    mj::Value propose_plan(const std::string& goal, const std::string& planner_name = "") {
        mj::Value snap = snapshot();
        // 目录按 kind 聚合（取任一执行器的目录）
        mj::Value catalog = mj::Value::object();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : executors_) {
                auto w = kv.second.worker;
                if (!w) continue;
                mj::Value r;
                if (w->call("tasks", mj::Value::object(), 15000, r) && r.is_array()) {
                    catalog.set(kv.second.kind, r);
                    break;   // 同 kind 目录相同，取一个即可
                }
            }
        }
        Plan plan;
        (void)0;
        if (planner_name == "heuristic") {
            HeuristicPlanner hp;
            plan = hp.plan(goal, snap, catalog);
        } else {
            LlmPlanner lp(llm_cfg_, HeuristicPlanner{});
            plan = lp.plan(goal, snap, catalog, accounts_json());
        }
        {
            std::lock_guard<std::mutex> lk(plans_mtx_);
            plans_.push_back(plan);
            if (plans_.size() > 50) plans_.erase(plans_.begin());
        }
        schedule_save();
        return plan.to_json();
    }

    bool execute_plan(const std::string& plan_id) {
        Plan plan;
        {
            std::lock_guard<std::mutex> lk(plans_mtx_);
            for (auto& p : plans_) {
                if (p.id == plan_id) { plan = p; break; }
            }
        }
        if (plan.id.empty()) return false;
        {
            std::lock_guard<std::mutex> lk(plans_mtx_);
            for (auto& p : plans_) {
                if (p.id == plan_id) p.status = "confirmed";
            }
        }
        stop_flag_ = false;
        exec_thread_ = std::thread([this, plan] { exec_worker(plan); });
        return true;
    }

    void stop_execution() {
        stop_flag_ = true;
        if (exec_thread_.joinable()) exec_thread_.detach();
    }

    mj::Value plans(int limit = 20) {
        std::lock_guard<std::mutex> lk(plans_mtx_);
        mj::Value arr = mj::Value::array();
        size_t start = plans_.size() > (size_t)limit ? plans_.size() - limit : 0;
        for (size_t i = start; i < plans_.size(); ++i) arr.push(plans_[i].to_json());
        return arr;
    }

    bool update_plan_status(const std::string& id, const std::string& status) {
        {
            std::lock_guard<std::mutex> lk(plans_mtx_);
            for (auto& p : plans_) {
                if (p.id == id) { p.status = status; break; }
            }
        }
        schedule_save();
        return true;
    }

    AccountStore& accounts() { return *accounts_; }
    CoreManager& cores() { return *cores_; }
    SettingsStore& settings() { return *settings_; }
    AppSettings app_settings() const { return settings_->get(); }
    mj::Value& config() { return config_; }

    // 后台线程：并行拉起所有执行器 worker
    // 串行时总耗时 = Σ(每个执行器 spawn+discover+load)，MaaEnd 的 load 要 ~6s，
    // 多执行器时启动被拖到 7s+。各执行器彼此独立，改为并发后总耗时 ≈ max(单个)。
    void start_all() {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : executors_) ids.push_back(kv.first);
        }
        if (ids.empty()) return;
        std::vector<std::thread> threads;
        for (auto& id : ids) {
            threads.emplace_back([this, id] { spawn_worker(id); });
        }
        for (auto& t : threads) t.join();
    }

    // 为每个已安装核心类型创建默认执行器（无核心时不创建，由 UI 引导拉取）
    void ensure_default_executors() {
        std::vector<ExecutorEntry> existing = executors();
        bool has_maa = false, has_end = false;
        for (auto& e : existing) {
            if (e.kind == "maa") has_maa = true;
            if (e.kind == "maaend") has_end = true;
        }
        if (!has_maa && !cores_->default_core("maa").id.empty()) {
            create_executor("maa", "MAA 默认实例");
        }
        if (!has_end && !cores_->default_core("maaend").id.empty()) {
            create_executor("maaend", "MaaEnd 默认实例");
        }
    }

    void refresh_catalogs() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& kv : executors_) {
            if (!kv.second.worker) continue;
            mj::Value r;
            if (kv.second.worker->call("tasks", mj::Value::object(), 15000, r) && r.is_array()) {
                kv.second.catalog = r;
            }
        }
    }

private:
    std::shared_ptr<WorkerClient> worker_of(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = executors_.find(id);
        if (it == executors_.end()) return nullptr;
        return it->second.worker;
    }

    void spawn_worker(const std::string& id) {
        std::shared_ptr<WorkerClient> w;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it == executors_.end()) return;
            if (it->second.worker && it->second.worker->alive()) return;
            w = std::make_shared<WorkerClient>(
                it->second.kind, id, it->second.config,
                [this, id](const AdapterEvent& ev) { this->on_worker_event(id, ev); });
            it->second.worker = w;
        }
        // 注意：start/discover/load 必须在 mtx_ 之外调用——worker 事件回调
        // （on_worker_event）也要拿 mtx_，持锁调用会死锁。
        if (!w->start()) {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it != executors_.end()) it->second.worker.reset();
            return;
        }
        mj::Value r;
        w->call("discover", mj::Value::object(), 180000, r);
        w->call("load", mj::Value::object(), 180000, r);
    }

    void exec_worker(Plan plan) {
        for (auto& step : plan.steps) {
            if (stop_flag_ || stopping_) {
                update_plan_status(plan.id, "stopped");
                return;
            }
            auto w = worker_of(step.exec_id);
            if (!w) {
                update_plan_status(plan.id, "failed");
                return;
            }
            mj::Value st;
            if (w->call("status", mj::Value::object(), 15000, st)) {
                if (st.get_str("conn", "") != "connected") {
                    if (!connect_executor(step.exec_id)) continue;
                }
            }
            int64_t tid = run_on_executor(step.exec_id, step.task, step.params);
            if (tid <= 0) {
                update_plan_status(plan.id, "failed");
                return;
            }
            ExecutorEntry e;
            if (get_executor(step.exec_id, e) && !e.account_id.empty()
                && (step.task == "Award" || step.task == "DailyRewards")) {
                accounts_->mark_daily_done(e.account_id);
            }
            // 轮询任务完成；stopping_ 时立即退出，避免 Hub 析构后仍访问成员
            int idle_rounds = 0;
            while (!stop_flag_ && !stopping_) {
                mj::Value s2;
                if (w->call("status", mj::Value::object(), 15000, s2)) {
                    if (s2.get_str("run", "idle") != "running") break;
                    idle_rounds = 0;
                } else {
                    if (++idle_rounds > 3) break;   // worker 无响应，放弃等待
                }
                Sleep(500);
            }
        }
        update_plan_status(plan.id, (stop_flag_ || stopping_) ? "stopped" : "done");
    }

    std::vector<mj::Value> accounts_json() const {
        std::vector<mj::Value> out;
        std::string today = util::today_str();
        for (auto& a : accounts_->list()) {
            mj::Value j = a.to_json();
            j.set("daily_done_today", a.daily_done_date == today);
            out.push_back(std::move(j));
        }
        return out;
    }

    mj::Value build_worker_config(const ExecutorEntry& e) const {
        CoreInfo core;
        // 绑定的核心可能已被删除：回退到该类型的默认核心（保证可用性）
        if (!cores_->get(e.core_id, core) || core.path.empty()) {
            core = cores_->default_core(e.kind);
        }
        mj::Value wcfg = mj::Value::object();
        wcfg.set("install_dir", core.path);
        wcfg.set("display_name", e.label);
        wcfg.set("game_window_class", "UnityWndClass");
        wcfg.set("game_window_title_regex", e.kind == "maa" ? "Arknights" : "Endfield");
        mj::Value conn = mj::Value::object();
        conn.set("mode", "win32");
        conn.set("screencap_method", 32);
        conn.set("mouse_method", 1);
        conn.set("keyboard_method", 1);
        wcfg.set("connect", std::move(conn));
        return wcfg;
    }

    void load_executors() {
        std::string content;
        if (!util::read_file(util::exe_dir() + "\\state\\executors.json", content)) return;
        mj::Value v;
        if (!mj::try_parse(content, v)) return;
        if (auto* arr = v.find("executors"); arr && arr->is_array()) {
            for (auto& e : arr->items()) {
                ExecutorEntry entry = ExecutorEntry::from_json(e);
                entry.config = build_worker_config(entry);
                executors_[entry.id] = entry;
            }
        }
    }

    void save_executors() {
        mj::Value v = mj::Value::object();
        mj::Value arr = mj::Value::array();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : executors_) arr.push(kv.second.to_json());
        }
        v.set("executors", std::move(arr));
        util::write_file(util::exe_dir() + "\\state\\executors.json",
                         mj::Value::dump_pretty(v));
    }

    void load_llm_config() {
        if (auto* llm = config_.find("llm")) {
            llm_cfg_.enabled = llm->get_bool("enabled", false);
            llm_cfg_.base_url = llm->get_str("base_url", llm_cfg_.base_url);
            llm_cfg_.api_key = llm->get_str("api_key", "");
            llm_cfg_.model = llm->get_str("model", llm_cfg_.model);
            llm_cfg_.temperature = (double)llm->get_int("temperature", 0);
            llm_cfg_.max_tokens = (int)llm->get_int("max_tokens", 2000);
            llm_cfg_.timeout_sec = (int)llm->get_int("timeout_sec", 60);
        }
    }

    void schedule_save() {
        uint64_t now = GetTickCount64();
        if (now - last_save_ < 1000) return;
        last_save_ = now;
        save_state();
    }

    void save_state() {
        mj::Value v = mj::Value::object();
        v.set("updated_at", util::now_sec());
        v.set("plans", plans(50));
        mj::Value data = mj::Value::object();
        {
            std::lock_guard<std::mutex> lk(data_mtx_);
            for (auto& kv : data_) data.set(kv.first, kv.second);
        }
        v.set("data", std::move(data));
        util::write_file(util::exe_dir() + "\\state\\state.json",
                         mj::Value::dump_pretty(v));
    }

    static std::string random_hex(size_t bytes) {
        static std::mt19937_64 rng{ std::random_device{}() };
        std::string out;
        const char* hex = "0123456789abcdef";
        for (size_t i = 0; i < bytes; ++i) {
            unsigned v = (unsigned)(rng() & 0xFF);
            out += hex[v >> 4];
            out += hex[v & 15];
        }
        return out;
    }

    mj::Value config_;
    std::unique_ptr<AccountStore> accounts_;
    std::unique_ptr<CoreManager> cores_;
    std::unique_ptr<SettingsStore> settings_;
    LlmPlanner::Config llm_cfg_;

    mutable std::mutex mtx_;
    std::map<std::string, ExecutorEntry> executors_;

    std::mutex ev_mtx_;
    std::vector<std::shared_ptr<HubEvent>> events_;
    std::vector<std::function<void(const HubEvent&)>> subscribers_;
    std::mutex data_mtx_;
    std::map<std::string, mj::Value> data_;

    std::mutex plans_mtx_;
    std::vector<Plan> plans_;
    std::atomic<bool> stop_flag_{ false };
    std::atomic<bool> stopping_{ false };
    std::thread exec_thread_;
    std::thread save_thread_;
    uint64_t last_save_ = 0;
};

} // namespace hub
