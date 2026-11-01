// 启发式规划器（执行器维度）：为每个未完成日常的执行器生成 唤醒/切号 → 日常 序列
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <random>
#include <windows.h>
#include <winhttp.h>

#include "json.hpp"
#include "util.hpp"

namespace hub {

struct PlanStep {
    std::string exec_id;        // 目标执行器
    std::string kind;           // maa | maaend（用于目录校验）
    std::string exec_label;
    std::string task;
    mj::Value params = mj::Value::object();
    std::string reason;
    int priority = 0;
    double est_minutes = 0;

    mj::Value to_json() const {
        mj::Value v = mj::Value::object();
        v.set("exec_id", exec_id);
        v.set("kind", kind);
        v.set("exec_label", exec_label);
        v.set("task", task);
        v.set("params", params);
        v.set("reason", reason);
        v.set("priority", priority);
        if (est_minutes > 0) v.set("est_minutes", est_minutes);
        return v;
    }
};

struct Plan {
    std::string id;
    double ts = 0;
    std::string planner;
    std::string goal;
    std::string rationale;
    std::vector<PlanStep> steps;
    std::vector<std::string> warnings;
    std::string status = "proposed";

    mj::Value to_json() const {
        mj::Value v = mj::Value::object();
        v.set("id", id);
        v.set("ts", ts);
        v.set("planner", planner);
        v.set("goal", goal);
        v.set("rationale", rationale);
        mj::Value arr = mj::Value::array();
        for (auto& s : steps) arr.push(s.to_json());
        v.set("steps", std::move(arr));
        mj::Value w = mj::Value::array();
        for (auto& s : warnings) w.push(s);
        v.set("warnings", std::move(w));
        v.set("status", status);
        return v;
    }
};

class HeuristicPlanner {
public:
    Plan plan(const std::string& goal, const mj::Value& state, const mj::Value& catalog) {
        Plan p;
        p.id = "plan_" + random_hex(6);
        p.ts = util::now_sec();
        p.planner = "heuristic";
        p.goal = goal;
        p.rationale = "启发式规则：为每个未完成日常的执行器生成 唤醒/切号 → 日常清理 序列；"
                      "执行器未连接时在 warnings 中说明，不生成步骤。";

        const mj::Value* execs = state.find("executors");
        if (!execs || !execs->is_array()) {
            p.warnings.push_back("尚未创建任何执行器，请先在左侧创建 MAA / MaaEnd 实例");
            return p;
        }

        auto task_exists = [&](const std::string& kind, const std::string& name) {
            if (!catalog.has(kind)) return false;
            const mj::Value& arr = catalog[kind];
            if (!arr.is_array()) return false;
            for (auto& t : arr.items()) {
                if (t.get_str("name", "") == name) return true;
            }
            return false;
        };

        static const char* maa_daily[] = { "Award", "Mall", "Infrast", "Recruit" };
        static const char* end_daily[] = { "DailyRewards", "CreditShopping", "AutoCollect",
                                           "ProtocolSpace", "GiftOperator", "VisitFriends",
                                           "BatchAddFriends", "ReadAllWiki" };

        int pending_total = 0;
        for (auto& ex : execs->items()) {
            std::string id = ex.get_str("id", "");
            std::string kind = ex.get_str("kind", "");
            std::string label = ex.get_str("label", "");
            bool enabled = ex.get_bool("enabled", true);
            std::string conn = ex.get_str("conn", "unknown");
            bool daily_done = ex.get_bool("daily_done_today", false);
            std::string acc_label = ex.get_str("account_label", "");

            if (!enabled) continue;
            if (conn != "connected") {
                p.warnings.push_back("执行器「" + label + "」未连接（游戏未启动），跳过。"
                                     + (daily_done ? std::string("（今日日常已完成）")
                                                   : std::string("（今日日常未完成）")));
                continue;
            }
            if (daily_done) {
                p.warnings.push_back("执行器「" + label + "」今日日常已完成，跳过");
                continue;
            }
            ++pending_total;

            auto add = [&](const std::string& task, mj::Value params, const std::string& reason) {
                PlanStep s;
                s.exec_id = id;
                s.kind = kind;
                s.exec_label = label;
                s.task = task;
                s.params = std::move(params);
                s.reason = reason;
                s.priority = 10 - (int)p.steps.size();
                p.steps.push_back(std::move(s));
            };

            if (kind == "maa") {
                if (task_exists("maa", "StartUp")) {
                    mj::Value params = mj::Value::object();
                    params.set("client_type", "Official");
                    params.set("start_game_enabled", true);
                    add("StartUp", std::move(params),
                        acc_label.empty() ? "唤醒游戏" : "唤醒游戏并切换到 " + acc_label);
                }
                for (auto* name : maa_daily) {
                    if (!task_exists("maa", name)) continue;
                    add(name, maa_params(name), "日常清理（" + label + "）");
                }
            } else {
                if (!acc_label.empty() && task_exists("maaend", "AccountSwitch")) {
                    add("AccountSwitch", mj::Value::object(), "切换到账号 " + acc_label);
                }
                for (auto* name : end_daily) {
                    if (!task_exists("maaend", name)) continue;
                    add(name, mj::Value::object(), "终末地日常清理（" + label + "）");
                }
            }
        }

        if (pending_total == 0 && p.steps.empty()) {
            p.warnings.push_back("没有需要处理的执行器（未连接或日常已完成）");
        }
        if (p.steps.size() > 12) p.steps.resize(12);
        return p;
    }

private:
    static mj::Value maa_params(const std::string& name) {
        mj::Value v = mj::Value::object();
        if (name == "Infrast") {
            mj::Value f = mj::Value::array();
            for (auto* x : { "Mfg", "Trade", "Control", "Power", "Reception", "Office", "Dorm" }) {
                f.push(x);
            }
            v.set("facility", std::move(f));
            v.set("drones", "Money");
        } else if (name == "Recruit") {
            mj::Value sel = mj::Value::array();
            sel.push(5); sel.push(4);
            mj::Value conf = mj::Value::array();
            conf.push(4); conf.push(3);
            v.set("select", std::move(sel));
            v.set("confirm", std::move(conf));
            v.set("times", 4);
        } else if (name == "Mall") {
            v.set("visit_friends", true);
            v.set("shopping", true);
        }
        return v;
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
};

// ---------------------------------------------------------------- LLM 规划器
class LlmPlanner {
public:
    struct Config {
        bool enabled = false;
        std::string base_url = "https://api.deepseek.com";
        std::string api_key;
        std::string model = "deepseek-chat";
        double temperature = 0.2;
        int max_tokens = 2000;
        int timeout_sec = 60;
    };

