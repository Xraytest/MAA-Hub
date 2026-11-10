// 核心管理：MAA / MaaEnd 核心的登记、版本探测、检查更新、拉取与更新
// - 核心 = 一个可用的 MAA/MaaEnd 程序目录（含 MaaCore.dll / MaaFramework.dll + resource）
// - 支持三种来源：adopt（收养本机已有安装）/ download（从官方源拉取）/ manual（手动登记）
// - 下载走 WinHTTP，多源回退（直连 GitHub → gh-proxy → ghfast → moeyy → Mirror酱）
// - 解包使用 Windows 自带 tar.exe（支持 zip），零第三方依赖
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace hub {

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

struct CoreInfo {
    std::string id;           // core-xxxx
    std::string kind;         // maa | maaend
    std::string name;         // 显示名，如 "MAA 官服核心"
    std::string path;         // 程序目录
    std::string version;      // 探测到的版本
    std::string source;       // adopt | download | manual
    std::string release_tag;  // 来源 release tag（下载安装时记录，版本比较以此为准）
    double created_at = 0;
    double updated_at = 0;
    bool is_default = false;  // 该类型的默认核心（新建执行器时使用）

    mj::Value to_json() const {
        mj::Value v = mj::Value::object();
        v.set("id", id);
        v.set("kind", kind);
        v.set("name", name);
        v.set("path", path);
        v.set("version", version);
        v.set("source", source);
        v.set("release_tag", release_tag);
        v.set("created_at", created_at);
        v.set("updated_at", updated_at);
        v.set("is_default", is_default);
        return v;
    }
    static CoreInfo from_json(const mj::Value& v) {
        CoreInfo c;
        c.id = v.get_str("id", "");
        c.kind = v.get_str("kind", "");
        c.name = v.get_str("name", "");
        c.path = v.get_str("path", "");
        c.version = v.get_str("version", "");
        c.source = v.get_str("source", "manual");
        c.release_tag = v.get_str("release_tag", "");
        c.created_at = v.get_int("created_at", 0);
        c.updated_at = v.get_int("updated_at", 0);
        c.is_default = v.get_bool("is_default", false);
        return c;
    }
};

struct UpdateInfo {
    bool ok = false;
    std::string error;
    std::string kind;            // maa | maaend（由调用方填充）
    std::string current;         // 当前版本（探测值）
    std::string current_tag;     // 当前核心记录的 release tag（可空）
    std::string latest;          // 上游版本名（tag_name / Mirror酱 version_name）
    std::string latest_tag;      // 上游 release tag
    std::string download_url;
    std::string asset_name;      // 选中的资产文件名
    long long asset_size = 0;    // 资产字节数（0 = 未知）
    std::string asset_sha256;    // 上游给出的 sha256（可空，空则不校验）
    std::string source_name;
    std::string release_notes;
    bool has_update = false;
    // 当前核心没有 tag 且版本口径与上游不同（如 MaaEnd tag=v3.0.0 / interface=5.15.0-beta.1）
    // 此时无法可靠判定新旧，置位让 UI 提示「口径不一致，建议按上游 tag 安装」
    bool tag_mismatch = false;
};

class CoreManager {
public:
    // state_dir: 登记表目录（<exe_dir>\state）
    // cores_root: 核心实际存放根目录（<exe_dir>\cores），由 MaaHub 内部管理
    CoreManager(std::string state_dir, std::string cores_root)
        : dir_(std::move(state_dir)), cores_root_(std::move(cores_root)) {
        util::ensure_dir(cores_root_);
        if (!load()) rebuild_from_cores_dir();
    }

    // 核心根目录（UI 展示用）
    const std::string& cores_root() const { return cores_root_; }

    // 内部核心根目录：<exe_dir>\cores
    static std::string default_cores_root() {
        return util::exe_dir() + "\\cores";
    }

    // 递归复制目录（用于一次性导入）
    static bool copy_tree(const std::string& src, const std::string& dst) {
        util::ensure_dir(dst);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(util::utf8_to_wide(src + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool ok = true;
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::string s = src + "\\" + util::wide_to_utf8(name);
            std::string d = dst + "\\" + util::wide_to_utf8(name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!copy_tree(s, d)) { ok = false; break; }
            } else {
                if (!CopyFileW(util::utf8_to_wide(s).c_str(),
                               util::utf8_to_wide(d).c_str(), FALSE)) {
                    ok = false;
                    break;
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return ok;
    }

    static void remove_tree(const std::string& path) {
        std::wstring p = util::utf8_to_wide(path);
        // SHFileOperation 需要双 null 结尾
        std::vector<wchar_t> buf(p.begin(), p.end());
        buf.push_back(L'\0');
        buf.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = buf.data();
        op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
        SHFileOperationW(&op);
    }


    // ---------------------------------------------------------------- 查询
    std::vector<CoreInfo> list(const std::string& kind = "") const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<CoreInfo> out;
        for (auto& c : cores_) {
            if (kind.empty() || c.kind == kind) out.push_back(c);
        }
        return out;
    }

    bool get(const std::string& id, CoreInfo& out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& c : cores_) {
            if (c.id == id) { out = c; return true; }
        }
        return false;
    }

    CoreInfo default_core(const std::string& kind) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& c : cores_) {
            if (c.kind == kind && c.is_default) return c;
        }
        for (auto& c : cores_) {
            if (c.kind == kind) return c;
        }
        return CoreInfo{};
    }

    // ---------------------------------------------------------------- 导入（一次性迁移）
    // 把系统里已有的安装**复制**进 MaaHub 内部 cores 目录后登记。
    // 仅由 --import-core 显式触发；默认流程不接触系统安装。
    std::string import_core(const std::string& kind, const std::string& src_path) {
        if (!valid_core_dir(kind, src_path)) return "";
        std::string version = probe_version(kind, src_path);
        std::string dest = cores_root_ + "\\" + kind + "-" + (version.empty() ? "unknown" : version);
        // 目标已存在则加后缀
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& x : cores_) {
                if (x.path == dest) {
                    dest = cores_root_ + "\\" + kind + "-" + version + "-" + random_hex(4);
                    break;
                }
            }
        }
        if (!copy_tree(src_path, dest)) return "";
        CoreInfo c;
        c.id = "core-" + random_hex(8);
        c.kind = kind;
        c.path = dest;
        c.name = (kind == "maa" ? "MAA 核心" : "MaaEnd 核心") + (" " + version);
        c.source = "import";
        c.created_at = util::now_sec();
        c.version = version;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            bool has_default = false;
            for (auto& x : cores_) {
                if (x.kind == kind && x.is_default) has_default = true;
            }
            c.is_default = !has_default;
            cores_.push_back(c);
        }
        save();
        return c.id;
    }

    bool remove(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = cores_.begin(); it != cores_.end(); ++it) {
            if (it->id != id) continue;
            std::string kind = it->kind;
            bool was_default = it->is_default;
            cores_.erase(it);
            if (was_default) {
                for (auto& x : cores_) {
                    if (x.kind == kind) { x.is_default = true; break; }
                }
            }
            save();
            return true;
        }
        return false;
    }

    bool set_default(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string kind;
        for (auto& c : cores_) {
            if (c.id == id) kind = c.kind;
        }
        if (kind.empty()) return false;
        for (auto& c : cores_) {
            if (c.kind == kind) c.is_default = (c.id == id);
        }
        save();
        return true;
    }

    // ---------------------------------------------------------------- 版本探测
    // 优先用子进程加载 DLL 查询（权威）；失败回退到接口文件
    static std::string probe_version(const std::string& kind, const std::string& path) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --core-version "
                           + util::utf8_to_wide(kind) + L" \""
                           + util::utf8_to_wide(path) + L"\"";
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE rp = nullptr, wp = nullptr;
        if (CreatePipe(&rp, &wp, &sa, 0)) {
            SetHandleInformation(rp, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = wp;
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> cmdv(cmd.begin(), cmd.end());
            cmdv.push_back(L'\0');
            if (CreateProcessW(nullptr, cmdv.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(wp);
                wp = nullptr;
                std::string out;
                char buf[256];
                DWORD n = 0;
                while (ReadFile(rp, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
                    buf[n] = 0;
                    out += buf;
                }
                WaitForSingleObject(pi.hProcess, 20000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                CloseHandle(rp);
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r'
                                        || out.back() == ' ')) out.pop_back();
                size_t st = out.find_first_not_of(" \r\n");
                if (st != std::string::npos) out = out.substr(st);
                if (out.find_first_of("abcdefghijklmnopqrstuvwxyz0123456789")
                        != std::string::npos) {
                    return out;
                }
            } else if (wp) {
                CloseHandle(wp);
                CloseHandle(rp);
            }
        }
        return read_version_from_files(kind, path);
    }

    static std::string read_version_from_files(const std::string& kind, const std::string& path) {
        // MaaEnd: interface.json 的 version 字段
        std::string content;
        if (kind == "maaend") {
            if (util::read_file(path + "\\interface.json", content)) {
                mj::Value v;
                if (mj::try_parse(content, v)) return v.get_str("version", "");
            }
        }
        // MAA: cache/version/summary.json 或 config 里的版本
        if (util::read_file(path + "\\cache\\version\\summary.json", content)) {
            mj::Value v;
            if (mj::try_parse(content, v)) {
                for (auto* key : { "version", "core_version", "current" }) {
                    std::string s = v.get_str(key, "");
                    if (!s.empty()) return s;
                }
            }
        }
        return "";
    }

    static bool valid_core_dir(const std::string& kind, const std::string& path) {
        if (kind == "maa") return util::file_exists(path + "\\MaaCore.dll");
        if (kind == "maaend") return util::file_exists(path + "\\maafw\\MaaFramework.dll")
                                  && util::file_exists(path + "\\interface.json");
        return false;
    }

    // ---------------------------------------------------------------- 更新检查
    // 对齐 MAA 官方 Update 组：VersionType(stable/beta/nightly) / UpdateSource / GitHubToken
    // current: 当前版本（探测值）；current_tag: 当前核心记录的 release tag（可空）
    static UpdateInfo check_update(const std::string& kind, const std::string& current,
                                   const std::string& current_tag = "",
                                   const AppSettings* settings = nullptr) {
        UpdateInfo ui;
        ui.current = current;
        ui.current_tag = current_tag;
        std::string repo = kind == "maa"
            ? "MaaAssistantArknights/MaaAssistantArknights"
            : "MaaEnd/MaaEnd";
        std::string cdk = settings ? settings->mirrorchyan_cdk : "";
        std::string ver_type = settings ? settings->update_version_type : "stable";
        std::string src_pref = settings ? settings->update_source : "auto";
        std::string gh_token = settings ? settings->github_token : "";

        std::vector<std::pair<std::string, std::string>> headers;
        if (!gh_token.empty()) headers.push_back({ "Authorization", "Bearer " + gh_token });
        headers.push_back({ "Accept", "application/vnd.github+json" });

        // stable 用 /releases/latest（GitHub 自动跳过 prerelease）；beta/nightly 用列表过滤
        std::string gh_path = "/releases/latest";
        if (ver_type == "beta" || ver_type == "nightly") gh_path = "/releases?per_page=20";

        std::vector<std::pair<std::string, std::string>> sources;
        auto add_mirror = [&]() {
            if (cdk.empty()) return;
            std::string rid = kind == "maa" ? "MAA" : "MaaEnd";
            std::string ch = ver_type.empty() ? "stable" : ver_type;
            sources.push_back({ "Mirror酱",
                "https://mirrorchyan.com/api/resources/" + rid + "/latest?cdk=" + cdk
                + "&os=win&arch=x64&channel=" + ch });
        };
        auto add_github = [&]() {
            sources.push_back({ "GitHub", "https://api.github.com/repos/" + repo + gh_path });
            sources.push_back({ "gh-proxy", "https://gh-proxy.com/https://api.github.com/repos/" + repo + gh_path });
            sources.push_back({ "ghfast", "https://ghfast.top/https://api.github.com/repos/" + repo + gh_path });
            sources.push_back({ "moeyy", "https://github.moeyy.xyz/https://api.github.com/repos/" + repo + gh_path });
        };
        if (src_pref == "mirrorchyan") { add_mirror(); add_github(); }
        else { add_github(); add_mirror(); }

        for (auto& [name, url] : sources) {
            std::string body = http_get(url, 20000, settings, headers);
            if (body.empty()) continue;
            mj::Value v;
            if (!mj::try_parse(body, v)) continue;
            bool is_mirror = (name == "Mirror酱");
            mj::Value rel_storage;
            mj::Value* rel = nullptr;
            if (is_mirror) {
                // {"code":0,"msg":"success","data":{"url":"...","version_name":"v2.36.0"}}
                if (v.get_int("code", -1) != 0) continue;
                auto* d = v.find("data");
                if (!d) continue;
                ui.latest = d->get_str("version_name", "");
                ui.latest_tag = ui.latest;
                ui.download_url = d->get_str("url", "");
                ui.source_name = name;
            } else {
                if (v.is_array()) {
                    for (auto& r : v.items()) {
                        std::string tag = r.get_str("tag_name", "");
                        std::string lower = tag;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        bool pre = r.get_bool("prerelease", false);
                        bool is_beta = lower.find("beta") != std::string::npos;
                        bool is_nightly = lower.find("nightly") != std::string::npos
                                       || lower.find("alpha") != std::string::npos;
                        bool pick = false;
                        if (ver_type == "nightly") pick = is_nightly || pre;
                        else if (ver_type == "beta") pick = is_beta || (pre && !is_nightly);
                        else pick = !pre;
                        if (pick) { rel_storage = r; rel = &rel_storage; break; }
                    }
                    if (!rel) continue;
                } else {
                    rel_storage = v;
                    rel = &rel_storage;
                }
                ui.latest_tag = rel->get_str("tag_name", "");
                ui.latest = ui.latest_tag;
                ui.source_name = name;
                ui.release_notes = rel->get_str("body", "").substr(0, 2000);
                auto* assets = rel->find("assets");
                if (assets && assets->is_array()) {
                    for (auto& a : assets->items()) {
                        std::string n = a.get_str("name", "");
                        std::string lower = n;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        // 注意 "x86_64" 不含 "x64" 子串，必须显式枚举；并排除 arm 包
                        bool is_arm = lower.find("aarch64") != std::string::npos
                                   || lower.find("arm64") != std::string::npos;
                        bool is_x64 = lower.find("x86_64") != std::string::npos
                                   || lower.find("x64") != std::string::npos
                                   || lower.find("amd64") != std::string::npos;
                        if (!is_arm && is_x64
                            && lower.find("win") != std::string::npos
                            && lower.size() > 4 && lower.substr(lower.size() - 4) == ".zip") {
                            ui.download_url = a.get_str("browser_download_url", "");
                            ui.asset_name = n;
                            ui.asset_size = a.get_int("size", 0);
                            ui.asset_sha256 = a.get_str("digest", "");
                            break;
                        }
                    }
                    if (ui.download_url.empty()) {
                        for (auto& a : assets->items()) {
                            std::string n = a.get_str("name", "");
                            std::string lower = n;
                            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                            // 回退时同样排除 arm 包，避免在 x64 机器上拉到 aarch64 核心
                            bool is_arm = lower.find("aarch64") != std::string::npos
                                       || lower.find("arm64") != std::string::npos;
                            if (!is_arm
                                && lower.size() > 4 && lower.substr(lower.size() - 4) == ".zip") {
                                ui.download_url = a.get_str("browser_download_url", "");
                                ui.asset_name = n;
                                ui.asset_size = a.get_int("size", 0);
                                ui.asset_sha256 = a.get_str("digest", "");
                                break;
                            }
                        }
                    }
                }
            }
            if (ui.latest.empty()) continue;
            if (ui.download_url.empty()) continue;
            ui.ok = true;
            // 版本比较优先 tag vs tag：MaaEnd 的 release tag(v3.0.0) 与 interface.json 版本
            // (5.15.0-beta.1) 口径不同，用数字比较会永远判「有更新」
            if (!current_tag.empty()) {
                ui.has_update = normalize_ver(ui.latest_tag) != normalize_ver(current_tag);
            } else {
                ui.has_update = normalize_ver(ui.latest_tag) != normalize_ver(current);
                if (ui.has_update) ui.tag_mismatch = true;
            }
            return ui;
        }
        ui.error = "所有更新源均不可达（GitHub 与镜像均无法连接）";
        return ui;
    }

    // ---------------------------------------------------------------- 拉取核心
    // 下载 zip → 校验 sha256 → tar 解包 → 定位程序目录 → 登记为新核心（可设为默认）
    // progress: 0~100；返回核心 id（失败返回空）
    // installed_version: 输出探测到的版本
    // already_latest: 输出「同版本已存在，直接复用」标记（不重复下载占盘）
    static std::string pull_core(const std::string& kind, const std::string& url,
                                 const std::string& display_name,
                                 std::atomic<int>* progress = nullptr,
                                 std::string* stage = nullptr,
                                 const AppSettings* settings = nullptr,
                                 const std::string& release_tag = "",
                                 bool set_default = false,
                                 const std::string& expected_sha256 = "",
                                 std::string* installed_version = nullptr,
                                 bool* already_latest = nullptr) {
        auto set_stage = [&](const std::string& s) { if (stage) *stage = s; };
        set_stage("准备下载");

        std::string tmp = default_cores_root() + "\\.tmp";
        remove_tree(tmp);
        util::ensure_dir(tmp);
        std::string zip = tmp + "\\core.zip";
        if (!http_download(url, zip, progress, settings)) {
            set_stage("下载失败");
            return "";
        }
        // 完整性校验：上游给了 sha256 才校验（GitHub release asset digest）
        if (!expected_sha256.empty()) {
            set_stage("校验哈希");
            std::string want = expected_sha256;
            if (want.rfind("sha256:", 0) == 0) want = want.substr(7);
            std::transform(want.begin(), want.end(), want.begin(), ::tolower);
            std::string got = sha256_file(zip);
            if (got.empty() || got != want) {
                set_stage(got.empty() ? "哈希计算失败" : "哈希校验失败");
                return "";
            }
        }
        set_stage("解压中");
        std::string out_dir = tmp + "\\extract";
        util::ensure_dir(out_dir);
        // 用 Windows 自带 tar 解包 zip
        std::wstring cmd = L"tar.exe -xf \"" + util::utf8_to_wide(zip) + L"\" -C \""
                           + util::utf8_to_wide(out_dir) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdv(cmd.begin(), cmd.end());
        cmdv.push_back(L'\0');
        if (!CreateProcessW(nullptr, cmdv.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return "";
        }
        WaitForSingleObject(pi.hProcess, 300000);
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (ec != 0) return "";

        set_stage("校验中");
        // 在解包目录中查找核心目录（可能在子目录里）
        std::string core_dir = find_core_dir(kind, out_dir);
        if (core_dir.empty()) {
            set_stage("包内未找到核心目录");
            return "";
        }

        // 移入 MaaHub 内部 cores 目录：<cores_root>\<kind>-<version>
        CoreManager self(dir_of_state(), default_cores_root());
        std::string version = probe_version(kind, core_dir);
        if (installed_version) *installed_version = version;

        // 同版本 / 同 tag 已登记 → 直接复用（不重复占盘）
        std::string reuse_id;
        {
            std::lock_guard<std::mutex> lk(self.mtx_);
            for (auto& x : self.cores_) {
                if (x.kind != kind) continue;
                bool same = (!release_tag.empty() && x.release_tag == release_tag)
                         || (!version.empty() && x.version == version);
                if (same) { reuse_id = x.id; break; }
            }
        }
        if (!reuse_id.empty()) {
            // 回填 release_tag：MaaEnd 的 release tag(v3.0.0) 与包内 interface.json
            // 版本(5.15.0-beta.1) 口径不同，只有记下 tag 才能做 tag-vs-tag 比较，
            // 否则每次检查都会误报「有更新」。
            if (!release_tag.empty()) {
                {
                    std::lock_guard<std::mutex> lk(self.mtx_);
                    for (auto& x : self.cores_) {
                        if (x.id == reuse_id) { x.release_tag = release_tag; break; }
                    }
                }
                self.save();
            }
            if (set_default) self.set_default(reuse_id);
            if (already_latest) *already_latest = true;
            if (progress) *progress = 100;
            set_stage("已是最新（复用现有核心）");
            remove_tree(default_cores_root() + "\\.tmp");
            return reuse_id;
        }

        std::string dest = self.cores_root_ + "\\" + kind + "-"
                           + (version.empty() ? "unknown" : version);
        {
            std::lock_guard<std::mutex> lk(self.mtx_);
            for (auto& x : self.cores_) {
                if (x.path == dest) {
                    dest = self.cores_root_ + "\\" + kind + "-"
                           + (version.empty() ? "unknown" : version) + "-" + random_hex(4);
                    break;
                }
            }
        }
        // 先清理可能残留的同名目标
        remove_tree(dest);
        // MoveFileEx 支持跨卷（MOVEFILE_COPY_ALLOWED）
        if (!MoveFileExW(util::utf8_to_wide(core_dir).c_str(),
                         util::utf8_to_wide(dest).c_str(),
                         MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            set_stage("移入核心目录失败");
            return "";
        }
        CoreInfo c;
        c.id = "core-" + random_hex(8);
        c.kind = kind;
        c.path = dest;
        c.name = display_name.empty()
            ? (std::string(kind == "maa" ? "MAA 核心" : "MaaEnd 核心") + " " + version)
            : display_name;
        c.source = "download";
        c.release_tag = release_tag;
        c.created_at = util::now_sec();
        c.version = version;
        c.is_default = false;
        {
            std::lock_guard<std::mutex> lk(self.mtx_);
            self.cores_.push_back(c);
        }
        self.save();
        if (set_default) self.set_default(c.id);
        if (progress) *progress = 100;
        set_stage("完成");
        // 清掉 .tmp（zip + 解包副本，通常 >1GB），避免白占磁盘
        remove_tree(default_cores_root() + "\\.tmp");
        return c.id;
    }

    void refresh_version(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& c : cores_) {
            if (c.id == id) {
                c.version = probe_version(c.kind, c.path);
                c.updated_at = util::now_sec();
                break;
            }
        }
        save();
    }

    // ---------------------------------------------------------------- 工具
    // 文件 SHA-256（小写 hex）；失败返回空串
    static std::string sha256_file(const std::string& path) {
        // 诊断开关：设置 MAAHUB_DEBUG_SHA=1 时把每一步的失败原因打到 stderr
        const bool dbg = getenv("MAAHUB_DEBUG_SHA") != nullptr;
        // 刚下载完的大包常被杀软/索引器短暂占用，open 失败要重试而不是直接判失败
        HANDLE f = INVALID_HANDLE_VALUE;
        DWORD last_err = 0;
        for (int i = 0; i < 8; ++i) {
            f = CreateFileW(util::utf8_to_wide(path).c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) break;
            last_err = GetLastError();
            Sleep(250);
        }
        if (f == INVALID_HANDLE_VALUE) {
            if (dbg) fprintf(stderr, "[sha] open failed err=%lu path=%s\n",
                             last_err, path.c_str());
            return "";
        }
        BCRYPT_ALG_HANDLE alg = nullptr;
        NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (st != 0) {
            if (dbg) fprintf(stderr, "[sha] open alg failed st=0x%08lx\n", (unsigned long)st);
            CloseHandle(f);
            return "";
        }
        DWORD hash_len = 0, cb = 0;
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len, sizeof(hash_len), &cb, 0);
        BCRYPT_HASH_HANDLE h = nullptr;
        st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
        if (st != 0) {
            if (dbg) fprintf(stderr, "[sha] create hash failed st=0x%08lx\n", (unsigned long)st);
            BCryptCloseAlgorithmProvider(alg, 0);
            CloseHandle(f);
            return "";
        }
        {
            std::vector<BYTE> buf(1 << 20);
            DWORD n = 0;
            while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n > 0)
                BCryptHashData(h, buf.data(), n, 0);
            std::vector<BYTE> digest(hash_len ? hash_len : 32);
            st = BCryptFinishHash(h, digest.data(), (ULONG)digest.size(), 0);
            if (st == 0) {
                std::string out;
                char hex[4];
                for (BYTE b : digest) {
                    sprintf_s(hex, "%02x", b);
                    out += hex;
                }
                BCryptDestroyHash(h);
                BCryptCloseAlgorithmProvider(alg, 0);
                CloseHandle(f);
                return out;
            }
            if (dbg) fprintf(stderr, "[sha] finish failed st=0x%08lx\n", (unsigned long)st);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(f);
        return "";
    }

    static std::string normalize_ver(const std::string& v) {
        std::string out;
        for (char ch : v) {
            if ((ch >= '0' && ch <= '9') || ch == '.') out += ch;
        }
        while (!out.empty() && out.back() == '.') out.pop_back();
        return out;
    }

private:
    static std::string dir_of_state() {
        return util::exe_dir() + "\\state";
    }

    static std::string find_core_dir(const std::string& kind, const std::string& root) {
        // BFS 找包含核心标志文件的目录（最多 3 层）
        std::vector<std::string> queue{ root };
        for (int depth = 0; depth < 4 && !queue.empty(); ++depth) {
            std::vector<std::string> next;
            for (auto& d : queue) {
                if (valid_core_dir(kind, d)) return d;
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW(util::utf8_to_wide(d + "\\*").c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    std::wstring name = fd.cFileName;
                    if (name == L"." || name == L"..") continue;
                    next.push_back(d + "\\" + util::wide_to_utf8(name));
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            queue = next;
        }
        return "";
    }

    // 返回 false 代表登记表本身丢了（文件缺失/损坏），需要按磁盘重建
    bool load() {
        std::string content;
        if (!util::read_file(dir_ + "\\cores.json", content)) return false;
        mj::Value v;
        if (!mj::try_parse(content, v)) return false;
        if (auto* arr = v.find("cores"); arr && arr->is_array()) {
            for (auto& c : arr->items()) cores_.push_back(CoreInfo::from_json(c));
        }
        return true;
    }

    // cores 目录里的文件是主体，登记表只是索引；丢了索引不该让已下载的核心变成孤儿。
    // 只在 cores.json 读不出来时调用：文件存在但登记为空代表用户主动删除，尊重它。
    void rebuild_from_cores_dir() {
        std::vector<std::string> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(util::utf8_to_wide(cores_root_ + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            names.push_back(util::wide_to_utf8(name));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end());
        for (auto& name : names) {
            // 目录名固定是 <kind>-<version>；先判 maaend，"maa-" 前缀才不会抢走它
            std::string kind;
            for (const char* k : { "maaend", "maa" }) {
                size_t n = strlen(k);
                if (name.size() > n + 1 && name.compare(0, n, k) == 0 && name[n] == '-') {
                    kind = k;
                    break;
                }
            }
            if (kind.empty()) continue;
            std::string path = cores_root_ + "\\" + name;
            if (!valid_core_dir(kind, path)) continue;
            CoreInfo c;
            c.id = "core-" + random_hex(8);
            c.kind = kind;
            c.path = path;
            c.version = probe_version(kind, path);
            c.name = std::string(kind == "maa" ? "MAA 核心" : "MaaEnd 核心") + " " + c.version;
            c.source = "download";
            c.created_at = util::now_sec();
            bool has_same_kind = false;
            for (auto& x : cores_) {
                if (x.kind == kind) has_same_kind = true;
            }
            c.is_default = !has_same_kind;
            cores_.push_back(c);
        }
        if (!cores_.empty()) save();
    }

    void save() {
        mj::Value v = mj::Value::object();
        mj::Value arr = mj::Value::array();
        for (auto& c : cores_) arr.push(c.to_json());
        v.set("cores", std::move(arr));
        v.set("updated_at", util::now_sec());
        util::write_file(dir_ + "\\cores.json", mj::Value::dump_pretty(v));
    }

    static std::string random_hex(size_t bytes) {
        static std::mt19937_64 rng{ std::random_device{}() };
        std::string out;
        const char* hex = "0123456789abcdef";
        for (size_t i = 0; i < bytes; ++i) {
            unsigned v = (unsigned)(rng() & 0xFF);
            out += hex[v >> 4];
            out += hex[v & 15];
        }
        return out;
    }

    // ---------------------------------------------------------------- HTTP
    // 按设置打开 WinHTTP 会话：none=自动探测；http/socks5=命名代理
    static HINTERNET open_session(int timeout_ms, const AppSettings* settings) {
        std::wstring proxy, bypass;
        DWORD access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
        if (settings && settings->proxy_mode != "none"
            && !settings->proxy_host.empty() && settings->proxy_port > 0) {
            proxy = settings->winhttp_proxy();
            bypass = settings->winhttp_bypass();
            access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        }
        HINTERNET session = WinHttpOpen(L"MaaHub/1.0", access,
                                        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),
                                        bypass.empty() ? WINHTTP_NO_PROXY_BYPASS : bypass.c_str(),
                                        0);
        if (!session) return nullptr;
        WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        // 代理认证（如有）
        if (settings && !settings->proxy_user.empty()) {
            std::wstring u = util::utf8_to_wide(settings->proxy_user);
            std::wstring p = util::utf8_to_wide(settings->proxy_pass);
            WinHttpSetOption(session, WINHTTP_OPTION_PROXY_USERNAME,
                             (LPVOID)u.c_str(), (DWORD)(u.size() + 1) * 2);
            WinHttpSetOption(session, WINHTTP_OPTION_PROXY_PASSWORD,
                             (LPVOID)p.c_str(), (DWORD)(p.size() + 1) * 2);
        }
        return session;
    }
    static std::string http_get(const std::string& url, int timeout_ms,
                                const AppSettings* settings = nullptr,
                                const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        std::wstring host, path;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
        bool https = true;
        {
            std::string u = url;
            if (u.rfind("https://", 0) == 0) u = u.substr(8);
            else if (u.rfind("http://", 0) == 0) { u = u.substr(7); https = false; port = INTERNET_DEFAULT_HTTP_PORT; }
            size_t slash = u.find('/');
            std::string hostpart = slash == std::wstring::npos ? u : u.substr(0, slash);
            path = util::utf8_to_wide(slash == std::string::npos ? "/" : u.substr(slash));
            size_t colon = hostpart.find(':');
            if (colon != std::string::npos) {
                host = util::utf8_to_wide(hostpart.substr(0, colon));
                port = (INTERNET_PORT)atoi(hostpart.substr(colon + 1).c_str());
            } else {
                host = util::utf8_to_wide(hostpart);
            }
        }
        HINTERNET session = open_session(timeout_ms, settings);
        if (!session) return "";
        HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
        if (!conn) { WinHttpCloseHandle(session); return ""; }
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           https ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return ""; }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        std::wstring header_block;
        for (auto& [hk, hv] : headers)
            header_block += util::utf8_to_wide(hk) + L": " + util::utf8_to_wide(hv) + L"\r\n";
        if (!WinHttpSendRequest(req,
                                header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
                                header_block.empty() ? 0 : (DWORD)-1L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return "";
        }
        if (!WinHttpReceiveResponse(req, nullptr)) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return "";
        }
        // 状态码
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return "";
        }
        std::string out;
        char buf[16384];
        DWORD n = 0;
        while (WinHttpReadData(req, buf, sizeof(buf), &n) && n > 0) out.append(buf, n);
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return out;
    }

    static bool http_download(const std::string& url, const std::string& dest,
                              std::atomic<int>* progress,
                              const AppSettings* settings = nullptr) {
        std::wstring host, path;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
        bool https = true;
        {
            std::string u = url;
            if (u.rfind("https://", 0) == 0) u = u.substr(8);
            else if (u.rfind("http://", 0) == 0) { u = u.substr(7); https = false; port = INTERNET_DEFAULT_HTTP_PORT; }
            size_t slash = u.find('/');
            std::string hostpart = slash == std::wstring::npos ? u : u.substr(0, slash);
            path = util::utf8_to_wide(slash == std::string::npos ? "/" : u.substr(slash));
            size_t colon = hostpart.find(':');
            if (colon != std::string::npos) {
                host = util::utf8_to_wide(hostpart.substr(0, colon));
                port = (INTERNET_PORT)atoi(hostpart.substr(colon + 1).c_str());
            } else {
                host = util::utf8_to_wide(hostpart);
            }
        }
        HINTERNET session = open_session(120000, settings);
        if (!session) return false;
        // 下载用更短的超时：资产 CDN 不可达时要在半分钟内失败，而不是把后台线程
        // 永久挂住（ENG01 上实测 objects.githubusercontent.com 不通，旧代码会挂死）
        WinHttpSetTimeouts(session, 10000, 20000, 30000, 30000);
        HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
        if (!conn) { WinHttpCloseHandle(session); return false; }
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           https ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return false; }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(req, nullptr)) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return false;
        }
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return false;
        }
        uint64_t total = 0;
        wchar_t lenbuf[64];
        DWORD lensz = sizeof(lenbuf);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                lenbuf, &lensz, WINHTTP_NO_HEADER_INDEX)) {
            total = _wtoi64(lenbuf);
        }
        HANDLE f = CreateFileW(util::utf8_to_wide(dest).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return false;
        }
        uint64_t got = 0;
        char buf[16384];
        DWORD n = 0;
        while (WinHttpReadData(req, buf, sizeof(buf), &n) && n > 0) {
            DWORD w = 0;
            if (!WriteFile(f, buf, n, &w, nullptr)) break;
            got += w;
            if (progress && total) *progress = (int)(got * 100 / total);
        }
        CloseHandle(f);
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        if (progress) *progress = 100;
        return got > 0 && (total == 0 || got == total);
    }

    std::string dir_;
    std::string cores_root_;
    mutable std::mutex mtx_;
    std::vector<CoreInfo> cores_;
};

} // namespace hub
