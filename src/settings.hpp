// 应用设置：更新代理、镜像源、LLM 配置（持久化到 state\settings.json）
#pragma once
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
    // 更新设置（官方 VersionUpdateSettings）
    bool update_check_on_start = true;   // 启动时检查更新
    bool update_auto_download = false;   // 自动下载更新包
    bool update_auto_install = false;    // 自动安装更新包
    bool update_force_github = false;    // 强制使用 GitHub
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
        st.set("update_auto_download", update_auto_download);
        st.set("update_auto_install", update_auto_install);
        st.set("update_force_github", update_force_github);
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
            s.update_auto_download = su->get_bool("update_auto_download", false);
            s.update_auto_install = su->get_bool("update_auto_install", false);
            s.update_force_github = su->get_bool("update_force_github", false);
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

    void update(const std::function<void(AppSettings&)>& fn) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fn(s_);
        }
        save();
    }

private:
    void load() {
        std::string content;
        if (!util::read_file(dir_ + "\\settings.json", content)) return;
        mj::Value v;
        if (!mj::try_parse(content, v)) return;
        s_ = AppSettings::from_json(v);
    }

    void save() {
        std::lock_guard<std::mutex> lk(mtx_);
        util::write_file(dir_ + "\\settings.json", mj::Value::dump_pretty(s_.to_json()));
    }

    std::string dir_;
    mutable std::mutex mtx_;
    AppSettings s_;
};

} // namespace hub
