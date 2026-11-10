// 应用设置：更新代理、镜像源、LLM 配置（持久化到 state\settings.json）
#pragma once
#include <functional>
#include <string>

#include "json.hpp"
#include "util.hpp"

namespace hub {

struct AppSettings {
    // 更新代理
    std::string proxy_mode = "none";     // none | http | socks5
    std::string proxy_host = "127.0.0.1";
    int proxy_port = 7890;
    std::string proxy_user;              // 可选
    std::string proxy_pass;              // 可选
    // Mirror酱 CDK（可选，填了优先使用）
    std::string mirrorchyan_cdk;
    // LLM
    std::string llm_base_url = "https://api.deepseek.com";
    std::string llm_api_key;
    std::string llm_model = "deepseek-chat";
    bool llm_enabled = false;
    // 启动设置（官方 StartSettings）
    bool auto_start = false;          // 开机自动启动
    bool start_minimized = false;     // 启动后直接最小化
    bool start_run = false;           // 启动后直接运行
    // 更新设置（对齐 MAA 官方 Update 组：VersionType / UpdateSource / GitHubToken /
    // CheckOnStartup / CheckOnSchedule / AutoDownloadUpdatePackage / AutoInstallUpdatePackage）
    bool update_check_on_start = true;      // 启动时检查更新
    bool update_check_on_schedule = true;   // 定时检查（运行期间每 6 小时）
    bool update_auto_download = false;      // 自动下载更新包
    bool update_auto_install = false;       // 自动安装（下载后切换为默认核心）
    std::string update_version_type = "stable";  // 更新渠道：stable | beta | nightly
    std::string update_source = "auto";          // 更新源：auto | github | mirrorchyan
    std::string github_token;                    // GitHub API Token（提高限额）
    // 界面主题（官方 GuiSettings）：light | dark | system
    std::string theme = "light";

    mj::Value to_json() const {
        mj::Value v = mj::Value::object();
        mj::Value p = mj::Value::object();
        p.set("mode", proxy_mode);
        p.set("host", proxy_host);
        p.set("port", proxy_port);
        p.set("user", proxy_user);
        p.set("pass", proxy_pass);
        v.set("proxy", std::move(p));
        v.set("mirrorchyan_cdk", mirrorchyan_cdk);
        mj::Value l = mj::Value::object();
        l.set("enabled", llm_enabled);
        l.set("base_url", llm_base_url);
        l.set("api_key", llm_api_key);
        l.set("model", llm_model);
        v.set("llm", std::move(l));
        // 启动 / 更新设置
        mj::Value st = mj::Value::object();
        st.set("auto_start", auto_start);
        st.set("start_minimized", start_minimized);
        st.set("start_run", start_run);
        st.set("update_check_on_start", update_check_on_start);
        st.set("update_check_on_schedule", update_check_on_schedule);
        st.set("update_auto_download", update_auto_download);
        st.set("update_auto_install", update_auto_install);
        st.set("update_version_type", update_version_type);
        st.set("update_source", update_source);
        st.set("github_token", github_token);
        v.set("startup_update", std::move(st));
        v.set("theme", theme);
        return v;
    }

