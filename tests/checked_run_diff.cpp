// r9 门4差分测试：action_run_from/action_run_checked 的「已勾选则运行」循环体
// 旧版（两处内联逐字节块） vs 新版（MaaHubWindow::run_item_if_checked）
// 比对维度：返回值(started增量)、once复位行为、传给 run_on_executor 的 (id,name,params)。
// 预期 mismatches = 0。
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// ---- 极简 mock：只复刻被改逻辑用到的部分 ----
struct Value {  // 简化 mj::Value：仅记录 set 的 kv
    std::vector<std::pair<std::string, std::string>> kv;
    static Value object() { return Value(); }
    void set(const std::string& k, const std::string& v) { kv.emplace_back(k, v); }
    bool operator==(const Value& o) const { return kv == o.kv; }
};
struct TaskUiState {
    bool checked = false, once = false, selected = false;
    std::map<std::string, Value> params;
};
struct CallLog { std::string id, name; Value params; };

struct HubMock {
    std::vector<CallLog> calls;
    int64_t next_ret = 1;                 // 可控的 run_on_executor 返回值
    int64_t run_on_executor(const std::string& id, const std::string& t, const Value& p) {
        calls.push_back({id, t, p});
        return next_ret;
    }
};

// ---- 旧版：逐字节复刻原内联块（以 vector 返回 started 并就地改 ui）----
int old_block(HubMock& hub, std::map<std::string, std::map<std::string, TaskUiState>>& ui,
              const std::string& id, const std::vector<std::string>& names) {
    int started = 0;
    for (auto& name : names) {
        auto it = ui.find(id);
        if (it == ui.end()) continue;
        auto tit = it->second.find(name);
        if (tit == it->second.end()) continue;
        if (!tit->second.checked && !tit->second.once) continue;
        Value params = Value::object();
        for (auto& kv : tit->second.params) params.set(kv.first, kv.second);
        if (hub.run_on_executor(id, name, params) > 0) {
            ++started;
            tit->second.once = false;
        }
    }
    return started;
}

// ---- 新版：与 app_window.cpp 的 run_item_if_checked 同逻辑 ----
bool run_item_if_checked(HubMock& hub, std::map<std::string, std::map<std::string, TaskUiState>>& ui,
                         const std::string& id, const std::string& name) {
    auto it = ui.find(id);
    if (it == ui.end()) return false;
    auto tit = it->second.find(name);
    if (tit == it->second.end()) return false;
    if (!tit->second.checked && !tit->second.once) return false;
    Value params = Value::object();
    for (auto& kv : tit->second.params) params.set(kv.first, kv.second);
    if (hub.run_on_executor(id, name, params) > 0) {
        tit->second.once = false;
        return true;
    }
    return false;
}
int new_block(HubMock& hub, std::map<std::string, std::map<std::string, TaskUiState>>& ui,
              const std::string& id, const std::vector<std::string>& names) {
    int started = 0;
    for (auto& name : names)
        if (run_item_if_checked(hub, ui, id, name)) ++started;
    return started;
}

// ---- 测试场景构造与比对 ----
struct StateSnap {
    int started; std::vector<CallLog> calls;
    std::map<std::string, std::map<std::string, TaskUiState>> ui;
};
StateSnap run_case(bool use_new, const std::map<std::string, std::map<std::string, TaskUiState>>& ui0,
                   const std::string& id, const std::vector<std::string>& names, int64_t ret) {
    auto ui = ui0; HubMock hub; hub.next_ret = ret;
    StateSnap s;
    s.started = use_new ? new_block(hub, ui, id, names) : old_block(hub, ui, id, names);
    s.calls = hub.calls; s.ui = ui; return s;
}
std::string render(const StateSnap& s) {
    std::string r = "started=" + std::to_string(s.started) + ";calls=" + std::to_string(s.calls.size());
    for (auto& c : s.calls) {
        r += "|" + c.id + "," + c.name + ",";
        for (auto& kv : c.params.kv) r += kv.first + ";";
    }
    for (auto& ex : s.ui) for (auto& tk : ex.second)
        r += ";ui:" + ex.first + "/" + tk.first + "=" +
             (tk.second.checked ? "C" : "-") + (tk.second.once ? "O" : "-") +
             std::to_string(tk.second.params.size());
    return r;
}

int main() {
    int mismatches = 0, cases = 0;
    auto check = [&](const std::string& id, const std::map<std::string, std::map<std::string, TaskUiState>>& u,
                     const std::vector<std::string>& names, int64_t ret, const char* label) {
        ++cases;
        auto so = run_case(false, u, id, names, ret);
        auto sn = run_case(true, u, id, names, ret);
        if (render(so) != render(sn)) {
            ++mismatches;
            std::printf("MISMATCH %s\n  old=%s\n  new=%s\n", label, render(so).c_str(), render(sn).c_str());
        } else {
            std::printf("ok  %s -> %s\n", label, render(sn).c_str());
        }
    };

    std::map<std::string, std::map<std::string, TaskUiState>> ui;
    TaskUiState a; a.checked = true; a.params["stage"] = Value{};
    TaskUiState b; b.once = true;  b.params["times"] = Value{}; b.params["refresh"] = Value{};
    TaskUiState c; /* 全未勾选 */
    TaskUiState d; d.checked = true; d.once = true;  // 勾选+仅一次
    ui["exec1"]["Fight"] = a; ui["exec1"]["Recruit"] = b;
    ui["exec1"]["Mall"] = c;  ui["exec1"]["Award"] = d;
    ui["exec2"]["Fight"] = a;

    check("exec1", ui, {"Fight", "Recruit", "Mall", "Award"}, 1, "all-check-run-ok");
    check("exec1", ui, {"Fight", "Recruit", "Mall", "Award"}, 0, "run-fails-ret0");
    check("exec1", ui, {"Fight", "Recruit", "Mall", "Award"}, -1, "run-fails-ret-neg");
    check("exec1", ui, {"Award"}, 1, "only-once-and-checked");
    check("exec1", ui, {"Recruit", "Recruit"}, 1, "once-double-run");
    check("ghost", ui, {"Fight"}, 1, "exec-not-found");
    check("exec1", ui, {"Ghost"}, 1, "task-not-found");
    check("exec1", ui, {}, 1, "empty-names");
    check("exec2", ui, {"Fight", "Mall"}, 1, "exec2-partial");

    std::printf("cases=%d mismatches=%d\n", cases, mismatches);
    return mismatches == 0 ? 0 : 1;
}
