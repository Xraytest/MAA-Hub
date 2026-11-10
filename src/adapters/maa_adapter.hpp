// MAA (MaaAssistantArknights) 适配器 —— 直接调用 MaaCore.dll 的 AsstApi
// 签名依据：MAA 官方 include/AsstCaller.h 与本机 Python 封装 asst.py（v6.13.0）
#include "../adapters/adapter.hpp"
#include "../util.hpp"

#include <windows.h>
#include <memory>

namespace hub {

// AsstMsg 枚举（官方回调消息协议）
enum AsstMsg {
    InternalError = 0,
    InitFailed = 1,
    ConnectionInfo = 2,
    AllTasksCompleted = 3,
    AsyncCallInfo = 4,
    Destroyed = 5,
    TaskChainError = 10000,
    TaskChainStart = 10001,
    TaskChainCompleted = 10002,
    TaskChainExtraInfo = 10003,
    TaskChainStopped = 10004,
    SubTaskError = 20000,
    SubTaskStart = 20001,
    SubTaskCompleted = 20002,
    SubTaskExtraInfo = 20003,
    SubTaskStopped = 20004,
    ReportRequest = 30000,
};

// AsstWin32ScreencapMethod / AsstWin32InputMethod
constexpr uint64_t SCREENCAP_GDI = 1;
constexpr uint64_t SCREENCAP_FRAMEPOOL = 2;
constexpr uint64_t SCREENCAP_SCREENDC = 32;
constexpr uint64_t INPUT_SEIZE = 1;

// AsstInstanceOpt::TouchMode（实测 MaaCore v6.13.0：仅接受小写 maatouch / minitouch）
constexpr int INST_TOUCH_MODE = 2;

class MaaAdapter : public IAdapter {
public:
    MaaAdapter() { kind_ = "maa"; }

    // ------------------------------------------------------------ 发现
    bool discover(const mj::Value& cfg) override {
        cfg_ = cfg;
        st_.kind = "maa";
        st_.name = cfg.get_str("display_name", "MAA");
        st_.install_dir = cfg.get_str("install_dir", "");
        st_.core_dll = st_.install_dir + "\\MaaCore.dll";
        if (!util::file_exists(st_.core_dll)) {
            st_.available = false;
            st_.error = "MaaCore.dll not found in " + st_.install_dir;
            return false;
        }
        st_.available = true;
        st_.error.clear();
        st_.task_count = (int64_t)catalog_.size();
        return true;
    }

