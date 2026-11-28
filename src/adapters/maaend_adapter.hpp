// MaaEnd (明日方舟：终末地) 适配器 —— 直接调用 MaaFramework.dll / MaaAgentClient.dll
// 签名依据：maa-framework-sys 5.14.3 crate bindings（bindgen 自 MaaFramework 5.x 头文件）
#include "../adapters/adapter.hpp"
#include "../util.hpp"

#include <windows.h>
#include <random>
#include <algorithm>

namespace hub {

// MaaStatusEnum
constexpr int MAA_STATUS_SUCCEEDED = 3000;

class MaaEndAdapter : public IAdapter {
public:
    MaaEndAdapter() { kind_ = "maaend"; g_instance = this; }
    ~MaaEndAdapter() override { if (g_instance == this) g_instance = nullptr; }

    // ------------------------------------------------------------ 发现
    bool discover(const mj::Value& cfg) override {
        cfg_ = cfg;
        st_.kind = "maaend";
        st_.name = cfg.get_str("display_name", "MaaEnd");
        st_.install_dir = cfg.get_str("install_dir", "");
        st_.core_dll = st_.install_dir + "\\maafw\\MaaFramework.dll";
        if (!util::file_exists(st_.core_dll)) {
            st_.available = false;
            st_.error = "MaaFramework.dll not found in " + st_.install_dir + "\\maafw";
            return false;
        }
        st_.available = true;
        st_.error.clear();
        // interface.json
        std::string iface_path = st_.install_dir + "\\interface.json";
        std::string content;
        if (util::read_file(iface_path, content)) {
            mj::Value v;
            if (mj::try_parse(content, v)) interface_ = std::move(v);
        }
        // 先设置本地化目录，scan_tasks 才能注入官方中文标签
        set_label_install_dir(st_.install_dir + "\\locales");
        catalog_ = scan_tasks();
        st_.task_count = (int64_t)catalog_.size();
        return true;
    }

    // ------------------------------------------------------------ 任务目录
    // 把一个 task 引用的 option 转成 UI 可用的参数描述列表。
    // option 定义常常在别的 import 文件里，所以传进来的是合并后的 option 表。
    static mj::Value task_option_specs(const mj::Value& task, const mj::Value& all_opts) {
        mj::Value arr = mj::Value::array();
        if (!all_opts.is_object()) return arr;
        const mj::Value* refs = task.find("option");
        if (!refs || !refs->is_array()) return arr;
        const std::string tn = task.get_str("name", "");
        for (auto& r : refs->items()) {
            if (!r.is_string()) continue;
            std::string key = r.as_string();
            auto it = all_opts.members().find(key);
            if (it == all_opts.members().end()) continue;
            const mj::Value& spec = it->second;
            if (!spec.is_object()) continue;
            mj::Value item = mj::Value::object();
            item.set("name", mj::Value(key));
            item.set("type", mj::Value(option_ui_type(spec.get_str("type", ""))));
            // 参数名：官方 "$..." 引用 > 语言包直查 > 键名兜底
            std::string lbl = zh_ref(spec.get_str("label", ""));
            if (lbl.empty()) lbl = option_label(tn, key);
            item.set("description", mj::Value(lbl.empty() ? key : lbl));
            if (auto* cases = spec.find("cases"); cases && cases->is_array()) {
                mj::Value names = mj::Value::array();
                const mj::Value* def_value = spec.find("default_case");
                std::string def_case = def_value && def_value->is_string()
                    ? def_value->as_string() : "";
                std::string def_label;
                for (auto& cs : cases->items()) {
                    std::string cn = cs.is_object() ? cs.get_str("name", "")
                                                    : (cs.is_string() ? cs.as_string() : "");
                    if (cn.empty()) continue;
                    // cases[].label 同样是 "$..." 引用，缺失时回退语言包/枚举名
                    std::string cl = cs.is_object() ? zh_ref(cs.get_str("label", "")) : "";
                    if (cl.empty()) cl = case_label(tn, key, cn);
                    if (cl.empty()) cl = cn;
                    if (cn == def_case) def_label = cl;
                    names.push(cl);
                }
                if (!names.items().empty()) item.set("cases", std::move(names));
                // 默认值要和 cases 的展示文案对齐，否则面板打开时选不中默认项；
                // switch/checkbox 落成 bool，勾选框才能显示官方默认态
                if (!def_case.empty()) {
                    if (item.get_str("type", "") == "bool") {
                        std::string dc = def_case;
                        for (auto& ch : dc) ch = (char)tolower((unsigned char)ch);
                        item.set("default", mj::Value(dc == "yes" || dc == "true"
                                                          || dc == "on" || dc == "1"));
                    } else if (!def_label.empty()) {
                        item.set("default", mj::Value(def_label));
                    }
                } else if (spec.get_str("type", "") == "checkbox"
                           && def_value && def_value->is_array()) {
                    item.set("default", *def_value);
                }
            }
            if (!item.has("default") && spec.has("default")) item.set("default", spec["default"]);
            if (!item.has("default")) {
                // input 型把首个子输入的默认值带出来（"60" 之类）
                if (auto* ins = spec.find("inputs"); ins && ins->is_array()
                        && !ins->items().empty() && ins->items()[0].is_object()) {
                    auto& i0 = ins->items()[0];
                    if (i0.has("default")) item.set("default", i0["default"]);
                }
            }
            arr.push(std::move(item));
        }
        return arr;
    }

