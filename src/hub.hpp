// Hub：统一管理器（执行器模型）
// 执行器 (Executor) = 一个独立的 MAA/MaaEnd 实例：
//   - 独立 worker 子进程（各自加载自己的核心 DLL，互不冲突）
//   - 绑定一个核心 (core) 与一个账号 (account)
//   - 多个执行器可同时运行，实现多账号并行
#pragma once
#include <algorithm>
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
    mj::Value connect = mj::Value::object();  // 用户覆盖的连接参数（持久化）

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
        if (connect.is_object() && !connect.members().empty()) v.set("connect", connect);
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
        if (auto* c = v.find("connect"); c && c->is_object()) e.connect = *c;
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
        load_task_params();
        load_schedules();
        load_llm_config();
        sched_thread_ = std::thread([this] { scheduler_loop(); });
    }

    ~Hub() {
        // 通知执行线程退出，再并行关停 worker
        util::trace("hub dtor begin");
        stopping_ = true;
        stop_execution();
        // 调度器线程可能正跑 run_task_sequence（同步、可长达数小时），不能无限等：
        // sched_done_ 由调度器线程退出前自设，有界等 1.5s；仍没退就 detach
        //（joinable 的 std::thread 析构 = terminate，必须先 detach 再销毁 Hub）
        if (sched_thread_.joinable()) {
            for (int i = 0; i < 150 && !sched_done_.load(); ++i) Sleep(10);
            if (sched_done_.load()) sched_thread_.join();
            else { util::trace("hub dtor sched still running, detach"); sched_thread_.detach(); }
        }
        std::vector<std::thread> stops;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : executors_) {
                auto w = kv.second.worker;
                if (!w) continue;
                util::trace("hub dtor shutdown " + kv.second.kind + " " + kv.first);
                stops.emplace_back([w] { w->shutdown(); });
            }
        }
        for (auto& t : stops) t.join();
        util::trace("hub dtor workers stopped");
        if (save_thread_.joinable()) save_thread_.join();
        util::trace("hub dtor end");
    }

    // ================================================================ 执行器
    std::vector<ExecutorEntry> executors() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<ExecutorEntry> out;
        for (auto& kv : executors_) out.push_back(kv.second);
        // map 按 id 迭代 = 按随机十六进制排，左列顺序会随机；
        // 按创建时间稳定排，ids[0] 默认选中才可预期
        std::sort(out.begin(), out.end(), [](const ExecutorEntry& a, const ExecutorEntry& bb) {
            return a.created_at != bb.created_at ? a.created_at < bb.created_at
                                                   : a.id < bb.id;
        });
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
        std::shared_ptr<WorkerClient> old;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it == executors_.end()) return false;
            old = it->second.worker;
            it->second.worker.reset();
            executors_.erase(it);
            save_executors_locked();
        }
        // shutdown 会等 worker 线程退出，而 worker 事件回调也要拿 mtx_，必须在锁外调用
        if (old) old->shutdown();
        return true;
    }

    bool update_executor(const std::string& id, const mj::Value& patch) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = executors_.find(id);
        if (it == executors_.end()) return false;
        if (patch.has("label")) it->second.label = patch.get_str("label", it->second.label);
        if (patch.has("account_id")) it->second.account_id = patch.get_str("account_id", "");
        if (patch.has("enabled")) it->second.enabled = patch.get_bool("enabled", true);
        save_executors_locked();
        return true;
    }

    // 覆盖某执行器的连接参数（mode / adb_path / touch_mode / mouse_method …）并重启其 worker
    bool update_executor_connect(const std::string& id, const mj::Value& patch) {
        if (!patch.is_object()) return false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it == executors_.end()) return false;
            for (auto& kv : patch.members()) it->second.connect.set(kv.first, kv.second);
            it->second.config = build_worker_config(it->second);
            save_executors_locked();
        }
        restart_worker(id);
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

    // 无头核对 entry + pipeline_override 的构造：只走映射，不连设备、不 post。
    // MaaEnd 的展示名/option 建模全靠这层，本地没游戏也能把 50 个任务全过一遍。
    mj::Value probe_override(const std::string& id, const std::string& task,
                             const mj::Value& params) {
        executor_catalog(id, 0.0);      // worker 里的 routes_ 要先建好
        auto w = worker_of(id);
        if (!w) return mj::Value::object();
        mj::Value req = mj::Value::object();
        req.set("task", task);
        req.set("params", params);
        mj::Value r;
        if (!w->call("override", req, 15000, r)) return mj::Value::object();
        return r;
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
            // 面板「换班设施」8 个勾选 -> 核心 facility 数组（数组序即换班优先级）
            // 名字必须是核心 infrast.json roomType 里的键，Workshop/Mine 之类会被拒
            static const char* kFac[] = { "Control", "Mfg", "Trade", "Power", "Reception",
                                          "Office", "Training", "Processing" };
            bool touched = false;
            mj::Value fac = mj::Value::array();
            for (int k = 0; k < 8; ++k) {
                std::string key = "facility_" + std::to_string(k);
                if (!in.find(key)) continue;
                touched = true;
                if (boolean(key, false)) fac.push(kFac[k]);
            }
            if (touched) out.set("facility", std::move(fac));
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

    // MaaCore 会校验任务参数：缺 required 字段时 AsstAppendTask 直接返回 0
    // （实测 StartUp 只给 {"account_name":...} 会被拒）。面板打开时已用 catalog
    // 的 default 预填，但 seq / 计划 / 从没配置过的任务拿到的是空参数，这里兜底补齐。
    static mj::Value fill_defaults(const mj::Value& catalog, const std::string& task,
                                   const mj::Value& in) {
        mj::Value out = in.is_object() ? in : mj::Value::object();
        if (!catalog.is_array()) return out;
        for (auto& t : catalog.items()) {
            if (t.get_str("name", "") != task) continue;
            auto* ps = t.find("params");
            if (!ps || !ps->is_array()) return out;
            for (auto& p : ps->items()) {
                std::string key = p.get_str("name", "");
                if (key.empty() || out.has(key) || !p.has("default")) continue;
                out.set(key, p["default"]);
            }
            return out;
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
        mj::Value filled = e.kind == "maa" ? fill_defaults(executor_catalog(id), task, params)
                                           : params;
        mj::Value normalized = normalize_task_params(e.kind, task, filled);
        mj::Value final_params = (is_aware || has_acc)
            ? AccountStore::apply_to_task(e.kind, task, normalized, has_acc ? &acc : nullptr)
            : normalized;
        {
            std::lock_guard<std::mutex> lk(tp_mtx_);
            running_task_[id] = task;
        }
        mj::Value req = mj::Value::object();
        req.set("task", task);
        req.set("params", final_params);
        mj::Value r;
        bool ok = w->call("run", req, 60000, r);
        if (!ok) {
            std::lock_guard<std::mutex> lk(tp_mtx_);
            running_task_.erase(id);
            return 0;
        }
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
            j.set("catalog", executor_catalog(id, 0));
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

    // 顺序跑一串任务（每个一次 run，等空闲再下一个），同步执行。
    // 设备联调 / 无头流程用；params 取各任务已保存的面板参数。
    std::string run_task_sequence(const std::string& exec_id,
                                  const std::vector<std::string>& tasks) {
        ExecutorEntry e;
        if (!get_executor(exec_id, e)) return "no-executor";
        Plan plan;
        plan.id = "cli-" + random_hex(6);
        plan.ts = util::now_sec();
        plan.planner = "cli";
        plan.goal = "cli sequence";
        for (auto& t : tasks) {
            PlanStep s;
            s.exec_id = exec_id;
            s.kind = e.kind;
            s.exec_label = e.label;
            s.task = t;
            s.params = task_params(exec_id, t);
            plan.steps.push_back(std::move(s));
        }
        if (plan.steps.empty()) return "empty";
        {
            std::lock_guard<std::mutex> lk(plans_mtx_);
            plans_.push_back(plan);
            if (plans_.size() > 50) plans_.erase(plans_.begin());
        }
        stop_flag_ = false;
        int ran = 0;
        exec_worker(plan, &ran);   // 同步：CLI 直接等整串跑完
        std::string status = "done";
        {
            std::lock_guard<std::mutex> lk(plans_mtx_);
            for (auto& p : plans_) if (p.id == plan.id) { status = p.status; break; }
        }
        return status;
    }

    // 合并导入的调度条目（GUI 导入按钮与 CLI import 共用）。
    // 按 exec_id+\x1f+source 去重：同键覆盖（保留已记录的 last_run_date），新键追加。
    void merge_schedules(const mj::Value& incoming_entries) {
        if (!incoming_entries.is_array() || incoming_entries.items().empty()) return;
        std::lock_guard<std::mutex> lk(sched_mtx_);
        // entries 缺失时 operator[] 会创建 Object；对 Object push 只进 arr_ 不改 type_，
        // dump 时静默丢数据——先保证它是 Array
        if (!schedules_.has("entries") || !schedules_["entries"].is_array())
            schedules_.set("entries", mj::Value::array());
        for (auto& in : incoming_entries.items()) {
            std::string key = in.get_str("exec_id", "") + "\x1f" + in.get_str("source", "");
            bool replaced = false;
            mj::Value& cur = schedules_["entries"];
            if (cur.is_array()) {
                for (auto& ex : cur.items()) {
                    if (ex.get_str("exec_id", "") + "\x1f" + ex.get_str("source", "") != key)
                        continue;
                    std::string lrd = ex.get_str("last_run_date", "");
                    ex = in;
                    if (!lrd.empty()) ex.set("last_run_date", lrd);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) schedules_["entries"].push(in);
        }
        schedules_.set("version", 1);
        save_schedules_locked();
    }

    // schedules.json 只读快照（CLI sched-list / GUI 展示用）
    mj::Value schedules_snapshot() const {
        std::lock_guard<std::mutex> lk(sched_mtx_);
        return schedules_;
    }

    // 调度器一次扫描。返回触发的执行器数。
    // 只从调度器线程或 CLI sched-tick 调用：会重置 stop_flag_ 并同步跑完任务串。
    int scheduler_tick() {
        if (stopping_) return 0;
        mj::Value sched;
        {
            std::lock_guard<std::mutex> lk(sched_mtx_);
            sched = schedules_;
        }
        auto* entries = sched.find("entries");
        if (!entries || !entries->is_array()) return 0;

        // 本地时间（wDayOfWeek 0=周日，直接对应 weekdays 约定；0=周日..6=周六）
        SYSTEMTIME st;
        GetLocalTime(&st);
        char hh[3] = { 0, 0, 0 }, mm[3] = { 0, 0, 0 };
        hh[0] = char('0' + st.wHour / 10);   hh[1] = char('0' + st.wHour % 10);
        mm[0] = char('0' + st.wMinute / 10); mm[1] = char('0' + st.wMinute % 10);
        std::string hhmm = std::string(hh) + ":" + mm;
        std::string today = util::today_str();

        // 任务序列：GUI 一键长草选择是唯一权威（state\task-lists.json）
        mj::Value lists = mj::Value::object();
        {
            std::string content;
            if (util::read_file(util::exe_dir() + "\\state\\task-lists.json", content)
                && mj::try_parse(content, lists) && !lists.is_object()) {
                lists = mj::Value::object();
            }
        }

        struct Fire { std::string exec_id; std::vector<std::string> tasks; };
        struct Due { size_t idx; std::string key; };
        std::vector<Fire> fires;
        std::vector<Due> due;

        size_t idx = 0;
        for (auto& entry : entries->items()) {
            const size_t i = idx++;
            if (!entry.get_bool("enabled", false)) continue;
            const std::string exec_id = entry.get_str("exec_id", "");
            if (exec_id.empty() || !running_task_of(exec_id).empty()) continue; // 忙：跳过且不标记
            // 到点判定
            if (entry.get_str("kind", "") == "maaend") {
                auto* wd = entry.find("weekdays");
                if (!wd || !wd->is_array()) continue;
                bool day_ok = false;
                for (auto& d : wd->items())
                    if (d.as_int(-1) == (int64_t)st.wDayOfWeek) { day_ok = true; break; }
                if (!day_ok) continue;
                auto* ts = entry.find("times");
                bool time_hit = false;
                if (ts && ts->is_array())
                    for (auto& t : ts->items())
                        if (t.as_string() == hhmm) { time_hit = true; break; }
                if (!time_hit) continue;
            } else {   // maa：hour/min 单点
                if (entry.get_int("hour", -1) != (int64_t)st.wHour
                    || entry.get_int("min", -1) != (int64_t)st.wMinute) continue;
            }
            if (entry.get_str("last_run_date", "") == today) continue;   // 当天已触发
            auto* lst = lists.find(exec_id);
            if (!lst || !lst->is_array() || lst->items().empty()) continue;
            std::vector<std::string> tasks;
            for (auto& t : lst->items()) {
                std::string name = t.as_string();
                if (!name.empty()) tasks.push_back(name);
            }
            if (tasks.empty()) continue;
            // 执行器存在且启用；worker 懒生成（executors.json 加载后 worker 为空）
            ExecutorEntry e;
            if (!get_executor(exec_id, e) || !e.enabled) continue;
            if (!worker_of(exec_id)) spawn_worker(exec_id);
            due.push_back({ i, exec_id + "\x1f" + entry.get_str("source", "") });
            fires.push_back({ exec_id, std::move(tasks) });
        }
        if (fires.empty()) return 0;

        // 先标记当日已触发再 fire：长任务期间 10 秒 tick 不会重复入队
        {
            std::lock_guard<std::mutex> lk(sched_mtx_);
            mj::Value& live = schedules_["entries"];
            if (live.is_array()) {
                auto& arr = live.items();
                for (auto& d : due) {
                    if (d.idx >= arr.size()) continue;
                    mj::Value& e2 = arr[d.idx];
                    // 内容一致性检查：导入并发替换后索引可能错位
                    if (e2.get_str("exec_id", "") + "\x1f" + e2.get_str("source", "") == d.key)
                        e2.set("last_run_date", today);
                }
            }
        }
        schedule_save_schedules();

        int fired = 0;
        for (auto& f : fires) {
            if (stopping_) break;
            std::string status = run_task_sequence(f.exec_id, f.tasks);
            util::trace("[sched] " + f.exec_id + " -> " + status);
            ++fired;
        }
        return fired;
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

    bool update_plan_status(const std::string& id, const std::string& status,
                            const std::string& error = std::string()) {
        {
            std::lock_guard<std::mutex> lk(plans_mtx_);
            for (auto& p : plans_) {
                if (p.id == id) { p.status = status; p.error = error; break; }
            }
        }
        schedule_save();
        return true;
    }

    // ---------------------------------------------------------------- 任务运行状态
    // 左列任务状态点用：当前运行中的任务名 + 最近一次结果
    std::string running_task_of(const std::string& exec_id) const {
        std::lock_guard<std::mutex> lk(tp_mtx_);
        auto it = running_task_.find(exec_id);
        return it == running_task_.end() ? std::string() : it->second;
    }

    void set_running_task(const std::string& exec_id, const std::string& task) {
        std::lock_guard<std::mutex> lk(tp_mtx_);
        if (task.empty()) running_task_.erase(exec_id);
        else running_task_[exec_id] = task;
    }

    std::string last_task_result(const std::string& exec_id, const std::string& task) const {
        std::lock_guard<std::mutex> lk(tp_mtx_);
        auto e = task_result_.find(exec_id);
        if (e == task_result_.end()) return "";
        auto t = e->second.find(task);
        return t == e->second.end() ? "" : t->second;
    }

    void set_task_result(const std::string& exec_id, const std::string& task,
                         const std::string& result) {
        std::lock_guard<std::mutex> lk(tp_mtx_);
        if (result.empty()) {
            auto e = task_result_.find(exec_id);
            if (e != task_result_.end()) e->second.erase(task);
        } else {
            task_result_[exec_id][task] = result;
        }
    }

    // ---------------------------------------------------------------- 任务参数持久化
    // 每个执行器每个任务的面板设置，持久化在 state\task-params.json
    mj::Value task_params(const std::string& exec_id, const std::string& task) const {
        std::lock_guard<std::mutex> lk(tp_mtx_);
        auto e = task_params_.find(exec_id);
        if (e == task_params_.end()) return mj::Value::object();
        auto t = e->second.find(task);
        if (t == e->second.end()) return mj::Value::object();
        mj::Value out = mj::Value::object();
        for (auto& kv : t->second) out.set(kv.first, kv.second);
        return out;
    }

    void set_task_param(const std::string& exec_id, const std::string& task,
                        const std::string& key, const mj::Value& value) {
        {
            std::lock_guard<std::mutex> lk(tp_mtx_);
            task_params_[exec_id][task][key] = value;
        }
        schedule_save_task_params();
    }

    void erase_task_param(const std::string& exec_id, const std::string& task,
                          const std::string& key) {
        {
            std::lock_guard<std::mutex> lk(tp_mtx_);
            auto e = task_params_.find(exec_id);
            if (e == task_params_.end()) return;
            auto t = e->second.find(task);
            if (t == e->second.end()) return;
            t->second.erase(key);
        }
        schedule_save_task_params();
    }

    void remove_task_params(const std::string& exec_id) {
        {
            std::lock_guard<std::mutex> lk(tp_mtx_);
            task_params_.erase(exec_id);
        }
        schedule_save_task_params();
    }

    AccountStore& accounts() { return *accounts_; }
    CoreManager& cores() { return *cores_; }
    SettingsStore& settings() { return *settings_; }
    AppSettings app_settings() const { return settings_->get(); }
    // 保存设置后让 LLM 配置立即生效（llm_cfg_ 原先只在构造时读一次）
    void reload_llm_config() { load_llm_config(); }
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

    // 绕过 800ms 节流立即落盘：批量导入后进程可能很快退出，
    // 节流窗口内的尾部写入不能丢
    void flush_task_params() {
        last_tp_save_ = GetTickCount64();
        save_task_params();
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
        bool loaded = w->call("load", mj::Value::object(), 180000, r);
        std::string ld = mj::Value::dump(r);
        if (ld.size() > 300) ld = ld.substr(0, 300);
        util::trace("spawn_worker id=" + id + " load_ok=" + (loaded ? "1" : "0") + " reply=" + ld);
    }

    void exec_worker(Plan plan, int* ran = nullptr) {
        int completed = 0;
        std::string status = "done";
        std::string fail_reason;
        for (auto& step : plan.steps) {
            if (stop_flag_ || stopping_) {
                status = "stopped";
                break;
            }
            auto w = worker_of(step.exec_id);
            if (!w) {
                status = "failed";
                break;
            }
            mj::Value st;
            if (w->call("status", mj::Value::object(), 15000, st)) {
                if (st.get_str("conn", "") != "connected") {
                    // 连不上就停在这里：后续任务同样依赖这个连接，跳过只会整串空跑
                    if (!connect_executor(step.exec_id)) {
                        status = "connect-failed";
                        break;
                    }
                }
            }
            int64_t tid = run_on_executor(step.exec_id, step.task, step.params);
            if (tid <= 0) {
                status = "failed";
                break;
            }
            ++completed;
            ExecutorEntry e;
            if (get_executor(step.exec_id, e) && !e.account_id.empty()
                && (step.task == "Award" || step.task == "DailyRewards")) {
                accounts_->mark_daily_done(e.account_id);
            }
            // 轮询任务完成；stopping_ 时立即退出，避免 Hub 析构后仍访问成员
            int idle_rounds = 0;
            const double t0 = util::now_sec();
            std::string run_state, run_error;
            bool lost = false;
            while (!stop_flag_ && !stopping_) {
                Sleep(500);
                mj::Value s2;
                if (!w->call("status", mj::Value::object(), 15000, s2)) {
                    // worker 不再应答基本就是进程没了（核心崩在 DLL 里就是这样）；
                    // 继续等只会把"这一步根本没跑成"读成最后一个 run_state 报成 done
                    if (++idle_rounds > 3) {
                        lost = true;
                        status = "failed";
                        fail_reason = step.task + ": executor 无响应（worker 可能已崩溃）";
                        break;
                    }
                    continue;
                }
                run_state = s2.get_str("run", "idle");
                run_error = s2.get_str("last_error", "");
                if (run_state == "running" || run_state == "stopping") {
                    idle_rounds = 0;
                    continue;
                }
                // append 之后 MaaCore 要过一会儿才进入 running：首轮就判完成会让
                // 下一步在"上一个仍在跑"时被 AsstAppendTask 拒绝，整串 failed
                if (util::now_sec() - t0 >= 2.0 && ++idle_rounds >= 2) break;
            }
            // 适配器把失败写在 run 里；不读它就会把"这一步根本没跑成"报成 done
            if (!lost && run_state == "failed") {
                status = "failed";
                fail_reason = step.task + ": " + run_error;
                break;
            }
        }
        if (ran) *ran = completed;
        if (stop_flag_ || stopping_) status = "stopped";
        update_plan_status(plan.id, status, fail_reason);
        // run_on_executor 正常完成后不清 running_task_（只清 call 失败路径）。
        // 这里按 steps 涉及的执行器统一收尾，调度器 tick 才能靠 running_task_of
        // 判断"忙"，否则上一串跑完后 executor 会被永久视作忙。
        for (size_t i = 0; i < plan.steps.size(); ++i) {
            bool last = true;
            for (size_t j = i + 1; j < plan.steps.size(); ++j)
                if (plan.steps[j].exec_id == plan.steps[i].exec_id) { last = false; break; }
            if (last) set_running_task(plan.steps[i].exec_id, "");
        }
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

    // 各 kind 的默认连接方式：
    //   MAA  —— 明日方舟常跑在模拟器里，优先探测 adb 并用 MaaTouch（仿真触控）；
    //           探测不到再退回 win32 挂窗。
    //   MaaEnd —— 官方 Win32-Front：ScreenDC 截图 + Seize 前台输入（取自官方运行日志）
    static mj::Value default_connect(const std::string& kind) {
        mj::Value conn = mj::Value::object();
        if (kind == "maa") {
            // adb devices 是秒级开销，只在首次调用时探测一次
            static const util::AdbDevice dev = [] {
                util::AdbDevice d;
                util::detect_adb(d);
                return d;
            }();
            if (!dev.address.empty()) {
                conn.set("mode", "adb");
                conn.set("adb_path", dev.adb_path);
                conn.set("address", dev.address);
                conn.set("config", dev.config);
                conn.set("touch_mode", "maatouch");
                return conn;
            }
            conn.set("mode", "win32");
            conn.set("screencap_method", 32);
            conn.set("mouse_method", 1);
            conn.set("keyboard_method", 1);
            return conn;
        }
        conn.set("mode", "win32");
        conn.set("screencap_method", 32);   // ScreenDC
        conn.set("mouse_method", 1);        // Seize
        conn.set("keyboard_method", 1);     // Seize
        return conn;
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
        mj::Value conn = default_connect(e.kind);
        // 执行器上的覆盖项（UI/CLI 写入）逐键合并
        if (e.connect.is_object()) {
            for (auto& kv : e.connect.members()) conn.set(kv.first, kv.second);
        }
        std::string mode = conn.get_str("mode", "win32");
        if (mode == "adb") {
            mj::Value adb = mj::Value::object();
            adb.set("adb_path", conn.get_str("adb_path", ""));
            adb.set("address", conn.get_str("address", ""));
            adb.set("config", conn.get_str("config", "General"));
            wcfg.set("adb", std::move(adb));
        }
        wcfg.set("connect", std::move(conn));
        return wcfg;
    }

    void restart_worker(const std::string& id) {
        std::shared_ptr<WorkerClient> old;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = executors_.find(id);
            if (it == executors_.end()) return;
            old = it->second.worker;
            it->second.worker.reset();
        }
        if (old) old->shutdown();
        spawn_worker(id);
    }

    // ================================================================ 调度
    // schedules.json 规范见 config_import.hpp 头部注释：
    // {"version":1,"entries":[{exec_id,kind,label,enabled,weekdays,times,hour,min,config,source,last_run_date}]}
    void load_schedules() {
        std::string content;
        if (!util::read_file(util::exe_dir() + "\\state\\schedules.json", content)) return;
        mj::Value v;
        if (!mj::try_parse(content, v)) return;
        std::lock_guard<std::mutex> lk(sched_mtx_);
        schedules_ = std::move(v);
    }

    // 调用方已持有 sched_mtx_ 时用这个（save_schedules 内部还要加锁，
    // 持锁调用 = 同线程递归加锁 UB，同 save_executors_locked 的教训）。
    // 磁盘 IO 在锁内做：schedules_ 体量小（几十条），换不来多少并发收益。
    void save_schedules_locked() {
        util::write_file(util::exe_dir() + "\\state\\schedules.json",
                         mj::Value::dump_pretty(schedules_));
    }

    void save_schedules() {
        std::lock_guard<std::mutex> lk(sched_mtx_);
        save_schedules_locked();
    }

    // 调度器线程保存节流（1s），避免每次 tick 都写盘
    void schedule_save_schedules() {
        uint64_t now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lk(sched_save_mtx_);
            if (now - last_sched_save_ < 1000) return;
            last_sched_save_ = now;
        }
        save_schedules();
    }

    void scheduler_loop() {
        util::trace("[sched] scheduler_loop start");
        while (!stopping_) {
            for (int i = 0; i < 10 && !stopping_; ++i) Sleep(1000);   // 10s tick
            if (stopping_) break;
            try {
                scheduler_tick();
            } catch (const std::exception& e) {
                util::trace(std::string("[sched] tick exception: ") + e.what());
            } catch (...) {
                util::trace("[sched] tick unknown exception");
            }
        }
        util::trace("[sched] scheduler_loop exit");
        sched_done_ = true;
    }

    void load_task_params() {
        std::string content;
        if (!util::read_file(util::exe_dir() + "\\state\\task-params.json", content)) return;
        mj::Value v;
        if (!mj::try_parse(content, v)) return;
        std::lock_guard<std::mutex> lk(tp_mtx_);
        for (auto& e : v.members()) {
            if (!e.second.is_object()) continue;
            for (auto& t : e.second.members()) {
                if (!t.second.is_object()) continue;
                for (auto& kv : t.second.members()) {
                    task_params_[e.first][t.first][kv.first] = kv.second;
                }
            }
        }
    }

    void schedule_save_task_params() {
        uint64_t now = GetTickCount64();
        if (now - last_tp_save_ < 800) return;
        last_tp_save_ = now;
        save_task_params();
    }

    void save_task_params() {
        mj::Value v = mj::Value::object();
        {
            std::lock_guard<std::mutex> lk(tp_mtx_);
            for (auto& e : task_params_) {
                mj::Value tasks = mj::Value::object();
                for (auto& t : e.second) {
                    mj::Value kv_obj = mj::Value::object();
                    for (auto& kv : t.second) kv_obj.set(kv.first, kv.second);
                    tasks.set(t.first, std::move(kv_obj));
                }
                v.set(e.first, std::move(tasks));
            }
        }
        util::write_file(util::exe_dir() + "\\state\\task-params.json",
                         mj::Value::dump_pretty(v));
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

    // 调用方已持有 mtx_ 时用这个：save_executors 内部还会再加一次锁，
    // 持锁再调用 = 同线程递归加锁（未定义行为，实测直接崩进程）
    void save_executors_locked() {
        mj::Value v = mj::Value::object();
        mj::Value arr = mj::Value::array();
        for (auto& kv : executors_) arr.push(kv.second.to_json());
        v.set("executors", std::move(arr));
        util::write_file(util::exe_dir() + "\\state\\executors.json",
                         mj::Value::dump_pretty(v));
    }

    void save_executors() {
        std::lock_guard<std::mutex> lk(mtx_);
        save_executors_locked();
    }

    // LLM 配置以设置存储为唯一事实来源（UI 保存后立即生效）。
    // config.json 的 llm 段只保留旧版兼容：settings 未填 api_key 时回退；
    // temperature / max_tokens / timeout_sec 不在 AppSettings 里，继续读 config.json。
    void load_llm_config() {
        AppSettings st = settings_->get();
        llm_cfg_.enabled = st.llm_enabled;
        if (!st.llm_base_url.empty()) llm_cfg_.base_url = st.llm_base_url;
        llm_cfg_.api_key = st.llm_api_key;
        if (!st.llm_model.empty()) llm_cfg_.model = st.llm_model;
        if (auto* llm = config_.find("llm")) {
            if (llm_cfg_.api_key.empty()) llm_cfg_.api_key = llm->get_str("api_key", "");
            if (auto* t = llm->find("temperature"); t && t->is_number())
                llm_cfg_.temperature = t->as_double();   // 原先 get_int 把 0.2 截成 0
            llm_cfg_.max_tokens = (int)llm->get_int("max_tokens", (int64_t)llm_cfg_.max_tokens);
            llm_cfg_.timeout_sec = (int)llm->get_int("timeout_sec", (int64_t)llm_cfg_.timeout_sec);
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

    std::map<std::string, std::string> running_task_;   // exec_id -> 当前运行任务
    std::map<std::string, std::map<std::string, std::string>> task_result_;
    mutable std::mutex tp_mtx_;
    std::map<std::string, std::map<std::string, std::map<std::string, mj::Value>>>
        task_params_;   // exec_id -> task -> key -> value
    uint64_t last_tp_save_ = 0;

    std::mutex plans_mtx_;
    std::vector<Plan> plans_;
    std::atomic<bool> stop_flag_{ false };
    std::atomic<bool> stopping_{ false };
    std::thread exec_thread_;
    std::thread save_thread_;
    uint64_t last_save_ = 0;

    // 调度器（构造起线程，析构 stopping_ + 有界等待回收）
    std::thread sched_thread_;
    std::atomic<bool> sched_done_{ false };
    mj::Value schedules_ = mj::Value::object();   // schedules.json 镜像
    mutable std::mutex sched_mtx_;
    uint64_t last_sched_save_ = 0;
    std::mutex sched_save_mtx_;
};

} // namespace hub
