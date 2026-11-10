// MaaHub C++ 入口（原生窗口版）
//   MaaHub.exe                     GUI 模式：原生 Win32 窗口（无任何网页特性）
//   MaaHub.exe --check             干跑自检（控制台输出，不启动 UI）
//   MaaHub.exe --worker K C        worker 子进程模式（内部使用）
//   MaaHub.exe --core-version K D  探测某核心目录的版本（子进程，内部使用）
#ifndef NOMINMAX
#define NOMINMAX
#endif
// GDI+ 需要完整 windows.h，不能定义 WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <cstdio>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "app_window.hpp"
#include "config_import.hpp"
#include "core_manager.hpp"
#include "hub.hpp"
#include "settings.hpp"
#include "json.hpp"
#include "util.hpp"
#include "worker_main.hpp"

using namespace hub;

static int g_port_override = 0;   // 保留参数兼容（GUI 模式忽略）

static mj::Value load_config(const std::string& path) {
    std::string content;
    std::string p = path.empty() ? (util::exe_dir() + "\\config.json") : path;
    if (!util::read_file(p, content)) {
        std::string dev = util::parent_dir(util::exe_dir()) + "\\config.example.json";
        if (!util::read_file(dev, content)) {
            mj::Value v = mj::Value::object();
            mj::Value a = mj::Value::object();
            mj::Value maa = mj::Value::object();
            maa.set("enabled", true);
            maa.set("install_dir", "D:\\Application\\MAA");
            maa.set("display_name", "MAA (明日方舟)");
            maa.set("game_window_class", "UnityWndClass");
            maa.set("game_window_title_regex", "Arknights");
            mj::Value c1 = mj::Value::object();
            c1.set("mode", "win32");
            c1.set("screencap_method", 32);
            c1.set("mouse_method", 1);
            c1.set("keyboard_method", 1);
            maa.set("connect", std::move(c1));
            mj::Value end = mj::Value::object();
            end.set("enabled", true);
            end.set("install_dir", "D:\\Application\\MaaEnd");
            end.set("display_name", "MaaEnd (终末地)");
            end.set("game_window_class", "UnityWndClass");
            end.set("game_window_title_regex", "Endfield");
            mj::Value c2 = mj::Value::object();
            c2.set("mode", "win32");
            c2.set("screencap_method", 32);
            c2.set("mouse_method", 1);
            c2.set("keyboard_method", 1);
            end.set("connect", std::move(c2));
            a.set("maa", std::move(maa));
            a.set("maaend", std::move(end));
            v.set("apps", std::move(a));
            mj::Value llm = mj::Value::object();
            llm.set("enabled", false);
            llm.set("base_url", "https://api.deepseek.com");
            llm.set("api_key", "");
            llm.set("model", "deepseek-chat");
            llm.set("temperature", 0.2);
            llm.set("max_tokens", 2000);
            llm.set("timeout_sec", 60);
            v.set("llm", std::move(llm));
            return v;
        }
    }
    mj::Value v;
    if (!mj::try_parse(content, v)) return mj::Value::object();
    return v;
}

static void crash_log(const char* why) {
    FILE* f = nullptr;
    fopen_s(&f, "maahub-crash.log", "a");
    if (f) {
        fprintf(f, "[%s] %s\n", util::now_str().c_str(), why);
        fclose(f);
    }
}

// 子进程模式：加载核心 DLL 查询真实版本
// 注意：必须在核心目录上设置 DLL 搜索目录，否则 MaaCore/MaaFramework 的
// 同目录依赖（MaaUtils/opencv/onnxruntime）无法解析。
static int core_version_mode(const std::string& kind, const std::string& path) {
    std::wstring wpath = util::utf8_to_wide(path);
    // 顺序很重要：先设默认搜索策略，再注册目录；反序会让 cookie 失效（err=87）
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                             | LOAD_LIBRARY_SEARCH_USER_DIRS);
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(wpath.c_str());
    HMODULE h = nullptr;
    std::wstring dll = util::utf8_to_wide(kind == "maa"
        ? path + "\\MaaCore.dll" : path + "\\maafw\\MaaFramework.dll");
    h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        fprintf(stderr, "[core-version] LoadLibrary failed: %s (err=%lu)\n",
                util::wide_to_utf8(dll).c_str(), GetLastError());
        if (cookie) RemoveDllDirectory(cookie);
        return 1;
    }
    using Fn = const char* (*)();
    auto f = (Fn)GetProcAddress(h, kind == "maa" ? "AsstGetVersion" : "MaaVersion");
    if (!f) {
        fprintf(stderr, "[core-version] export not found (err=%lu)\n", GetLastError());
        FreeLibrary(h);
        if (cookie) RemoveDllDirectory(cookie);
        return 1;
    }
    printf("%s", f());
    FreeLibrary(h);
    if (cookie) RemoveDllDirectory(cookie);
    return 0;
}

// ---------------- 无头设备驱动（不创建任何窗口） ----------------
// MaaHub.exe --cli <cmd> [args]
//   ping / devices / tasks <exec> / connect <exec> / disconnect <exec>
//   run <exec> <task> [params|@file] [--timeout N] / stop <exec>
//   events [n] [exec] / get-connect <exec> / set-connect <exec> <json|@file>
//   override <exec> <task|*> [params|@file]  只映射不执行，回显 entry + pipeline_override
//   accounts / add-account <game> <label> <identifier> [match] [server] [notes]
//   use-account <game> <id> / link <exec> <account_id>
static std::string cli_arg(const std::vector<std::string>& a, size_t i, const std::string& def = "") {
    return i < a.size() ? a[i] : def;
}