    LlmPlanner(Config cfg, HeuristicPlanner fallback)
        : cfg_(std::move(cfg)), fallback_(std::move(fallback)) {}

    Plan plan(const std::string& goal, const mj::Value& state, const mj::Value& catalog,
              const std::vector<mj::Value>& accounts, int max_steps = 12) {
        std::string content = chat(build_prompt(goal, state, catalog, accounts, max_steps));
        if (content.empty()) {
            Plan p = fallback_.plan(goal, state, catalog);
            p.warnings.push_back("LLM 调用失败，已回退启发式规划: " + last_error_);
            return p;
        }
        mj::Value parsed;
        if (!mj::try_parse(content, parsed)) {
            Plan p = fallback_.plan(goal, state, catalog);
            p.warnings.push_back("LLM 输出不是合法 JSON，已回退启发式规划");
            return p;
        }
        auto [steps, warnings] = validate(parsed, catalog, state, max_steps);
        if (steps.empty()) {
            Plan p = fallback_.plan(goal, state, catalog);
            p.warnings.push_back("LLM 未产出有效步骤，已回退启发式规划");
            return p;
        }
        Plan p;
        p.id = "plan_llm_" + random_hex(6);
        p.ts = util::now_sec();
        p.planner = "llm";
        p.goal = goal;
        p.rationale = parsed.get_str("rationale", "");
        p.steps = std::move(steps);
        p.warnings = std::move(warnings);
        if (auto* w = parsed.find("warnings"); w && w->is_array()) {
            for (auto& x : w->items()) p.warnings.push_back(x.as_string());
        }
        return p;
    }

private:
    // ---------------------------------------------------------------- HTTP
    std::string chat(const std::string& user_prompt) {
        HINTERNET session = WinHttpOpen(L"MaaHub/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { last_error_ = "WinHttpOpen failed"; return {}; }
        DWORD timeout = (DWORD)cfg_.timeout_sec * 1000;
        WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

        std::wstring host, path;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
        bool https = true;
        {
            std::string u = cfg_.base_url;
            if (u.rfind("https://", 0) == 0) u = u.substr(8);
            else if (u.rfind("http://", 0) == 0) { u = u.substr(7); https = false; port = INTERNET_DEFAULT_HTTP_PORT; }
            size_t slash = u.find('/');
            std::string hostpart = slash == std::string::npos ? u : u.substr(0, slash);
            path = util::utf8_to_wide(slash == std::string::npos ? "/chat/completions" : u.substr(slash) + "/chat/completions");
            size_t colon = hostpart.find(':');
            if (colon != std::string::npos) {
                host = util::utf8_to_wide(hostpart.substr(0, colon));
                port = (INTERNET_PORT)atoi(hostpart.substr(colon + 1).c_str());
            } else {
                host = util::utf8_to_wide(hostpart);
            }
        }

        HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
        if (!conn) { WinHttpCloseHandle(session); last_error_ = "WinHttpConnect failed"; return {}; }
        HINTERNET req = WinHttpOpenRequest(conn, L"POST", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           https ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); last_error_ = "WinHttpOpenRequest failed"; return {}; }

        mj::Value body = mj::Value::object();
        body.set("model", cfg_.model);
        mj::Value msgs = mj::Value::array();
        mj::Value sys = mj::Value::object();
        sys.set("role", "system");
        sys.set("content", system_prompt());
        msgs.push(std::move(sys));
        mj::Value usr = mj::Value::object();
        usr.set("role", "user");
        usr.set("content", user_prompt);
        msgs.push(std::move(usr));
        body.set("messages", std::move(msgs));
        body.set("temperature", cfg_.temperature);
        body.set("max_tokens", (int64_t)cfg_.max_tokens);
        mj::Value rf = mj::Value::object();
        rf.set("type", "json_object");
        body.set("response_format", std::move(rf));
        std::string body_str = mj::Value::dump(body);

        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer "
                               + util::utf8_to_wide(cfg_.api_key) + L"\r\n";
        BOOL ok = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1,
                                     (LPVOID)body_str.data(), (DWORD)body_str.size(),
                                     (DWORD)body_str.size(), 0);
        if (!ok || !WinHttpReceiveResponse(req, nullptr)) {
            last_error_ = "request failed err=" + std::to_string(GetLastError());
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return {};
        }
        std::string resp;
        char buf[8192];
        DWORD n = 0;
        while (WinHttpReadData(req, buf, sizeof(buf), &n) && n > 0) resp.append(buf, n);
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);

