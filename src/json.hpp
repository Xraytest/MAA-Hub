// MaaHub 极简 JSON 库（header-only，零依赖）
// 支持：对象/数组/字符串(含转义与UTF-8)/整数/浮点/布尔/null
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace mj {

class Value;
using ValuePtr = std::shared_ptr<Value>;

class Value {
public:
    enum Type { Null, Bool, Int, Double, Str, Array, Object };

    Value() : type_(Null) {}
    Value(std::nullptr_t) : type_(Null) {}
    Value(bool b) : type_(Bool), b_(b) {}
    Value(int i) : type_(Int), i_(i) {}
    Value(int64_t i) : type_(Int), i_(i) {}
    Value(size_t i) : type_(Int), i_((int64_t)i) {}
    Value(double d) : type_(Double), d_(d) {}
    Value(const char* s) : type_(Str), s_(s ? s : "") {}
    Value(const std::string& s) : type_(Str), s_(s) {}
    Value(std::string&& s) : type_(Str), s_(std::move(s)) {}

    static Value array() { Value v; v.type_ = Array; return v; }
    static Value object() { Value v; v.type_ = Object; return v; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Null; }
    bool is_bool() const { return type_ == Bool; }
    bool is_number() const { return type_ == Int || type_ == Double; }
    bool is_int() const { return type_ == Int; }
    bool is_string() const { return type_ == Str; }
    bool is_array() const { return type_ == Array; }
    bool is_object() const { return type_ == Object; }

    bool as_bool(bool def = false) const {
        if (type_ == Bool) return b_;
        if (type_ == Int) return i_ != 0;
        return def;
    }
    int64_t as_int(int64_t def = 0) const {
        if (type_ == Int) return i_;
        if (type_ == Double) return (int64_t)d_;
        if (type_ == Bool) return b_ ? 1 : 0;
        return def;
    }
    double as_double(double def = 0.0) const {
        if (type_ == Double) return d_;
        if (type_ == Int) return (double)i_;
        return def;
    }
    const std::string& as_string() const {
        static const std::string empty;
        return type_ == Str ? s_ : empty;
    }

    // 容器访问
    std::vector<Value>& items() { return arr_; }
    const std::vector<Value>& items() const { return arr_; }
    std::map<std::string, Value>& members() { return obj_; }
    const std::map<std::string, Value>& members() const { return obj_; }

    void push(Value v) { arr_.push_back(std::move(v)); }
    void set(const std::string& key, Value v) { obj_[key] = std::move(v); }

    bool has(const std::string& key) const { return type_ == Object && obj_.count(key) > 0; }
    const Value* find(const std::string& key) const {
        if (type_ != Object) return nullptr;
        auto it = obj_.find(key);
        return it == obj_.end() ? nullptr : &it->second;
    }
    Value& operator[](const std::string& key) {
        type_ = Object;
        return obj_[key];
    }
    const Value& operator[](const std::string& key) const {
        static const Value null_value;
        auto it = obj_.find(key);
        return it == obj_.end() ? null_value : it->second;
    }

    // 便捷读取
    std::string get_str(const std::string& key, const std::string& def = "") const {
        auto* v = find(key);
        return (v && v->is_string()) ? v->as_string() : def;
    }
    int64_t get_int(const std::string& key, int64_t def = 0) const {
        auto* v = find(key);
        return (v && v->is_number()) ? v->as_int() : def;
    }
    bool get_bool(const std::string& key, bool def = false) const {
        auto* v = find(key);
        return (v && (v->is_bool() || v->is_number())) ? v->as_bool() : def;
    }

    // 点号路径读取（如 "connect.mode"、"server.port"），逐层下钻
    const Value* find_path(const std::string& dotted) const {
        const Value* cur = this;
        size_t start = 0;
        while (cur) {
            size_t dot = dotted.find('.', start);
            std::string part = dotted.substr(start, dot == std::string::npos
                                                     ? std::string::npos : dot - start);
            cur = cur->find(part);
            if (dot == std::string::npos) return cur;
            start = dot + 1;
        }
        return nullptr;
    }
    std::string get_path_str(const std::string& dotted, const std::string& def = "") const {
        auto* v = find_path(dotted);
        return (v && v->is_string()) ? v->as_string() : def;
    }
    int64_t get_path_int(const std::string& dotted, int64_t def = 0) const {
        auto* v = find_path(dotted);
        return (v && v->is_number()) ? v->as_int() : def;
    }
    bool get_path_bool(const std::string& dotted, bool def = false) const {
        auto* v = find_path(dotted);
        return (v && (v->is_bool() || v->is_number())) ? v->as_bool() : def;
    }

    std::string dump() const { return dump(*this); }

    static std::string dump(const Value& v) {
        std::string out;
        write(v, out, -1, 0);
        return out;
    }
    static std::string dump_pretty(const Value& v, int indent = 2) {
        std::string out;
        write(v, out, indent, 0);
        return out;
    }

private:
    static void escape(const std::string& s, std::string& out) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
            }
        }
        out += '"';
    }

    static void write(const Value& v, std::string& out, int indent, int depth) {
        auto nl = [&]() {
            if (indent >= 0) {
                out += '\n';
                out.append((size_t)indent * (depth + 1), ' ');
            }
        };
        switch (v.type_) {
        case Null: out += "null"; break;
        case Bool: out += v.b_ ? "true" : "false"; break;
        case Int: out += std::to_string(v.i_); break;
        case Double: {
            char buf[40];
            snprintf(buf, sizeof(buf), "%.10g", v.d_);
            out += buf;
            break;
        }
        case Str: escape(v.s_, out); break;
        case Array:
            if (v.arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < v.arr_.size(); ++i) {
                if (i) out += indent >= 0 ? "," : ", ";
                nl();
                write(v.arr_[i], out, indent, depth + 1);
            }
            nl();
            out += ']';
            break;
        case Object:
            if (v.obj_.empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (auto& kv : v.obj_) {
                if (!first) out += indent >= 0 ? "," : ", ";
                first = false;
                nl();
                escape(kv.first, out);
                out += indent >= 0 ? ": " : ":";
                write(kv.second, out, indent, depth + 1);
            }
            nl();
            out += '}';
            break;
        }
    }

    Type type_;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<Value> arr_;
    std::map<std::string, Value> obj_;

    friend class Parser;
};

