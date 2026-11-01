// 公共工具：UTF-8/Wide 转换、Base64、SHA1、窗口查找、进程启动、时间格式化
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <chrono>

namespace util {

inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

// ---------------- Base64 ----------------
inline std::string base64_encode(const uint8_t* data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = data[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (len - i == 2) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

inline std::string base64_decode(const std::string& in) {
    static int8_t tbl[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) tbl[i] = -1;
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) tbl[(unsigned char)chars[i]] = (int8_t)i;
        init = true;
    }
    std::string out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') break;
        int8_t v = tbl[(unsigned char)c];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

inline std::string base64_encode_str(const std::string& s) {
    return base64_encode((const uint8_t*)s.data(), s.size());
}

// ---------------- SHA1 ----------------
inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::vector<uint8_t> msg(data, data + len);
    uint64_t bitlen = (uint64_t)len * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)((bitlen >> (i * 8)) & 0xFF));

    auto rol = [](uint32_t v, int c) { return (v << c) | (v >> (32 - c)); };

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint32_t)msg[chunk + i * 4] << 24) | ((uint32_t)msg[chunk + i * 4 + 1] << 16)
                 | ((uint32_t)msg[chunk + i * 4 + 2] << 8) | (uint32_t)msg[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)hs[i];
    }
}

// ---------------- 窗口查找 ----------------
struct WindowInfo {
    HWND hwnd = nullptr;
    std::string cls;
    std::string title;
    DWORD pid = 0;
};

// 按窗口类名 + 标题正则关键字查找可见顶层窗口（与 Python 版行为一致）
inline bool find_window(const std::string& class_regex, const std::string& title_regex, WindowInfo& out) {
    struct Ctx {
        const std::string* cls;
        const std::string* title_re;
        WindowInfo* out;
        bool found = false;
    } ctx{ &class_regex, &title_regex, &out };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t cls[256] = {};
        GetClassNameW(hwnd, cls, 256);
        std::wstring wcls(cls);
        if (!c->cls->empty()) {
            std::string a = util::wide_to_utf8(wcls);
            if (a != *c->cls) return TRUE;
        }
        wchar_t title[512] = {};
        GetWindowTextW(hwnd, title, 512);
        std::string t = util::wide_to_utf8(title);
        if (!c->title_re->empty()) {
            // 简易大小写不敏感子串匹配（标题关键字场景足够）
            std::string hay = t, needle = *c->title_re;
            for (auto& ch : hay) ch = (char)tolower((unsigned char)ch);
            for (auto& ch : needle) ch = (char)tolower((unsigned char)ch);
            if (hay.find(needle) == std::string::npos) return TRUE;
        }
        c->out->hwnd = hwnd;
        c->out->cls = util::wide_to_utf8(wcls);
        c->out->title = t;
        GetWindowThreadProcessId(hwnd, &c->out->pid);
        c->found = true;
        return FALSE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

// ---------------- 进程启动 ----------------
struct ProcHandle {
    HANDLE process = nullptr;
    HANDLE stdin_write = nullptr;   // 主进程写 -> worker 读
    HANDLE stdout_read = nullptr;   // worker 写 -> 主进程读
    DWORD pid = 0;
    bool alive() const {
        if (!process) return false;
        return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }
    void close() {
        for (HANDLE h : { stdin_write, stdout_read, process }) {
            if (h) { CloseHandle(h); }
        }
        stdin_write = nullptr;
        stdout_read = nullptr;
        process = nullptr;
    }
};

// 启动子进程，stdin/stdout 重定向到管道（stderr 继承）
inline bool spawn_process(const std::wstring& cmdline, const std::wstring& workdir,
                          const std::wstring& env_block, ProcHandle& out) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) return false;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        CloseHandle(in_read); CloseHandle(in_write);
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.wShowWindow = SW_HIDE;

    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> env_copy;
    LPVOID env_ptr = nullptr;
    if (!env_block.empty()) {
        env_copy.assign(env_block.begin(), env_block.end());
        env_copy.push_back(L'\0');
        env_ptr = env_copy.data();
    }
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                             env_ptr, workdir.empty() ? nullptr : workdir.c_str(), &si, &pi);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!ok) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        return false;
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.pid = pi.dwProcessId;
    out.stdin_write = in_write;
    out.stdout_read = out_read;
    return true;
}

// 构造环境块（在 PATH 前插入目录），返回 "\0\0" 结尾的宽字符串
inline std::wstring make_env_block(const std::wstring& extra_path_dir) {
    std::wstring path;
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32768);
    if (n > 0) path.assign(buf, n);
    std::wstring env = L"PATH=" + extra_path_dir + (path.empty() ? L"" : (L";" + path)) + L"\0";
    // 只传 PATH 足够（CreateProcess 会用父进程其余环境？不会——传完整块才安全）
    // 因此这里拷贝整个环境块并替换 PATH
    LPWCH old = GetEnvironmentStringsW();
    std::wstring full;
    if (old) {
        for (LPWCH p = old; *p; ) {
            size_t len = wcslen(p);
            std::wstring entry(p, len);
            if (_wcsnicmp(entry.c_str(), L"PATH=", 5) != 0) {
                full += entry;
                full += L'\0';
            }
            p += len + 1;
        }
        FreeEnvironmentStringsW(old);
    }
    full += env;
    full += L'\0';
    return full;
}

inline uint64_t now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline double now_sec() {
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
}

inline std::string now_str() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

inline std::string today_str() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return buf;
}

inline std::string exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return util::wide_to_utf8(p);
}

inline std::string parent_dir(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    size_t pos = p.find_last_of("\\/");
    return pos == std::string::npos ? std::string() : p.substr(0, pos);
}

inline bool file_exists(const std::string& path) {
    DWORD a = GetFileAttributesW(utf8_to_wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool dir_exists(const std::string& path) {
    DWORD a = GetFileAttributesW(utf8_to_wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline void ensure_dir(const std::string& path) {
    std::string cur;
    size_t i = 0;
    while (i < path.size()) {
        size_t next = path.find_first_of("\\/", i);
        if (next == std::string::npos) next = path.size();
        cur = path.substr(0, next);
        if (!cur.empty() && !dir_exists(cur)) CreateDirectoryW(utf8_to_wide(cur).c_str(), nullptr);
        i = next + 1;
    }
}

inline bool read_file(const std::string& path, std::string& out) {
    HANDLE h = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD n = 0;
    out.clear();
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(h);
    return true;
}

inline bool write_file(const std::string& path, const std::string& content) {
    ensure_dir(parent_dir(path));
    HANDLE h = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &n, nullptr);
    CloseHandle(h);
    return ok && n == content.size();
}

inline std::string read_file_tail(const std::string& path, size_t max_bytes = 65536) {
    HANDLE h = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return {}; }
    LONGLONG start = size.QuadPart > (LONGLONG)max_bytes ? size.QuadPart - (LONGLONG)max_bytes : 0;
    LARGE_INTEGER li{};
    li.QuadPart = start;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    std::string out;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(h);
    return out;
}

} // namespace util