        mj::Value j;
        if (!mj::try_parse(resp, j)) { last_error_ = "bad response json"; return {}; }
        auto* choices = j.find("choices");
        if (!choices || !choices->is_array() || choices->items().empty()) {
            last_error_ = j.get_str("error", "no choices in response");
            return {};
        }
        if (auto* m = choices->items()[0].find("message")) {
            return m->get_str("content", "");
        }
        last_error_ = "no message content";
        return {};
    }

    static std::string system_prompt() {
        return "你是 MaaHub 的自动化规划引擎。MaaHub 统一管理多个游戏自动化实例（执行器），支持多账号并行。\n"
               "每个执行器是一个独立的 MAA（明日方舟）或 MaaEnd（明日方舟：终末地）实例，绑定一个账号。\n"
               "- kind=\"maa\"：任务为 StartUp/CloseDown/Fight/Recruit/Infrast/Mall/Award/Roguelike/Copilot/Depot/OperBox/Reclamation/Custom\n"
               "- kind=\"maaend\"：任务为 AccountSwitch/DailyRewards/CreditShopping/AutoCollect/ProtocolSpace 等\n\n"
               "你会收到：state.executors（执行器列表：id/kind/label/conn/daily_done_today/account_label）、"
               "state.accounts（账号）、catalog（按 kind 的任务目录）、goal（用户目标）。\n\n"
               "规则：\n"
               "- 只使用 catalog 中真实存在的 kind + task，禁止发明任务名\n"
               "- 每个步骤必须带 exec_id（来自 state.executors 的 id）\n"
               "- 只为已连接（conn=connected）且今日日常未完成（daily_done_today=false）的执行器规划日常\n"
               "- MAA 用 StartUp 切号；MaaEnd 用 AccountSwitch 切号\n"
               "- params 只填该任务声明过的参数，不确定留空 {}\n"
               "- 步骤顺序：先唤醒/切号，再日常，最后消耗体力\n"
               "- 只输出 JSON：{\"rationale\":\"...\",\"steps\":[{\"exec_id\":\"exec_xxx\",\"kind\":\"maa\",\"task\":\"StartUp\",\"params\":{},\"reason\":\"...\",\"priority\":10}],\"warnings\":[]}";
    }

