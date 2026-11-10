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
// 所有子进程挂进同一个 Job：父进程一退出（正常退出或崩溃）系统连带结束它们。
// 之前 worker 会变成孤儿：既继续占着设备/ADB，也牵着父进程的 stdout 管道不放。
inline HANDLE child_job() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (!j) return j;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(j, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
        return j;
    }();
    return job;
}

struct ProcHandle {
    HANDLE process = nullptr;
    HANDLE stdin_write = nullptr;   // 主进程写 -> worker 读
    HANDLE stdout_read = nullptr;   // worker 写 -> 主进程读
    DWORD pid = 0;
    bool alive() const {
        if (!process) return false;
        return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }
    void kill() {
        if (process) TerminateProcess(process, 1);
    }
    // 父进程不再需要 worker 时用它兜底：优雅退出等不到就结束，
    // 否则 MaaCore 会变成孤儿继续占着 ADB / 设备控制
    bool wait_exit(DWORD ms) {
        if (!process) return true;
        return WaitForSingleObject(process, ms) != WAIT_TIMEOUT;
    }
    // 只关管道、留着进程句柄：让 worker 读到 EOF，之后再等它退出
    void close_pipes() {
        if (stdin_write) { CloseHandle(stdin_write); stdin_write = nullptr; }
        if (stdout_read) { CloseHandle(stdout_read); stdout_read = nullptr; }
    }
    void close() {
        close_pipes();
        if (process) { CloseHandle(process); process = nullptr; }
        pid = 0;
    }
};

// 启动子进程，stdin/stdout 重定向到管道，stderr 指向 NUL。
// 不能直接继承父进程的 stderr：孩子会牵着父进程的输出句柄不放（SSH / 管道调用方
// 等不到 EOF），而 worker 的诊断本来就写进 asst.log / maahub-crash.log。
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

    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE use_err = (nul != nullptr && nul != INVALID_HANDLE_VALUE) ? nul : out_write;

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = use_err;
    si.StartupInfo.wShowWindow = SW_HIDE;

    // 句柄白名单：只有这三个进得去子进程，父进程其余可继承句柄（包括它自己的
    // stdout 管道）一律不带过去
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    LPPROC_THREAD_ATTRIBUTE_LIST attr = nullptr;
    std::vector<HANDLE> keep = { in_read, out_write };
    std::vector<char> attr_buf;
    if (use_err != out_write) keep.push_back(use_err);
    SIZE_T asize = 0;
    if (InitializeProcThreadAttributeList(nullptr, 1, 0, &asize)
        || GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        attr_buf.assign(asize, 0);
        attr = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
        if (InitializeProcThreadAttributeList(attr, 1, 0, &asize)
            && UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         keep.data(), keep.size() * sizeof(HANDLE),
                                         nullptr, nullptr)) {
            si.lpAttributeList = attr;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        } else {
            attr = nullptr;
        }
    }

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
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags,
                             env_ptr, workdir.empty() ? nullptr : workdir.c_str(),
                             &si.StartupInfo, &pi);
    if (attr) DeleteProcThreadAttributeList(attr);
    if (nul) CloseHandle(nul);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!ok) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        return false;
    }
    CloseHandle(pi.hThread);
    if (HANDLE job = child_job()) AssignProcessToJobObject(job, pi.hProcess);
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