// --timeout 60 和 --timeout=60 都得认：命令行是从 mp\cli_cmd.txt 读回来的，
// 写成等号形式时旧代码只认空格形式，超时会静默退回默认值，卡死的任务再也停不下来。
static int cli_flag_int(const std::vector<std::string>& a, const std::string& name, int def) {
    std::string eq = name + "=";
    for (size_t i = 1; i < a.size(); ++i) {
        if (a[i] == name && i + 1 < a.size()) return atoi(a[i + 1].c_str());
        if (a[i].rfind(eq, 0) == 0) return atoi(a[i].c_str() + eq.size());
    }
    return def;
}

static std::string cli_read_blob(const std::string& s) {
    if (!s.empty() && s[0] == '@') {
        std::string c;
        if (util::read_file(s.substr(1), c)) return c;
        return "{}";
    }
    return s;
}

static int run_cli(const std::vector<std::string>& args, const mj::Value& config) {
    std::string cmd = args.empty() ? "" : args[0];
    if (cmd.empty()) {
        printf("usage: MaaHub --cli ping|devices|tasks|import|sched-list|sched-run|sched-tick|serve|connect|run|seq|stop|events|get-connect|set-connect|override|accounts|add-account|use-account|link|core-check|core-update\n");
        return 2;
    }
    Hub hub(config);
    hub.ensure_default_executors();

    auto execs = hub.executors();
    auto resolve = [&](const std::string& key) -> std::string {
        if (key.empty() || key == "auto") {
            // 只有一个执行器时可直接省略
            return execs.size() == 1 ? execs[0].id : std::string();
        }
        for (auto& e : execs) if (e.id == key) return e.id;
        std::string hits;
        for (auto& e : execs) {
            if (e.kind == key) { if (hits.empty()) hits = e.id; }
            if (e.id.rfind(key, 0) == 0) { if (hits.empty()) hits = e.id; }
            if (!e.label.empty() && e.label.find(key) != std::string::npos) { if (hits.empty()) hits = e.id; }
        }
        return hits;
    };
    auto show_status = [&](const std::string& id) {
        mj::Value st = hub.executor_status(id, 0.0);
        mj::Value ctrl = st.has("controller") ? st["controller"] : mj::Value::object();
        printf("exec=%s kind=%s conn=%s run=%s version=%s tasks=%lld\n", id.c_str(),
               st.get_str("kind", "?").c_str(), st.get_str("conn", "unknown").c_str(),
               st.get_str("run", "-").c_str(), st.get_str("version", "-").c_str(),
               (long long)st.get_int("task_count", 0));
        std::string err = st.get_str("last_error", "");
        if (!err.empty()) printf("  last_error=%s\n", err.c_str());
        std::string le = st.get_str("error", "");
        if (!le.empty()) printf("  load_error=%s\n", le.c_str());
        std::string fl = st.get_str("fw_log", "");
        if (!fl.empty()) printf("  fw_log=%s\n", fl.c_str());
        if (ctrl.is_object()) {
            printf("  ctrl mode=%s screencap=%lld mouse=%lld keyboard=%lld touch=%s"
                   " adb=%s addr=%s config=%s uuid=%s\n",
                   ctrl.get_str("mode", "-").c_str(),
                   (long long)ctrl.get_int("screencap_method", 0),
                   (long long)ctrl.get_int("mouse_method", 0),
                   (long long)ctrl.get_int("keyboard_method", 0),
                   ctrl.get_str("touch_mode", "-").c_str(),
                   ctrl.get_str("adb_path", "-").c_str(),
                   ctrl.get_str("address", "-").c_str(),
                   ctrl.get_str("config", "-").c_str(),
                   st.get_str("uuid", "-").c_str());
        }
    };
    auto print_events = [&](int n, const std::string& id) {
        auto evs = hub.recent_events(n, id);
        for (auto& e : evs) {
            std::string line = mj::Value::dump(e->to_json());
            if (line.size() > 400) line = line.substr(0, 400) + "...";
            printf("%s\n", line.c_str());
        }
        printf("events=%zu\n", evs.size());
    };

    // 核心更新：自动更新链路的命令行入口（便于脚本化实测）
    if (cmd == "core-check" || cmd == "core-update") {
        std::string kind = args.size() > 1 ? args[1] : "maaend";
        std::string state = util::exe_dir() + "\\state";
        util::ensure_dir(state);
        CoreManager cm(state, CoreManager::default_cores_root());
        CoreInfo cur = cm.default_core(kind);
        SettingsStore ss(state);
        AppSettings st = ss.get();
        printf("CURRENT kind=%s id=%s version=%s tag=%s\n", kind.c_str(), cur.id.c_str(),
               cur.version.c_str(), cur.release_tag.c_str());
        UpdateInfo ui = CoreManager::check_update(kind, cur.version, cur.release_tag, &st);
        if (!ui.ok) {
            printf("CHECK_FAILED %s\n", ui.error.c_str());
            return 1;
        }
        printf("CHECK source=%s latest=%s latest_tag=%s has_update=%d tag_mismatch=%d"
               " asset=%s size=%lld sha=%s\n",
               ui.source_name.c_str(), ui.latest.c_str(), ui.latest_tag.c_str(),
               (int)ui.has_update, (int)ui.tag_mismatch, ui.asset_name.c_str(),
               (long long)ui.asset_size, ui.asset_sha256.c_str());
        if (cmd == "core-check") return 0;
        if (!ui.has_update) { printf("ALREADY_LATEST\n"); return 0; }
        if (ui.download_url.empty()) { printf("NO_DOWNLOAD_URL\n"); return 1; }
        std::atomic<int> prog{ 0 };
        std::string stage, installed;
        bool unchanged = false;
        std::string id = CoreManager::pull_core(kind, ui.download_url, "", &prog, &stage, &st,
                                                ui.latest_tag, /*set_default=*/true,
                                                ui.asset_sha256, &installed, &unchanged);
        if (id.empty()) {
            printf("UPDATE_FAILED stage=%s\n", stage.c_str());
            return 1;
        }
        // pull_core 内部另建 CoreManager 写盘，这里要重新加载才能读到新核心
        CoreManager cm2(state, CoreManager::default_cores_root());
        CoreInfo c;
        cm2.get(id, c);
        printf("UPDATED id=%s version=%s tag=%s unchanged=%d is_default=%d\n",
               c.id.c_str(), c.version.c_str(), c.release_tag.c_str(),
               (int)unchanged, (int)c.is_default);
        return 0;
    }

    hub.start_all();   // 拉起 worker + discover + load（并行，无窗口）

    if (cmd == "ping") { printf("CLI OK\n"); return 0; }

    if (cmd == "devices") {
        for (auto& e : execs) show_status(e.id);
        return 0;
    }
    if (cmd == "tasks") {
        std::string id = resolve(cli_arg(args, 1));
        if (id.empty()) { printf("executor not found: %s\n", cli_arg(args, 1).c_str()); return 1; }
        hub.refresh_catalogs();
        mj::Value cat = hub.executor_catalog(id, 0.0);
        if (cat.is_array()) {
            // --json 把整份 catalog 吐出来（含 entry 与 params 的 option 规格），
            // 无头测试靠它核对 #33 的 entry 映射和 #34/#35 的 option 类型
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--json") { printf("%s\n", mj::Value::dump(cat).c_str()); return 0; }
            }
            // 第三列给 entry（post 给核心的 pipeline 节点），无头测试靠它核对映射是否断链
            for (auto& t : cat.items()) printf("%s\t%s\t%s\n", t.get_str("name", "?").c_str(),
                                                t.get_str("description", "").c_str(),
                                                t.get_str("entry", "").c_str());
            printf("tasks=%zu\n", cat.items().size());
        }
        return 0;
    }
    if (cmd == "get-connect") {
        std::string id = resolve(cli_arg(args, 1));
        for (auto& e : execs) {
            if (!id.empty() && e.id != id) continue;
            printf("%s\t%s\n", e.id.c_str(), mj::Value::dump(e.config.has("connect")
                   ? e.config["connect"] : mj::Value::object()).c_str());
        }
        return 0;
    }
    if (cmd == "set-connect") {
        std::string id = resolve(cli_arg(args, 1));
        if (id.empty()) { printf("executor not found\n"); return 1; }
        std::string raw = cli_read_blob(cli_arg(args, 2, "{}"));
        mj::Value patch;
        if (!mj::try_parse(raw, patch) || !patch.is_object()) {
            printf("bad json: %s\n", raw.c_str()); return 1;
        }
        if (!hub.update_executor_connect(id, patch)) { printf("set failed\n"); return 1; }
        auto execs2 = hub.executors();
        for (auto& e : execs2) if (e.id == id) printf("%s\t%s\n", e.id.c_str(),
              mj::Value::dump(e.config.has("connect") ? e.config["connect"] : mj::Value::object()).c_str());
        return 0;
    }
    if (cmd == "accounts") {
        for (auto& a : hub.accounts().list()) {
            printf("%s\t%s\t%s\tenabled=%s\ts=%s\tid=%s\n", a.id.c_str(), a.game.c_str(),
                   a.label.c_str(), a.enabled ? "1" : "0", a.server.c_str(), a.identifier.c_str());
        }
        return 0;
    }
    if (cmd == "add-account") {
        std::string game = cli_arg(args, 1, "maa");
        std::string label = cli_arg(args, 2, game);
        std::string ident = cli_arg(args, 3, "");
        std::string match = cli_arg(args, 4, game == "maa" ? "" : "LastFourDigits");
        std::string server = cli_arg(args, 5, "Official");
        std::string notes = cli_arg(args, 6, "");
        if (ident.empty()) { printf("identifier required\n"); return 1; }
        Account a = hub.accounts().add(label, game, ident, match, server, notes);
        printf("added %s\n", a.id.c_str());
        return 0;
    }
    if (cmd == "use-account") {
        std::string game = cli_arg(args, 1, "maa");
        std::string id = cli_arg(args, 2, "");
        printf("set_active=%s\n", hub.accounts().set_active(game, id) ? "1" : "0");
        return 0;
    }
    if (cmd == "link") {
        std::string id = resolve(cli_arg(args, 1));
        mj::Value patch = mj::Value::object();
        patch.set("account_id", cli_arg(args, 2, ""));
        if (id.empty() || !hub.update_executor(id, patch)) { printf("link failed\n"); return 1; }
        printf("linked %s\n", id.c_str());
        return 0;
    }

    if (cmd == "import") {
        // 用法：--cli import <kind:auto|maa|maaend> <file-or-install-dir>
        // 无 GUI 的导入入口；CLI 模式没有内存任务列表，task_lists 可直接落盘
        // 注意：放在公共 id 解析之前 —— import 不需要预先存在执行器
        std::string kind_hint = cli_arg(args, 1, "auto");
        std::string file = cli_arg(args, 2);
        if (file.empty()) {
            printf("usage: MaaHub --cli import <kind:auto|maa|maaend> <file-or-install-dir>\n");
            return 2;
        }
        mj::Value rep = import_config::import_from_path(file, hub, kind_hint);
        hub.flush_task_params();   // 批量写入绕过 800ms 节流，防 CLI 退出丢尾
        std::string msg = import_config::report_message(rep);
        printf("%s\n", msg.c_str());
        printf("report=%s\n", mj::Value::dump_pretty(rep).c_str());

        // CLI 口径：task_lists 直接写 state/task-lists.json（与 GUI 保存格式一致）
        if (auto* tl = rep.find("task_lists"); tl && tl->is_object() && !tl->members().empty()) {
            std::string state_dir = util::exe_dir() + "\\state";
            util::ensure_dir(state_dir);
            std::string tl_path = state_dir + "\\task-lists.json";
            mj::Value disk;
            std::string old;
            if (util::read_file(tl_path, old) && !old.empty()) mj::try_parse(old, disk);
            for (auto& kv : tl->members()) {
                std::set<std::string> merged;
                if (auto* cur = disk.find(kv.first); cur && cur->is_array()) {
                    for (auto& x : cur->items()) if (x.is_string()) merged.insert(x.as_string());
                }
                for (auto& x : kv.second.items()) if (x.is_string()) merged.insert(x.as_string());
                mj::Value arr = mj::Value::array();
                for (auto& s : merged) arr.push(s);
                disk[kv.first] = arr;
            }
            if (util::write_file(tl_path, mj::Value::dump_pretty(disk))) {
                printf("task-lists=%s\n", tl_path.c_str());
            }
        }

        // schedules：合并进 state/schedules.json（按 exec_id+source 去重，保留 last_run_date）
        if (auto* sch = rep.find("schedules"); sch && sch->is_array() && !sch->items().empty()) {
            hub.merge_schedules(*sch);
            printf("schedules-merged=%zu state=%s\\state\\schedules.json\n",
                   sch->items().size(), util::exe_dir().c_str());
        }
        return rep.find("errors") && !rep.find("errors")->items().empty() ? 1 : 0;
    }

    // 调度命令（放在公共 id 解析之前——不针对单个执行器）
    if (cmd == "sched-list") {
        mj::Value snap = hub.schedules_snapshot();
        auto* entries = snap.find("entries");
        if (!entries || !entries->is_array()) { printf("schedules=0\n"); return 0; }
        printf("schedules=%zu\n", entries->items().size());
        for (auto& e : entries->items()) {
            std::string when;
            if (e.get_str("kind", "") == "maaend") {
                mj::Value wd;
                std::string days;
                if (auto* w = e.find("weekdays"); w && w->is_array())
                    for (auto& d : w->items()) {
                        if (!days.empty()) days += ",";
                        days += std::to_string(d.as_int(-1));
                    }
                std::string ts;
                if (auto* t = e.find("times"); t && t->is_array())
                    for (auto& x : t->items()) {
                        if (!ts.empty()) ts += ",";
                        ts += x.as_string();
                    }
                when = "wd=[" + days + "] times=[" + ts + "]";
            } else {
                when = "daily " + std::to_string(e.get_int("hour", -1)) + ":"
                     + std::to_string(e.get_int("min", -1));
            }
            printf("%s\t%s\t%s\tenabled=%s\t%s\tlast=%s\n",
                   e.get_str("exec_id", "?").c_str(), e.get_str("kind", "?").c_str(),
                   e.get_str("label", "").c_str(),
                   e.get_bool("enabled", false) ? "1" : "0", when.c_str(),
                   e.get_str("last_run_date", "").c_str());
        }
        return 0;
    }
    if (cmd == "sched-run") {
        // 手动触发一条：--cli sched-run <exec_id> [<source>]；source 省略=该 exec 第一条
        std::string target = cli_arg(args, 1);
        std::string source = cli_arg(args, 2);
        mj::Value snap = hub.schedules_snapshot();
        auto* entries = snap.find("entries");
        if (!entries || !entries->is_array()) { printf("no schedules\n"); return 1; }
        const mj::Value* found = nullptr;
        for (auto& e : entries->items()) {
            if (e.get_str("exec_id", "") != target) continue;
            if (!source.empty() && e.get_str("source", "") != source) continue;
            found = &e;
            break;
        }
        if (!found) { printf("schedule not found: %s\n", target.c_str()); return 1; }
        // 读该 exec 的任务串（一键长草选择）
        mj::Value lists = mj::Value::object();
        std::string content;
        if (util::read_file(util::exe_dir() + "\\state\\task-lists.json", content))
            mj::try_parse(content, lists);
        auto* lst = lists.find(target);
        std::vector<std::string> tasks;
        if (lst && lst->is_array())
            for (auto& t : lst->items()) if (t.is_string() && !t.as_string().empty())
                tasks.push_back(t.as_string());
        if (tasks.empty()) { printf("no task list for %s (set in GUI or import)\n", target.c_str()); return 1; }
        printf("sched-run exec=%s tasks=%zu\n", target.c_str(), tasks.size());
        std::string result = hub.run_task_sequence(target, tasks);
        printf("SCHED %s\n", result.c_str());
        return result == "done" ? 0 : 1;
    }
    if (cmd == "sched-tick") {
        // 手动跑一次调度扫描（不等待 10s tick）；返回触发的条数
        int fired = hub.scheduler_tick();
        printf("sched-tick fired=%d\n", fired);
        return 0;
    }
    if (cmd == "serve") {
        // 常驻：调度器线程已在 Hub 构造时启动；这里只是不让进程退出。
        // Ctrl+C 或关机时 Hub 析构会 stopping_ + 回收调度线程。
        printf("serve: scheduler running (10s tick). Ctrl+C to exit.\n");
        fflush(stdout);
        while (true) Sleep(3600000);
    }

    std::string id = resolve(cli_arg(args, 1));
    if (id.empty()) { printf("executor not found: %s\n", cli_arg(args, 1).c_str()); return 1; }

    if (cmd == "connect") {
        bool ok = hub.connect_executor(id);
        printf("connect=%s\n", ok ? "1" : "0");
        show_status(id);
        if (!ok) {
            // 连接失败时把整份状态吐出来：load 失败原因分散在 available/error/fw_log 里
            std::string raw = mj::Value::dump(hub.executor_status(id, 5.0));
            if (raw.size() > 900) raw = raw.substr(0, 900);
            printf("  state=%s\n", raw.c_str());
        }
        return ok ? 0 : 1;
    }
    if (cmd == "disconnect") {
        hub.disconnect_executor(id);
        show_status(id);
        return 0;
    }
    if (cmd == "stop") {
        bool ok = hub.stop_executor(id);
        printf("stop=%s\n", ok ? "1" : "0");
        show_status(id);
        return 0;
    }
    if (cmd == "events") {
        // 上面已把 args[1] 当执行器解析过，条数只能取 args[2]
        int n = atoi(cli_arg(args, 2, "50").c_str());
        print_events(n > 0 ? n : 50, id);
        return 0;
    }
    if (cmd == "override") {
        // 只映射不执行：打印 {"entry":...,"override":{...}}。
        // 任务名给 "*" 时按 catalog 顺序全量输出（每行 "任务名<TAB>json"），
        // 本地没有游戏窗口也能把 MaaEnd 全部任务的 entry 映射与 override 构造过一遍。
        std::string task = cli_arg(args, 2);
        if (task.empty()) { printf("task name required\n"); return 1; }
        std::string raw = cli_read_blob(cli_arg(args, 3, "{}"));
        mj::Value params;
        if (!mj::try_parse(raw, params)) params = mj::Value::object();
        if (task != "*") {
            mj::Value plan = hub.probe_override(id, task, params);
            if (!plan.is_object() || !plan.has("entry")) {
                printf("override-unsupported exec=%s task=%s\n", id.c_str(), task.c_str());
                return 1;
            }
            printf("%s\n", mj::Value::dump(plan).c_str());
            return 0;
        }
        hub.refresh_catalogs();
        mj::Value cat = hub.executor_catalog(id, 0.0);
        if (!cat.is_array()) { printf("catalog empty\n"); return 1; }
        for (auto& t : cat.items()) {
            std::string n = t.get_str("name", "");
            mj::Value one = mj::Value::object();
            const mj::Value* u = params.is_object() ? params.find(n) : nullptr;
            if (u) one = *u;
            mj::Value plan = hub.probe_override(id, n, one);
            printf("%s\t%s\n", n.c_str(), mj::Value::dump(plan).c_str());
        }
        return 0;
    }
    if (cmd == "run") {
        std::string task = cli_arg(args, 2);
        if (task.empty()) { printf("task name required\n"); return 1; }
        std::string raw = cli_read_blob(cli_arg(args, 3, "{}"));
        mj::Value params;
        if (!mj::try_parse(raw, params)) params = mj::Value::object();
        int timeout = cli_flag_int(args, "--timeout", 3600);
        if (!hub.connect_executor(id)) {
            printf("connect-failed\n");
            show_status(id);
            return 1;
        }
        double t0 = util::now_sec();
        int64_t tid = hub.run_on_executor(id, task, params);
        if (tid <= 0) {
            printf("run-rejected task=%s\n", task.c_str());
            show_status(id);
            return 1;
        }
        printf("started task=%s id=%lld\n", task.c_str(), (long long)tid);
        int idle_rounds = 0;
        std::string run_state = "running";
        while (util::now_sec() - t0 < timeout) {
            Sleep(1000);
            mj::Value st = hub.executor_status(id, 0.0);
            run_state = st.get_str("run", "idle");
            if (run_state != "running") { if (++idle_rounds >= 2) break; }
            else idle_rounds = 0;
        }
        double el = util::now_sec() - t0;
        mj::Value st = hub.executor_status(id, 0.0);
        std::string err = st.get_str("last_error", "");
        printf("finished task=%s state=%s elapsed=%.0fs%s\n", task.c_str(), run_state.c_str(), el,
               err.empty() ? "" : (" error=" + err).c_str());
        if (!err.empty() && err.size() > 300) err = err.substr(0, 300);
        return run_state == "failed" ? 1 : 0;
    }
    if (cmd == "seq") {
        std::string id = resolve(cli_arg(args, 1));
        if (id.empty()) { printf("executor not found: %s\n", cli_arg(args, 1).c_str()); return 1; }
        std::string list = cli_arg(args, 2);
        std::vector<std::string> tasks;
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string t = list.substr(pos, comma == std::string::npos
                                            ? std::string::npos : comma - pos);
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
            if (!t.empty()) tasks.push_back(t);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (tasks.empty()) { printf("task list required\n"); return 1; }
        int timeout = cli_flag_int(args, "--timeout", 5400);
        printf("seq exec=%s tasks=%zu\n", id.c_str(), tasks.size());
        for (size_t k = 0; k < tasks.size(); ++k) printf("  %zu. %s\n", k + 1, tasks[k].c_str());
        fflush(stdout);
        std::string result;
        std::atomic<bool> finished{ false };
        std::thread worker([&] { result = hub.run_task_sequence(id, tasks); finished = true; });
        double t0 = util::now_sec();
        double last_print = 0;
        while (!finished && util::now_sec() - t0 < timeout) {
            Sleep(2000);
            if (util::now_sec() - t0 - last_print < 10) continue;
            last_print = util::now_sec() - t0;
            mj::Value st = hub.executor_status(id, 0.0);
            printf("t=%.0fs run=%s task=%s conn=%s%s\n", util::now_sec() - t0,
                   st.get_str("run", "-").c_str(), hub.running_task_of(id).c_str(),
                   st.get_str("conn", "?").c_str(),
                   st.get_str("last_error", "").empty() ? "" : " ERR");
            fflush(stdout);
        }
        bool timed_out = !finished;
        if (timed_out) {
            hub.stop_execution();
            hub.stop_executor(id);
        }
        worker.join();
        double el = util::now_sec() - t0;
        if (timed_out) {
            printf("TIMEOUT after %ds\n", timeout);
            result = "timeout";
        }
        printf("SEQ %s elapsed=%.0fs", result.c_str(), el);
        if (result != "done") {
            // 计划里的 error 是失败瞬间抓的，节点级细节要再拉一次 status 才补得上
            mj::Value ls = hub.executor_status(id, 5.0);
            std::string le = ls.get_str("last_error", "");
            if (le.size() > 600) le = le.substr(0, 600);
            if (!le.empty()) printf(" live_err=%s", le.c_str());
            mj::Value ps = hub.plans(1);
            if (ps.is_array() && !ps.items().empty()) {
                std::string pe = ps.items().back().get_str("error", "");
                if (pe.size() > 300) pe = pe.substr(0, 300);
                if (!pe.empty()) printf(" error=%s", pe.c_str());
            }
        }
        printf("\n");
        print_events(200, id);
        return result == "done" ? 0 : 1;
    }
    printf("unknown cli cmd: %s\n", cmd.c_str());
    return 2;
}

