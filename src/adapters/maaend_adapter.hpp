// MaaEnd (明日方舟：终末地) 适配器 —— 直接调用 MaaFramework.dll / MaaAgentClient.dll
// 签名依据：maa-framework-sys 5.14.3 crate bindings（bindgen 自 MaaFramework 5.x 头文件）
#include "../adapters/adapter.hpp"
#include "../util.hpp"

#include <windows.h>
#include <memory>
#include <random>
#include <algorithm>

namespace hub {

// MaaStatusEnum
constexpr int MAA_STATUS_SUCCEEDED = 3000;

class MaaEndAdapter : public IAdapter {
public:
    MaaEndAdapter() { kind_ = "maaend"; g_instance = this; }
    ~MaaEndAdapter() override { if (g_instance == this) g_instance = nullptr; }

    // ------------------------------------------------------------ 发现
    bool discover(const mj::Value& cfg) override {
        cfg_ = cfg;
        st_.kind = "maaend";
        st_.name = cfg.get_str("display_name", "MaaEnd");
        st_.install_dir = cfg.get_str("install_dir", "");
        st_.core_dll = st_.install_dir + "\\maafw\\MaaFramework.dll";
        if (!util::file_exists(st_.core_dll)) {
            st_.available = false;
            st_.error = "MaaFramework.dll not found in " + st_.install_dir + "\\maafw";
            return false;
        }
        st_.available = true;
        st_.error.clear();
        // interface.json
        std::string iface_path = st_.install_dir + "\\interface.json";
        std::string content;
        if (util::read_file(iface_path, content)) {
            mj::Value v;
            if (mj::try_parse(content, v)) interface_ = std::move(v);
        }
        // 先设置本地化目录，scan_tasks 才能注入官方中文标签
        set_label_install_dir(st_.install_dir + "\\locales");
        catalog_ = scan_tasks();
        st_.task_count = (int64_t)catalog_.size();
        return true;
    }

    // ------------------------------------------------------------ 任务目录
    // 把一个 task 引用的 option 转成 UI 可用的参数描述列表
    // MaaEnd 的 option 定义在同文件 "option" 节：{name: {type, cases:[{name,option}], default}}
    static mj::Value task_option_specs(const mj::Value& task, const mj::Value& file_data) {
        mj::Value arr = mj::Value::array();
        const mj::Value* opts = file_data.find("option");
        if (!opts || !opts->is_object()) return arr;
        const mj::Value* refs = task.find("option");
        if (!refs || !refs->is_array()) return arr;
        for (auto& r : refs->items()) {
            if (!r.is_string()) continue;
            std::string key = r.as_string();
            auto it = opts->members().find(key);
            if (it == opts->members().end()) continue;
            const mj::Value& spec = it->second;
            if (!spec.is_object()) continue;
            std::string type = spec.get_str("type", "");
            mj::Value item = mj::Value::object();
            item.set("name", mj::Value(key));
            item.set("type", mj::Value(option_ui_type(type)));
            std::string lbl = option_label(task.get_str("name", ""), key);
            if (!lbl.empty()) item.set("description", mj::Value(lbl));
            if (auto* cases = spec.find("cases"); cases && cases->is_array()) {
                mj::Value names = mj::Value::array();
                for (auto& cs : cases->items()) {
                    std::string cn = cs.is_object() ? cs.get_str("name", "")
                                                    : (cs.is_string() ? cs.as_string() : "");
                    if (cn.empty()) continue;
                    // 优先用官方 cases 中文名，缺失时回退枚举名
                    std::string cl = case_label(task.get_str("name", ""), key, cn);
                    names.push(cl.empty() ? cn : cl);
                }
                if (!names.items().empty()) item.set("cases", std::move(names));
            }
            if (spec.has("default")) item.set("default", spec["default"]);
            arr.push(std::move(item));
        }
        return arr;
    }

    // 官方 option 中文标签：读 <install_dir>\locales\**\zh_cn.json
    // 键优先级：option.<key>.label > task.<task>.option.<key>.label > 空（回退 key）
    static std::string option_label(const std::string& task, const std::string& key) {
        static std::map<std::string, std::string> cache;
        static bool loaded = false;
        std::string ck = task + "|" + key;
        auto ci = cache.find(ck);
        if (ci != cache.end()) return ci->second;
        if (!loaded) {
            loaded = true;
            load_option_labels(cache);
        }
        auto it = cache.find(ck);
        std::string v = it == cache.end() ? "" : it->second;
        cache[ck] = v;
        return v;
    }

