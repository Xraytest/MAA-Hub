// Worker 客户端：主进程侧管理适配器 worker 子进程
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adapters/adapter.hpp"
#include "json.hpp"
#include "util.hpp"

namespace hub {

class WorkerClient {
public:
    using EventFn = std::function<void(const AdapterEvent&)>;

    // kind: worker 类型（maa | maaend），决定加载哪个核心
    // id:   执行器 id（仅用于日志）
    WorkerClient(std::string kind, std::string id, mj::Value cfg, EventFn on_event)
        : kind_(std::move(kind)), id_(std::move(id)),
          cfg_(std::move(cfg)), on_event_(std::move(on_event)) {}

    ~WorkerClient() { shutdown(); }

    bool start() {
        std::string exe = util::exe_dir() + "\\MaaHub.exe";
        std::wstring cmd = L"\"" + util::utf8_to_wide(exe) + L"\" --worker "
                           + util::utf8_to_wide(kind_) + L" "
                           + util::utf8_to_wide(util::base64_encode_str(mj::Value::dump(cfg_)));
        // 工作目录 = exe 所在目录的上级（项目根），与 Python 版一致
        std::wstring workdir = util::utf8_to_wide(util::parent_dir(util::exe_dir()));
        if (!util::spawn_process(cmd, workdir, L"", proc_)) {
            last_error_ = "worker spawn failed";
            return false;
        }
        reader_ = std::thread([this] { read_loop(); });
        // 等就绪
        for (int i = 0; i < 200; ++i) {
            if (ready_) return true;
            if (!proc_.alive()) {
                last_error_ = "worker exited early";
                return false;
            }
            Sleep(100);
        }
        last_error_ = "worker start timeout";
        return false;
    }

    // 同步调用；timeout_ms 内无响应返回 false
    bool call(const std::string& cmd, mj::Value params, int timeout_ms, mj::Value& out_result) {
        // 串行化所有调用：HTTP 线程与执行线程可能并发调用同一 worker，
        // 而内部 mtx_/cv_ 不支持重入。
        std::lock_guard<std::mutex> call_lk(call_mtx_);
        if (!proc_.alive()) {
            last_error_ = "worker not running";
            return false;
        }
        int64_t cid = 0;
        std::unique_lock<std::mutex> lk(mtx_);
        cid = ++seq_;
        pending_[cid] = mj::Value::object();
        mj::Value req = mj::Value::object();
        req.set("cmd", cmd);
        req.set("id", cid);
        for (auto& kv : params.members()) req.set(kv.first, kv.second);
        std::string line = mj::Value::dump(req) + "\n";
        {
            std::lock_guard<std::mutex> wlk(wmtx_);
            DWORD n = 0;
            if (!WriteFile(proc_.stdin_write, line.data(), (DWORD)line.size(), &n, nullptr)) {
                last_error_ = "write to worker failed";
                return false;
            }
        }
        bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            auto it = pending_.find(cid);
            return it != pending_.end() && it->second.get_bool("__done__", false);
        });
        auto it = pending_.find(cid);
        if (got && it != pending_.end()) {
            mj::Value resp = it->second;
            pending_.erase(it);
            lk.unlock();
            ok_last_ = resp.get_bool("ok", false);
            out_result = resp["result"];
            return ok_last_;
        }
        pending_.erase(cid);
        last_error_ = cmd + " timeout";
        return false;
    }

    bool alive() const { return proc_.alive(); }
    const std::string& last_error() const { return last_error_; }

    void shutdown() {
        if (reader_.joinable()) {
            // 尝试优雅退出
            mj::Value dummy;
            call("exit", mj::Value::object(), 3000, dummy);
            proc_.close();
            reader_.join();
        } else {
            proc_.close();
        }
    }

private:
    void read_loop() {
        std::string buf;
        char chunk[8192];
        while (true) {
            DWORD n = 0;
            BOOL ok = ReadFile(proc_.stdout_read, chunk, sizeof(chunk), &n, nullptr);
            if (!ok || n == 0) break;
            buf.append(chunk, n);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) handle_line(line);
            }
        }
    }

    void handle_line(const std::string& line) {
        mj::Value msg;
        if (!mj::try_parse(line, msg)) return;
        if (msg.get_bool("ready", false)) {
            ready_ = true;
            return;
        }
        if (msg.has("event")) {
            const mj::Value& e = msg["event"];
            AdapterEvent ev;
            ev.app = e.get_str("app", kind_);
            ev.source = e.get_str("source", "");
            ev.category = e.get_str("category", "info");
            ev.what = e.get_str("what", "");
            ev.task = e.get_str("task", "");
            if (auto* t = e.find("task_id")) ev.task_id = t->as_int();
            if (auto* d = e.find("details")) ev.details = *d;
            ev.raw = e.get_str("raw", "");
            if (on_event_) on_event_(ev);
            return;
        }
        if (msg.has("id")) {
            int64_t cid = msg.get_int("id", 0);
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = pending_.find(cid);
            if (it != pending_.end()) {
                it->second = msg;
                it->second.set("__done__", true);
                cv_.notify_all();
            }
        }
    }

    std::string kind_;
    std::string id_;
    mj::Value cfg_;
    EventFn on_event_;
    util::ProcHandle proc_;
    std::thread reader_;
    std::atomic<bool> ready_{ false };
    std::mutex mtx_;
    std::condition_variable cv_;
    std::mutex wmtx_;                // 保护 stdin 写
    std::mutex call_mtx_;            // 串行化 call()
    int64_t seq_ = 0;
    std::map<int64_t, mj::Value> pending_;
    bool ok_last_ = false;
    std::string last_error_;
};

} // namespace hub