    // ------------------------------------------------------------ 加载
    bool load() override {
        if (!st_.available) return false;
        std::wstring dll = util::utf8_to_wide(st_.core_dll);
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) h = LoadLibraryW(dll.c_str());
        if (!h) {
            st_.error = "LoadLibrary failed: " + st_.core_dll + " err=" + std::to_string(GetLastError());
            return false;
        }
        lib_ = h;
        if (!resolve_symbols()) {
            st_.error = "missing AsstApi exports";
            return false;
        }
        // 依赖解析保障：安装目录加入 DLL 搜索路径
        cookie_ = AddDllDirectory(util::utf8_to_wide(st_.install_dir).c_str());
        {
            std::wstring path;
            wchar_t buf[32768];
            DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32768);
            if (n) path.assign(buf, n);
            std::wstring add = util::utf8_to_wide(st_.install_dir);
            if (path.find(add) == std::wstring::npos) {
                SetEnvironmentVariableW(L"PATH", (add + L";" + path).c_str());
            }
        }
        std::string user_dir = st_.install_dir + "\\cache\\maahub";
        util::ensure_dir(user_dir);
        fn_.AsstSetUserDir(user_dir.c_str());
        if (!fn_.AsstLoadResource(st_.install_dir.c_str())) {
            st_.error = "AsstLoadResource failed";
            return false;
        }
        const char* ver = fn_.AsstGetVersion();
        st_.version = ver ? ver : "";
        cb_ = make_callback();
        handle_ = fn_.AsstCreateEx((void*)cb_, (void*)this);
        if (!handle_) {
            st_.error = "AsstCreateEx returned null";
            return false;
        }
        return true;
    }

    void unload() override {
        if (handle_) { fn_.AsstDestroy(handle_); handle_ = nullptr; }
        if (cookie_) { RemoveDllDirectory(cookie_); cookie_ = nullptr; }
        if (lib_) { FreeLibrary(lib_); lib_ = nullptr; }
    }

    // ------------------------------------------------------------ 连接
    bool connect() override {
        if (!handle_) { st_.last_error = "not loaded"; return false; }
        st_.conn = "connecting";
        std::string mode = cfg_.get_path_str("connect.mode", "win32");
        if (mode == "adb") {
            std::string adb = cfg_.get_path_str("adb.adb_path", "");
            std::string addr = cfg_.get_path_str("adb.address", "");
            std::string conf = cfg_.get_path_str("adb.config", "General");
            std::string touch = cfg_.get_path_str("connect.touch_mode", "");
            if (adb.empty() || addr.empty()) {
                st_.conn = "failed";
                st_.last_error = "ADB 未配置（adb_path / address 为空）";
                return false;
            }
            // 触控方式必须在连接前设置；仿真触控 = maatouch
            std::string touch_note;
            if (!touch.empty()) {
                bool set_ok = fn_.AsstSetInstanceOption(handle_, INST_TOUCH_MODE, touch.c_str());
                touch_note = " touch_mode=" + touch + (set_ok ? "" : "(核心不接受)");
            }
            int r = fn_.AsstAsyncConnect(handle_, adb.c_str(), addr.c_str(), conf.c_str(), true);
            if (r <= 0) {
                st_.conn = "failed";
                st_.last_error = "AsstAsyncConnect 返回 " + std::to_string(r)
                                 + " (adb=" + adb + ", addr=" + addr + ", config=" + conf + ")"
                                 + touch_note;
                return false;
            }
            bool connected = fn_.AsstConnected(handle_) != 0;
            for (int i = 0; i < 80 && !connected; ++i) {   // 20s：装 mini-program + 首帧截图
                Sleep(250);
                connected = fn_.AsstConnected(handle_) != 0;
            }
            st_.ctrl_mode = "adb";
            if (!connected) {
                st_.conn = "failed";
                std::string why;
                {
                    std::lock_guard<std::mutex> lk(ev_mtx_);
                    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
                        if (it->category == "connection" || it->category == "error") {
                            why = it->what + " " + it->raw;
                            break;
                        }
                    }
                }
                st_.last_error = "连接失败：设备 " + addr + " 未就绪。" + why + touch_note;
                return false;
            }
            {   // 设备信息（uuid / 分辨率）来自 ConnectionInfo 事件
                std::lock_guard<std::mutex> lk(ev_mtx_);
                for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
                    std::string u = it->details.get_str("uuid", "");
                    if (u.empty()) continue;
                    st_.uuid = u;
                    break;
                }
            }
            st_.conn = "connected";
            st_.last_error.clear();
            return true;
        }
        // Win32 挂窗
        std::string cls = cfg_.get_str("game_window_class", "UnityWndClass");
        std::string title = cfg_.get_str("game_window_title_regex", "Arknights");
        util::WindowInfo wi;
        if (!util::find_window(cls, title, wi)) {
            st_.conn = "failed";
            st_.last_error = "game window not found (class=" + cls + ", title~/" + title + "/). 请先启动游戏客户端。";
            return false;
        }
        uint64_t sc = (uint64_t)cfg_.get_path_int("connect.screencap_method", (int64_t)SCREENCAP_SCREENDC);
        uint64_t mm = (uint64_t)cfg_.get_path_int("connect.mouse_method", (int64_t)INPUT_SEIZE);
        uint64_t km = (uint64_t)cfg_.get_path_int("connect.keyboard_method", (int64_t)INPUT_SEIZE);
        int call_id = fn_.AsstAsyncAttachWindow(handle_, (void*)wi.hwnd, sc, mm, km, true);
        if (call_id <= 0) {
            st_.conn = "failed";
            st_.last_error = "AsstAsyncAttachWindow returned " + std::to_string(call_id);
            return false;
        }
        // 等待 AsyncCallInfo 回执
        bool ok = false;
        for (int i = 0; i < 75; ++i) {   // 15s
            {
                std::lock_guard<std::mutex> lk(ev_mtx_);
                for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
                    if (it->what == "AttachWindow" && it->task_id == call_id) {
                        auto* d = it->details.find("details");
                        ok = d && d->get_bool("ret", false);
                        break;
                    }
                }
            }
            if (ok) break;
            Sleep(200);
        }
        st_.ctrl_mode = "win32";
        st_.ctrl_hwnd = (int64_t)(intptr_t)wi.hwnd;
        if (ok) {
            st_.conn = "connected";
            st_.last_error.clear();
        } else {
            st_.conn = "failed";
            st_.last_error = "attach rejected by MaaCore";
        }
        return ok;
    }

    void disconnect() override { st_.conn = "disconnected"; }

    bool is_connected() override {
        if (!handle_) return false;
        return fn_.AsstConnected(handle_) != 0;
    }

    // ------------------------------------------------------------ 任务
    bool run_task(const std::string& task, const mj::Value& params, int64_t& out) override {
        out = 0;
        if (!handle_) { st_.last_error = "not loaded"; return false; }
        if (st_.conn != "connected" && !is_connected()) { st_.last_error = "not connected"; return false; }
        std::string p = mj::Value::dump(params);
        int tid = fn_.AsstAppendTask(handle_, task.c_str(), p.c_str());
        if (tid <= 0) {
            st_.last_error = "AsstAppendTask(" + task + ") failed";
            return false;
        }
        if (!fn_.AsstStart(handle_)) {
            st_.last_error = "AsstStart failed";
            return false;
        }
        st_.run = "running";
        chain_err_ = false;
        out = tid;
        return true;
    }

    bool stop() override {
        if (!handle_) return false;
        bool ok = fn_.AsstStop(handle_) != 0;
        if (ok) { st_.run = "idle"; chain_err_ = false; }
        return ok;
    }

    bool is_running() override {
        if (!handle_) return false;
        return fn_.AsstRunning(handle_) != 0;
    }

    AppStatus status() const override {
        std::lock_guard<std::mutex> lk(st_mtx_);
        return st_;
    }

    std::vector<mj::Value> task_catalog() const override { return catalog_; }

    std::vector<AdapterEvent> drain_events() override {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        std::vector<AdapterEvent> out;
        out.swap(events_);
        return out;
    }

