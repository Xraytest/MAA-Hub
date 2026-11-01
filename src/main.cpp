// MaaHub C++ 入口（原生窗口版）
//   MaaHub.exe                     GUI 模式：原生 Win32 窗口（无任何网页特性）
//   MaaHub.exe --check             干跑自检（控制台输出，不启动 UI）
//   MaaHub.exe --worker K C        worker 子进程模式（内部使用）
//   MaaHub.exe --core-version K D  探测某核心目录的版本（子进程，内部使用）
#include <cstdio>
#include <string>

#include "app_window.hpp"
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
static int core_version_mode(const std::string& kind, const std::string& path) {
    HMODULE h = nullptr;
    if (kind == "maa") {
        std::wstring dll = util::utf8_to_wide(path + "\\MaaCore.dll");
        h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) { printf(""); return 1; }
        using Fn = const char* (*)();
        auto f = (Fn)GetProcAddress(h, "AsstGetVersion");
        if (!f) { FreeLibrary(h); printf(""); return 1; }
        printf("%s", f());
        FreeLibrary(h);
        return 0;
    }
    if (kind == "maaend") {
        std::wstring dll = util::utf8_to_wide(path + "\\maafw\\MaaFramework.dll");
        h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) { printf(""); return 1; }
        using Fn = const char* (*)();
        auto f = (Fn)GetProcAddress(h, "MaaVersion");
        if (!f) { FreeLibrary(h); printf(""); return 1; }
        printf("%s", f());
        FreeLibrary(h);
        return 0;
    }
    return 2;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate([] {
        crash_log("std::terminate (uncaught exception in thread)");
        abort();
    });

    std::string config_path;
    bool check_only = false;
    bool self_test = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check") check_only = true;
        else if (a == "--self-test") self_test = true;
        else if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--port" && i + 1 < argc) g_port_override = atoi(argv[++i]);
        else if (a == "--core-version" && i + 2 < argc) {
            return core_version_mode(argv[i + 1], argv[i + 2]);
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

    // ---------------- 干跑自检（控制台） ----------------
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

        // 4. 账号
        hub.accounts().add("自检账号", execs.empty() ? "maa" : execs[0].kind,
                           "1234", "LastFourDigits", "Official", "");
        check("账号创建", hub.accounts().list().size() == 1);
        hub.accounts().mark_run(hub.accounts().list()[0].id);
        hub.accounts().mark_daily_done(hub.accounts().list()[0].id);
        Account acc;
        check("账号状态更新",
              hub.accounts().get(hub.accounts().list()[0].id, acc)
              && acc.total_runs >= 1 && !acc.daily_done_date.empty());
        hub.accounts().remove(hub.accounts().list()[0].id);

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
    MaaHubWindow win(hInst, config);
    if (!win.create()) {
        MessageBoxW(nullptr, L"窗口创建失败", L"MaaHub", MB_ICONERROR);
        return 1;
    }
    return win.run();
}
