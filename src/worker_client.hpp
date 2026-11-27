// Worker 客户端：主进程侧管理适配器 worker 子进程
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

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
        std::string exe_d = util::exe_dir();
        std::string exe = exe_d + "\\MaaHub.exe";
        // 配置通过首条 stdin JSON 下发，避免命令行 32K 上限及在进程列表中暴露配置/账号信息
        std::wstring cmd = L"\"" + util::utf8_to_wide(exe) + L"\" --worker "
                           + util::utf8_to_wide(kind_) + L" --stdin-cfg";
        // 开发布局（build\MaaHub.exe）沿用项目根目录；安装/便携布局使用 exe 所在目录
        std::string parent_d = util::parent_dir(exe_d);
        std::string wd = util::dir_exists(parent_d + "\\src") ? parent_d : exe_d;
        std::wstring workdir = util::utf8_to_wide(wd);
        if (!util::spawn_process(cmd, workdir, L"", proc_)) {
            return false;
        }
        reader_exited_ = false;
        ready_ = false;
        reader_ = std::thread([this] { read_loop(); });

        // 首条消息发送配置 JSON
        std::string cfg_line = mj::Value::dump(cfg_) + "\n";
        bool sent_cfg = false;
        {
            std::lock_guard<std::mutex> wlk(wmtx_);
            sent_cfg = write_all(proc_.stdin_write, cfg_line);
        }
        if (!sent_cfg) {
            abort_start();
            return false;
        }

        // 等就绪（可被 ready_ 或 reader_exited_ 提前唤醒）
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::seconds(20), [&] {
                return ready_.load() || reader_exited_.load() || shutting_down_.load();
            });
        }
        if (ready_.load()) return true;
        abort_start();
        return false;
    }

    // 同步调用；timeout_ms 内无响应返回 false
    bool call(const std::string& cmd, mj::Value params, int timeout_ms, mj::Value& out_result) {
        if (!proc_.alive() || reader_exited_.load()) {
            return false;
        }
        int64_t cid = 0;
        std::string line;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cid = ++seq_;
            pending_[cid] = mj::Value::object();
            mj::Value req = mj::Value::object();
            req.set("cmd", cmd);
            req.set("id", cid);
            for (auto& kv : params.members()) req.set(kv.first, kv.second);
            line = mj::Value::dump(req) + "\n";
        }
        {
            std::lock_guard<std::mutex> wlk(wmtx_);
            if (!proc_.stdin_write || !write_all(proc_.stdin_write, line)) {
                std::lock_guard<std::mutex> lk(mtx_);
                pending_.erase(cid);
                return false;
            }
        }
        std::unique_lock<std::mutex> lk(mtx_);
        bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            auto it = pending_.find(cid);
            return (it != pending_.end() && it->second.get_bool("__done__", false))
                   || reader_exited_.load();
        });
        auto it = pending_.find(cid);
        if (got && it != pending_.end() && it->second.get_bool("__done__", false)) {
            mj::Value resp = std::move(it->second);
            pending_.erase(it);
            lk.unlock();
            bool ok = resp.get_bool("ok", false);
            out_result = resp["result"];
            return ok;
        }
        if (it != pending_.end()) pending_.erase(it);
        return false;
    }

    bool alive() const { return proc_.alive() && !reader_exited_.load(); }

    void shutdown() {
        if (shutting_down_.exchange(true)) return;
        util::trace("shutdown begin " + kind_ + " " + id_);
        if (reader_.joinable()) {
            // 尝试优雅退出（短超时；worker 无响应也不阻塞太久）
            if (proc_.alive() && !reader_exited_.load()) {
                mj::Value dummy;
                bool ok = call("exit", mj::Value::object(), 1500, dummy);
                util::trace("exit rpc ok=" + std::to_string(ok ? 1 : 0));
            }
            bool exited = proc_.wait_exit(1500);
            util::trace("wait_exit=" + std::to_string(exited ? 1 : 0));
            if (!exited) {
                util::trace("kill begin");
                proc_.kill();
                exited = proc_.wait_exit(1500);
                util::trace("kill done exited=" + std::to_string(exited ? 1 : 0));
            }
            // 关闭 stdin 写端并取消可能仍阻塞在 ReadFile 的同步 I/O，确保 reader_ 无条件可 join
            {
                std::lock_guard<std::mutex> wlk(wmtx_);
                if (proc_.stdin_write) {
                    CloseHandle(proc_.stdin_write);
                    proc_.stdin_write = nullptr;
                }
            }
            if (!reader_exited_.load()) {
                CancelSynchronousIo(static_cast<HANDLE>(reader_.native_handle()));
            }
            if (WaitForSingleObject(static_cast<HANDLE>(reader_.native_handle()), 1500) != WAIT_OBJECT_0) {
                // 兜底：若子进程孙进程仍持有 stdout 写端导致 ReadFile 未返回，再次取消 I/O
                CancelSynchronousIo(static_cast<HANDLE>(reader_.native_handle()));
            }
            reader_.join();
            util::trace("reader joined");
            proc_.close();
            util::trace("proc closed");
        } else {
            if (!proc_.wait_exit(1500)) proc_.kill();
            proc_.wait_exit(1500);
            proc_.close();
        }
        cv_.notify_all();
        util::trace("shutdown end " + kind_);
    }

private:
    static bool write_all(HANDLE h, const std::string& data) {
        if (!h) return false;
        const char* p = data.data();
        size_t rem = data.size();
        while (rem > 0) {
            DWORD chunk = rem > 65536 ? 65536 : (DWORD)rem;
            DWORD n = 0;
            if (!WriteFile(h, p, chunk, &n, nullptr) || n == 0) return false;
            p += n;
            rem -= n;
        }
        return true;
    }

    void abort_start() {
        proc_.kill();
        proc_.wait_exit(1500);
        {
            std::lock_guard<std::mutex> wlk(wmtx_);
            if (proc_.stdin_write) {
                CloseHandle(proc_.stdin_write);
                proc_.stdin_write = nullptr;
            }
        }
        if (reader_.joinable()) {
            if (!reader_exited_.load()) {
                CancelSynchronousIo(static_cast<HANDLE>(reader_.native_handle()));
            }
            reader_.join();
        }
        proc_.close();
    }

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
        {
            std::lock_guard<std::mutex> lk(mtx_);
            reader_exited_ = true;
        }
        cv_.notify_all();
    }

    void handle_line(const std::string& line) {
        mj::Value msg;
        if (!mj::try_parse(line, msg)) return;
        if (msg.get_bool("ready", false)) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                ready_ = true;
            }
            cv_.notify_all();
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
    std::atomic<bool> reader_exited_{ false };
    std::atomic<bool> shutting_down_{ false };
    std::mutex mtx_;
    std::condition_variable cv_;
    std::mutex wmtx_;                // 保护 stdin 写
    int64_t seq_ = 0;
    std::map<int64_t, mj::Value> pending_;
};

} // namespace hub
