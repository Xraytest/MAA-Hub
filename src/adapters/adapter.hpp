// 适配器接口：MAA / MaaEnd 统一抽象
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"

namespace hub {

struct AdapterEvent {
    std::string app;        // maa | maaend
    std::string source;     // 原始 message / msg id
    std::string category;   // connection | task | node | subtask | info | error
    std::string what;
    std::string task;
    int64_t task_id = 0;
    mj::Value details = mj::Value::object();
    std::string raw;
};

struct AppStatus {
    std::string kind;
    std::string name;
    std::string install_dir;
    std::string version;
    std::string core_dll;
    bool available = false;
    std::string error;          // 加载/发现错误
    std::string conn = "unknown";   // unknown|disconnected|connecting|connected|failed
    std::string run = "idle";       // idle|running|stopping|failed
    std::string last_error;
    // 控制器
    std::string ctrl_mode;
    int64_t ctrl_hwnd = 0;
    int res_w = 0, res_h = 0;
    std::string uuid;
    std::string resource_hash;
    int64_t task_count = 0;
};

class IAdapter {
public:
    virtual ~IAdapter() = default;

    virtual bool discover(const mj::Value& cfg) = 0;
    virtual bool load() = 0;
    virtual void unload() = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() = 0;
    virtual bool run_task(const std::string& task, const mj::Value& params, int64_t& out_task_id) = 0;
    virtual bool stop() = 0;
    virtual bool is_running() = 0;
    virtual AppStatus status() const = 0;
    virtual std::vector<mj::Value> task_catalog() const = 0;

    // 取出并清空事件队列（worker 循环调用）
    virtual std::vector<AdapterEvent> drain_events() = 0;

protected:
    void emit(AdapterEvent ev) {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        ev.app = kind_;
        events_.push_back(std::move(ev));
        if (events_.size() > 1000) events_.erase(events_.begin(), events_.begin() + 500);
    }

    mutable std::mutex ev_mtx_;
    std::vector<AdapterEvent> events_;
    std::string kind_;
};

} // namespace hub