    // ------------------------------------------------------------ 本地化文案
    // locales/<子目录>/zh_cn.json 是扁平点号键（约 3000 条）。官方界面用
    // "$task.X.option.Y.label" 这类引用取文案，逐条猜键名会漏掉 $option.* /
    // $global.* / 省略 .label 的写法，参数就只能退化成英文键名。
    static std::vector<std::string> locale_files() {
        std::vector<std::string> files;
        std::string root = label_dir_ref();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(util::utf8_to_wide(root + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return files;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::string sub = root + "\\" + util::wide_to_utf8(n);
            WIN32_FIND_DATAW fd2;
            HANDLE h2 = FindFirstFileW(util::utf8_to_wide(sub + "\\zh_cn.json").c_str(), &fd2);
            if (h2 != INVALID_HANDLE_VALUE) {
                files.push_back(sub + "\\zh_cn.json");
                FindClose(h2);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return files;
    }

    static const std::map<std::string, std::string>& locale_flat() {
        static std::map<std::string, std::string> m;
        // r24：按 locale 目录名失效，进程内切换核心目录后重新解析（原 loaded 标志
        // 一旦置位就永久锁在第一次的 install_dir 上）
        static std::string loaded_dir;
        const std::string cur_dir = label_dir_ref();
        if (loaded_dir != cur_dir) {
            loaded_dir = cur_dir;
            m.clear();
            for (auto& fp : locale_files()) {
                std::string content;
                if (!util::read_file(fp, content)) continue;
                mj::Value j;
                if (!mj::try_parse(content, j)) continue;
                for (auto& kv : j.members())
                    if (kv.second.is_string()) m[kv.first] = kv.second.as_string();
            }
        }
        return m;
    }

    // "$a.b.c" -> 文案；再试补 ".label" 与 task.X.Y -> task.X.option.Y.label。
    // 不以 $ 开头的本身就是一句文案，原样返回。
    static std::string zh_ref(const std::string& ref) {
        if (ref.empty()) return "";
        if (ref[0] != '$') return ref;
        const std::string key = ref.substr(1);
        const auto& m = locale_flat();
        auto it = m.find(key);
        if (it != m.end()) return it->second;
        auto it2 = m.find(key + ".label");
        if (it2 != m.end()) return it2->second;
        if (key.rfind("task.", 0) == 0) {
            size_t dot = key.find('.', 5);
            if (dot != std::string::npos) {
                std::string tail = key.substr(dot + 1);
                if (tail.rfind("option.", 0) != 0) {
                    auto it3 = m.find("task." + key.substr(5, dot - 5) + ".option." + tail + ".label");
                    if (it3 != m.end()) return it3->second;
                }
            }
        }
        return "";
    }

    // 官方 option 中文标签：读 <install_dir>\locales\**\zh_cn.json
    // 键优先级：option.<key>.label > task.<task>.option.<key>.label > 空（回退 key）
    static std::string option_label(const std::string& task, const std::string& key) {
        static std::map<std::string, std::string> cache;
        // r24：`static bool loaded` 会把标签永久锁在首次的 install_dir；改为按目录失效
        static std::string loaded_dir;
        const std::string cur_dir = label_dir_ref();
        if (loaded_dir != cur_dir) {
            loaded_dir = cur_dir;
            cache.clear();
            load_option_labels(cache);
        }
        std::string ck = task + "|" + key;
        auto ci = cache.find(ck);
        if (ci != cache.end()) return ci->second;
        auto it = cache.find(ck);
        std::string v = it == cache.end() ? "" : it->second;
        cache[ck] = v;
        return v;
    }

    // 大小写/分隔符/UTC-GMT 变体归一化，缓解不同版本间键名差异
    static std::string norm_case_key(const std::string& s) {
        std::string o;
        for (char ch : s) {
            if (ch == '_' || ch == '-' || ch == ' ') continue;
            o += (char)toupper((unsigned char)ch);
        }
        // UTC 与 GMT 在不同版本间混用
        size_t p;
        while ((p = o.find("UTC")) != std::string::npos) o.replace(p, 3, "GMT");
        return o;
    }

    static std::string case_label(const std::string& task, const std::string& key,
                                  const std::string& case_name) {
        static std::map<std::string, std::string> cache;
        // r24：`static bool loaded` 会把标签永久锁在首次的 install_dir；改为按目录失效
        static std::string loaded_dir;
        const std::string cur_dir = label_dir_ref();
        if (loaded_dir != cur_dir) {
            loaded_dir = cur_dir;
            cache.clear();
            load_case_labels(cache);
        }
        std::string ck = task + "|" + key + "|" + case_name;
        auto ci = cache.find(ck);
        if (ci != cache.end()) return ci->second;
        // 依次尝试 task 专属、通配、归一化匹配
        std::string v;
        auto it = cache.find(ck);
        if (it != cache.end()) v = it->second;
        if (v.empty()) {
            auto it2 = cache.find("*|" + key + "|" + case_name);
            if (it2 != cache.end()) v = it2->second;
        }
        if (v.empty()) {
            auto it3 = cache.find(norm_case_key(key) + "|" + norm_case_key(case_name));
            if (it3 != cache.end()) v = it3->second;
        }
        cache[ck] = v;
        return v;
    }

    static void load_case_labels(std::map<std::string, std::string>& cache) {
        for (auto& fp : locale_files()) {
            std::string content;
            if (!util::read_file(fp, content)) continue;
            mj::Value j;
            if (!mj::try_parse(content, j)) continue;
            for (auto& kv : j.members()) {
                if (!kv.second.is_string()) continue;
                const std::string& k = kv.first;
                if (k.size() < 7 || k.substr(k.size() - 6) != ".label") continue;
                std::string body = k.substr(0, k.size() - 6);   // 去掉 .label
                size_t cp = body.find(".cases.");
                if (cp == std::string::npos) continue;
                std::string head = body.substr(0, cp);           // option.<O> 或 task.<T>.option.<O>
                std::string cn = body.substr(cp + 7);
                if (cn.empty()) continue;
                std::string ok;
                std::string tk;
                if (head.rfind("option.", 0) == 0) {
                    ok = head.substr(7);
                    cache["*|" + ok + "|" + cn] = kv.second.as_string();
                } else if (head.rfind("task.", 0) == 0) {
                    std::string rest = head.substr(5);
                    size_t op = rest.find(".option.");
                    if (op == std::string::npos) continue;
                    tk = rest.substr(0, op);
                    ok = rest.substr(op + 8);
                    cache[tk + "|" + ok + "|" + cn] = kv.second.as_string();
                } else {
                    continue;
                }
                if (ok.empty()) continue;
                // 归一化键（UTC/GMT 等变体），便于跨版本匹配
                std::string nk = norm_case_key(ok) + "|" + norm_case_key(cn);
                if (cache.find(nk) == cache.end()) cache[nk] = kv.second.as_string();
            }
        }
    }

    static void load_option_labels(std::map<std::string, std::string>& cache) {
        for (auto& fp : locale_files()) {
            std::string content;
            if (!util::read_file(fp, content)) continue;
            mj::Value j;
            if (!mj::try_parse(content, j)) continue;
            for (auto& kv : j.members()) {
                if (!kv.second.is_string()) continue;
                const std::string& k = kv.first;
                if (k.rfind("option.", 0) == 0 && k.size() > 7
                    && k.substr(k.size() - 6) == ".label") {
                    cache["*|" + k.substr(7, k.size() - 7 - 6)] = kv.second.as_string();
                } else if (k.rfind("task.", 0) == 0 && k.find(".option.") != std::string::npos
                           && k.size() > 6 && k.substr(k.size() - 6) == ".label") {
                    std::string rest = k.substr(5, k.size() - 5 - 6);
                    size_t dot = rest.find(".option.");
                    if (dot != std::string::npos) {
                        std::string tk = rest.substr(0, dot);
                        std::string ok = rest.substr(dot + 8);
                        cache[tk + "|" + ok] = kv.second.as_string();
                    }
                }
            }
        }
    }

    static std::string& label_dir_ref() {
        static std::string dir;
        return dir;
    }
    static void set_label_install_dir(const std::string& d) { label_dir_ref() = d; }

    // MaaEnd option type -> UI 控件类型
    static std::string option_ui_type(const std::string& t) {
        if (t == "switch" || t == "checkbox") return "bool";
        if (t == "input") return "string";
        if (t == "select") return "select";
        return "string";
    }

    std::vector<mj::Value> scan_tasks() {
        std::vector<mj::Value> out;
        std::vector<std::string> imports;
        if (auto* arr = interface_.find("import"); arr && arr->is_array()) {
            for (auto& rel : arr->items()) imports.push_back(rel.as_string());
        }
        // 一次读全部文档：option 定义、任务定义、预设常常分散在不同文件
        std::vector<std::pair<std::string, mj::Value>> docs;
        docs.emplace_back("interface.json", interface_);
        for (const auto& rel : imports) {
            std::string path = st_.install_dir + "\\" + rel;
            std::replace(path.begin(), path.end(), '/', '\\');
            std::string content;
            if (!util::read_file(path, content)) continue;
            mj::Value data;
            if (!mj::try_parse(content, data)) continue;
            docs.emplace_back(rel, std::move(data));
        }
        mj::Value all_opts = mj::Value::object();
        for (auto& d : docs) {
            auto* o = d.second.find("option");
            if (!o || !o->is_object()) continue;
            for (auto& kv : o->members())
                if (!all_opts.has(kv.first)) all_opts.set(kv.first, kv.second);
        }
        all_opts_ = all_opts;
        routes_.clear();
        pretasks_.clear();
        // pretask 单独建表：它是 agent 子进程（exec + args），不是 pipeline 节点。
        // 以前按普通任务投给 MaaTaskerPostTask，LaunchGamePC 会在 4s 内直接 failed。
        for (auto& d : docs) {
            auto* pt = d.second.find("pretask");
            if (!pt) continue;
            std::vector<const mj::Value*> items;
            if (pt->is_array())
                for (auto& e : pt->items()) items.push_back(&e);
            else if (pt->is_object())
                items.push_back(pt);
            for (const mj::Value* e : items) {
                const std::string nm = e->get_str("name", "");
                if (nm.empty() || pretasks_.count(nm)) continue;
                PreTask p;
                p.exec = e->get_str("exec", "");
                if (p.exec.empty()) continue;
                if (auto* a = e->find("args"); a && a->is_array())
                    for (auto& x : a->items()) p.args.push_back(x.as_string());
                if (auto* c = e->find("controller"); c && c->is_array())
                    for (auto& x : c->items()) p.controllers.push_back(x.as_string());
                if (auto* o = e->find("option"); o && o->is_array())
                    for (auto& x : o->items()) if (x.is_string()) p.opts.push_back(x.as_string());
                // 只建表不进 catalog：pretask 是序列自动执行的准备步骤，
                // 不是可勾选任务；塞进列表会让 devices/tasks 的任务数虚高。
                pretasks_[nm] = p;
            }
        }

        std::vector<std::string> seen;
        for (auto& d : docs) {
            const std::string& rel = d.first;
            bool is_preset = rel.find("preset") != std::string::npos;
            auto* tasks = d.second.find("task");
            if (!tasks || !tasks->is_array()) continue;
            for (auto& t : tasks->items()) {
                std::string name = t.get_str("name", "");
                if (name.empty()) continue;
                if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
                seen.push_back(name);
                TaskRoute rt;
                rt.entry = t.get_str("entry", name);
                if (auto* po = t.find("pipeline_override"); po && po->is_object()) rt.base = *po;
                if (auto* op = t.find("option"); op && op->is_array())
                    for (auto& o : op->items()) if (o.is_string()) rt.opts.push_back(o.as_string());
                routes_[name] = rt;
                mj::Value tv = mj::Value::object();
                tv.set("app", mj::Value("maaend"));
                tv.set("name", mj::Value(name));
                tv.set("kind", mj::Value(is_preset ? "preset" : "task"));
                tv.set("label", mj::Value(zh_ref(t.get_str("label", ""))));
                tv.set("description", mj::Value("来自 " + rel));
                // entry 是真正 post 给核心的 pipeline 节点名，展示名直接 post 会秒失败（#33）
                tv.set("entry", mj::Value(rt.entry));
                tv.set("params", task_option_specs(t, all_opts));
                out.push_back(std::move(tv));
            }
        }

        // 官方预设（DailyFull 全套日常 / QuickDaily / RealtimeAssist / IndependentTasks）：
        // 记下每个任务属于哪些预设，UI 用它给 MaaEnd 执行器铺默认勾选
        std::map<std::string, std::vector<std::string>> member_of;
        for (auto& d : docs) {
            auto* presets = d.second.find("preset");
            if (!presets || !presets->is_array()) continue;
            for (auto& p : presets->items()) {
                std::string pn = p.get_str("name", "");
                if (pn.empty()) continue;
                auto* pt = p.find("task");
                if (!pt || !pt->is_array()) continue;
                for (auto& one : pt->items()) {
                    if (!one.is_object()) continue;
                    if (one.has("enabled") && !one.get_bool("enabled", true)) continue;
                    std::string tn2 = one.get_str("name", "");
                    if (tn2.empty()) continue;
                    auto& v = member_of[tn2];
                    if (std::find(v.begin(), v.end(), pn) == v.end()) v.push_back(pn);
                }
            }
        }
        for (auto& tv : out) {
            auto it = member_of.find(tv.get_str("name", ""));
            if (it == member_of.end()) continue;
            mj::Value arr = mj::Value::array();
            for (auto& pn : it->second) arr.push(pn);
            tv.set("presets", std::move(arr));
        }
        return out;
    }

    // ------------------------------------------------------------ 任务路由
    // 官方 client 的跑法是：展示名 → interface 的 entry 节点，再把选中的 option case
    // 合并成 pipeline_override。把展示名直接丢给 MaaTaskerPostTask 命中的是一个
    // 不存在的节点，任务 Starting 后立刻 Failed（DailyRewards/DailyRewardStart 实测）。
    struct TaskRoute {
        std::string entry;
        std::vector<std::string> opts;
        mj::Value base = mj::Value::object();
    };

    // pretask：interface 里 exec + args 描述的 agent 子进程步骤（如 LaunchGamePC）。
    // 它不是 pipeline 节点，投给 MaaTaskerPostTask 只会秒失败。
    struct PreTask {
        std::string exec;                    // 相对 install_dir，如 agent/go-service
        std::vector<std::string> args;       // 如 --pretask LaunchGamePC
        std::vector<std::string> controllers;
        std::vector<std::string> opts;
    };

    static std::string scalar_text(const mj::Value& v) {
        if (v.is_bool()) return v.as_bool() ? "true" : "false";
        if (v.is_int()) return std::to_string(v.as_int());
        if (v.is_number()) { char b[32]; snprintf(b, sizeof(b), "%g", v.as_double()); return b; }
        return v.as_string();
    }

    // 参数值可能来自 UI（中文 case 文案或 bool），也可能来自命令行（官方 case 名）
    static bool case_hit(const std::string& task, const std::string& key,
                         const mj::Value& cs, const std::string& want) {
        std::string nm = cs.get_str("name", "");
        if (nm.empty() || want.empty()) return false;
        if (norm_case_key(nm) == norm_case_key(want)) return true;
        if (zh_ref(cs.get_str("label", "")) == want) return true;
        if (case_label(task, key, nm) == want) return true;
        std::string w = norm_case_key(want), n = norm_case_key(nm);
        static const char* ON[] = { "YES", "TRUE", "ON", "1", "ENABLE", "ENABLED" };
        static const char* OFF[] = { "NO", "FALSE", "OFF", "0", "DISABLE", "DISABLED" };
        auto hits = [](const char* const* lst, size_t cnt, const std::string& v) {
            for (size_t i = 0; i < cnt; ++i) if (v == lst[i]) return true;
            return false;
        };
        if (hits(ON, 6, w)) return hits(ON, 6, n);
        if (hits(OFF, 6, w)) return hits(OFF, 6, n);
        return false;
    }

    static void merge_node(mj::Value& dst, const mj::Value& srcv) {
        if (!srcv.is_object()) { dst = srcv; return; }
        if (!dst.is_object()) dst = mj::Value::object();
        for (auto& kv : srcv.members()) {
            auto it = dst.members().find(kv.first);
            if (it == dst.members().end()) dst.set(kv.first, kv.second);
            else merge_node(it->second, kv.second);
        }
    }

    static void merge_override(mj::Value& dst, const mj::Value& srcv) {
        if (!srcv.is_object()) return;
        for (auto& kv : srcv.members()) {
            auto it = dst.members().find(kv.first);
            if (it == dst.members().end()) dst.set(kv.first, kv.second);
            else merge_node(it->second, kv.second);
        }
    }

    // 占位符替换后的值仍在 JSON 字符串内，引号与反斜杠必须重新转义
    static std::string json_escape_text(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else o += c;
        }
        return o;
    }

    // 占位符变量：文本值 + 官方 inputs[].pipeline_type（决定写进 pipeline 的 JSON 类型）
    struct SubstVar {
        std::string text;
        std::string ptype;
    };

    static bool num_text(const std::string& s, bool& is_int) {
        size_t i = 0;
        bool dot = false;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        if (i >= s.size()) return false;
        for (; i < s.size(); ++i) {
            char c = s[i];
            if (c == '.') { if (dot) return false; dot = true; continue; }
            if (c < '0' || c > '9') return false;
        }
        is_int = !dot;
        return true;
    }

    // PipelineParser 对 repeat / expected 这类字段有强类型校验，
    // 类型错了整份 override 就 parse_task 失败，任务根本 post 不进去。
    static mj::Value typed_token(const SubstVar& v) {
        bool is_int = false;
        if ((v.ptype == "int" || v.ptype == "integer" || v.ptype == "number" ||
             v.ptype == "float" || v.ptype == "double") && num_text(v.text, is_int)) {
            if (is_int) return mj::Value((int64_t)std::stoll(v.text));
            return mj::Value(std::stod(v.text));
        }
        if (v.ptype == "bool" || v.ptype == "boolean") {
            std::string t = v.text;
            for (auto& c : t) c = (char)tolower((unsigned char)c);
            return mj::Value(t == "true" || t == "1" || t == "yes" || t == "on");
        }
        return mj::Value(v.text);
    }

    // input/hotkey 型 option 的 pipeline_override 是带 {Name} 占位符的模板。
    // 占位符独占整个字符串时按 pipeline_type 换类型，嵌在长句子里才做纯文本替换。
    static mj::Value subst_tokens(const mj::Value& tpl, const std::map<std::string, SubstVar>& vals) {
        if (tpl.is_string()) {
            std::string s = tpl.as_string();
            if (s.size() > 2 && s.front() == '{' && s.back() == '}' &&
                s.find('{', 1) == std::string::npos) {
                auto it = vals.find(s.substr(1, s.size() - 2));
                if (it != vals.end()) return typed_token(it->second);
            }
            for (auto& kv : vals) {
                std::string tok = "{" + kv.first + "}";
                std::string rep = json_escape_text(kv.second.text);
                size_t p = 0;
                while ((p = s.find(tok, p)) != std::string::npos) {
                    s.replace(p, tok.size(), rep);
                    p += rep.size();
                }
            }
            return mj::Value(s);
        }
        if (tpl.is_array()) {
            mj::Value a = mj::Value::array();
            for (auto& e : tpl.items()) a.push(subst_tokens(e, vals));
            return a;
        }
        if (tpl.is_object()) {
            mj::Value o = mj::Value::object();
            for (auto& kv : tpl.members()) o.set(kv.first, subst_tokens(kv.second, vals));
            return o;
        }
        return tpl;
    }

    void apply_option(mj::Value& out, const std::string& task, const std::string& key,
                      const mj::Value* user, int depth) const {
        if (depth > 8 || !user_null_ok(user)) return;
        const mj::Value* spec = all_opts_.find(key);
        if (!spec || !spec->is_object()) return;
        std::string type = spec->get_str("type", "");
        if (type == "input" || type == "hotkey") {
            std::map<std::string, SubstVar> vals;
            auto* lst = spec->find(type == "input" ? "inputs" : "hotkeys");
            if (lst && lst->is_array()) {
                for (auto& in : lst->items()) {
                    std::string nm = in.get_str("name", "");
                    if (nm.empty()) continue;
                    std::string v;
                    if (auto* d = in.find("default")) v = scalar_text(*d);
                    std::string uv;
                    if (user && user->is_object() && user->has(nm)) uv = scalar_text((*user)[nm]);
                    else if (user && (user->is_string() || user->is_bool() || user->is_number()))
                        uv = scalar_text(*user);
                    // 面板里清空过的输入框会存成 ""，直接覆盖 default 会写出
                    // {"max_hit":""} 这种非法值，整份 override parse 失败、任务 post 不进去。
                    if (!uv.empty()) v = uv;
                    SubstVar sv;
                    sv.text = v;
                    sv.ptype = in.get_str("pipeline_type", "");
                    vals[nm] = sv;
                    if (type == "hotkey") vals[nm + ".primary"] = sv;
                }
            }
            auto* po = spec->find("pipeline_override");
            if (po && po->is_object()) merge_override(out, subst_tokens(*po, vals));
            return;
        }
        auto* cases = spec->find("cases");
        if (!cases || !cases->is_array()) return;
        std::string def = norm_case_key(spec->get_str("default_case", ""));
        auto& items = cases->items();
        for (size_t ci = 0; ci < items.size(); ++ci) {
            const mj::Value& cs = items[ci];
            if (!cs.is_object()) continue;
            bool sel;
            if (user && user->is_array()) {
                sel = false;
                for (auto& e : user->items())
                    if (case_hit(task, key, cs, scalar_text(e))) { sel = true; break; }
            } else if (type == "checkbox"
                       && (!user || (user->is_bool() && user->as_bool()))) {
                sel = false;
                const mj::Value* defs = spec->find("default_case");
                if (defs && defs->is_array()) {
                    for (auto& d : defs->items()) {
                        if (d.is_string() && norm_case_key(cs.get_str("name", ""))
                            == norm_case_key(d.as_string())) {
                            sel = true;
                            break;
                        }
                    }
                } else if (defs && defs->is_string()
                           && norm_case_key(cs.get_str("name", ""))
                              == norm_case_key(defs->as_string())) {
                    sel = true;
                }
            } else if (user) {
                sel = case_hit(task, key, cs, scalar_text(*user));
            } else if (type == "checkbox") {
                sel = false;
            } else if (!def.empty()) {
                sel = norm_case_key(cs.get_str("name", "")) == def;
            } else {
                sel = ci == 0;  // switch/select 缺 default_case 时按首个 case
            }
            if (!sel) continue;
            auto* po = cs.find("pipeline_override");
            if (po && po->is_object()) merge_override(out, *po);
            auto* sub = cs.find("option");
            if (!sub || !sub->is_array()) continue;
            for (auto& sk : sub->items()) {
                if (!sk.is_string()) continue;
                const mj::Value* nu = (user && user->is_object()) ? user->find(sk.as_string()) : nullptr;
                apply_option(out, task, sk.as_string(), nu, depth + 1);
            }
        }
    }

    // UI 里未选的开关会留个 null，它表示"没给值"而不是"选了空值"
    static bool user_null_ok(const mj::Value* u) { return !u || !u->is_null(); }

    // ------------------------------------------------------------ 加载
    bool load() override {
        if (!st_.available) return false;
        set_label_install_dir(st_.install_dir + "\\locales");
        std::string maafw = st_.install_dir + "\\maafw";
        std::wstring dll = util::utf8_to_wide(st_.core_dll);
        // r24 顺序修正：原先先 LoadLibrary、成功之后才 AddDllDirectory / 改 PATH，
        // 顺序颠倒（回退分支与间接依赖解析不到同目录 DLL），且 PATH 会永久污染进程
        // 环境、从不还原。改为先设默认搜索策略 + 注册 maafw 目录，再加载两个 DLL。
        static std::once_flag s_dll_dirs_once;
        std::call_once(s_dll_dirs_once, [] {
            SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                                     | LOAD_LIBRARY_SEARCH_USER_DIRS);
        });
        cookie_ = AddDllDirectory(util::utf8_to_wide(maafw).c_str());
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) h = LoadLibraryW(dll.c_str());
        if (!h) {
            st_.error = "LoadLibrary MaaFramework failed err=" + std::to_string(GetLastError());
            if (cookie_) { RemoveDllDirectory(cookie_); cookie_ = nullptr; }
            return false;
        }
        lib_ = h;
        if (!resolve_symbols()) {
            st_.error = "missing MaaFramework exports";
            return false;
        }
        std::wstring agent_dll = util::utf8_to_wide(maafw + "\\MaaAgentClient.dll");
        HMODULE ah = LoadLibraryExW(agent_dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!ah) ah = LoadLibraryW(agent_dll.c_str());
        if (!ah) {
            st_.error = "LoadLibrary MaaAgentClient failed err=" + std::to_string(GetLastError());
            return false;
        }
        agent_lib_ = ah;
        if (!resolve_agent_symbols()) {
            st_.error = "missing MaaAgentClient exports";
            return false;
        }
        // 版本
        const char* ver = fn_.MaaVersion();
        st_.version = ver ? ver : "";
        // LogDir 要 const wchar_t* 本体：传指针的指针或窄字符串都会被当成宽字符
        // 解出乱码路径，框架随即抛 filesystem_error 把整个 load 打挂。
        // 日志只是诊断手段，任何失败都不许影响核心加载
        auto set_log_dir = [&](void* val, size_t sz) -> bool {
            try { return fn_.MaaGlobalSetOption(1, val, sz); }
            catch (const std::exception& e) { st_.log_state = std::string("threw: ") + e.what(); return false; }
            catch (...) { st_.log_state = "threw: unknown"; return false; }
        };
        st_.log_state = "rejected";
        {
            std::string log_dir = st_.install_dir + "\\debug\\maahub-cpp";
            util::ensure_dir(log_dir);
            // LogDir 期望 UTF-8 char*。之前传 wchar_t* 会被当窄字符串在第一个 NUL
            // 截断成 "C"，框架只在 CWD 下建出个空目录 "C"，核心日志永远不落盘。
            if (set_log_dir((void*)log_dir.c_str(), log_dir.size())) st_.log_state = "ok";
        }
        // stdout 日志关掉（StdoutLevel=0, int32）
        try {
            int32_t level0 = 0;
            fn_.MaaGlobalSetOption(4, &level0, sizeof(level0));
        } catch (...) {}

        tasker_ = fn_.MaaTaskerCreate();
        if (!tasker_) { st_.error = "MaaTaskerCreate failed"; return false; }
        res_ = fn_.MaaResourceCreate();
        if (!res_) { st_.error = "MaaResourceCreate failed"; return false; }
        std::string res_path = st_.install_dir + "\\resource";
        int64_t rid = fn_.MaaResourcePostBundle(res_, res_path.c_str());
        if (rid <= 0) { st_.error = "MaaResourcePostBundle failed"; return false; }
        int st = fn_.MaaResourceWait(res_, rid);
        if (st != MAA_STATUS_SUCCEEDED) {
            st_.error = "resource load status=" + std::to_string(st);
            return false;
        }
        if (!fn_.MaaTaskerBindResource(tasker_, res_)) {
            st_.error = "MaaTaskerBindResource failed";
            return false;
        }
        // 事件 sink
        sink_ = make_sink();
        fn_.MaaTaskerAddSink(tasker_, sink_, nullptr);
        util::trace("maaend load agents begin");
        // agent 子进程
        if (!start_agents()) {
            st_.error = "agent children failed to start: " + agent_error_;
            return false;
        }
        util::trace("maaend load done tasker=" + std::to_string((intptr_t)tasker_));
        return true;
    }

