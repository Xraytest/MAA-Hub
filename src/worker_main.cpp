// MaaHub worker 子进程入口
// 协议（stdin/stdout 每行一个 JSON）：
//   命令:  {"cmd": "discover"|"load"|"connect"|"disconnect"|"run"|"stop"|"status"|"tasks"|"unload"|"exit", "id": N, ...}
//   响应:  {"id": N, "ok": bool, "result": ...}
//   事件:  {"event": {...}}（异步推送）
//
// 与 Python 版 worker_main.py 协议完全一致，可互相替换。
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "adapters/adapter.hpp"
#include "adapters/maa_adapter.hpp"
#include "adapters/maaend_adapter.hpp"
#include "json.hpp"
#include "util.hpp"
#include "worker_main.hpp"

namespace hub {


static std::unique_ptr<IAdapter> make_adapter(const std::string& kind, const mj::Value& cfg) {
    if (kind == "maa") return std::make_unique<MaaAdapter>();
    if (kind == "maaend") return std::make_unique<MaaEndAdapter>();
    return nullptr;
}

static mj::Value status_to_json(const AppStatus& s, const mj::Value& cfg) {
    mj::Value v = mj::Value::object();
    v.set("kind", s.kind);
    v.set("name", s.name);
    v.set("install_dir", s.install_dir);
    v.set("version", s.version);
    v.set("core_dll", s.core_dll);
    v.set("available", s.available);
    v.set("error", s.error);
    v.set("conn", s.conn);
    v.set("run", s.run);
    v.set("last_error", s.last_error);
    mj::Value ctrl = mj::Value::object();
    std::string mode = cfg.get_path_str("connect.mode", "win32");
    ctrl.set("mode", s.ctrl_mode.empty() ? mode : s.ctrl_mode);
    ctrl.set("hwnd", s.ctrl_hwnd ? mj::Value(s.ctrl_hwnd) : mj::Value(nullptr));
    auto eff = [](int64_t resolved, const mj::Value& c, const char* path, int64_t def) {
        return resolved >= 0 ? resolved : c.get_path_int(path, def);
    };
    ctrl.set("screencap_method", eff(s.screencap_method, cfg, "connect.screencap_method", 32));
    ctrl.set("mouse_method", eff(s.mouse_method, cfg, "connect.mouse_method", 1));
    ctrl.set("keyboard_method", eff(s.keyboard_method, cfg, "connect.keyboard_method", 1));
    ctrl.set("touch_mode", cfg.get_path_str("connect.touch_mode", ""));
    ctrl.set("adb_path", cfg.get_path_str("adb.adb_path", ""));
    ctrl.set("address", cfg.get_path_str("adb.address", ""));
    ctrl.set("config", cfg.get_path_str("adb.config", "General"));
    mj::Value res = mj::Value::array();
    res.push(mj::Value(s.res_w));
    res.push(mj::Value(s.res_h));
    ctrl.set("resolution", std::move(res));
    ctrl.set("uuid", s.uuid);
    v.set("controller", std::move(ctrl));
    v.set("resource_hash", s.resource_hash);
    v.set("task_count", s.task_count);
    if (!s.log_state.empty()) v.set("fw_log", s.log_state);
    return v;
}

int worker_main(const std::string& kind, const mj::Value& cfg) {
    util::trace("worker start kind=" + kind);
    auto adapter = make_adapter(kind, cfg);
    if (!adapter) {
        printf("{\"error\":\"unknown kind %s\"}\n", kind.c_str());
        return 2;
    }

    auto emit = [](const mj::Value& v) {
        std::string line = mj::Value::dump(v);
        fwrite(line.c_str(), 1, line.size(), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    };

    // 就绪
    {
        mj::Value r = mj::Value::object();
        r.set("ready", true);
        r.set("kind", kind);
        emit(r);
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        mj::Value req;
        if (!mj::try_parse(line, req)) continue;
        std::string cmd = req.get_str("cmd", "");
        int64_t cid = req.get_int("id", 0);

        auto reply = [&](bool ok, mj::Value result) {
            mj::Value r = mj::Value::object();
            r.set("id", cid);
            r.set("ok", ok);
            r.set("result", std::move(result));
            emit(r);
        };

        try {
            if (cmd == "discover") {
                adapter->discover(cfg);
                reply(true, status_to_json(adapter->status(), cfg));
            } else if (cmd == "load") {
                bool ok = adapter->load();
                reply(ok, status_to_json(adapter->status(), cfg));
            } else if (cmd == "connect") {
                bool ok = adapter->connect();
                reply(ok, status_to_json(adapter->status(), cfg));
            } else if (cmd == "disconnect") {
                adapter->disconnect();
                reply(true, status_to_json(adapter->status(), cfg));
            } else if (cmd == "run") {
                int64_t tid = 0;
                bool ok = adapter->run_task(req.get_str("task", ""), req["params"], tid);
                mj::Value r = mj::Value::object();
                r.set("task_id", tid);
                r.set("app", status_to_json(adapter->status(), cfg));
                reply(ok, std::move(r));
            } else if (cmd == "stop") {
                reply(adapter->stop(), status_to_json(adapter->status(), cfg));
            } else if (cmd == "status") {
                reply(true, status_to_json(adapter->status(), cfg));
            } else if (cmd == "is_connected") {
                reply(adapter->is_connected(), mj::Value::object());
            } else if (cmd == "tasks") {
                mj::Value arr = mj::Value::array();
                for (auto& t : adapter->task_catalog()) arr.push(t);
                reply(true, std::move(arr));
            } else if (cmd == "override") {
                mj::Value out = mj::Value::object();
                bool ok = adapter->build_override(req.get_str("task", ""), req["params"], out);
                reply(ok, std::move(out));
            } else if (cmd == "unload" || cmd == "exit") {
                util::trace("worker unload begin cmd=" + cmd);
                adapter->unload();
                util::trace("worker unload end");
                reply(true, mj::Value::object());
                if (cmd == "exit") return 0;
            } else {
                mj::Value e = mj::Value::object();
                e.set("error", "unknown cmd " + cmd);
                reply(false, std::move(e));
            }
        } catch (const std::exception& e) {
            mj::Value er = mj::Value::object();
            er.set("error", e.what());
            reply(false, std::move(er));
        }

        // 推送事件
        for (auto& ev : adapter->drain_events()) {
            mj::Value v = mj::Value::object();
            mj::Value inner = mj::Value::object();
            inner.set("app", ev.app);
            inner.set("source", ev.source);
            inner.set("category", ev.category);
            inner.set("what", ev.what);
            inner.set("task", ev.task);
            inner.set("task_id", ev.task_id ? mj::Value(ev.task_id) : mj::Value(nullptr));
            inner.set("details", ev.details);
            inner.set("raw", ev.raw);
            v.set("event", std::move(inner));
            emit(v);
        }
    }
    return 0;
}

} // namespace hub