    static AppSettings from_json(const mj::Value& v) {
        AppSettings s;
        if (auto* p = v.find("proxy")) {
            s.proxy_mode = p->get_str("mode", "none");
            s.proxy_host = p->get_str("host", "127.0.0.1");
            s.proxy_port = (int)p->get_int("port", 7890);
            s.proxy_user = p->get_str("user", "");
            s.proxy_pass = p->get_str("pass", "");
        }
        s.mirrorchyan_cdk = v.get_str("mirrorchyan_cdk", "");
        if (auto* l = v.find("llm")) {
            s.llm_enabled = l->get_bool("enabled", false);
            s.llm_base_url = l->get_str("base_url", s.llm_base_url);
            s.llm_api_key = l->get_str("api_key", "");
            s.llm_model = l->get_str("model", s.llm_model);
        }
        if (auto* su = v.find("startup_update")) {
            s.auto_start = su->get_bool("auto_start", false);
            s.start_minimized = su->get_bool("start_minimized", false);
            s.start_run = su->get_bool("start_run", false);
            s.update_check_on_start = su->get_bool("update_check_on_start", true);
            s.update_check_on_schedule = su->get_bool("update_check_on_schedule", true);
            s.update_auto_download = su->get_bool("update_auto_download", false);
            s.update_auto_install = su->get_bool("update_auto_install", false);
            s.update_version_type = su->get_str("update_version_type", "stable");
            s.update_source = su->get_str("update_source", "auto");
            s.github_token = su->get_str("github_token", "");
        }
        s.theme = v.get_str("theme", "light");
        return s;
    }

    // 生成 WinHTTP 的代理参数字符串（空 = 不使用命名代理）
    // HTTP 代理: "http=host:port"（同时用于 https）
    // SOCKS5:   "socks=host:port"（Win10 1903+ 支持 socks= 前缀）
    std::wstring winhttp_proxy() const {
        if (proxy_mode == "none" || proxy_host.empty() || proxy_port <= 0) return L"";
        std::wstring w = util::utf8_to_wide(proxy_host) + L":" + std::to_wstring(proxy_port);
        if (proxy_mode == "socks5") return L"socks=" + w;
        return L"http=" + w;
    }
    std::wstring winhttp_bypass() const {
        return L"localhost;127.0.0.1;<local>";
    }
};

class SettingsStore {
public:
    explicit SettingsStore(std::string state_dir) : dir_(std::move(state_dir)) { load(); }

    AppSettings get() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return s_;
    }

    // 保存失败要让调用方感知（原先 update 丢弃写盘结果，UI 永远提示"已保存"）
    bool update(const std::function<void(AppSettings&)>& fn) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fn(s_);
        }
        return save();
    }

    // 加载结果说明："" = 正常；missing/corrupt/recovered 的可读描述
    std::string load_note() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return load_note_;
    }

private:
    void load() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string path = dir_ + "\\settings.json";
        std::string content;
        if (util::read_file(path, content)) {
            mj::Value v;
            if (mj::try_parse(content, v)) {
                s_ = AppSettings::from_json(v);
                load_note_.clear();
                return;
            }
            // 主文件损坏：留证据再尝试备份（原子写后不应再发生，兼容修复旧损伤）
            load_note_ = "settings.json 解析失败";
            MoveFileExW(util::utf8_to_wide(path).c_str(),
                        util::utf8_to_wide(path + ".corrupt").c_str(),
                        MOVEFILE_REPLACE_EXISTING);
        } else if (!util::file_exists(path)) {
            return;   // 首次运行：安静地用默认值
        } else {
            load_note_ = "settings.json 读取失败";
        }
        // 上一份好内容兜底
        std::string bak;
        if (util::read_file(dir_ + "\\settings.json.bak", bak) && !bak.empty()) {
            mj::Value v;
            if (mj::try_parse(bak, v)) {
                s_ = AppSettings::from_json(v);
                load_note_ += "，已从 settings.json.bak 恢复";
                util::write_file(path, mj::Value::dump_pretty(s_.to_json()));
                return;
            }
        }
        if (!load_note_.empty()) load_note_ += "，使用默认设置";
    }

    bool save() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string path = dir_ + "\\settings.json";
        // 滚动备份：覆盖前把上一份内容留作 .bak（load 失败时的恢复源）
        if (util::file_exists(path))
            CopyFileW(util::utf8_to_wide(path).c_str(),
                      util::utf8_to_wide(path + ".bak").c_str(), FALSE);
        return util::write_file(path, mj::Value::dump_pretty(s_.to_json()));
    }

    std::string dir_;
    mutable std::mutex mtx_;
    AppSettings s_;
    std::string load_note_;
};

} // namespace hub