    void unload() override {
        if (fn_.MaaGlobalLoggingFlush) fn_.MaaGlobalLoggingFlush();
        util::trace("maaend unload begin agents=" + std::to_string(agent_clients_.size()));
        for (auto& c : agent_clients_) {
            if (c) afn_.MaaAgentClientDestroy(c);
        }
        agent_clients_.clear();
        for (auto& p : agent_procs_) {
            if (p.alive()) {
                TerminateProcess(p.process, 0);
            }
            p.close();
        }
        agent_procs_.clear();
        if (tasker_) { fn_.MaaTaskerDestroy(tasker_); tasker_ = nullptr; }
        // r24：ctrl_（Win32 控制器）原先在任何路径都没被销毁 —— 每次连接泄漏一个
        if (ctrl_) { fn_.MaaControllerDestroy(ctrl_); ctrl_ = nullptr; }
        if (res_) { fn_.MaaResourceDestroy(res_); res_ = nullptr; }
        if (cookie_) { RemoveDllDirectory(cookie_); cookie_ = nullptr; }
        if (lib_) { FreeLibrary(lib_); lib_ = nullptr; }
        if (agent_lib_) { FreeLibrary(agent_lib_); agent_lib_ = nullptr; }
        util::trace("maaend unload end");
    }

    // ------------------------------------------------------------ agent 子进程
    std::string new_uuid() {
        static std::random_device rd;
        static std::mt19937_64 rng(rd());
        uint64_t a = rng(), b = rng();
        char buf[40];
        snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                 (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xFFFF), (unsigned)(a & 0xFFFF),
                 (unsigned)((b >> 48) & 0xFFFF), (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
        return buf;
    }

    bool start_agents() {
        auto* agents = interface_.find("agent");
        if (!agents || !agents->is_array()) {
            agent_error_ = "interface.json has no agent list";
            return false;
        }
        // 并行拉起所有 agent：串行时每个 agent 的固定等待会叠加（实测 2 个 agent ~5s）
        struct AgentJob {
            std::string child;
            std::string exe;
            std::string identifier;
        };
        std::vector<AgentJob> jobs;
        for (auto& spec : agents->items()) {
            std::string child = spec.get_str("child_exec", "");
            if (child.empty()) continue;
            std::string exe = st_.install_dir + "\\" + child;
            std::replace(exe.begin(), exe.end(), '/', '\\');
            if (!util::file_exists(exe)) {
                // Windows 下 child_exec 省略 .exe
                if (!util::file_exists(exe + ".exe")) {
                    agent_error_ = "agent binary missing: " + exe;
                    continue;
                }
                exe += ".exe";
            }
            jobs.push_back({ child, exe, new_uuid() });
        }
        if (jobs.empty()) return false;

        std::mutex res_mtx;
        std::vector<void*> clients(jobs.size(), nullptr);
        std::vector<std::string> errors(jobs.size());
        std::vector<std::thread> threads;
        for (size_t i = 0; i < jobs.size(); ++i) {
            threads.emplace_back([&, i] {
                auto& job = jobs[i];
                // PATH 注入 maafw（cpp-algo 静态链接 maafw 中的 DLL）
                std::wstring env_block = util::make_env_block(
                    util::utf8_to_wide(st_.install_dir + "\\maafw"));
                std::wstring cmd = L"\"" + util::utf8_to_wide(job.exe) + L"\" "
                                   + util::utf8_to_wide(job.identifier);
                util::ProcHandle proc;
                if (!util::spawn_process(cmd, util::utf8_to_wide(st_.install_dir),
                                         env_block, proc)) {
                    std::lock_guard<std::mutex> lk(res_mtx);
                    errors[i] = "spawn failed: " + job.exe;
                    return;
                }
                // 2026-11-23 修复：agent 的 stdout 管道此前建了从不读——MaaEnd 的
                // dynamic_html_log 往 stdout 打 UI 日志，写满 4KB 默认缓冲即把整个
                // agent 冻成僵尸（AutoEcoFarm 第 4 次倒计时必现，两次实锤，
                // docs/audit-20261123-daily-failures.md）。排空落盘 debug\<stem>.stdout.log。
                {
                    std::string stem = job.exe;
                    size_t slash = stem.find_last_of("\\/");
                    if (slash != std::string::npos) stem = stem.substr(slash + 1);
                    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".exe") == 0)
                        stem.resize(stem.size() - 4);
                    util::drain_proc_stdout(proc, st_.install_dir + "\\debug", stem);
                }
                {
                    std::lock_guard<std::mutex> lk(proc_mtx_);
                    agent_procs_.push_back(proc);
                }
                // IPC 连接（client 侧 bind，server 连过来）：
                // 轮询等待而非固定 sleep——agent 就绪即可立即连上，无需空等 2.5s
                void* client = nullptr;
                for (int attempt = 0; attempt < 120; ++attempt) {   // 最长 6s
                    void* sb = fn_.MaaStringBufferCreate();
                    fn_.MaaStringBufferSetEx(sb, job.identifier.c_str(),
                                             job.identifier.size());
                    client = afn_.MaaAgentClientCreateV2(sb);
                    if (client) {
                        afn_.MaaAgentClientSetTimeout(client, 8000);
                        if (afn_.MaaAgentClientBindResource(client, res_)) {
                            if (afn_.MaaAgentClientConnect(client)) break;
                        }
                        afn_.MaaAgentClientDestroy(client);
                        client = nullptr;
                    }
                    if (!proc.alive()) break;
                    Sleep(50);
                }
                std::lock_guard<std::mutex> lk(res_mtx);
                if (client) {
                    clients[i] = client;
                } else {
                    errors[i] = "agent " + job.child + " did not connect";
                }
            });
        }
        for (auto& t : threads) t.join();