// 收尾排查用：--cli 跑完任务后卡死时，只有日志能指出卡在 shutdown 的哪一步
inline void trace(const std::string& what) {
    static CRITICAL_SECTION cs = [] {
        CRITICAL_SECTION c;
        InitializeCriticalSection(&c);
        return c;
    }();
    ensure_dir(exe_dir() + "\\state");
    EnterCriticalSection(&cs);
    if (FILE* f = fopen((exe_dir() + "\\state\\trace.log").c_str(), "a")) {
        fprintf(f, "%.1f pid=%lu tid=%lu %s\n", now_sec(), GetCurrentProcessId(),
                GetCurrentThreadId(), what.c_str());
        fclose(f);
    }
    LeaveCriticalSection(&cs);
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

// 原子写：先写同目录 .tmp，FlushFileBuffers 落盘后 MoveFileExW 原子替换目标。
// 旧行为 CREATE_ALWAYS 直接截断目标，进程被杀/断电会留下半截文件（settings.json 损坏根因）。
inline bool write_file(const std::string& path, const std::string& content) {
    ensure_dir(parent_dir(path));
    auto write_full = [&content](HANDLE h) {
        BOOL ok = TRUE;
        const char* p = content.data();
        size_t left = content.size();
        while (ok && left > 0) {
            DWORD chunk = left > 0x40000000u ? 0x40000000u : (DWORD)left;
            DWORD n = 0;
            ok = WriteFile(h, p, chunk, &n, nullptr);
            if (!ok || n != chunk) { ok = FALSE; break; }
            p += chunk;
            left -= chunk;
        }
        if (ok) ok = FlushFileBuffers(h);
        CloseHandle(h);
        return ok != FALSE;
    };
    std::string tmp = path + ".tmp";
    HANDLE h = CreateFileW(utf8_to_wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // 临时文件建不了（目录只读/杀软拦截）时退回原地直写，至少不比旧行为差
        h = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        return write_full(h);
    }
    if (!write_full(h)) {
        DeleteFileW(utf8_to_wide(tmp).c_str());
        return false;
    }
    if (!MoveFileExW(utf8_to_wide(tmp).c_str(), utf8_to_wide(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(utf8_to_wide(tmp).c_str());
        return false;
    }
    return true;
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

inline std::string trim_ascii(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 运行命令并捕获 stdout（无窗口；超时即杀）
inline bool run_capture(const std::wstring& cmdline, std::string& out, DWORD max_wait_ms = 6000) {
    ProcHandle p;
    if (!spawn_process(cmdline, L"", L"", p)) return false;
    if (p.stdin_write) { CloseHandle(p.stdin_write); p.stdin_write = nullptr; }
    char buf[1024];
    DWORD t0 = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(p.stdout_read, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            if (GetTickCount() - t0 > max_wait_ms) break;
            Sleep(20);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(p.stdout_read, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        out.append(buf, n);
    }
    if (p.alive()) TerminateProcess(p.process, 0);
    p.close();
    return !out.empty();
}

struct AdbDevice {
    std::string adb_path;
    std::string address;   // 设备序列号，如 127.0.0.1:16384
    std::string config;    // MAA 连接配置名，如 MuMuEmulator12 / General
};

// 探测本机 adb 与在线设备（模拟器优先），供 MAA 的 ADB 连接做默认值
inline bool detect_adb(AdbDevice& out) {
    static const char* kAdbPaths[] = {
        "C:\\Program Files\\Netease\\MuMuPlayer\\nx_main\\adb.exe",
        "C:\\Program Files\\Netease\\MuMu Player 12\\nx_main\\adb.exe",
        "D:\\Program Files\\Netease\\MuMuPlayer\\nx_main\\adb.exe",
        "D:\\Program Files\\Netease\\MuMu Player 12\\nx_main\\adb.exe",
        "D:\\MuMuPlayer\\nx_main\\adb.exe",
        "C:\\leidian\\LDPlayer9\\adb.exe",
        "D:\\leidian\\LDPlayer9\\adb.exe",
        "C:\\LDPlayer\\LDPlayer9\\adb.exe",
        "C:\\Program Files\\LDPlayer9\\adb.exe",
        "C:\\Program Files\\BlueStacks_nxt\\hd-adb.exe",
        "C:\\Program Files (x86)\\Nox\\bin\\nox_adb.exe",
        "D:\\Nox\\bin\\nox_adb.exe",
    };
    std::string path;
    for (auto* c : kAdbPaths) {
        if (file_exists(c)) { path = c; break; }
    }
    if (path.empty()) {
        // PATH 里逐个目录找 adb.exe（避免起子进程，输出不被污染）
        wchar_t buf[32768];
        DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32768);
        if (n > 0 && n < 32768) {
            std::wstring p(buf, n);
            size_t pos = 0;
            while (pos <= p.size()) {
                size_t semi = p.find(L';', pos);
                std::wstring dir = p.substr(pos, semi == std::wstring::npos
                                               ? std::wstring::npos : semi - pos);
                pos = (semi == std::wstring::npos) ? p.size() + 1 : semi + 1;
                if (dir.empty()) continue;
                if (dir.back() != L'\\') dir += L'\\';
                std::string cand = wide_to_utf8(dir + L"adb.exe");
                if (file_exists(cand)) { path = cand; break; }
            }
        }
    }
    if (path.empty()) return false;

    std::string o;
    std::string first;
    if (run_capture(L"\"" + utf8_to_wide(path) + L"\" devices", o, 8000)) {
        size_t pos = 0;
        while (pos < o.size()) {
            size_t nl = o.find('\n', pos);
            std::string line = trim_ascii(o.substr(pos, nl == std::string::npos ?
                                                      std::string::npos : nl - pos));
            pos = (nl == std::string::npos) ? o.size() : nl + 1;
            if (line.empty() || line.rfind("List of devices", 0) == 0) continue;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string serial = line.substr(0, tab);
            std::string state = line.substr(tab + 1);
            if (state != "device") continue;
            if (first.empty()) first = serial;
            // 模拟器本地端口优先（127.0.0.1 / emulator-）
            if (serial.rfind("127.0.0.1:", 0) == 0 || serial.rfind("emulator-", 0) == 0) {
                first = serial;
                break;
            }
        }
    }
    if (first.empty()) return false;

    out.adb_path = path;
    out.address = first;
    std::string lower = path;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    if (lower.find("mumu") != std::string::npos) out.config = "MuMuEmulator12";
    else if (lower.find("ldplayer") != std::string::npos) out.config = "LDPlayer";
    else if (lower.find("bluestacks") != std::string::npos) out.config = "BlueStacks";
    else if (lower.find("nox") != std::string::npos) out.config = "Nox";
    else out.config = "General";
    return true;
}

} // namespace util