    static std::string build_prompt(const std::string& goal, const mj::Value& state,
                                    const mj::Value& catalog,
                                    const std::vector<mj::Value>& accounts, int max_steps) {
        mj::Value slim = mj::Value::object();
        if (auto* execs = state.find("executors")) {
            mj::Value arr = mj::Value::array();
            for (auto& e : execs->items()) {
                mj::Value one = mj::Value::object();
                for (auto* k : { "id", "kind", "label", "conn", "run", "daily_done_today",
                                 "account_label", "version", "last_error" }) {
                    if (auto* v = e.find(k)) one.set(k, *v);
                }
                arr.push(std::move(one));
            }
            slim.set("executors", std::move(arr));
        }
        mj::Value accs = mj::Value::array();
        for (auto& a : accounts) accs.push(a);
        slim.set("accounts", std::move(accs));
        slim.set("catalog", catalog);
        slim.set("goal", goal);
        slim.set("max_steps", max_steps);
        return mj::Value::dump(slim);
    }

    // ---------------------------------------------------------------- 校验
    static std::pair<std::vector<PlanStep>, std::vector<std::string>>
    validate(const mj::Value& parsed, const mj::Value& catalog, const mj::Value& state,
             int max_steps) {
        std::vector<PlanStep> steps;
        std::vector<std::string> warnings;
        auto* raw_steps = parsed.find("steps");
        if (!raw_steps || !raw_steps->is_array()) return { steps, warnings };

        // 执行器索引：id -> {kind, label, conn, daily_done}
        std::map<std::string, const mj::Value*> exec_map;
        if (auto* execs = state.find("executors"); execs && execs->is_array()) {
            for (auto& e : execs->items()) {
                exec_map[e.get_str("id", "")] = &e;
            }
        }

        for (size_t i = 0; i < raw_steps->items().size() && i < (size_t)max_steps; ++i) {
            const mj::Value& raw = raw_steps->items()[i];
            std::string exec_id = raw.get_str("exec_id", "");
            std::string task = raw.get_str("task", "");
            auto eit = exec_map.find(exec_id);
            if (eit == exec_map.end()) {
                warnings.push_back("步骤" + std::to_string(i + 1) + ": 执行器 " + exec_id + " 不存在，已忽略");
                continue;
            }
            const mj::Value& ex = *eit->second;
            std::string kind = ex.get_str("kind", "");
            std::string label = ex.get_str("label", "");
            // 目录校验
            const mj::Value* spec = nullptr;
            if (catalog.has(kind) && catalog[kind].is_array()) {
                for (auto& t : catalog[kind].items()) {
                    if (t.get_str("name", "") == task) { spec = &t; break; }
                }
            }
            if (!spec) {
                warnings.push_back("步骤" + std::to_string(i + 1) + ": " + kind
                                   + " 无任务 " + task + "，已忽略");
                continue;
            }
            PlanStep s;
            s.exec_id = exec_id;
            s.kind = kind;
            s.exec_label = label;
            s.task = task;
            s.reason = raw.get_str("reason", "");
            s.priority = (int)raw.get_int("priority", 0);
            s.est_minutes = (double)raw.get_int("est_minutes", 0);
            if (auto* params = raw.find("params"); params && params->is_object()) {
                if (auto* decl = spec->find("params"); decl && decl->is_array()) {
                    for (auto& kv : params->members()) {
                        bool known = false;
                        for (auto& d : decl->items()) {
                            if (d.get_str("name", "") == kv.first) { known = true; break; }
                        }
                        if (known) s.params.set(kv.first, kv.second);
                        else warnings.push_back("步骤" + std::to_string(i + 1) + " " + task
                                                + ": 参数 " + kv.first + " 不在目录中，已剔除");
                    }
                }
            }
            steps.push_back(std::move(s));
        }
        return { steps, warnings };
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

    Config cfg_;
    HeuristicPlanner fallback_;
    std::string last_error_;
};

} // namespace hub
