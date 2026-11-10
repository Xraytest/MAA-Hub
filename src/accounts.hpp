// 多账号管理（与 Python 版 accounts.py 行为一致）
#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <map>
#include <random>
#include <algorithm>

#include "json.hpp"
#include "util.hpp"

namespace hub {

struct Account {
    std::string id;
    std::string label;
    std::string game;          // maa | maaend
    std::string identifier;    // MAA 登录名片段 / MaaEnd 邮箱或后四位
    std::string match_mode = "LastFourDigits";
    std::string server = "Official";
    bool enabled = true;
    std::string notes;
    double created_at = 0;
    double last_run_at = 0;
    std::string daily_done_date;
    int64_t total_runs = 0;
    std::string last_error;

    mj::Value to_json() const {
        mj::Value v = mj::Value::object();
        v.set("id", id);
        v.set("label", label);
        v.set("game", game);
        v.set("identifier", identifier);
        v.set("match_mode", match_mode);
        v.set("server", server);
        v.set("enabled", enabled);
        v.set("notes", notes);
        v.set("created_at", created_at);
        mj::Value st = mj::Value::object();
        st.set("last_run_at", last_run_at);
        st.set("daily_done_date", daily_done_date);
        st.set("total_runs", total_runs);
        st.set("last_error", last_error);
        v.set("state", std::move(st));
        return v;
    }

    static Account from_json(const mj::Value& v) {
        Account a;
        a.id = v.get_str("id", "");
        a.label = v.get_str("label", "");
        a.game = v.get_str("game", "");
        a.identifier = v.get_str("identifier", "");
        a.match_mode = v.get_str("match_mode", "LastFourDigits");
        a.server = v.get_str("server", "Official");
        a.enabled = v.get_bool("enabled", true);
        a.notes = v.get_str("notes", "");
        a.created_at = (double)v.get_int("created_at", 0);
        if (auto* st = v.find("state")) {
            a.last_run_at = (double)st->get_int("last_run_at", 0);
            a.daily_done_date = st->get_str("daily_done_date", "");
            a.total_runs = st->get_int("total_runs", 0);
            a.last_error = st->get_str("last_error", "");
        }
        return a;
    }
};

class AccountStore {
public:
    explicit AccountStore(std::string dir) : dir_(std::move(dir)) { load(); }

    std::vector<Account> list(const std::string& game = "") const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<Account> out;
        for (auto& a : accounts_) {
            if (game.empty() || a.game == game) out.push_back(a);
        }
        return out;
    }