class ParseError : public std::exception {
public:
    explicit ParseError(std::string msg) : msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }
private:
    std::string msg_;
};

class Parser {
public:
    explicit Parser(const std::string& src) : s_(src), i_(0) {}

    Value parse() {
        skip_ws();
        Value v = parse_value(0);
        skip_ws();
        if (i_ != s_.size()) throw ParseError("trailing characters");
        return v;
    }

private:
    const std::string& s_;
    size_t i_;

    [[noreturn]] void err(const std::string& m) { throw ParseError(m + " at offset " + std::to_string(i_)); }

    void skip_ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i_; continue; }
            // JSONC：MaaEnd 官方 interface 任务文件里带 // 与 /* */ 注释，
            // 整文件解析失败会连带丢掉该文件的全部任务和参数定义。
            if (c == '/' && i_ + 1 < s_.size()) {
                if (s_[i_ + 1] == '/') {
                    i_ += 2;
                    while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                    continue;
                }
                if (s_[i_ + 1] == '*') {
                    size_t e = s_.find("*/", i_ + 2);
                    i_ = (e == std::string::npos) ? s_.size() : e + 2;
                    continue;
                }
            }
            break;
        }
    }

    char peek() { return i_ < s_.size() ? s_[i_] : '\0'; }

    Value parse_value(int depth) {
        if (depth > 64) err("nesting too deep");
        skip_ws();
        char c = peek();
        switch (c) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': return Value(parse_string());
        case 't':
            expect("true"); return Value(true);
        case 'f':
            expect("false"); return Value(false);
        case 'n':
            expect("null"); return Value(nullptr);
        default:
            return parse_number();
        }
    }

    void expect(const char* lit) {
        size_t n = strlen(lit);
        if (s_.compare(i_, n, lit) != 0) err(std::string("expected ") + lit);
        i_ += n;
    }

    Value parse_object(int depth) {
        Value v = Value::object();
        ++i_; // {
        skip_ws();
        if (peek() == '}') { ++i_; return v; }
        while (true) {
            skip_ws();
            if (peek() == '}') { ++i_; return v; }   // 容忍尾随逗号
            if (peek() != '"') err("expected object key");
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') err("expected ':'");
            ++i_;
            skip_ws();
            v.obj_[key] = parse_value(depth + 1);
            skip_ws();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == '}') { ++i_; break; }
            err("expected ',' or '}'");
        }
        return v;
    }

    Value parse_array(int depth) {
        Value v = Value::array();
        ++i_; // [
        skip_ws();
        if (peek() == ']') { ++i_; return v; }
        while (true) {
            skip_ws();
            if (peek() == ']') { ++i_; return v; }   // 容忍尾随逗号
            v.arr_.push_back(parse_value(depth + 1));
            skip_ws();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == ']') { ++i_; break; }
            err("expected ',' or ']'");
        }
        return v;
    }

    std::string parse_string() {
        std::string out;
        ++i_; // opening quote
        while (true) {
            if (i_ >= s_.size()) err("unterminated string");
            unsigned char c = (unsigned char)s_[i_++];
            if (c == '"') break;
            if (c == '\\') {
                if (i_ >= s_.size()) err("bad escape");
                char e = s_[i_++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i_ + 4 > s_.size()) err("bad \\u escape");
                    unsigned cp = hex4();
                    // 代理对处理
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size()
                        && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            append_utf8(out, cp);
                            cp = lo;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: err("bad escape char");
                }
            } else {
                out += (char)c;
            }
        }
        return out;
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else err("bad hex digit");
        }
        return v;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp <= 0x7F) {
            out += (char)cp;
        } else if (cp <= 0x7FF) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    Value parse_number() {
        size_t start = i_;
        if (peek() == '-' || peek() == '+') ++i_;
        bool is_double = false;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c >= '0' && c <= '9') { ++i_; continue; }
            if (c == '.' || c == 'e' || c == 'E') { is_double = true; ++i_; continue; }
            if (c == '+' || c == '-') {
                // 只允许出现在 e/E 之后
                if (i_ > start && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E')) { ++i_; continue; }
            }
            break;
        }
        if (start == i_) err("invalid value");
        std::string num = s_.substr(start, i_ - start);
        try {
            if (is_double) return Value(std::stod(num));
            return Value((int64_t)std::stoll(num));
        } catch (...) {
            err("invalid number");
        }
    }
};

inline Value parse(const std::string& src) { return Parser(src).parse(); }

inline bool try_parse(const std::string& src, Value& out) {
    try {
        // 容忍 UTF-8 BOM（记事本 / PowerShell 5.x Set-Content -Encoding UTF8 会加）
        const char* p = src.c_str();
        size_t n = src.size();
        if (n >= 3 && (unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            p += 3;
            n -= 3;
        }
        out = Parser(std::string(p, n)).parse();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace mj