private:
    // ------------------------------------------------------------ 符号
    struct Fn {
        void* (*AsstCreate)() = nullptr;
        void* (*AsstCreateEx)(void*, void*) = nullptr;
        void (*AsstDestroy)(void*) = nullptr;
        bool (*AsstSetUserDir)(const char*) = nullptr;
        bool (*AsstLoadResource)(const char*) = nullptr;
        bool (*AsstSetInstanceOption)(void*, int, const char*) = nullptr;
        bool (*AsstSetStaticOption)(int, const char*) = nullptr;
        int (*AsstAppendTask)(void*, const char*, const char*) = nullptr;
        bool (*AsstSetTaskParams)(void*, int, const char*) = nullptr;
        bool (*AsstStart)(void*) = nullptr;
        bool (*AsstStop)(void*) = nullptr;
        bool (*AsstRunning)(void*) = nullptr;
        bool (*AsstConnected)(void*) = nullptr;
        const char* (*AsstGetVersion)() = nullptr;
        void (*AsstLog)(const char*, const char*) = nullptr;
        int (*AsstAsyncConnect)(void*, const char*, const char*, const char*, bool) = nullptr;
        int (*AsstAsyncAttachWindow)(void*, void*, uint64_t, uint64_t, uint64_t, bool) = nullptr;
        uint64_t (*AsstGetImage)(void*, void*, uint64_t) = nullptr;
        uint64_t (*AsstGetNullSize)() = nullptr;
    };

    template <typename T>
    bool sym(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(lib_, name));
        return target != nullptr;
    }

    bool resolve_symbols() {
        bool ok = true;
        ok &= sym(fn_.AsstCreate, "AsstCreate");
        ok &= sym(fn_.AsstCreateEx, "AsstCreateEx");
        ok &= sym(fn_.AsstDestroy, "AsstDestroy");
        ok &= sym(fn_.AsstSetUserDir, "AsstSetUserDir");
        ok &= sym(fn_.AsstLoadResource, "AsstLoadResource");
        ok &= sym(fn_.AsstSetInstanceOption, "AsstSetInstanceOption");
        ok &= sym(fn_.AsstSetStaticOption, "AsstSetStaticOption");
        ok &= sym(fn_.AsstAppendTask, "AsstAppendTask");
        ok &= sym(fn_.AsstSetTaskParams, "AsstSetTaskParams");
        ok &= sym(fn_.AsstStart, "AsstStart");
        ok &= sym(fn_.AsstStop, "AsstStop");
        ok &= sym(fn_.AsstRunning, "AsstRunning");
        ok &= sym(fn_.AsstConnected, "AsstConnected");
        ok &= sym(fn_.AsstGetVersion, "AsstGetVersion");
        ok &= sym(fn_.AsstLog, "AsstLog");
        ok &= sym(fn_.AsstAsyncConnect, "AsstAsyncConnect");
        ok &= sym(fn_.AsstAsyncAttachWindow, "AsstAsyncAttachWindow");
        ok &= sym(fn_.AsstGetImage, "AsstGetImage");
        ok &= sym(fn_.AsstGetNullSize, "AsstGetNullSize");
        return ok;
    }

    // ------------------------------------------------------------ 回调
    // AsstApiCallback: void(AsstMsgId msg, const char* details_json, void* custom_arg)
    // x64 Windows 单一调用约定，直接用普通函数指针即可。
    using CallbackT = void (*)(int, const char*, void*);
    static void trampoline(int msg, const char* details_json, void* arg) {
        auto* self = reinterpret_cast<MaaAdapter*>(arg);
        self->on_callback(msg, details_json ? details_json : "");
    }

    CallbackT make_callback() { return &MaaAdapter::trampoline; }

    void on_callback(int msg, const std::string& raw) {
        static const std::map<int, std::pair<const char*, const char*>> kMsg = {
            { 0, {"error", "InternalError"} },
            { 1, {"error", "InitFailed"} },
            { 2, {"connection", "ConnectionInfo"} },
            { 3, {"task", "AllTasksCompleted"} },
            { 4, {"connection", "AsyncCallInfo"} },
            { 5, {"info", "Destroyed"} },
            { 10000, {"error", "TaskChainError"} },
            { 10001, {"task", "TaskChainStart"} },
            { 10002, {"task", "TaskChainCompleted"} },
            { 10003, {"task", "TaskChainExtraInfo"} },
            { 10004, {"task", "TaskChainStopped"} },
            { 20000, {"error", "SubTaskError"} },
            { 20001, {"subtask", "SubTaskStart"} },
            { 20002, {"subtask", "SubTaskCompleted"} },
            { 20003, {"subtask", "SubTaskExtraInfo"} },
            { 20004, {"subtask", "SubTaskStopped"} },
            { 30000, {"info", "ReportRequest"} },
        };
        auto it = kMsg.find(msg);
        const char* cat = "info";
        const char* fallback_what = "Msg";
        if (it != kMsg.end()) { cat = it->second.first; fallback_what = it->second.second; }
        AdapterEvent ev;
        ev.source = "maa:" + std::to_string(msg);
        ev.category = cat;
        ev.raw = raw;
        if (!raw.empty()) {
            mj::Value d;
            if (mj::try_parse(raw, d)) {
                ev.details = std::move(d);
                std::string w = ev.details.get_str("what", "");
                ev.what = w.empty() ? fallback_what : w;
                ev.task = ev.details.get_str("taskchain", "");
                if (ev.task.empty()) ev.task = ev.details.get_str("subtask", "");
                ev.task_id = ev.details.get_int("taskid", 0);
            } else {
                ev.what = fallback_what;
                ev.details.set("_raw", mj::Value(raw));
            }
        } else {
            ev.what = fallback_what;
        }
        // 运行状态维护
        std::lock_guard<std::mutex> lk(st_mtx_);
        if (ev.what == "TaskChainStart") st_.run = "running";
        if (ev.what == "TaskChainError") {
            chain_err_ = true;
            st_.run = "failed";
            st_.last_error = raw.substr(0, 400);
        }
        if (ev.what == "TaskChainCompleted") st_.run = chain_err_ ? "failed" : "idle";
        if (ev.what == "AllTasksCompleted") st_.run = chain_err_ ? "failed" : "idle";
        emit(std::move(ev));
    }

    // ------------------------------------------------------------ 任务目录（与官方文档一致）
    static std::vector<mj::Value> build_catalog() {
        std::vector<mj::Value> v;
        auto add = [&](const char* name, const char* desc, std::vector<mj::Value> params) {
            mj::Value t = mj::Value::object();
            t.set("app", mj::Value("maa"));
            t.set("name", mj::Value(name));
            t.set("kind", mj::Value("task"));
            t.set("description", mj::Value(desc));
            t.set("params", mj::Value::array());
            for (auto& p : params) t["params"].push(std::move(p));
            v.push_back(std::move(t));
        };
        auto p = [](const char* n, const char* type, bool req, mj::Value def, const char* d,
                    std::vector<std::string> en = {}) {
            mj::Value pv = mj::Value::object();
            pv.set("name", mj::Value(n));
            pv.set("type", mj::Value(type));
            pv.set("required", mj::Value(req));
            pv.set("default", std::move(def));
            pv.set("description", mj::Value(d));
            if (!en.empty()) {
                pv.set("enum", mj::Value::array());
                for (auto& e : en) pv["enum"].push(mj::Value(e));
            }
            return pv;
        };

        add("StartUp", "开始唤醒（启动/切换账号/进入游戏）", {
            p("client_type", "string", true, mj::Value("Official"), "客户端版本",
              { "Official", "Bilibili", "txwy", "YoStarEN", "YoStarJP", "YoStarKR" }),
            p("start_game_enabled", "bool", false, mj::Value(true), "是否自动启动客户端（MAA 默认开启）"),
            p("account_name", "string", false, mj::Value(""), "切换账号（登录名片段）"),
        });
        add("CloseDown", "关闭游戏客户端", {
            p("client_type", "string", true, mj::Value("Official"), "客户端版本"),
        });
        add("Fight", "理智作战（刷关卡/掉落统计）", {
            p("stage", "string", false, mj::Value(""), "关卡名，空=当前/上次"),
            p("medicine", "number", false, mj::Value(0), "最大理智药数量"),
            p("stone", "number", false, mj::Value(0), "最大吃石头数量"),
            p("times", "number", false, mj::Value(2147483647), "战斗次数"),
            p("series", "number", false, mj::Value(1), "代理倍率 -1~10"),
            p("drops", "object", false, mj::Value::object(), "指定掉落即停 {item_id: qty}"),
            p("report_to_penguin", "bool", false, mj::Value(false), "上报企鹅数据"),
        });
        add("Recruit", "公开招募", {
            p("refresh", "bool", false, mj::Value(false), "刷新三星 Tag"),
            p("select", "array", true, mj::Value::array(), "点击的 Tag 等级"),
            p("confirm", "array", true, mj::Value::array(), "确认的 Tag 等级"),
            p("times", "number", false, mj::Value(0), "招募次数，0=仅计算"),
        });
        add("Infrast", "基建换班", {
            p("mode", "number", false, mj::Value(0), "0默认 10000自定义 20000轮换"),
            p("facility", "array", true, mj::Value::array(), "设施列表"),
            p("drones", "string", false, mj::Value("_NotUse"), "无人机用途",
              { "_NotUse", "Money", "SyntheticJade", "CombatRecord", "PureGold", "OriginStone", "Chip" }),
        });
        add("Mall", "领取信用并购物", {
            p("visit_friends", "bool", false, mj::Value(true), "访问好友基建"),
            p("shopping", "bool", false, mj::Value(true), "购物"),
            p("buy_first", "array", false, mj::Value::array(), "优先购买"),
            p("blacklist", "array", false, mj::Value::array(), "黑名单"),
        });
        add("Award", "领取日常/邮件等奖励", {
            p("award", "bool", false, mj::Value(true), "每日/每周任务奖励"),
            p("mail", "bool", false, mj::Value(false), "邮件奖励"),
            p("recruit", "bool", false, mj::Value(false), "每日免费单抽"),
        });
        add("Roguelike", "集成战略（肉鸽）", {
            p("theme", "string", false, mj::Value("Sami"), "主题",
              { "Phantom", "Mizuki", "Sami", "Sarkaz", "JieGarden", "BlackFlow" }),
            p("mode", "number", false, mj::Value(0), "模式"),
            p("squad", "string", false, mj::Value(""), "开局分队"),
            p("starts_count", "number", false, mj::Value(2147483647), "探索次数"),
        });
        add("Copilot", "自动抄作业", {
            p("filename", "string", false, mj::Value(""), "作业 JSON 路径"),
            p("loop_times", "number", false, mj::Value(1), "循环次数"),
        });
        add("Depot", "仓库识别（数据采集）", {});
        add("OperBox", "干员 box 识别（数据采集）", {});
        add("Reclamation", "生息演算", {
            p("theme", "string", false, mj::Value("Tales"), "主题"),
            p("mode", "number", false, mj::Value(0), "模式"),
        });
        add("Custom", "自定义 pipeline 任务", {
            p("task_names", "array", true, mj::Value::array(), "任务名列表"),
        });
        return v;
    }

    mj::Value cfg_;
    AppStatus st_;
    // MAA 报错链后紧跟 AllTasksCompleted，不锁住失败就会被冲成 idle，整串误报 done
    bool chain_err_ = false;
    mutable std::mutex st_mtx_;
    HMODULE lib_ = nullptr;
    DLL_DIRECTORY_COOKIE cookie_ = nullptr;
    void* handle_ = nullptr;
    CallbackT cb_ = nullptr;
    Fn fn_;
    std::vector<mj::Value> catalog_ = build_catalog();
};

} // namespace hub