    // 大小写/分隔符/UTC-GMT 变体归一化，缓解不同版本间键名差异
    static std::string norm_case_key(const std::string& s) {
        std::string o;
        for (char ch : s) {
            if (ch == '_' || ch == '-' || ch == ' ') continue;
            o += (char)toupper((unsigned char)ch);
        }
        // UTC 与 GMT 在不同版本间混用
        size_t p;
        while ((p = o.find("UTC")) != std::string::npos) o.replace(p, 3, "GMT");
        return o;
    }

    static std::string case_label(const std::string& task, const std::string& key,
                                  const std::string& case_name) {
        static std::map<std::string, std::string> cache;
        static bool loaded = false;
        std::string ck = task + "|" + key + "|" + case_name;
        auto ci = cache.find(ck);
        if (ci != cache.end()) return ci->second;
        if (!loaded) {
            loaded = true;
            load_case_labels(cache);
        }
        // 依次尝试 task 专属、通配、归一化匹配
        std::string v;
        auto it = cache.find(ck);
        if (it != cache.end()) v = it->second;
        if (v.empty()) {
            auto it2 = cache.find("*|" + key + "|" + case_name);
            if (it2 != cache.end()) v = it2->second;
        }
        if (v.empty()) {
            auto it3 = cache.find(norm_case_key(key) + "|" + norm_case_key(case_name));
            if (it3 != cache.end()) v = it3->second;
        }
        cache[ck] = v;
        return v;
    }

