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

static mj::Value status_to_json(const AppStatus& s) {
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
    ctrl.set("mode", s.ctrl_mode.empty() ? "win32" : s.ctrl_mode);
    ctrl.set("hwnd", s.ctrl_hwnd ? mj::Value(s.ctrl_hwnd) : mj::Value(nullptr));
    ctrl.set("screencap_method", 32);
    ctrl.set("mouse_method", 1);
    ctrl.set("keyboard_method", 1);
    ctrl.set("adb_path", "");
    ctrl.set("address", "");
    ctrl.set("config", "General");
    mj::Value res = mj::Value::array();
    res.push(mj::Value(s.res_w));
    res.push(mj::Value(s.res_h));
    ctrl.set("resolution", std::move(res));
    ctrl.set("uuid", s.uuid);
    v.set("controller", std::move(ctrl));
    v.set("resource_hash", s.resource_hash);
    v.set("task_count", s.task_count);
    return v;
}

int worker_main(const std::string& kind, const mj::Value& cfg) {
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
                reply(true, status_to_json(adapter->status()));
            } else if (cmd == "load") {
                bool ok = adapter->load();
                reply(ok, status_to_json(adapter->status()));
            } else if (cmd == "connect") {
                bool ok = adapter->connect();
                reply(ok, status_to_json(adapter->status()));
            } else if (cmd == "disconnect") {
                adapter->disconnect();
                reply(true, status_to_json(adapter->status()));
            } else if (cmd == "run") {
                int64_t tid = 0;
                bool ok = adapter->run_task(req.get_str("task", ""), req["params"], tid);
                mj::Value r = mj::Value::object();
                r.set("task_id", tid);
                r.set("app", status_to_json(adapter->status()));
                reply(ok, std::move(r));
            } else if (cmd == "stop") {
                reply(adapter->stop(), status_to_json(adapter->status()));
            } else if (cmd == "status") {
                reply(true, status_to_json(adapter->status()));
            } else if (cmd == "is_connected") {
                reply(adapter->is_connected(), mj::Value::object());
            } else if (cmd == "tasks") {
                mj::Value arr = mj::Value::array();
                for (auto& t : adapter->task_catalog()) arr.push(t);
                reply(true, std::move(arr));
            } else if (cmd == "unload" || cmd == "exit") {
                adapter->unload();
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