        bool any = false;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (clients[i]) {
                std::lock_guard<std::mutex> lk(cli_mtx_);
                agent_clients_.push_back(clients[i]);
                any = true;
            } else if (!errors[i].empty()) {
                std::lock_guard<std::mutex> lk(err_mtx_);
                if (agent_error_.empty()) agent_error_ = errors[i];
            }
        }
        return any && !agent_clients_.empty();
    }

    // ------------------------------------------------------------ 连接
    // MaaFramework 的截图/输入方式既接受数字掩码，也接受枚举名（忽略大小写与 - _ 空格）
    static std::string norm_method(const std::string& s) {
        std::string k;
        for (char c : s) {
            if (c == '-' || c == '_' || c == ' ') continue;
            k += (char)tolower((unsigned char)c);
        }
        return k;
    }
    static const std::map<std::string, uint64_t>& screencap_names() {
        static const std::map<std::string, uint64_t> m = {
            {"gdi", 1}, {"framepool", 2}, {"dxgidesktopdup", 4},
            {"dxgidesktopdupwindow", 8}, {"printwindow", 16}, {"screendc", 32},
        };
        return m;
    }
    static const std::map<std::string, uint64_t>& input_names() {
        static const std::map<std::string, uint64_t> m = {
            {"seize", 1}, {"sendmessage", 2}, {"postmessage", 4}, {"legacyevent", 8},
            {"postthreadmessage", 16}, {"sendmessagewithcursorpos", 32},
            {"postmessagewithcursorpos", 64}, {"sendmessagewithwindowpos", 128},
            {"postmessagewithwindowpos", 256}, {"interception", 512},
            {"anchoredtouch", 1024},
        };
        return m;
    }
    static uint64_t method_value(const mj::Value& cfg, const std::string& dotted,
                                 uint64_t def, const std::map<std::string, uint64_t>& table) {
        const mj::Value* v = cfg.find_path(dotted);
        if (!v) return def;
        if (v->is_number()) return (uint64_t)v->as_int();
        if (v->is_string()) {
            auto it = table.find(norm_method(v->as_string()));
            if (it != table.end()) return it->second;
        }
        return def;
    }

    bool parse_fit_window(int& fw, int& fh) const {
        std::string fit = cfg_.get_path_str("connect.fit_window", "1280x720");
        const size_t xp = fit.find('x');
        fw = atoi(fit.c_str());
        fh = xp == std::string::npos ? 0 : atoi(fit.c_str() + xp + 1);
        return fit != "off" && fit != "none" && fw > 0 && fh > 0;
    }

    static void ensure_endfield_registry_resolution(int w, int h) {
        if (w <= 0 || h <= 0) return;
        HKEY hk = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Hypergryph\\Endfield",
                            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
            return;
        }
        auto set_dword = [&](const wchar_t* name, DWORD val) {
            RegSetValueExW(hk, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        };
        set_dword(L"video_resolution_width_h583690364", (DWORD)w);
        set_dword(L"video_resolution_height_h2517654917", (DWORD)h);
        set_dword(L"Screenmanager Resolution Width_h182942802", (DWORD)w);
        set_dword(L"Screenmanager Resolution Height_h2627697771", (DWORD)h);
        set_dword(L"Screenmanager Resolution Window Width_h2524650974", (DWORD)w);
        set_dword(L"Screenmanager Resolution Window Height_h1684712807", (DWORD)h);
        set_dword(L"Screenmanager Fullscreen mode_h3630240806", 3u);
        set_dword(L"Screenmanager Resolution Use Native_h1405027254", 0u);
        RegCloseKey(hk);
    }

    static void restore_game_window(HWND hwnd) {
        if (!IsWindow(hwnd)) return;
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        RECT cr{};
        if (GetClientRect(hwnd, &cr)
            && (cr.right <= cr.left || cr.bottom <= cr.top)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
    }

    void check_and_refit_window() const {
        int fw = 0, fh = 0;
        if (!parse_fit_window(fw, fh)) return;
        HWND hwnd = nullptr;
        double now = util::now_sec();
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            if (st_.conn != "connected" || st_.ctrl_hwnd == 0) return;
            if ((now - last_refit_check_) < 1.0) return;
            hwnd = (HWND)(intptr_t)st_.ctrl_hwnd;
            last_refit_check_ = now;
        }
        RECT cr{};
        restore_game_window(hwnd);
        if (!IsWindow(hwnd) || !GetClientRect(hwnd, &cr)) return;
        const int cur_w = (int)(cr.right - cr.left);
        const int cur_h = (int)(cr.bottom - cr.top);
        if (cur_w == fw && cur_h == fh) return;

        ensure_endfield_registry_resolution(fw, fh);
        int cw = 0, ch = 0;
        const bool fitted = util::fit_client_size(hwnd, fw, fh, &cw, &ch);
        util::trace("maaend auto-refit client want=" + std::to_string(fw) + "x" + std::to_string(fh)
                    + " was=" + std::to_string(cur_w) + "x" + std::to_string(cur_h)
                    + " actual=" + std::to_string(cw) + "x" + std::to_string(ch)
                    + (fitted ? " ok" : " stuck"));
        if (cw > 0 && ch > 0) {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.res_w = cw;
            st_.res_h = ch;
        }
    }

    bool connect() override {
        auto set_fail = [&](const std::string& err) {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.conn = "failed";
            st_.last_error = err;
            return false;
        };
        if (!tasker_) {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.last_error = "not loaded";
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.conn = "connecting";
        }
        std::string mode = cfg_.get_path_str("connect.mode", "win32");
        if (mode == "adb") return set_fail("ADB 模式未配置（请使用 win32）");
        std::string cls = cfg_.get_str("game_window_class", "UnityWndClass");
        std::string title = cfg_.get_str("game_window_title_regex", "Endfield");
        util::WindowInfo wi;
        if (!util::find_window(cls, title, wi)) {
            std::lock_guard<std::mutex> lk(st_mtx_);
            cached_win_running_ = false;
            cached_win_title_.clear();
            last_win_probe_ = util::now_sec();
            st_.game_running = false;
            st_.game_title.clear();
            st_.conn = "failed";
            st_.last_error = "game window not found (class=" + cls + ", title~/" + title + "/). 请先启动终末地客户端。";
            return false;
        }
        restore_game_window(wi.hwnd);
        // 客户区必须与 pipeline 基准尺寸一致。游戏被拉成 1280x768 这类非 16:9
        // 尺寸后，UI 图标相对模板整体缩放，模板匹配会静默失败——DailyRewards
        // 就是在这里空转的（WorldMenu 图标在 ROI 内却匹配不上）。
        int fw = 0, fh = 0;
        const bool do_fit = parse_fit_window(fw, fh);
        int client_w = 0, client_h = 0;
        if (do_fit) {
            ensure_endfield_registry_resolution(fw, fh);
            const bool fitted = util::fit_client_size(wi.hwnd, fw, fh, &client_w, &client_h);
            util::trace("maaend fit client want=" + std::to_string(fw) + "x" + std::to_string(fh) +
                        " actual=" + std::to_string(client_w) + "x" + std::to_string(client_h) +
                        (fitted ? " ok" : " stuck"));
            if (fitted) Sleep(500);   // 尺寸确实变了才需要等渲染跟上
            // r25：只告警不拒连——实测部分任务在缩放图上仍能 ret=true
            // （AutoCollectSchedule），拒连会把能跑的链路一起杀掉；但必须说清：
            // 抓图会被等比缩放到基准高度（实测 1280x768 → 1200x720），
            // x>1200 的模板 ROI 越界、识别静默失败（AutoEcoFarm 即栽在这里）。
            if (client_w > 0 && client_h > 0
                && (long long)client_w * fh != (long long)client_h * fw) {
                util::trace("maaend WARN client " + std::to_string(client_w) + "x" + std::to_string(client_h)
                    + " != fit target " + std::to_string(fw) + "x" + std::to_string(fh)
                    + " : capture scales to ~" + std::to_string((long long)fh * client_w / client_h)
                    + "x" + std::to_string(fh)
                    + ", ROIs beyond will silently fail; set the game to 16:9"
                    + " or match connect.fit_window to its real size");
            }
        } else {
            RECT cr{};
            if (GetClientRect(wi.hwnd, &cr)) { client_w = (int)(cr.right - cr.left); client_h = (int)(cr.bottom - cr.top); }
        }
        uint64_t sc = method_value(cfg_, "connect.screencap_method", 32, screencap_names());
        uint64_t mm = method_value(cfg_, "connect.mouse_method", 1, input_names());
        uint64_t km = method_value(cfg_, "connect.keyboard_method", 1, input_names());
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.screencap_method = (int64_t)sc;
            st_.mouse_method = (int64_t)mm;
            st_.keyboard_method = (int64_t)km;
        }
        // r24：重复 connect 直接覆盖 ctrl_ 会泄漏上一句柄（UI「连接」按钮可反复点）
        if (ctrl_) { fn_.MaaControllerDestroy(ctrl_); ctrl_ = nullptr; }
        void* ctrl = fn_.MaaWin32ControllerCreate((void*)wi.hwnd, sc, mm, km);
        if (!ctrl) return set_fail("MaaWin32ControllerCreate failed");
        ctrl_ = ctrl;
        util::trace("maaend connect ctrl=" + std::to_string((intptr_t)ctrl)
                    + " hwnd=" + std::to_string((intptr_t)wi.hwnd));
        int64_t cid = fn_.MaaControllerPostConnection(ctrl);
        if (cid <= 0) {
            fn_.MaaControllerDestroy(ctrl); ctrl_ = nullptr;   // r24：失败也要回收
            return set_fail("MaaControllerPostConnection failed");
        }
        int st = fn_.MaaControllerWait(ctrl, cid);
        util::trace("maaend connect waited st=" + std::to_string(st));
        if (st != MAA_STATUS_SUCCEEDED) {
            fn_.MaaControllerDestroy(ctrl); ctrl_ = nullptr;   // r24：失败也要回收
            return set_fail("controller connect status=" + std::to_string(st));
        }
        if (!fn_.MaaTaskerBindController(tasker_, ctrl)) {
            fn_.MaaControllerDestroy(ctrl); ctrl_ = nullptr;   // r24：失败也要回收
            return set_fail("MaaTaskerBindController failed");
        }
        int w = 0, hgt = 0;
        const bool rok = fn_.MaaControllerGetResolution(ctrl, &w, &hgt) != false;
        if (!rok && client_w > 0 && client_h > 0) {
            // r25：核心报不出分辨率（Endfield 实测恒 0x0）时退回自己实测的客户区尺寸，
            // 状态里不再拿 0x0 冒充真实值
            w = client_w; hgt = client_h;
        }
        void* sb = fn_.MaaStringBufferCreate();
        std::string uuid;
        if (fn_.MaaControllerGetUuid(ctrl, sb)) uuid = strbuf_get(sb);
        void* hb = fn_.MaaStringBufferCreate();
        std::string rhash;
        if (fn_.MaaResourceGetHash(res_, hb)) rhash = strbuf_get(hb);
        std::string wtitle = util::window_title(wi.hwnd);
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            if (!rhash.empty()) st_.resource_hash = rhash;
            st_.ctrl_mode = "win32";
            st_.ctrl_hwnd = (int64_t)(intptr_t)wi.hwnd;
            st_.res_w = w;
            st_.res_h = hgt;
            st_.uuid = uuid;
            st_.game_running = true;
            st_.game_title = wtitle;
            cached_win_running_ = true;
            cached_win_title_ = wtitle;
            last_win_probe_ = util::now_sec();
            st_.conn = "connected";
            st_.last_error.clear();
        }
        // r24：分辨率探测失败（返回 0x0）在 Endfield 实跑里正是模板匹配失效的
        // 前兆，原先把返回值整个丢弃，这里打进 trace 便于定位
        util::trace("maaend connected res=" + std::to_string(w) + "x" + std::to_string(hgt)
                    + (rok ? "" : " (GetResolution failed)"));
        return true;
    }

    void disconnect() override {
        // r24：原实现只改状态字符串，控制器句柄一直存活。这里真正销毁；
        // tasker_ 上绑定的引用由框架内部引用计数保活，is_connected() 随 ctrl_
        // 置空自然变 false，后续重连会重建新的控制器。
        if (ctrl_) { fn_.MaaControllerDestroy(ctrl_); ctrl_ = nullptr; }
        std::lock_guard<std::mutex> lk(st_mtx_);
        st_.conn = "disconnected";
        st_.ctrl_hwnd = 0;
    }

    bool is_connected() override {
        if (!ctrl_) return false;
        return fn_.MaaControllerConnected(ctrl_) != 0;
    }

    // ------------------------------------------------------------ 任务
    // 展示名 -> entry 节点 + 把 option 选中项合并成 pipeline_override。
    // run_task 与 --cli override 共用这一段：无头测试要靠后者核对构造结果（#33/#34/#35）
    mj::Value build_plan(const std::string& task, const mj::Value& params) {
        mj::Value out = mj::Value::object();
        std::string entry = task;
        mj::Value ov = mj::Value::object();
        auto rt = routes_.find(task);
        if (rt != routes_.end() && !rt->second.entry.empty()) {
            entry = rt->second.entry;
            merge_override(ov, rt->second.base);
            for (auto& k : rt->second.opts) {
                const mj::Value* u = params.is_object() ? params.find(k) : nullptr;
                apply_option(ov, task, k, u, 0);
            }
        } else if (params.is_object()) {
            ov = params;
        }
        out.set("entry", entry);
        out.set("override", std::move(ov));
        return out;
    }

    bool build_override(const std::string& task, const mj::Value& params,
                        mj::Value& out) override {
        if (routes_.empty()) return false;   // catalog 还没扫，entry 映射无从谈起
        out = build_plan(task, params);
        return true;
    }

    // pretask 走 CreateProcess：agent 子进程自己去拉启动器/进游戏，退出码 0 才算成功。
    // 它不接受 pipeline override，所以不能混进 tasker 的任务队列。
    bool run_pretask(const std::string& name, std::string& err) {
        auto it = pretasks_.find(name);
        if (it == pretasks_.end()) { err = "unknown pretask " + name; return false; }
        const PreTask& p = it->second;
        std::string exe = st_.install_dir + "\\" + p.exec;
        std::replace(exe.begin(), exe.end(), '/', '\\');
        if (exe.size() < 4 || exe.substr(exe.size() - 4) != ".exe") exe += ".exe";
        ensure_endfield_registry_resolution(1280, 720);
        std::string cmd = "\"" + exe + "\"";
        for (const auto& a : p.args)
            cmd += (a.find(' ') == std::string::npos) ? (" " + a) : (" \"" + a + "\"");
        if (name == "LaunchGamePC") {
            cmd += " \"{\\\"LaunchGamePCGameSetting\\\":\\\"Yes\\\",\\\"LaunchGamePCDisplayType\\\":\\\"Window\\\",\\\"LaunchGamePCResolution\\\":\\\"1280x720\\\"}\"";
        }
        util::trace("maaend pretask " + name + " cmd=" + cmd);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring wcmd = util::utf8_to_wide(cmd);
        std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
        buf.push_back(L'\0');
        std::wstring wdir = util::utf8_to_wide(st_.install_dir);
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, wdir.c_str(), &si, &pi)) {
            err = "CreateProcess pretask failed err=" + std::to_string(GetLastError());
            return false;
        }
        const double t0 = util::now_sec();
        const DWORD w = WaitForSingleObject(pi.hProcess, 300000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        util::trace("maaend pretask " + name + " wait=" + std::to_string((int)w)
                    + " code=" + std::to_string((int)code)
                    + " sec=" + std::to_string((int)(util::now_sec() - t0)));
        if (w != WAIT_OBJECT_0) { err = "pretask timeout"; return false; }
        if (code != 0) { err = "pretask exit=" + std::to_string((int)code); return false; }
        return true;
    }

    // 任务序列的第一段：游戏没起来才按 pretask 拉起。
    // 语义是"确保可执行现场"，游戏已在跑就跳过，不做无谓重启。
    bool prepare(const mj::Value& params, std::string& err) override {
        (void)params;
        std::string want = cfg_.get_path_str("pretask.name", "LaunchGamePC");
        if (want.empty() || want == "off" || want == "none") return true;
        if (!pretasks_.count(want)) { err = "pretask 未定义：" + want; return false; }
        std::string cls = cfg_.get_str("game_window_class", "UnityWndClass");
        std::string title = cfg_.get_str("game_window_title_regex", "Endfield");
        util::WindowInfo wi;
        if (util::find_window(cls, title, wi)) {
            util::trace("maaend prepare skip: game window already present");
            return true;
        }
        util::trace("maaend prepare: game window missing -> " + want);
        return run_pretask(want, err);
    }

    bool run_task(const std::string& task, const mj::Value& params, int64_t& out) override {
        // pretask 名直接投进来时同样走子进程，避免"任务链里选了 LaunchGamePC 就必失败"
        if (pretasks_.count(task)) {
            std::string err;
            const bool ok = run_pretask(task, err);
            {
                std::lock_guard<std::mutex> lk(st_mtx_);
                st_.run = ok ? "idle" : "failed";
                st_.last_error = ok ? std::string() : (task + ": " + err);
            }
            if (ok) out = 1;
            return ok;
        }
        out = 0;
        if (!tasker_) {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.last_error = "not loaded";
            return false;
        }
        if (!is_connected()) {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.last_error = "not connected";
            return false;
        }
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            hwnd = (HWND)(intptr_t)st_.ctrl_hwnd;
        }
        restore_game_window(hwnd);
        if (IsWindow(hwnd) && GetForegroundWindow() != hwnd) {
            SetForegroundWindow(hwnd);
            if (GetForegroundWindow() != hwnd)
                util::trace("maaend WARN failed to bring game window to foreground");
        }
        check_and_refit_window();
        mj::Value plan = build_plan(task, params);
        const mj::Value& ov = plan["override"];
        std::string entry = plan.get_str("entry", task);
        std::string override_json = ov.is_object() ? mj::Value::dump(ov) : std::string("{}");
        std::string dbg = override_json;
        if (dbg.size() > 160) dbg = dbg.substr(0, 160);
        util::trace("maaend post " + task + " entry=" + entry + " override=" + dbg);
        int64_t tid = fn_.MaaTaskerPostTask(tasker_, entry.c_str(), override_json.c_str());
        if (tid <= 0) {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.last_error = "MaaTaskerPostTask(" + task + ") failed";
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.run = "running";
            st_.last_error.clear();
        }
        out = tid;
        return true;
    }

    bool stop() override {
        if (!tasker_) return false;
        int64_t tid = fn_.MaaTaskerPostStop(tasker_);
        if (tid <= 0) return false;
        int waited = 0;
        while (waited < 10000) {
            if (fn_.MaaTaskerRunning(tasker_) == 0) break;
            Sleep(100);
            waited += 100;
        }
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            st_.run = fn_.MaaTaskerRunning(tasker_) != 0 ? "stopping" : "idle";
        }
        return true;
    }

    bool is_running() override {
        if (!tasker_) return false;
        bool r = fn_.MaaTaskerRunning(tasker_) != 0;
        // 框架日志有缓冲，真机卡死时最后几行永远停在内存里。轮询顺手冲刷一次。
        if (r) {
            if (fn_.MaaGlobalLoggingFlush) fn_.MaaGlobalLoggingFlush();
            check_and_refit_window();
        }
        return r;
    }

    AppStatus status() const override {
        // 实时探测终末地客户端窗口（约 1s 节流）与 Win32Controller / Agent 存活状态
        bool ctrl_alive = false;
        if (ctrl_ && fn_.MaaControllerConnected) {
            ctrl_alive = (fn_.MaaControllerConnected(ctrl_) != 0);
        }
        int alive_agents = 0;
        int total_agents = 0;
        {
            std::lock_guard<std::mutex> lk(cli_mtx_);
            if (afn_.MaaAgentClientAlive) {
                for (void* c : agent_clients_) {
                    ++total_agents;
                    if (c && afn_.MaaAgentClientAlive(c)) ++alive_agents;
                }
            }
        }
        if (total_agents == 0) {
            std::lock_guard<std::mutex> lk(proc_mtx_);
            for (const auto& p : agent_procs_) {
                ++total_agents;
                if (p.alive()) ++alive_agents;
            }
        }

        double now = util::now_sec();
        bool need_probe = false;
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            need_probe = (now - last_win_probe_) >= 1.0;
        }
        bool running = false;
        std::string wtitle;
        if (need_probe) {
            std::string cls = cfg_.get_str("game_window_class", "UnityWndClass");
            std::string title = cfg_.get_str("game_window_title_regex", "Endfield");
            util::WindowInfo wi;
            running = util::find_window(cls, title, wi);
            wtitle = running ? util::window_title(wi.hwnd) : std::string();
            if (running) check_and_refit_window();
        }

        std::lock_guard<std::mutex> lk(st_mtx_);
        if (need_probe) {
            cached_win_running_ = running;
            cached_win_title_ = wtitle;
            last_win_probe_ = now;
        }
        st_.game_running = cached_win_running_;
        st_.game_title = cached_win_title_;
        st_.agents_alive = alive_agents;
        st_.agents_total = total_agents;
        if (st_.conn == "connected") {
            bool hwnd_ok = !st_.ctrl_hwnd || IsWindow((HWND)(intptr_t)st_.ctrl_hwnd) != FALSE;
            if (!ctrl_ || !ctrl_alive || !hwnd_ok) {
                st_.conn = "disconnected";
                st_.ctrl_hwnd = 0;
                st_.last_error = st_.game_running ? "控制器与终末地连接已断开" : "终末地客户端已关闭";
            }
        }
        return st_;
    }

    std::vector<mj::Value> task_catalog() const override { return catalog_; }

    std::vector<AdapterEvent> drain_events() override {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        std::vector<AdapterEvent> out;
        out.swap(events_);
        return out;
    }