    static void load_case_labels(std::map<std::string, std::string>& cache) {
        std::string root = label_dir_ref();
        std::vector<std::string> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(util::utf8_to_wide(root + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::string sub = root + "\\" + util::wide_to_utf8(n);
            WIN32_FIND_DATAW fd2;
            HANDLE h2 = FindFirstFileW(util::utf8_to_wide(sub + "\\zh_cn.json").c_str(), &fd2);
            if (h2 != INVALID_HANDLE_VALUE) {
                files.push_back(sub + "\\zh_cn.json");
                FindClose(h2);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (auto& fp : files) {
            std::string content;
            if (!util::read_file(fp, content)) continue;
            mj::Value j;
            if (!mj::try_parse(content, j)) continue;
            for (auto& kv : j.members()) {
                if (!kv.second.is_string()) continue;
                const std::string& k = kv.first;
                if (k.size() < 7 || k.substr(k.size() - 6) != ".label") continue;
                std::string body = k.substr(0, k.size() - 6);   // 去掉 .label
                size_t cp = body.find(".cases.");
                if (cp == std::string::npos) continue;
                std::string head = body.substr(0, cp);           // option.<O> 或 task.<T>.option.<O>
                std::string cn = body.substr(cp + 7);
                if (cn.empty()) continue;
                std::string ok;
                std::string tk;
                if (head.rfind("option.", 0) == 0) {
                    ok = head.substr(7);
                    cache["*|" + ok + "|" + cn] = kv.second.as_string();
                    cache["|" + ok + "|" + cn] = kv.second.as_string();
                } else if (head.rfind("task.", 0) == 0) {
                    std::string rest = head.substr(5);
                    size_t op = rest.find(".option.");
                    if (op == std::string::npos) continue;
                    tk = rest.substr(0, op);
                    ok = rest.substr(op + 8);
                    cache[tk + "|" + ok + "|" + cn] = kv.second.as_string();
                } else {
                    continue;
                }
                if (ok.empty()) continue;
                // 归一化键（UTC/GMT 等变体），便于跨版本匹配
                std::string nk = norm_case_key(ok) + "|" + norm_case_key(cn);
                if (cache.find(nk) == cache.end()) cache[nk] = kv.second.as_string();
            }
        }
    }

    static void load_option_labels(std::map<std::string, std::string>& cache) {
        std::string root = label_dir_ref();
        std::vector<std::string> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(util::utf8_to_wide(root + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::string sub = root + "\\" + util::wide_to_utf8(n);
            WIN32_FIND_DATAW fd2;
            HANDLE h2 = FindFirstFileW(util::utf8_to_wide(sub + "\\zh_cn.json").c_str(), &fd2);
            if (h2 != INVALID_HANDLE_VALUE) {
                files.push_back(sub + "\\zh_cn.json");
                FindClose(h2);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (auto& fp : files) {
            std::string content;
            if (!util::read_file(fp, content)) continue;
            mj::Value j;
            if (!mj::try_parse(content, j)) continue;
            for (auto& kv : j.members()) {
                if (!kv.second.is_string()) continue;
                const std::string& k = kv.first;
                if (k.rfind("option.", 0) == 0 && k.size() > 7
                    && k.substr(k.size() - 6) == ".label") {
                    cache["*|" + k.substr(7, k.size() - 7 - 6)] = kv.second.as_string();
                } else if (k.rfind("task.", 0) == 0 && k.find(".option.") != std::string::npos
                           && k.size() > 6 && k.substr(k.size() - 6) == ".label") {
                    std::string rest = k.substr(5, k.size() - 5 - 6);
                    size_t dot = rest.find(".option.");
                    if (dot != std::string::npos) {
                        std::string tk = rest.substr(0, dot);
                        std::string ok = rest.substr(dot + 8);
                        cache[tk + "|" + ok] = kv.second.as_string();
                    }
                }
            }
        }
    }

    static std::string& label_dir_ref() {
        static std::string dir;
        return dir;
    }
    static void set_label_install_dir(const std::string& d) { label_dir_ref() = d; }

    // MaaEnd option type -> UI 控件类型
    static std::string option_ui_type(const std::string& t) {
        if (t == "switch" || t == "checkbox") return "bool";
        if (t == "input") return "string";
        if (t == "select") return "select";
        return "string";
    }

    std::vector<mj::Value> scan_tasks() const {
        std::vector<mj::Value> out;
        std::vector<std::string> imports;
        if (auto* arr = interface_.find("import"); arr && arr->is_array()) {
            for (auto& rel : arr->items()) imports.push_back(rel.as_string());
        }
        std::vector<std::string> seen;
        for (const auto& rel : imports) {
            std::string path = st_.install_dir + "\\" + rel;
            std::replace(path.begin(), path.end(), '/', '\\');
            std::string content;
            if (!util::read_file(path, content)) continue;
            mj::Value data;
            if (!mj::try_parse(content, data)) continue;
            bool is_preset = rel.find("preset") != std::string::npos;
            if (auto* tasks = data.find("task"); tasks && tasks->is_array()) {
                for (auto& t : tasks->items()) {
                    std::string name = t.get_str("name", "");
                    if (name.empty()) continue;
                    if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
                    seen.push_back(name);
                    mj::Value tv = mj::Value::object();
                    tv.set("app", mj::Value("maaend"));
                    tv.set("name", mj::Value(name));
                    tv.set("kind", mj::Value(is_preset ? "preset" : "task"));
                    tv.set("description", mj::Value("来自 " + rel));
                    // 从同文件 option 节读取该任务引用的选项定义，
                    // 让 UI 能按类型渲染控件（原来硬编码为空数组，导致任务无可配置界面）
                    tv.set("params", task_option_specs(t, data));
                    out.push_back(std::move(tv));
                }
            }
        }
        return out;
    }

    // ------------------------------------------------------------ 加载
    bool load() override {
        if (!st_.available) return false;
        set_label_install_dir(st_.install_dir + "\\locales");
        std::string maafw = st_.install_dir + "\\maafw";
        std::wstring dll = util::utf8_to_wide(st_.core_dll);
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) h = LoadLibraryW(dll.c_str());
        if (!h) {
            st_.error = "LoadLibrary MaaFramework failed err=" + std::to_string(GetLastError());
            return false;
        }
        lib_ = h;
        if (!resolve_symbols()) {
            st_.error = "missing MaaFramework exports";
            return false;
        }
        std::wstring agent_dll = util::utf8_to_wide(maafw + "\\MaaAgentClient.dll");
        HMODULE ah = LoadLibraryExW(agent_dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!ah) ah = LoadLibraryW(agent_dll.c_str());
        if (!ah) {
            st_.error = "LoadLibrary MaaAgentClient failed err=" + std::to_string(GetLastError());
            return false;
        }
        agent_lib_ = ah;
        if (!resolve_agent_symbols()) {
            st_.error = "missing MaaAgentClient exports";
            return false;
        }
        // DLL 依赖解析保障
        cookie_ = AddDllDirectory(util::utf8_to_wide(maafw).c_str());
        {
            std::wstring path;
            wchar_t buf[32768];
            DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32768);
            if (n) path.assign(buf, n);
            std::wstring add = util::utf8_to_wide(maafw);
            if (path.find(add) == std::wstring::npos) {
                SetEnvironmentVariableW(L"PATH", (add + L";" + path).c_str());
            }
        }
        // 版本
        const char* ver = fn_.MaaVersion();
        st_.version = ver ? ver : "";
        // 日志目录
        std::string log_dir = st_.install_dir + "\\debug\\maahub-cpp";
        util::ensure_dir(log_dir);
        fn_.MaaGlobalSetOption(1, (void*)log_dir.c_str(), log_dir.size() + 1);
        // 关闭 stdout 日志（StdoutLevel=0, 4 字节）
        int32_t level0 = 0;
        fn_.MaaGlobalSetOption(4, &level0, sizeof(level0));

        tasker_ = fn_.MaaTaskerCreate();
        if (!tasker_) { st_.error = "MaaTaskerCreate failed"; return false; }
        res_ = fn_.MaaResourceCreate();
        if (!res_) { st_.error = "MaaResourceCreate failed"; return false; }
        std::string res_path = st_.install_dir + "\\resource";
        int64_t rid = fn_.MaaResourcePostBundle(res_, res_path.c_str());
        if (rid <= 0) { st_.error = "MaaResourcePostBundle failed"; return false; }
        int st = fn_.MaaResourceWait(res_, rid);
        if (st != MAA_STATUS_SUCCEEDED) {
            st_.error = "resource load status=" + std::to_string(st);
            return false;
        }
        if (!fn_.MaaTaskerBindResource(tasker_, res_)) {
            st_.error = "MaaTaskerBindResource failed";
            return false;
        }
        // 事件 sink
        sink_ = make_sink();
        fn_.MaaTaskerAddSink(tasker_, sink_, nullptr);
        // agent 子进程
        if (!start_agents()) {
            st_.error = "agent children failed to start: " + agent_error_;
            return false;
        }
        return true;
    }

    void unload() override {
        for (auto& c : agent_clients_) {
            if (c) afn_.MaaAgentClientDestroy(c);
        }
        agent_clients_.clear();
        for (auto& p : agent_procs_) {
            if (p.alive()) {
                TerminateProcess(p.process, 0);
            }
            p.close();
        }
        agent_procs_.clear();
        if (tasker_) { fn_.MaaTaskerDestroy(tasker_); tasker_ = nullptr; }
        if (res_) { fn_.MaaResourceDestroy(res_); res_ = nullptr; }
        if (cookie_) { RemoveDllDirectory(cookie_); cookie_ = nullptr; }
        if (lib_) { FreeLibrary(lib_); lib_ = nullptr; }
        if (agent_lib_) { FreeLibrary(agent_lib_); agent_lib_ = nullptr; }
    }

    // ------------------------------------------------------------ agent 子进程
    std::string new_uuid() {
        static std::random_device rd;
        static std::mt19937_64 rng(rd());
        uint64_t a = rng(), b = rng();
        char buf[40];
        snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                 (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xFFFF), (unsigned)(a & 0xFFFF),
                 (unsigned)((b >> 48) & 0xFFFF), (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
        return buf;
    }

    bool start_agents() {
        auto* agents = interface_.find("agent");
        if (!agents || !agents->is_array()) {
            agent_error_ = "interface.json has no agent list";
            return false;
        }
        // 并行拉起所有 agent：串行时每个 agent 的固定等待会叠加（实测 2 个 agent ~5s）
        struct AgentJob {
            std::string child;
            std::string exe;
            std::string identifier;
        };
        std::vector<AgentJob> jobs;
        for (auto& spec : agents->items()) {
            std::string child = spec.get_str("child_exec", "");
            if (child.empty()) continue;
            std::string exe = st_.install_dir + "\\" + child;
            std::replace(exe.begin(), exe.end(), '/', '\\');
            if (!util::file_exists(exe)) {
                // Windows 下 child_exec 省略 .exe
                if (!util::file_exists(exe + ".exe")) {
                    agent_error_ = "agent binary missing: " + exe;
                    continue;
                }
                exe += ".exe";
            }
            jobs.push_back({ child, exe, new_uuid() });
        }
        if (jobs.empty()) return false;

        std::mutex res_mtx;
        std::vector<void*> clients(jobs.size(), nullptr);
        std::vector<std::string> errors(jobs.size());
        std::vector<std::thread> threads;
        for (size_t i = 0; i < jobs.size(); ++i) {
            threads.emplace_back([&, i] {
                auto& job = jobs[i];
                // PATH 注入 maafw（cpp-algo 静态链接 maafw 中的 DLL）
                std::wstring env_block = util::make_env_block(
                    util::utf8_to_wide(st_.install_dir + "\\maafw"));
                std::wstring cmd = L"\"" + util::utf8_to_wide(job.exe) + L"\" "
                                   + util::utf8_to_wide(job.identifier);
                util::ProcHandle proc;
                if (!util::spawn_process(cmd, util::utf8_to_wide(st_.install_dir),
                                         env_block, proc)) {
                    std::lock_guard<std::mutex> lk(res_mtx);
                    errors[i] = "spawn failed: " + job.exe;
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(proc_mtx_);
                    agent_procs_.push_back(proc);
                }
                // IPC 连接（client 侧 bind，server 连过来）：
                // 轮询等待而非固定 sleep——agent 就绪即可立即连上，无需空等 2.5s
                void* client = nullptr;
                for (int attempt = 0; attempt < 120; ++attempt) {   // 最长 6s
                    void* sb = fn_.MaaStringBufferCreate();
                    fn_.MaaStringBufferSetEx(sb, job.identifier.c_str(),
                                             job.identifier.size());
                    client = afn_.MaaAgentClientCreateV2(sb);
                    if (client) {
                        afn_.MaaAgentClientSetTimeout(client, 8000);
                        if (afn_.MaaAgentClientBindResource(client, res_)) {
                            if (afn_.MaaAgentClientConnect(client)) break;
                        }
                        afn_.MaaAgentClientDestroy(client);
                        client = nullptr;
                    }
                    if (!proc.alive()) break;
                    Sleep(50);
                }
                std::lock_guard<std::mutex> lk(res_mtx);
                if (client) {
                    clients[i] = client;
                } else {
                    errors[i] = "agent " + job.child + " did not connect";
                }
            });
        }
        for (auto& t : threads) t.join();

        bool any = false;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (clients[i]) {
                std::lock_guard<std::mutex> lk(cli_mtx_);
                agent_clients_.push_back(clients[i]);
                any = true;
            } else if (!errors[i].empty()) {
                std::lock_guard<std::mutex> lk(err_mtx_);
                if (agent_error_.empty()) agent_error_ = errors[i];
            }
        }
        return any && !agent_clients_.empty();
    }

    // ------------------------------------------------------------ 连接
    bool connect() override {
        if (!tasker_) { st_.last_error = "not loaded"; return false; }
        st_.conn = "connecting";
        std::string mode = cfg_.get_path_str("connect.mode", "win32");
        if (mode == "adb") {
            st_.conn = "failed";
            st_.last_error = "ADB 模式未配置（请使用 win32）";
            return false;
        }
        std::string cls = cfg_.get_str("game_window_class", "UnityWndClass");
        std::string title = cfg_.get_str("game_window_title_regex", "Endfield");
        util::WindowInfo wi;
        if (!util::find_window(cls, title, wi)) {
            st_.conn = "failed";
            st_.last_error = "game window not found (class=" + cls + ", title~/" + title + "/). 请先启动终末地客户端。";
            return false;
        }
        uint64_t sc = (uint64_t)cfg_.get_path_int("connect.screencap_method", 32);
        uint64_t mm = (uint64_t)cfg_.get_path_int("connect.mouse_method", 1);
        uint64_t km = (uint64_t)cfg_.get_path_int("connect.keyboard_method", 1);
        void* ctrl = fn_.MaaWin32ControllerCreate((void*)wi.hwnd, sc, mm, km);
        if (!ctrl) {
            st_.conn = "failed";
            st_.last_error = "MaaWin32ControllerCreate failed";
            return false;
        }
        ctrl_ = ctrl;
        int64_t cid = fn_.MaaControllerPostConnection(ctrl);
        if (cid <= 0) {
            st_.conn = "failed";
            st_.last_error = "MaaControllerPostConnection failed";
            return false;
        }
        int st = fn_.MaaControllerWait(ctrl, cid);
        if (st != MAA_STATUS_SUCCEEDED) {
            st_.conn = "failed";
            st_.last_error = "controller connect status=" + std::to_string(st);
            return false;
        }
        if (!fn_.MaaTaskerBindController(tasker_, ctrl)) {
            st_.conn = "failed";
            st_.last_error = "MaaTaskerBindController failed";
            return false;
        }
        int w = 0, hgt = 0;
        fn_.MaaControllerGetResolution(ctrl, &w, &hgt);
        void* sb = fn_.MaaStringBufferCreate();
        std::string uuid;
        if (fn_.MaaControllerGetUuid(ctrl, sb)) uuid = strbuf_get(sb);
        void* hb = fn_.MaaStringBufferCreate();
        if (fn_.MaaResourceGetHash(res_, hb)) st_.resource_hash = strbuf_get(hb);
        st_.ctrl_mode = "win32";
        st_.ctrl_hwnd = (int64_t)(intptr_t)wi.hwnd;
        st_.res_w = w;
        st_.res_h = hgt;
        st_.uuid = uuid;
        st_.conn = "connected";
        st_.last_error.clear();
        return true;
    }

    void disconnect() override { st_.conn = "disconnected"; }

    bool is_connected() override {
        if (!ctrl_) return false;
        return fn_.MaaControllerConnected(ctrl_) != 0;
    }

    // ------------------------------------------------------------ 任务
    bool run_task(const std::string& task, const mj::Value& params, int64_t& out) override {
        out = 0;
        if (!tasker_) { st_.last_error = "not loaded"; return false; }
        if (!is_connected()) { st_.last_error = "not connected"; return false; }
        std::string override_json = "{}";
        if (params.is_object() && !params.members().empty()) override_json = mj::Value::dump(params);
        int64_t tid = fn_.MaaTaskerPostTask(tasker_, task.c_str(), override_json.c_str());
        if (tid <= 0) {
            st_.last_error = "MaaTaskerPostTask(" + task + ") failed";
            return false;
        }
        st_.run = "running";
        out = tid;
        return true;
    }

    bool stop() override {
        if (!tasker_) return false;
        int64_t tid = fn_.MaaTaskerPostStop(tasker_);
        if (tid <= 0) return false;
        // 绝不用 MaaTaskerWait 无限阻塞：任务不结束会把调用线程（进而整个
        // WorkerClient::call 与 UI 刷新）永久挂住。改为轮询 Running，
        // 最多等 10 秒后放弃等待（任务仍由 MaaEnd 侧自行收尾）。
        int waited = 0;
        while (waited < 10000) {
            if (fn_.MaaTaskerRunning(tasker_) == 0) break;
            Sleep(100);
            waited += 100;
        }
        st_.run = fn_.MaaTaskerRunning(tasker_) != 0 ? "stopping" : "idle";
        return true;
    }

    bool is_running() override {
        if (!tasker_) return false;
        return fn_.MaaTaskerRunning(tasker_) != 0;
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
    std::string strbuf_get(void* buf) {
        if (!buf) return "";
        const char* s = fn_.MaaStringBufferGet(buf);
        std::string out = s ? s : "";
        fn_.MaaStringBufferDestroy(buf);
        return out;
    }

    // ------------------------------------------------------------ 符号
    struct Fn {
        const char* (*MaaVersion)() = nullptr;
        bool (*MaaGlobalSetOption)(int, void*, uint64_t) = nullptr;
        void* (*MaaTaskerCreate)() = nullptr;
        void (*MaaTaskerDestroy)(void*) = nullptr;
        bool (*MaaTaskerBindResource)(void*, void*) = nullptr;
        bool (*MaaTaskerBindController)(void*, void*) = nullptr;
        int64_t (*MaaTaskerPostTask)(void*, const char*, const char*) = nullptr;
        int64_t (*MaaTaskerPostStop)(void*) = nullptr;
        int (*MaaTaskerStatus)(void*, int64_t) = nullptr;
        int (*MaaTaskerWait)(void*, int64_t) = nullptr;
        bool (*MaaTaskerInited)(void*) = nullptr;
        bool (*MaaTaskerRunning)(void*) = nullptr;
        bool (*MaaTaskerAddSink)(void*, void*, void*) = nullptr;
        void* (*MaaResourceCreate)() = nullptr;
        void (*MaaResourceDestroy)(void*) = nullptr;
        int64_t (*MaaResourcePostBundle)(void*, const char*) = nullptr;
        int (*MaaResourceWait)(void*, int64_t) = nullptr;
        int (*MaaResourceStatus)(void*, int64_t) = nullptr;
        bool (*MaaResourceGetHash)(void*, void*) = nullptr;
        void* (*MaaWin32ControllerCreate)(void*, uint64_t, uint64_t, uint64_t) = nullptr;
        void (*MaaControllerDestroy)(void*) = nullptr;
        int64_t (*MaaControllerPostConnection)(void*) = nullptr;
        int (*MaaControllerWait)(void*, int64_t) = nullptr;
        bool (*MaaControllerConnected)(void*) = nullptr;
        bool (*MaaControllerGetResolution)(void*, int*, int*) = nullptr;
        bool (*MaaControllerGetUuid)(void*, void*) = nullptr;
        void* (*MaaStringBufferCreate)() = nullptr;
        void (*MaaStringBufferDestroy)(void*) = nullptr;
        const char* (*MaaStringBufferGet)(void*) = nullptr;
        uint64_t (*MaaStringBufferSize)(void*) = nullptr;
        bool (*MaaStringBufferSetEx)(void*, const char*, uint64_t) = nullptr;
    };

    struct AgentFn {
        void* (*MaaAgentClientCreateV2)(void*) = nullptr;
        void* (*MaaAgentClientCreateTcp)(uint16_t) = nullptr;
        void (*MaaAgentClientDestroy)(void*) = nullptr;
        bool (*MaaAgentClientBindResource)(void*, void*) = nullptr;
        bool (*MaaAgentClientConnect)(void*) = nullptr;
        bool (*MaaAgentClientAlive)(void*) = nullptr;
        bool (*MaaAgentClientSetTimeout)(void*, int64_t) = nullptr;
        bool (*MaaAgentClientIdentifier)(void*, void*) = nullptr;
    };

    template <typename T>
    bool sym(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(lib_, name));
        return target != nullptr;
    }
    template <typename T>
    bool asym(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(agent_lib_, name));
        return target != nullptr;
    }

    bool resolve_symbols() {
        bool ok = true;
        ok &= sym(fn_.MaaVersion, "MaaVersion");
        ok &= sym(fn_.MaaGlobalSetOption, "MaaGlobalSetOption");
        ok &= sym(fn_.MaaTaskerCreate, "MaaTaskerCreate");
        ok &= sym(fn_.MaaTaskerDestroy, "MaaTaskerDestroy");
        ok &= sym(fn_.MaaTaskerBindResource, "MaaTaskerBindResource");
        ok &= sym(fn_.MaaTaskerBindController, "MaaTaskerBindController");
        ok &= sym(fn_.MaaTaskerPostTask, "MaaTaskerPostTask");
        ok &= sym(fn_.MaaTaskerPostStop, "MaaTaskerPostStop");
        ok &= sym(fn_.MaaTaskerStatus, "MaaTaskerStatus");
        ok &= sym(fn_.MaaTaskerWait, "MaaTaskerWait");
        ok &= sym(fn_.MaaTaskerInited, "MaaTaskerInited");
        ok &= sym(fn_.MaaTaskerRunning, "MaaTaskerRunning");
        ok &= sym(fn_.MaaTaskerAddSink, "MaaTaskerAddSink");
        ok &= sym(fn_.MaaResourceCreate, "MaaResourceCreate");
        ok &= sym(fn_.MaaResourceDestroy, "MaaResourceDestroy");
        ok &= sym(fn_.MaaResourcePostBundle, "MaaResourcePostBundle");
        ok &= sym(fn_.MaaResourceWait, "MaaResourceWait");
        ok &= sym(fn_.MaaResourceStatus, "MaaResourceStatus");
        ok &= sym(fn_.MaaResourceGetHash, "MaaResourceGetHash");
        ok &= sym(fn_.MaaWin32ControllerCreate, "MaaWin32ControllerCreate");
        ok &= sym(fn_.MaaControllerDestroy, "MaaControllerDestroy");
        ok &= sym(fn_.MaaControllerPostConnection, "MaaControllerPostConnection");
        ok &= sym(fn_.MaaControllerWait, "MaaControllerWait");
        ok &= sym(fn_.MaaControllerConnected, "MaaControllerConnected");
        ok &= sym(fn_.MaaControllerGetResolution, "MaaControllerGetResolution");
        ok &= sym(fn_.MaaControllerGetUuid, "MaaControllerGetUuid");
        ok &= sym(fn_.MaaStringBufferCreate, "MaaStringBufferCreate");
        ok &= sym(fn_.MaaStringBufferDestroy, "MaaStringBufferDestroy");
        ok &= sym(fn_.MaaStringBufferGet, "MaaStringBufferGet");
        ok &= sym(fn_.MaaStringBufferSize, "MaaStringBufferSize");
        ok &= sym(fn_.MaaStringBufferSetEx, "MaaStringBufferSetEx");
        return ok;
    }

    bool resolve_agent_symbols() {
        bool ok = true;
        ok &= asym(afn_.MaaAgentClientCreateV2, "MaaAgentClientCreateV2");
        ok &= asym(afn_.MaaAgentClientCreateTcp, "MaaAgentClientCreateTcp");
        ok &= asym(afn_.MaaAgentClientDestroy, "MaaAgentClientDestroy");
        ok &= asym(afn_.MaaAgentClientBindResource, "MaaAgentClientBindResource");
        ok &= asym(afn_.MaaAgentClientConnect, "MaaAgentClientConnect");
        ok &= asym(afn_.MaaAgentClientAlive, "MaaAgentClientAlive");
        ok &= asym(afn_.MaaAgentClientSetTimeout, "MaaAgentClientSetTimeout");
        ok &= asym(afn_.MaaAgentClientIdentifier, "MaaAgentClientIdentifier");
        return ok;
    }

    // ------------------------------------------------------------ sink 回调
    // MaaEventCallback: void(handle, message, details_json, trans_arg)
    using SinkT = void (*)(void*, const char*, const char*, void*);
    SinkT make_sink() {
        // worker 进程中只有一个 MaaEndAdapter 实例，用静态 thunk + 单例反查
        return [](void*, const char* message, const char* details_json, void*) {
            if (g_instance) g_instance->on_event(message ? message : "", details_json ? details_json : "");
        };
    }

    void on_event(const std::string& message, const std::string& raw) {
        AdapterEvent ev;
        ev.app = "maaend";
        ev.source = "fw:" + message;
        ev.raw = raw;
        // message 形如 Resource.Loading.Succeeded / Tasker.Task.Succeeded
        std::string cat = "info";
        size_t dot = message.find('.');
        if (dot != std::string::npos) cat = message.substr(0, dot);
        std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
        if (cat == "resource") cat = "connection";
        ev.category = cat;
        ev.what = message;
        if (!raw.empty()) {
            mj::Value d;
            if (mj::try_parse(raw, d)) {
                ev.details = std::move(d);
                ev.task = ev.details.get_str("entry", "");
                if (ev.task.empty()) ev.task = ev.details.get_str("node_name", "");
                ev.task_id = ev.details.get_int("task_id", 0);
            } else {
                ev.details.set("_raw", mj::Value(raw));
            }
        }
        std::lock_guard<std::mutex> lk(st_mtx_);
        if (message.find("Task.Succeeded") != std::string::npos
            || message.find("Tasker.Task.Completed") != std::string::npos) {
            st_.run = "idle";
        } else if (message.find("Task.Failed") != std::string::npos) {
            st_.run = "failed";
            st_.last_error = raw.substr(0, 400);
        }
        emit(std::move(ev));
    }

    mj::Value cfg_;
    mj::Value interface_;
    AppStatus st_;
    mutable std::mutex st_mtx_;
    HMODULE lib_ = nullptr;
    HMODULE agent_lib_ = nullptr;
    DLL_DIRECTORY_COOKIE cookie_ = nullptr;
    void* tasker_ = nullptr;
    void* res_ = nullptr;
    void* ctrl_ = nullptr;
    SinkT sink_ = nullptr;
    std::vector<util::ProcHandle> agent_procs_;
    std::mutex proc_mtx_;      // 保护 agent_procs_
    std::mutex cli_mtx_;      // 保护 agent_clients_
    std::mutex err_mtx_;      // 保护 agent_error_
    std::vector<void*> agent_clients_;
    std::string agent_error_;
    Fn fn_;
    AgentFn afn_;
    std::vector<mj::Value> catalog_;

public:
    static MaaEndAdapter* g_instance;
};

// 每个 worker 进程仅一个 MaaEndAdapter 实例
MaaEndAdapter* MaaEndAdapter::g_instance = nullptr;

} // namespace hub