int main(int argc, char** argv) {
    // GDI+ 初始化（抗锯齿圆角/线条需要；gui.hpp 的 round_rect 走 GDI+）
    Gdiplus::GdiplusStartupInput gpsi;
    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartup(&gdiplus_token, &gpsi, nullptr);

    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate([] {
        crash_log("std::terminate (uncaught exception in thread)");
        abort();
    });

    std::string config_path;
    bool check_only = false;
    bool self_test = false;
    bool startup_bench = false;
    bool offscreen = false;
    bool show_add_task = false;
    bool show_advanced = false;
    bool show_ctx_menu = false;
    bool show_new_exec = false;
    std::string ctx_menu_task;
    bool dump_render = false;
    bool settings_dump = false;
    std::string dump_render_path;
    std::string dump_page;
    std::string dump_task;
    int dump_w = 0, dump_h = 0;   // --dump-size WxH：覆盖离屏画布客户区尺寸
    bool dump_update_demo = false; // --dump-update-demo：注入假更新结果核对面板排版
    bool cli_mode = false;
    std::vector<std::string> cli_args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check") check_only = true;
        else if (a == "--settings-dump") settings_dump = true;
        else if (a == "--self-test") self_test = true;
        else if (a == "--startup-bench") startup_bench = true;
        else if (a == "--offscreen") offscreen = true;
        else if (a == "--show-add-task") show_add_task = true;
        else if (a == "--advanced") show_advanced = true;
        else if (a == "--show-new-exec") show_new_exec = true;
        else if (a == "--show-ctx-menu" && i + 1 < argc) {
            show_ctx_menu = true;
            ctx_menu_task = argv[++i];
        }
        else if (a == "--dump-render" && i + 1 < argc) {
            dump_render = true;
            dump_render_path = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') dump_page = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') dump_task = argv[++i];
        }
        else if (a == "--dump-size" && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &dump_w, &dump_h);
        }
        else if (a == "--dump-update-demo") {
            dump_update_demo = true;   // 注入一条假更新结果，核对「更新检查」面板排版
        }
        else if (a == "--cli") {
            for (int j = i + 1; j < argc; ++j) cli_args.emplace_back(argv[j]);
            cli_mode = true;
            i = argc;
        }
        else if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--port" && i + 1 < argc) g_port_override = atoi(argv[++i]);
        else if (a == "--core-version" && i + 2 < argc) {
            return core_version_mode(argv[i + 1], argv[i + 2]);
        } else if (a == "--import-core" && i + 2 < argc) {
            // 一次性迁移：把系统已有安装复制进 MaaHub 内部 cores 目录
            std::string kind = argv[++i];
            std::string path = argv[++i];
            std::string state = util::exe_dir() + "\\state";
            util::ensure_dir(state);
            CoreManager cm(state, CoreManager::default_cores_root());
            std::string id = cm.import_core(kind, path);
            if (id.empty()) {
                printf("IMPORT FAILED: %s 不是有效的 %s 核心目录\n",
                       path.c_str(), kind.c_str());
                return 1;
            }
            CoreInfo info;
            cm.get(id, info);
            printf("IMPORTED %s -> %s (version=%s)\n",
                   path.c_str(), info.path.c_str(), info.version.c_str());
            return 0;
        } else if (a == "--worker" && i + 2 < argc) {
            std::string kind = argv[++i];
            std::string cfg_b64 = argv[++i];
            std::string cfg = util::base64_decode(cfg_b64);
            mj::Value cfgv;
            if (!mj::try_parse(cfg, cfgv)) {
                fprintf(stderr, "[worker] bad config json (decoded): %s\n", cfg.c_str());
                printf("{\"error\":\"bad worker config json\"}\n");
                return 2;
            }
            return worker_main(kind, cfgv);
        }
    }

    mj::Value config = load_config(config_path);

    if (cli_mode) {
        util::trace("cli begin cmd=" + (cli_args.empty() ? std::string() : cli_args[0]));
        int rc = run_cli(cli_args, config);
        util::trace("cli end rc=" + std::to_string(rc));
        return rc;
    }

    // ---------------- 启动性能基准（纯无头，不创建窗口） ----------------
    if (startup_bench) {
        double t0 = util::now_sec();
        Hub hub(config);
        hub.ensure_default_executors();
        mj::Value fast = hub.snapshot_fast();     // 首帧：只读持久化
        double t1 = util::now_sec();
        hub.start_all();                          // 并行拉起 worker + 加载核心
        double t2 = util::now_sec();
        hub.refresh_catalogs();
        mj::Value full = hub.snapshot();          // 完整快照
        double t3 = util::now_sec();
        int n_exec = 0;
        if (auto* e = fast.find("executors"); e && e->is_array()) {
            n_exec = (int)e->items().size();
        }
        printf("[startup] first_paint=%.0fms start_all=%.0fms full_ready=%.0fms executors=%d\n",
               (t1 - t0) * 1000, (t2 - t1) * 1000, (t3 - t0) * 1000, n_exec);
        (void)full;
        double fp = (t1 - t0) * 1000;
        printf(fp < 200 ? "BENCH PASS\n" : "BENCH FAIL\n");
        return fp < 200 ? 0 : 1;
    }

    // ---------------- 离屏渲染转储（无可见窗口，输出 PNG 供验证） ----------------
    if (dump_render) {
        // 进程内直接构造窗口对象但不创建 HWND：调用其页面绘制到内存 DC。
        // 需要 app_window 暴露一个纯渲染入口，这里通过 offscreen 窗口 + 内存 DC 完成。
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        MaaHubWindow win(hInst, config, /*offscreen=*/true);
        if (dump_w > 0 && dump_h > 0) win.debug_set_client_size(dump_w, dump_h);
        if (!win.create()) {
            printf("DUMP FAILED: create\n");
            return 1;
        }
        std::string page = dump_page.empty() ? "executor" : dump_page;
        // 等待**所有**执行器的核心加载与目录刷新完成（否则任务列表为空）
        for (int i = 0; i < 120; ++i) {
            Sleep(100);
            bool all_ready = true;
            for (auto& e : win.debug_executors()) {
                mj::Value cat = win.debug_catalog(e.id, /*ttl=*/0);
                if (!(cat.is_array() && !cat.items().empty())) { all_ready = false; break; }
            }
            if (all_ready) break;
        }
        win.debug_refresh_snapshot();   // 强制拉一次完整快照（含 catalog）
        if (dump_update_demo) {
            UpdateInfo ui;
            ui.ok = true;
            ui.kind = "maaend";
            ui.current = "v5.15.0-beta.1";
            ui.current_tag = "";
            ui.latest = "v3.0.0";
            ui.latest_tag = "v3.0.0";
            ui.source_name = "GitHub";
            ui.has_update = true;
            ui.tag_mismatch = true;
            ui.asset_name = "MaaEnd-win-x86_64-v3.0.0.zip";
            ui.asset_size = 315946882LL;
            ui.asset_sha256 = "sha256:36cd998fadd361ed82a3917a5b618674c192ae7ca02a2174136fc1f69313ce4d";
            ui.download_url = "https://example.invalid/core.zip";
            win.debug_set_update_info(ui);
        }
        if (!dump_task.empty()) win.debug_select_task(dump_task);
        if (show_advanced) win.debug_show_advanced();
        if (show_add_task) win.debug_open_add_task();
        if (show_ctx_menu) win.debug_open_ctx_menu(ctx_menu_task);
        if (show_new_exec) win.debug_open_new_exec();
        Sleep(200);   // 等面板切换过渡（PANEL_ANIM_MS=140）落定，否则截到滑入中途
        win.render_to_file(page, dump_render_path);
        return 0;
    }

    // ---------------- 干跑自检（控制台） ----------------
    if (settings_dump) {
        // 加载 settings.json -> 规范化输出（验证 from_json/to_json 往返）
        std::string sdir = util::exe_dir() + "\\state";
        hub::SettingsStore store(sdir);
        hub::AppSettings st = store.get();
        printf("%s\n", mj::Value::dump_pretty(st.to_json()).c_str());
        return 0;
    }

    if (check_only) {
        Hub hub(config);
        printf("== MaaHub 自检 ==\n");
        hub.ensure_default_executors();
        hub.start_all();
        hub.refresh_catalogs();
        auto execs = hub.executors();
        printf("执行器数量: %zu\n", execs.size());
        bool ok = !execs.empty();
        for (auto& e : execs) {
            mj::Value st = hub.executor_status(e.id);
            std::string ver = st.get_str("version", "");
            std::string err = st.get_str("last_error", "");
            int64_t tasks = st.get_int("task_count", 0);
            printf("  [%s] %s: version=%s tasks=%lld conn=%s %s\n",
                   e.kind.c_str(), e.label.c_str(), ver.empty() ? "-" : ver.c_str(),
                   (long long)tasks, st.get_str("conn", "unknown").c_str(),
                   err.empty() ? "" : ("err=" + err).c_str());
            if (ver.empty()) ok = false;
        }
        printf(ok ? "ALL OK\n" : "CHECK FAILED\n");
        return ok ? 0 : 1;
    }

    // ---------------- 自检模式（无窗口，验证核心功能） ----------------
    if (self_test) {
        int fails = 0;
        auto check = [&](const char* name, bool cond, const std::string& extra = "") {
            printf("  [%s] %s%s\n", cond ? "OK" : "FAIL", name,
                   extra.empty() ? "" : (" — " + extra).c_str());
            if (!cond) ++fails;
        };

        printf("== MaaHub 自检（无窗口） ==\n");

        // 1. 设置读写
        SettingsStore ss(util::exe_dir() + "\\state");
        ss.update([](AppSettings& s) {
            s.proxy_mode = "socks5";
            s.proxy_host = "127.0.0.1";
            s.proxy_port = 7890;
            s.mirrorchyan_cdk = "TESTCDK";
        });
        AppSettings got = ss.get();
        check("设置保存/读取", got.proxy_mode == "socks5" && got.proxy_port == 7890
                                && got.mirrorchyan_cdk == "TESTCDK",
              got.proxy_mode + " " + std::to_string(got.proxy_port));
        check("SOCKS5 代理串", got.winhttp_proxy() == L"socks=127.0.0.1:7890",
              util::wide_to_utf8(got.winhttp_proxy()));
        ss.update([](AppSettings& s) {
            s.proxy_mode = "http";
            s.proxy_host = "127.0.0.1";
            s.proxy_port = 7890;
        });
        check("HTTP 代理串", ss.get().winhttp_proxy() == L"http=127.0.0.1:7890");
        ss.update([](AppSettings& s) {
            s.proxy_mode = "none";
            s.proxy_host.clear();
            s.proxy_port = 0;
            s.mirrorchyan_cdk.clear();
        });

        // 2. Hub + 执行器
        Hub hub(config);
        hub.ensure_default_executors();
        hub.start_all();
        auto execs = hub.executors();
        check("默认执行器创建", !execs.empty(), std::to_string(execs.size()) + " 个");
        bool all_loaded = true;
        for (auto& e : execs) {
            mj::Value st = hub.executor_status(e.id);
            if (st.get_str("version", "").empty()) all_loaded = false;
        }
        check("全部执行器加载核心", all_loaded);

        // 3. catalog 缓存命中（第二次调用应明显更快）
        if (!execs.empty()) {
            std::string id = execs[0].id;
            double t0 = util::now_sec();
            mj::Value c1 = hub.executor_catalog(id, 0);   // 强制刷新
            double t1 = util::now_sec();
            mj::Value c2 = hub.executor_catalog(id, 30);  // 命中缓存
            double t2 = util::now_sec();
            check("catalog 缓存命中", (t2 - t1) < (t1 - t0) + 0.05,
                  "fresh=" + std::to_string((int)((t1 - t0) * 1000)) + "ms cached="
                  + std::to_string((int)((t2 - t1) * 1000)) + "ms");
            check("catalog 非空", c1.is_array() && !c1.items().empty(),
                  std::to_string(c1.items().size()) + " tasks");
        }

        // 4. 账号（自检直接跑在真实 state 上，不能假设列表为空）
        Account acc;
        Account made = hub.accounts().add("自检账号", execs.empty() ? "maa" : execs[0].kind,
                           "1234", "LastFourDigits", "Official", "");
        check("账号创建", !made.id.empty() && hub.accounts().get(made.id, acc)
                           && acc.identifier == "1234");
        hub.accounts().mark_run(made.id);
        hub.accounts().mark_daily_done(made.id);
        check("账号状态更新",
              hub.accounts().get(made.id, acc)
              && acc.total_runs >= 1 && !acc.daily_done_date.empty());
        check("账号删除", hub.accounts().remove(made.id) && !hub.accounts().get(made.id, acc));

        // 4b. 执行器增/改/删（曾因持 mtx_ 时递归加锁写文件直接把进程带走）
        ExecutorEntry tmp;
        std::string tmp_id = hub.create_executor(execs.empty() ? "maa" : execs[0].kind, "自检执行器");
        mj::Value link_patch = mj::Value::object();
        link_patch.set("account_id", made.id);
        check("执行器创建", !tmp_id.empty());
        check("执行器关联账号", !tmp_id.empty() && hub.update_executor(tmp_id, link_patch)
                                && hub.get_executor(tmp_id, tmp) && tmp.account_id == made.id);
        check("执行器删除", !tmp_id.empty() && hub.remove_executor(tmp_id)
                                && !hub.get_executor(tmp_id, tmp));

        // 5. 快照性能（连续 20 次）
        double ts = util::now_sec();
        for (int i = 0; i < 20; ++i) { mj::Value s = hub.snapshot(); (void)s; }
        double per = (util::now_sec() - ts) / 20.0 * 1000.0;
        check("快照平均耗时 < 50ms", per < 50, std::to_string((int)per) + "ms");

        // 6. 规划器（启发式，未连接应只有 warnings）
        mj::Value plan = hub.propose_plan("自检目标", "heuristic");
        check("启发式规划", plan.get_str("planner", "") == "heuristic"
                            && plan.find("steps") != nullptr);
        check("计划历史记录", hub.plans(10).is_array()
                            && !hub.plans(10).items().empty());

        printf(fails == 0 ? "ALL OK\n" : "SELF-TEST FAILED (%d)\n", fails);
        return fails == 0 ? 0 : 1;
    }

    // ---------------- GUI 模式（原生窗口） ----------------
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    MaaHubWindow win(hInst, config, offscreen);
    if (!win.create()) {
        MessageBoxW(nullptr, L"窗口创建失败", L"MaaHub", MB_ICONERROR);
        return 1;
    }
    return win.run();
}
