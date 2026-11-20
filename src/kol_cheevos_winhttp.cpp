// RetroAchievements' HTTP on Windows: WinHTTP, the system's own (kol_cheevos.h).
// Windows ships no libcurl; the other desktops use kol_cheevos_curl.cpp, Android
// Java (main_android.cpp, Cheevos.java). WinHTTP takes the system's
// certificates, proxy settings and TLS versions, and follows redirects.

#include "kol_cheevos.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace kol {
namespace {

std::wstring wide(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// One session for the game's life (WinHTTP's handles are thread-safe; requests
// come from rc_client's worker one at a time).
HINTERNET session(const char* user_agent) {
    static HINTERNET s = [user_agent] {
        const std::wstring ua = wide(user_agent);
        // The system's proxy settings (Windows 8.1 on), else the WinHTTP default.
        HINTERNET h = WinHttpOpen(ua.c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)
            h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            // TLS 1.2 and 1.3 where the system has them (Windows 7 needs 1.2 asked for).
            DWORD tls = 0x00000800 /* TLS1_2 */ | 0x00002000 /* TLS1_3 */;
            if (!WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) {
                tls = 0x00000800;
                WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
            }
            WinHttpSetTimeouts(h, 30000, 30000, 30000, 30000);
        }
        return h;
    }();
    return s;
}

bool winhttp_post(const char* url, const char* post, const char* content_type, const char* user_agent,
                  int* status, std::string* body) {
    HINTERNET s = session(user_agent);
    if (!s) {
        std::fprintf(stderr, "cheevos: http: WinHttpOpen failed (%lu)\n", GetLastError());
        return false;
    }
    const std::wstring wurl = wide(url);
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    wchar_t host[256], path[2048];
    u.lpszHostName = host;
    u.dwHostNameLength = static_cast<DWORD>(sizeof host / sizeof host[0]);
    u.lpszUrlPath = path;
    u.dwUrlPathLength = static_cast<DWORD>(sizeof path / sizeof path[0]);
    wchar_t extra[2048];
    u.lpszExtraInfo = extra;
    u.dwExtraInfoLength = static_cast<DWORD>(sizeof extra / sizeof extra[0]);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) {
        std::fprintf(stderr, "cheevos: http: bad URL\n");
        return false;
    }
    const std::wstring object = std::wstring(path, u.dwUrlPathLength) + std::wstring(extra, u.dwExtraInfoLength);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, std::wstring(host, u.dwHostNameLength).c_str(), u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, post ? L"POST" : L"GET", object.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (r) {
        std::wstring headers;
        if (content_type) headers = L"Content-Type: " + wide(content_type) + L"\r\n";
        const DWORD len = post ? static_cast<DWORD>(std::strlen(post)) : 0;
        if (WinHttpSendRequest(r, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                               headers.empty() ? 0 : static_cast<DWORD>(-1L),
                               post ? const_cast<char*>(post) : WINHTTP_NO_REQUEST_DATA, len, len, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, size = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &size, WINHTTP_NO_HEADER_INDEX);
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                if (avail == 0) break;
                const std::size_t at = body->size();
                body->resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, body->data() + at, avail, &got)) { ok = false; break; }
                body->resize(at + got);
                if (got == 0) break;
            }
            *status = static_cast<int>(code);
        }
    }
    if (!ok) std::fprintf(stderr, "cheevos: http: WinHTTP error %lu\n", GetLastError());
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    return ok;
}

void print_notice(const char* text) { std::fprintf(stderr, "[RetroAchievements] %s\n", text); }

}  // namespace

// The desktop's setup (as kol_cheevos_curl.cpp's): the HTTP, the notices on
// stderr, and a login from KOL_RA_USER + KOL_RA_PASSWORD if both are set.
void cheevos_desktop_init() {
    static bool done = false;
    if (done) return;
    done = true;
    cheevos_set_http(&winhttp_post);
    cheevos_set_notify(&print_notice);
    cheevos_set_platform("Windows");
    const char* u = std::getenv("KOL_RA_USER");
    const char* p = std::getenv("KOL_RA_PASSWORD");
    if (u && *u && p && *p) cheevos_login(u, p);
    else cheevos_start();
}

}  // namespace kol
