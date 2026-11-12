// parse_url 搬移等价性差分测试（本轮第 7 项专用门）
// orig_parse = core_manager.hpp 旧内联块（逐字节复制自改动前源码）
// new_parse  = 提取后的 CoreManager::parse_url 函数体（逐字节复制自当前源码）
// 断言两者对同一 URL 表产出 (host, path, port, https) 完全一致。
#include <string>
#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include <cstdio>
#include <utility>

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

// ---------- 改动前的原始内联块（逐字节保留，含 std::wstring::npos 笔误） ----------
static void orig_parse(const std::string& url, std::wstring& host, std::wstring& path,
                       INTERNET_PORT& port, bool& https) {
    host.clear(); path.clear();
    port = INTERNET_DEFAULT_HTTPS_PORT;
    https = true;
    {
        std::string u = url;
        if (u.rfind("https://", 0) == 0) u = u.substr(8);
        else if (u.rfind("http://", 0) == 0) { u = u.substr(7); https = false; port = INTERNET_DEFAULT_HTTP_PORT; }
        size_t slash = u.find('/');
        std::string hostpart = slash == std::wstring::npos ? u : u.substr(0, slash);
        path = utf8_to_wide(slash == std::string::npos ? "/" : u.substr(slash));
        size_t colon = hostpart.find(':');
        if (colon != std::string::npos) {
            host = utf8_to_wide(hostpart.substr(0, colon));
            port = (INTERNET_PORT)atoi(hostpart.substr(colon + 1).c_str());
        } else {
            host = utf8_to_wide(hostpart);
        }
    }
}

// ---------- 提取后的 helper 体（逐字节复制自当前 core_manager.hpp；r13 已规范化 npos） ----------
static void new_parse(const std::string& url, std::wstring& host, std::wstring& path,
                      INTERNET_PORT& port, bool& https) {
    host.clear(); path.clear();
    port = INTERNET_DEFAULT_HTTPS_PORT;
    https = true;
    std::string u = url;
    if (u.rfind("https://", 0) == 0) u = u.substr(8);
    else if (u.rfind("http://", 0) == 0) { u = u.substr(7); https = false; port = INTERNET_DEFAULT_HTTP_PORT; }
    size_t slash = u.find('/');
    std::string hostpart = slash == std::string::npos ? u : u.substr(0, slash);
    path = utf8_to_wide(slash == std::string::npos ? "/" : u.substr(slash));
    size_t colon = hostpart.find(':');
    if (colon != std::string::npos) {
        host = utf8_to_wide(hostpart.substr(0, colon));
        port = (INTERNET_PORT)atoi(hostpart.substr(colon + 1).c_str());
    } else {
        host = utf8_to_wide(hostpart);
    }
}

int main() {
    const char* urls[] = {
        "https://api.github.com/repos/MAA1999/MaaEnd/releases/latest",
        "https://github.com",
        "http://example.com/a/b?c=1",
        "https://raw.githubusercontent.com/x/y/v1.2.3/file.json",
        "http://127.0.0.1:7860/sdapi/v1/options",
        "https://10.10.224.254:8443/api",
        "https://host:8443",
        "http://host:0",
        "https://host:",
        "example.com/noScheme",
        "",
        "/",
        "://weird",
        "https://",
        "http://",
        "https://a.b:65535/path/to/obj",
        "https://a.b:999999/x",
        "https://a.b:-1/x",
        "https://xn--fiqs8s.example/\xe4\xb8\xad\xe6\x96\x87",
    };
    int bad = 0;
    for (auto s : urls) {
        std::wstring h1, p1, h2, p2; INTERNET_PORT pt1 = 0, pt2 = 0; bool g1 = false, g2 = false;
        orig_parse(s, h1, p1, pt1, g1);
        new_parse(s, h2, p2, pt2, g2);
        if (h1 != h2 || p1 != p2 || pt1 != pt2 || g1 != g2) {
            ++bad;
            printf("MISMATCH <%s> orig=(%ls,%ls,%u,%d) new=(%ls,%ls,%u,%d)\n",
                   s, h1.c_str(), p1.c_str(), (unsigned)pt1, (int)g1,
                   h2.c_str(), p2.c_str(), (unsigned)pt2, (int)g2);
        }
    }
    printf("DIFFERENTIAL parse_url: %zu urls, mismatches=%d\n",
           sizeof(urls)/sizeof(urls[0]), bad);
    return bad == 0 ? 0 : 1;
}
