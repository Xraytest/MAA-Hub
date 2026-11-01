// MaaHub 原生窗口应用（Win32 + 自绘 GUI）
// 布局：左侧 = 执行器列表（可新建/删除，每个执行器 = 一个独立的 MAA/MaaEnd 实例）
//       主区 = 选中执行器的 MAA 风格实例页 / 全局页（账号管理、核心管理、智能规划、事件日志）
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "accounts.hpp"
#include "core_manager.hpp"
#include "gui.hpp"
#include "hub.hpp"
#include "json.hpp"
#include "util.hpp"

namespace hub {

// 任务勾选状态（按执行器）
struct TaskUiState {
    bool checked = false;
    bool once = false;          // 仅生效一次（官方右键语义，不写入配置）
    bool selected = false;      // 当前选中项（右侧展示其设置，对应 MAA 左列选中）
    bool open = false;
    std::map<std::string, mj::Value> params;
};

class MaaHubWindow {
public:
    MaaHubWindow(HINSTANCE hInst, mj::Value config, bool offscreen = false);
    ~MaaHubWindow();

    bool create();
    // 离屏渲染指定页面到 PNG（窗口不可见，仅用于验证）
    bool render_to_file(const std::string& page, const std::string& png_path);
    // 调试用访问器（--dump-render 等待目录就绪）
    std::vector<ExecutorEntry> debug_executors() { return hub_->executors(); }
    mj::Value debug_catalog(const std::string& id, double ttl = 0) {
        return hub_->executor_catalog(id, ttl);
    }
    void debug_refresh_snapshot() { refresh_snapshot(); }
    void debug_select_task(const std::string& task);
    int run();

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg(HWND, UINT, WPARAM, LPARAM);
    void on_paint(HDC dc, RECT client);
    void render(RECT client);

    // 页面
    void draw_sidebar(RECT r);
    void draw_topbar(RECT r);
    void page_executor(RECT content);       // MAA 1:1 实例页
    void page_welcome(RECT content);
    void page_accounts(RECT content);
    void page_cores(RECT content);
    void page_planner(RECT content);
    void page_events(RECT content);
    void page_settings(RECT content);

    // MAA 官方任务显示名（取自 MaaWpfGui Res/Localizations/zh-cn.xaml）
    static std::wstring display_name(const std::string& kind, const std::string& task);

    // 后台
    void backend_loop();
    void poll_task_states();
    void post_action(std::function<void()> fn, const std::string& busy_text);
    void refresh_snapshot();
    void refresh_snapshot_fast();   // 即时快照：只读持久化文件，启动首帧用
    void set_notice(const std::string& s);

    // 动作
    void action_create_executor();
    void action_delete_executor(const std::string& id);
    void action_connect(const std::string& id);
    void action_disconnect(const std::string& id);
    void action_stop(const std::string& id);
    void action_run(const std::string& id, const std::string& task, const mj::Value& params);
    void action_run_checked(const std::string& id);
    void action_run_from(const std::string& id, const std::string& from_task);
    void action_propose(const std::string& goal, const std::string& planner);
    void action_execute(const std::string& plan_id);
    void action_reject(const std::string& plan_id);
    void action_stop_exec();
    void action_check_update(const std::string& core_id);
    void action_pull_core(const std::string& kind, const std::string& url);
    void action_pull_latest(const std::string& kind);
    void action_adopt(const std::string& kind);
    void action_save_settings();

    // 数据辅助（返回拷贝：快照会被后台线程整体替换，指针会悬空）
    bool find_executor(const std::string& id, mj::Value& out) const;
    std::vector<mj::Value> executors_of(const std::string& kind) const;
    std::vector<mj::Value> catalog_of(const std::string& kind) const;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    std::unique_ptr<Hub> hub_;
    mj::Value config_;
    bool offscreen_ = false;   // 离屏模式：窗口永不显示（仅测试/渲染用）

    int sidebar_w_ = 250;
    int topbar_h_ = 56;

    gui::Gui gui_;
    gui::Input input_;
    HDC mem_dc_ = nullptr;
    HBITMAP mem_bmp_ = nullptr;
    void* mem_bits_ = nullptr;
    SIZE mem_size_{ 0, 0 };

    // UI 状态
    std::string page_ = "welcome";       // welcome | executor | accounts | cores | planner | events
    std::string selected_exec_;
    std::map<std::string, std::map<std::string, TaskUiState>> task_ui_;
    std::map<std::string, float> scroll_;
    // 左列拖拽排序（对应 MAA 的 dd:DragDrop）
    std::map<std::string, std::vector<std::string>> task_order_;  // 执行器 -> 任务顺序
    std::string drag_task_;      // 正在拖拽的任务名
    int drag_y_ = -1;            // 拖拽中的鼠标 y
    int drop_before_ = -1;       // 插入位置索引
    // 新建执行器表单
    bool show_new_exec_ = false;
    int new_kind_ = 0;
    int new_acc_idx_ = 0;                 // 0 = 不绑定
    std::string new_label_;
    // 账号表单
    int acc_form_game_ = 0;
    int acc_form_match_ = 0;
    std::string form_label_, form_ident_;
    // 设置表单
    int set_proxy_mode_ = 0;      // 0 none 1 http 2 socks5
    std::string set_proxy_host_ = "127.0.0.1";
    std::string set_proxy_port_ = "7890";
    std::string set_proxy_user_, set_proxy_pass_;
    std::string set_cdk_;
    std::string set_llm_url_ = "https://api.deepseek.com";
    std::string set_llm_key_, set_llm_model_ = "deepseek-chat";
    bool set_llm_enabled_ = false;
    bool settings_loaded_ = false;
    // 规划
    int planner_sel_ = 0;
    std::string goal_ = "清理所有执行器的今日日常，理智/体力富余时继续刷关";
    std::string last_plan_json_;
    // 核心
    std::string core_msg_;
    double core_msg_ts_ = 0;
    UpdateInfo update_info_;
    bool has_update_info_ = false;

    mutable std::mutex snap_mtx_;
    mj::Value snap_ = mj::Value::object();
    uint64_t snap_version_ = 0;
    uint64_t snap_seen_ = 0;

    std::thread backend_;
    std::atomic<bool> running_{ false };
    std::mutex action_mtx_;
    std::deque<std::pair<std::function<void()>, std::string>> actions_;

    // 下载进度
    std::atomic<int> dl_progress_{ -1 };
    std::string dl_stage_;

    static const UINT WM_APP_SNAP = WM_APP + 1;
    static const UINT_PTR TIMER_REFRESH = 1;
};

} // namespace hub
