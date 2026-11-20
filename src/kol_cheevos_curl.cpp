// RetroAchievements' HTTP on the desktop: libcurl (kol_cheevos.h). Android
// has its own, through Java (main_android.cpp, Cheevos.java).

#include "kol_cheevos.h"

#include <curl/curl.h>

#include <cstdlib>
#include <cstdio>
#include <string>

namespace kol {
namespace {

size_t on_body(char* data, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

// The system's certificate bundle (Linux). The AppImage brings its own libcurl
// and OpenSSL, from the freedesktop SDK, whose OpenSSL looks under
// /etc/pki/tls (Fedora's layout); SteamOS keeps them in /etc/ssl/certs, so
// every request failed to connect ("No answer from the server", 2026-11-20,
// the Deck in Gaming Mode: curl error 35, "TLS connect error ... No such file
// or directory"). The first of the distributions' usual places that exists;
// none when SSL_CERT_FILE says where (OpenSSL reads it), or off Linux.
const char* ca_bundle() {
#if defined(__linux__)
    static const char* const found = []() -> const char* {
        if (const char* e = std::getenv("SSL_CERT_FILE"); e && *e) return nullptr;
        static const char* const kPlaces[] = {
            "/etc/ssl/certs/ca-certificates.crt",                  // Debian, Ubuntu, Arch, SteamOS
            "/etc/pki/tls/certs/ca-bundle.crt",                    // Fedora, RHEL
            "/etc/ssl/ca-bundle.pem",                              // openSUSE
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",   // CentOS, RHEL 7
            "/etc/pki/tls/cacert.pem",
            "/etc/ssl/cert.pem",                                   // Alpine
        };
        for (const char* p : kPlaces)
            if (std::FILE* f = std::fopen(p, "rb")) {
                std::fclose(f);
                return p;
            }
        return nullptr;
    }();
    return found;
#else
    return nullptr;
#endif
}

bool curl_post(const char* url, const char* post, const char* content_type, const char* user_agent,
               int* status, std::string* body) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_slist* headers = nullptr;
    if (content_type) headers = curl_slist_append(headers, (std::string("Content-Type: ") + content_type).c_str());
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (const char* ca = ca_bundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
    const CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        std::fprintf(stderr, "cheevos: http %s\n", curl_easy_strerror(rc));
        return false;
    }
    *status = static_cast<int>(code);
    return true;
}

void print_notice(const char* text) { std::fprintf(stderr, "[RetroAchievements] %s\n", text); }

}  // namespace

// The desktop's setup, from the per-frame policy's first call: the HTTP, the
// notices on stderr, and a login from the environment (there is no text entry
// on the desktop): KOL_RA_USER + KOL_RA_PASSWORD log in once and save the
// token; after that the token is enough.
void cheevos_desktop_init() {
    static bool done = false;
    if (done) return;
    done = true;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    cheevos_set_http(&curl_post);
    cheevos_set_notify(&print_notice);
#if defined(__APPLE__)
    cheevos_set_platform("macOS");
#elif defined(_WIN32)
    cheevos_set_platform("Windows");
#else
    cheevos_set_platform("Linux");
#endif
    const char* u = std::getenv("KOL_RA_USER");
    const char* p = std::getenv("KOL_RA_PASSWORD");
    if (u && *u && p && *p) cheevos_login(u, p);
    else cheevos_start();
}

}  // namespace kol