    bool get(const std::string& id, Account& out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& a : accounts_) {
            if (a.id == id) { out = a; return true; }
        }
        return false;
    }

    Account add(const std::string& label, const std::string& game,
                const std::string& identifier, const std::string& match_mode,
                const std::string& server, const std::string& notes) {
        Account a;
        a.id = "acc_" + random_hex(8);
        a.label = label;
        a.game = game;
        a.identifier = identifier;
        a.match_mode = match_mode;
        a.server = server;
        a.notes = notes;
        a.created_at = util::now_sec();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            accounts_.push_back(a);
            // 该游戏没有有效当前账号时，首个账号自动设为当前
            bool exists = false;
            for (auto& x : accounts_) {
                if (x.id == active_[game]) { exists = true; break; }
            }
            if (!exists) active_[game] = a.id;
        }
        save();
        return a;
    }

    bool update(const std::string& id, const mj::Value& patch) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& a : accounts_) {
            if (a.id != id) continue;
            if (patch.has("label")) a.label = patch.get_str("label", a.label);
            if (patch.has("identifier")) a.identifier = patch.get_str("identifier", a.identifier);
            if (patch.has("match_mode")) a.match_mode = patch.get_str("match_mode", a.match_mode);
            if (patch.has("server")) a.server = patch.get_str("server", a.server);
            if (patch.has("enabled")) a.enabled = patch.get_bool("enabled", a.enabled);
            if (patch.has("notes")) a.notes = patch.get_str("notes", a.notes);
            save();
            return true;
        }
        return false;
    }

    bool remove(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = accounts_.begin(); it != accounts_.end(); ++it) {
            if (it->id != id) continue;
            std::string game = it->game;
            accounts_.erase(it);
            if (active_[game] == id) {
                active_[game] = "";
                for (auto& x : accounts_) {
                    if (x.game == game && x.enabled) { active_[game] = x.id; break; }
                }
            }
            save();
            return true;
        }
        return false;
    }

    bool active(const std::string& game, Account& out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = active_.find(game);
        if (it == active_.end() || it->second.empty()) return false;
        for (auto& a : accounts_) {
            if (a.id == it->second) { out = a; return true; }
        }
        return false;
    }

    bool set_active(const std::string& game, const std::string& id) {
        Account a;
        if (!get(id, a) || a.game != game) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        active_[game] = id;
        save();
        return true;
    }

    void mark_run(const std::string& id, const std::string& error = "") {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& a : accounts_) {
            if (a.id != id) continue;
            a.last_run_at = util::now_sec();
            a.total_runs++;
            a.last_error = error;
            break;
        }
        save();
    }

    void mark_daily_done(const std::string& id, const std::string& date = "") {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& a : accounts_) {
            if (a.id != id) continue;
            a.daily_done_date = date.empty() ? util::today_str() : date;
            break;
        }
        save();
    }

    // 账号注入：MAA StartUp.account_name / MaaEnd AccountSwitch.pipeline_override
    static mj::Value apply_to_task(const std::string& game, const std::string& task,
                                   const mj::Value& params, const Account* account) {
        mj::Value out = params.is_object() ? params : mj::Value::object();
        if (!account || !account->enabled || account->identifier.empty()) return out;
        if (game == "maa") {
            // catalog 默认值会把 account_name 预填成 ""，所以"缺失"和"空串"都按未指定处理
            if ((task == "StartUp" || task == "CloseDown") && out.get_str("account_name", "").empty()) {
                out.set("account_name", account->identifier);
            }
        } else if (game == "maaend") {
            if (task == "AccountSwitch") {
                mj::Value ov = out.has("pipeline_override") ? out["pipeline_override"] : mj::Value::object();
                if (account->match_mode == "Email") {
                    for (const char* node : { "__AccountSwitchOCRSelectAccount", "__AccountSwitchVerifySelected" }) {
                        mj::Value n = ov.has(node) ? ov[node] : mj::Value::object();
                        mj::Value attach = n.has("attach") ? n["attach"] : mj::Value::object();
                        attach.set("email", account->identifier);
                        n.set("attach", std::move(attach));
                        ov.set(node, std::move(n));
                    }
                } else {
                    mj::Value patterns = mj::Value::array();
                    patterns.push(".*[*\\d]\\s*" + account->identifier + "\\s*(?:最近)?.*");
                    patterns.push(".*" + account->identifier + "\\s*(?:最近)?.*");
                    patterns.push(account->identifier);
                    for (const char* node : { "__AccountSwitchOCRSelectAccount", "__AccountSwitchVerifySelected" }) {
                        mj::Value n = ov.has(node) ? ov[node] : mj::Value::object();
                        mj::Value reco = n.has("recognition") ? n["recognition"] : mj::Value::object();
                        mj::Value param = reco.has("param") ? reco["param"] : mj::Value::object();
                        param.set("expected", std::move(patterns));
                        reco.set("param", std::move(param));
                        n.set("recognition", std::move(reco));
                        ov.set(node, std::move(n));
                    }
                }
                out.set("pipeline_override", std::move(ov));
            }
        }
        return out;
    }

private:
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

    void load() {
        std::string content;
        if (!util::read_file(dir_ + "\\accounts.json", content)) return;
        mj::Value v;
        if (!mj::try_parse(content, v)) return;
        if (auto* arr = v.find("accounts"); arr && arr->is_array()) {
            for (auto& a : arr->items()) accounts_.push_back(Account::from_json(a));
        }
        if (auto* act = v.find("active"); act && act->is_object()) {
            for (auto& kv : act->members()) active_[kv.first] = kv.second.as_string();
        }
    }

    void save() {
        mj::Value v = mj::Value::object();
        v.set("revision", (int64_t)1);
        mj::Value arr = mj::Value::array();
        for (auto& a : accounts_) arr.push(a.to_json());
        v.set("accounts", std::move(arr));
        mj::Value act = mj::Value::object();
        for (auto& kv : active_) act.set(kv.first, kv.second);
        v.set("active", std::move(act));
        v.set("updated_at", util::now_sec());
        util::write_file(dir_ + "\\accounts.json", mj::Value::dump_pretty(v));
    }

    std::string dir_;
    mutable std::mutex mtx_;
    std::vector<Account> accounts_;
    std::map<std::string, std::string> active_;
};

} // namespace hub
