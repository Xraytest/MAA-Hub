// MaaHub 原生窗口应用（Win32 + 自绘 GUI）
// 布局：左侧 = 执行器列表（可新建/删除，每个执行器 = 一个独立的 MAA/MaaEnd 实例）
//       主区 = 选中执行器的 MAA 风格实例页 / 全局页（账号管理、核心管理、智能规划、事件日志）
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <set>
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
#include "config_import.hpp"

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
    void debug_show_advanced() { advanced_view_ = true; }
    void debug_open_add_task();
    void debug_open_ctx_menu(const std::string& task);
    void debug_open_new_exec();
    // 离屏画布尺寸（真实窗口客户区为 800x600；不设置就按这个值建）
    void debug_set_client_size(int w, int h) { dump_w_ = w; dump_h_ = h; }
    int run();

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg(HWND, UINT, WPARAM, LPARAM);
    void on_paint(HDC dc, RECT client);
    void render(RECT client);

    // 页面
    // 原生标题栏：窗口标题对齐官方 UpdateWindowTitle（实例名 + 连接地址）
    void sync_window_title();
    std::wstring win_title_;      // 上次写入的标题，避免每帧 SetWindowTextW
    bool topmost_ = false;
    bool auto_reload_ = false;   // 官方 EnableAutoReload（自动重载资源）
    // 任务切换过渡动画（官方 TransitioningContentControl 简化实现）
    std::string panel_anim_task_;      // 正在动画的任务
    std::string panel_anim_exec_;      // 动画所属执行器
    double panel_anim_t0_ = 0;         // 动画开始时间（秒）
    int panel_anim_dir_ = 1;           // 1=向下进入, -1=向上进入
    static constexpr double PANEL_ANIM_MS = 140.0;
    void draw_top_tabs(RECT r);
    // 左侧栏：执行器列表 + 所有非执行器页面入口（账号/核心/规划/日志/设置）
    void draw_left_rail(RECT r);
    static constexpr int RAIL_W = 190;
    // 右下角 GIF 挂件（官方 GifImage：100x100、可拖拽、右键切换）
    void draw_gif_widget(RECT client);
    int gif_x_ = -1, gif_y_ = -1;          // -1 = 默认右下角
    bool gif_dragging_ = false;
    int gif_drag_dx_ = 0, gif_drag_dy_ = 0;
    size_t gif_index_ = 0;
    bool gif_hidden_ = true;   // 默认隐藏（无内置 GIF 资源，避免遮挡内容）
    // 执行器列表：一个执行器 = 一个独立的 MAA/MaaEnd 实例，常驻左侧栏
    void draw_executor_list(RECT r);
    static constexpr int EXEC_ROW_H = 26;
    // 右列日志区（官方 TaskQueueView Column=2）
    void draw_log_column(RECT area, const std::string& id);
    void page_executor(RECT content);
    void draw_add_task_overlay(RECT area, const std::string& id, const std::string& kind);
    void draw_post_action(RECT area, const std::string& id);
    void draw_task_ctx_menu();
    void draw_rename_box(RECT area, const std::string& id);
    void draw_new_exec_overlay();
    void save_task_alias();
    void load_task_alias();
    std::string kind_of(const std::string& id) const;
    void save_post_action(const std::string& id);
    std::wstring post_action_desc(const std::string& id);
    void load_post_action();
    void save_task_list();
    void load_task_list();       // MAA 1:1 实例页
    void save_task_checks();     // 左列勾选状态（官方队列状态会持久化）
    void load_task_checks();
    void sync_task_checks(const std::string& id,
                              const std::vector<mj::Value>& tasks);
    // 官方 CopilotView（自动战斗）
    void page_copilot(RECT content);
    // 官方 ToolboxView（小工具）
    void page_toolbox(RECT content);
    std::string copilot_path_;
    std::map<std::string, bool> copilot_opt_;
    void page_accounts(RECT content);
    void page_cores(RECT content);
    void page_planner(RECT content);
    void page_events(RECT content);
    void page_settings(RECT content);

    // MAA 官方任务显示名（取自 MaaWpfGui Res/Localizations/zh-cn.xaml）
    std::wstring display_name(const std::string& kind, const std::string& task);
    // MaaEnd 任务名→官方 label（取自 interface 的 locales），随快照变化重建

    const std::map<std::string, std::wstring>& end_labels();
    std::map<std::string, std::wstring> end_label_;
    std::string end_label_sig_;

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
    void action_import_config();

    // 数据辅助（返回拷贝：快照会被后台线程整体替换，指针会悬空）
    bool find_executor(const std::string& id, mj::Value& out) const;
    std::vector<mj::Value> executors_of(const std::string& kind) const;
    std::vector<mj::Value> catalog_of(const std::string& kind) const;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    std::unique_ptr<Hub> hub_;
    mj::Value config_;
    bool offscreen_ = false;   // 离屏模式：窗口永不显示（仅测试/渲染用）
    int dump_w_ = 0, dump_h_ = 0;   // 离屏客户区尺寸，0 = 用真实窗口的 800x600

    int sidebar_w_ = 250;
    int topbar_h_ = 36;   // 官方 TabItem 高度约 30，加内边距

    gui::Gui gui_;
    gui::Input input_;
    HDC mem_dc_ = nullptr;
    HBITMAP mem_bmp_ = nullptr;
    void* mem_bits_ = nullptr;
    SIZE mem_size_{ 0, 0 };

    // UI 状态
    std::string page_ = "executor";        // executor | accounts | cores | planner | events
    std::string selected_exec_;
    std::map<std::string, std::map<std::string, TaskUiState>> task_ui_;
    // 首次打开某执行器时按官方预设初始化勾选
    std::map<std::string, bool> task_ui_seeded_;
    std::map<std::string, float> scroll_;
    // 滚动区内容高度（上一帧实测；scroll_ 的键一一对应）
    std::map<std::string, int> content_h_;
    // 左列拖拽排序（对应 MAA 的 dd:DragDrop）
    std::map<std::string, std::vector<std::string>> task_order_;  // 执行器 -> 任务顺序
    std::string drag_task_;      // 正在拖拽的任务名
    int drag_y_ = -1;            // 拖拽中的鼠标 y
    int drop_before_ = -1;       // 插入位置索引
    // 任务设置区视图切换：false=常规设置 true=高级设置（官方 ButtonGroup）
    bool advanced_view_ = false;
    // 任务项右键菜单（官方 ContextMenu）
    bool ctx_menu_open_ = false;
    int ctx_menu_x_ = 0, ctx_menu_y_ = 0;
    std::string ctx_menu_task_;
    std::string ctx_menu_exec_;
    std::string renaming_task_;
    std::string rename_exec_;
    std::string rename_buf_;
    std::map<std::string, std::string> task_alias_;   // exec|task -> 别名

    // 新建执行器表单
    // 「完成后动作设置」浮层（官方 TaskQueue.PostAction.Settings）
    bool show_post_action_ = false;
    std::map<std::string, mj::Value> post_action_;
    // 「添加新任务」浮层（官方左列底部 TaskQueue.AddTask）
    bool show_add_task_ = false;
    std::string add_task_filter_;
    // exec_id -> 左列显示的任务集（空 = 显示全部）
    std::map<std::string, std::set<std::string>> task_list_;
    // 执行器 -> 已勾选任务；没有记录 = 首次使用，默认全部勾选
    std::map<std::string, std::set<std::string>> task_checks_;
    std::map<std::string, std::set<std::string>> task_ui_init_;
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
    // 启动设置（官方 StartSettings）
    std::string set_theme_ = "light";   // 界面主题 light|dark|system
    bool set_auto_start_ = false;
    bool set_start_min_ = false;
    bool set_start_run_ = false;
    // 更新设置（官方 VersionUpdateSettings）
    bool set_upd_check_ = true;
    bool set_upd_dl_ = false;
    bool set_upd_install_ = false;
    bool set_upd_gh_ = false;
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
    static const UINT WM_APP_IMPORT = WM_APP + 2;   // backend 线程完成导入 -> UI 线程合并任务列表
    static const UINT_PTR TIMER_REFRESH = 1;

    // 导入动作的跨线程交接：backend 线程写入（import_mtx_ 保护），
    // UI 线程在 WM_APP_IMPORT 里取走并合并进 task_list_
    std::mutex import_mtx_;
    mj::Value import_task_lists_;   // { exec_id: [任务名...] }
    // 上次导入选过的安装目录（记住位置，下次直接停在那）
    std::string import_path_ = "C:\\Program Files";
};

} // namespace hub