private:
    std::string strbuf_get(void* buf) {
        if (!buf) return "";
        const char* s = fn_.MaaStringBufferGet(buf);
        std::string out = s ? s : "";
        fn_.MaaStringBufferDestroy(buf);
        return out;
    }

    // ------------------------------------------------------------ 符号
    struct Fn {
        const char* (*MaaVersion)() = nullptr;
        bool (*MaaGlobalSetOption)(int, void*, uint64_t) = nullptr;
        void (*MaaGlobalLoggingFlush)() = nullptr;
        void* (*MaaTaskerCreate)() = nullptr;
        void (*MaaTaskerDestroy)(void*) = nullptr;
        bool (*MaaTaskerBindResource)(void*, void*) = nullptr;
        bool (*MaaTaskerBindController)(void*, void*) = nullptr;
        int64_t (*MaaTaskerPostTask)(void*, const char*, const char*) = nullptr;
        int64_t (*MaaTaskerPostStop)(void*) = nullptr;
        int (*MaaTaskerStatus)(void*, int64_t) = nullptr;
        int (*MaaTaskerWait)(void*, int64_t) = nullptr;
        bool (*MaaTaskerInited)(void*) = nullptr;
        bool (*MaaTaskerRunning)(void*) = nullptr;
        bool (*MaaTaskerAddSink)(void*, void*, void*) = nullptr;
        void* (*MaaResourceCreate)() = nullptr;
        void (*MaaResourceDestroy)(void*) = nullptr;
        int64_t (*MaaResourcePostBundle)(void*, const char*) = nullptr;
        int (*MaaResourceWait)(void*, int64_t) = nullptr;
        int (*MaaResourceStatus)(void*, int64_t) = nullptr;
        bool (*MaaResourceGetHash)(void*, void*) = nullptr;
        void* (*MaaWin32ControllerCreate)(void*, uint64_t, uint64_t, uint64_t) = nullptr;
        void (*MaaControllerDestroy)(void*) = nullptr;
        int64_t (*MaaControllerPostConnection)(void*) = nullptr;
        int (*MaaControllerWait)(void*, int64_t) = nullptr;
        bool (*MaaControllerConnected)(void*) = nullptr;
        bool (*MaaControllerGetResolution)(void*, int*, int*) = nullptr;
        bool (*MaaControllerGetUuid)(void*, void*) = nullptr;
        void* (*MaaStringBufferCreate)() = nullptr;
        void (*MaaStringBufferDestroy)(void*) = nullptr;
        const char* (*MaaStringBufferGet)(void*) = nullptr;
        uint64_t (*MaaStringBufferSize)(void*) = nullptr;
        bool (*MaaStringBufferSetEx)(void*, const char*, uint64_t) = nullptr;
    };

    struct AgentFn {
        void* (*MaaAgentClientCreateV2)(void*) = nullptr;
        void* (*MaaAgentClientCreateTcp)(uint16_t) = nullptr;
        void (*MaaAgentClientDestroy)(void*) = nullptr;
        bool (*MaaAgentClientBindResource)(void*, void*) = nullptr;
        bool (*MaaAgentClientConnect)(void*) = nullptr;
        bool (*MaaAgentClientAlive)(void*) = nullptr;
        bool (*MaaAgentClientSetTimeout)(void*, int64_t) = nullptr;
        bool (*MaaAgentClientIdentifier)(void*, void*) = nullptr;
    };

    template <typename T>
    bool sym(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(lib_, name));
        return target != nullptr;
    }
    template <typename T>
    bool asym(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(agent_lib_, name));
        return target != nullptr;
    }

    bool resolve_symbols() {
        bool ok = true;
        ok &= sym(fn_.MaaVersion, "MaaVersion");
        ok &= sym(fn_.MaaGlobalSetOption, "MaaGlobalSetOption");
        sym(fn_.MaaGlobalLoggingFlush, "MaaGlobalLoggingFlush");   // 缺了不影响加载
        ok &= sym(fn_.MaaTaskerCreate, "MaaTaskerCreate");
        ok &= sym(fn_.MaaTaskerDestroy, "MaaTaskerDestroy");
        ok &= sym(fn_.MaaTaskerBindResource, "MaaTaskerBindResource");
        ok &= sym(fn_.MaaTaskerBindController, "MaaTaskerBindController");
        ok &= sym(fn_.MaaTaskerPostTask, "MaaTaskerPostTask");
        ok &= sym(fn_.MaaTaskerPostStop, "MaaTaskerPostStop");
        ok &= sym(fn_.MaaTaskerStatus, "MaaTaskerStatus");
        ok &= sym(fn_.MaaTaskerWait, "MaaTaskerWait");
        ok &= sym(fn_.MaaTaskerInited, "MaaTaskerInited");
        ok &= sym(fn_.MaaTaskerRunning, "MaaTaskerRunning");
        ok &= sym(fn_.MaaTaskerAddSink, "MaaTaskerAddSink");
        ok &= sym(fn_.MaaResourceCreate, "MaaResourceCreate");
        ok &= sym(fn_.MaaResourceDestroy, "MaaResourceDestroy");
        ok &= sym(fn_.MaaResourcePostBundle, "MaaResourcePostBundle");
        ok &= sym(fn_.MaaResourceWait, "MaaResourceWait");
        ok &= sym(fn_.MaaResourceStatus, "MaaResourceStatus");
        ok &= sym(fn_.MaaResourceGetHash, "MaaResourceGetHash");
        ok &= sym(fn_.MaaWin32ControllerCreate, "MaaWin32ControllerCreate");
        ok &= sym(fn_.MaaControllerDestroy, "MaaControllerDestroy");
        ok &= sym(fn_.MaaControllerPostConnection, "MaaControllerPostConnection");
        ok &= sym(fn_.MaaControllerWait, "MaaControllerWait");
        ok &= sym(fn_.MaaControllerConnected, "MaaControllerConnected");
        ok &= sym(fn_.MaaControllerGetResolution, "MaaControllerGetResolution");
        ok &= sym(fn_.MaaControllerGetUuid, "MaaControllerGetUuid");
        ok &= sym(fn_.MaaStringBufferCreate, "MaaStringBufferCreate");
        ok &= sym(fn_.MaaStringBufferDestroy, "MaaStringBufferDestroy");
        ok &= sym(fn_.MaaStringBufferGet, "MaaStringBufferGet");
        ok &= sym(fn_.MaaStringBufferSize, "MaaStringBufferSize");
        ok &= sym(fn_.MaaStringBufferSetEx, "MaaStringBufferSetEx");
        return ok;
    }

    bool resolve_agent_symbols() {
        bool ok = true;
        ok &= asym(afn_.MaaAgentClientCreateV2, "MaaAgentClientCreateV2");
        ok &= asym(afn_.MaaAgentClientCreateTcp, "MaaAgentClientCreateTcp");
        ok &= asym(afn_.MaaAgentClientDestroy, "MaaAgentClientDestroy");
        ok &= asym(afn_.MaaAgentClientBindResource, "MaaAgentClientBindResource");
        ok &= asym(afn_.MaaAgentClientConnect, "MaaAgentClientConnect");
        ok &= asym(afn_.MaaAgentClientAlive, "MaaAgentClientAlive");
        ok &= asym(afn_.MaaAgentClientSetTimeout, "MaaAgentClientSetTimeout");
        ok &= asym(afn_.MaaAgentClientIdentifier, "MaaAgentClientIdentifier");
        return ok;
    }

    // ------------------------------------------------------------ sink 回调
    // MaaEventCallback: void(handle, message, details_json, trans_arg)
    using SinkT = void (*)(void*, const char*, const char*, void*);
    SinkT make_sink() {
        // worker 进程中只有一个 MaaEndAdapter 实例，用静态 thunk + 单例反查
        return [](void*, const char* message, const char* details_json, void*) {
            if (g_instance) g_instance->on_event(message ? message : "", details_json ? details_json : "");
        };
    }

    void on_event(const std::string& message, const std::string& raw) {
        AdapterEvent ev;
        ev.app = "maaend";
        ev.source = "fw:" + message;
        ev.raw = raw;
        // message 形如 Resource.Loading.Succeeded / Tasker.Task.Succeeded
        std::string cat = "info";
        size_t dot = message.find('.');
        if (dot != std::string::npos) cat = message.substr(0, dot);
        std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
        if (cat == "resource") cat = "connection";
        ev.category = cat;
        ev.what = message;
        if (!raw.empty()) {
            mj::Value d;
            if (mj::try_parse(raw, d)) {
                ev.details = std::move(d);
                ev.task = ev.details.get_str("entry", "");
                if (ev.task.empty()) ev.task = ev.details.get_str("node_name", "");
                ev.task_id = ev.details.get_int("task_id", 0);
            } else {
                ev.details.set("_raw", mj::Value(raw));
            }
        }
        {
            std::lock_guard<std::mutex> lk(st_mtx_);
            if (message.find("Task.Succeeded") != std::string::npos
                || message.find("Tasker.Task.Completed") != std::string::npos) {
                st_.run = "idle";
            } else if (message.find("Task.Failed") != std::string::npos) {
                st_.run = "failed";
                st_.last_error = raw.substr(0, 400);
                // 节点级原因只能去核心日志找：在 Task.Failed 之后调
                // MaaTaskerGetTaskDetail 会在 MaaFramework.dll 里 0xc0000005，
                // 直接把 worker 进程打挂（maaend-v5.15.0-beta.1 实测两次）
            }
        }
        if (message == "Node.PipelineNode.Starting") check_and_refit_window();
        emit(std::move(ev));
    }

    mj::Value cfg_;
    mj::Value interface_;
    mutable AppStatus st_;
    mutable double last_win_probe_ = 0;
    mutable double last_refit_check_ = 0;
    mutable bool cached_win_running_ = false;
    mutable std::string cached_win_title_;
    mutable std::mutex st_mtx_;
    HMODULE lib_ = nullptr;
    HMODULE agent_lib_ = nullptr;
    DLL_DIRECTORY_COOKIE cookie_ = nullptr;
    void* tasker_ = nullptr;
    void* res_ = nullptr;
    void* ctrl_ = nullptr;
    SinkT sink_ = nullptr;
    std::vector<util::ProcHandle> agent_procs_;
    mutable std::mutex proc_mtx_;      // 保护 agent_procs_
    mutable std::mutex cli_mtx_;      // 保护 agent_clients_
    std::mutex err_mtx_;      // 保护 agent_error_
    std::vector<void*> agent_clients_;
    std::string agent_error_;
    Fn fn_;
    AgentFn afn_;
    std::vector<mj::Value> catalog_;
    mj::Value all_opts_ = mj::Value::object();          // 合并后的 option 定义表
    std::map<std::string, TaskRoute> routes_;           // 任务名 → entry 节点 + 默认 override
    std::map<std::string, PreTask> pretasks_;           // pretask 名 → agent 子进程调用方式

public:
    static MaaEndAdapter* g_instance;
};

// 每个 worker 进程仅一个 MaaEndAdapter 实例
MaaEndAdapter* MaaEndAdapter::g_instance = nullptr;

} // namespace hub
