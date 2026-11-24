#include "kol_cheevos.h"

#include "kol_settings.h"   // settings_dir
#include "kol_menu.h"       // menu_cheevo_popup_render, menu_confirm_sound
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus
#include "gba_bus.h"

#include "rc_client.h"
#include "rc_consoles.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <system_error>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace kol {
namespace {

// Our ROM, as RetroAchievements knows it (game 5029, "Tactics Ogre: The Knight
// of Lodis"): the MD5 of the whole file. One ROM is accepted (sha1
// 85a46320...), so the hash is fixed and nothing is hashed at run time.
constexpr const char* kRomMd5 = "5d4f385e4e1cfa4a057d0ac828993906";
// The user agent's own part. A name of ours, its version numeric and rising
// with each release (RetroAchievements' client-identity rule); the server
// does not know it, so it allows casual (softcore) unlocks only.
constexpr const char* kClientName = "KnightOfLodisRecomp";
constexpr const char* kClientVersion = "1.0.0";
// The server puts a made-up achievement first for a client it does not list
// ("Warning: Unknown Emulator", id 101000001 and up; RAWeb
// CreateWarningAchievementAction). rcheevos leaves it out of the counts and
// never submits it; we leave it out of the list and the notices too.
constexpr uint32_t kWarningId = 101000001u;

std::mutex g_mu;   // everything below but the rc_client (which locks itself)
rc_client_t* g_client = nullptr;
CheevosHttp g_http = nullptr;
CheevosNotify g_notify = nullptr;
CheevosPopup g_popup = nullptr;
std::string g_os;
std::string g_user_agent;
CheevosState g_state = CheevosState::LoggedOut;
std::string g_message;
bool g_game_requested = false;   // the set was asked for (begin_game_load)
bool g_game_loaded = false;
std::atomic<bool> g_login_form{false};
std::atomic<int> g_version{0};
unsigned long long g_last_frame = ~0ull;

void bump() { g_version.fetch_add(1); }

// Notices for the player in the game's window (the achievement popup's,
// top right): queued from any thread (rc_client's callbacks), drawn on the
// guest thread in cheevos_frame, where the ROM's art is. `avatar`: a badge key
// (cheevos_badge) to wait for, up to kAvatarWait frames, for the picture.
struct Notice {
    std::string heading, corner, line, avatar;
    unsigned long long since = 0;   // the frame it was first tried
};
std::deque<Notice> g_notices;   // under g_mu
constexpr unsigned long long kAvatarWait = 180;
// A badge key for an account's avatar: "user-<username>" (badges are numbers).
constexpr const char* kAvatarPrefix = "user-";
// The badge cache's lock (cheevos_badge), and the avatars' URLs by badge key
// (filled by welcome() from the logged-in account).
std::mutex g_badge_mu;
std::map<std::string, std::string> g_avatar_urls;   // under g_badge_mu

void notice(std::string heading, std::string line, std::string corner = {}, std::string avatar = {}) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_notices.push_back({std::move(heading), std::move(corner), std::move(line), std::move(avatar), 0});
}

void notify(const std::string& s) {
    std::fprintf(stderr, "cheevos: %s\n", s.c_str());
    CheevosNotify fn;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        fn = g_notify;
    }
    if (fn) fn(s.c_str());
}

std::string token_path() {
    const std::string dir = settings_dir();
    return dir.empty() ? std::string("retroachievements.cfg") : dir + "/retroachievements.cfg";
}

// The saved login: "user=NAME" and "token=TOKEN" lines. Never a password.
bool token_read(std::string* user, std::string* token) {
    std::FILE* f = std::fopen(token_path().c_str(), "r");
    if (!f) return false;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("user=", 0) == 0) *user = s.substr(5);
        else if (s.rfind("token=", 0) == 0) *token = s.substr(6);
    }
    std::fclose(f);
    return !user->empty() && !token->empty();
}

void token_write(const char* user, const char* token) {
    const std::string path = token_path();
    if (std::FILE* f = std::fopen(path.c_str(), "w")) {
        std::fprintf(f, "user=%s\ntoken=%s\n", user, token);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "cheevos: could not write %s\n", path.c_str());
    }
}

// ── rc_client's two callbacks ──────────────────────────────────────────────

// RetroAchievements' GBA addresses: 0x00000-0x07FFF IWRAM (0x03000000),
// 0x08000-0x47FFF EWRAM (0x02000000), 0x48000-0x57FFF the cartridge's save
// RAM. Save RAM is not mapped by the bus as plain memory: read as zeros, so an
// achievement that looks there is not disabled as unreadable.
uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t*) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    uint32_t done = 0;
    while (done < num_bytes) {
        const uint32_t a = address + done;
        if (a < 0x8000u) {
            const uint32_t n = std::min(num_bytes - done, 0x8000u - a);
            std::memcpy(buffer + done, bus->iwram_ptr() + a, n);
            done += n;
        } else if (a < 0x48000u) {
            const uint32_t n = std::min(num_bytes - done, 0x48000u - a);
            std::memcpy(buffer + done, bus->ewram_ptr() + (a - 0x8000u), n);
            done += n;
        } else if (a < 0x58000u) {
            const uint32_t n = std::min(num_bytes - done, 0x58000u - a);
            std::memset(buffer + done, 0, n);
            done += n;
        } else {
            break;
        }
    }
    return done;
}

// Each request on a thread of its own: a handful a session (login, the set,
// unlocks, a ping every two minutes), never on the guest thread.
void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
                          void* callback_data, rc_client_t*) {
    std::string url = request->url ? request->url : "";
    const bool has_post = request->post_data != nullptr;
    std::string post = has_post ? request->post_data : "";
    std::string ctype = request->content_type ? request->content_type : "";
    CheevosHttp http;
    std::string ua;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        http = g_http;
        ua = g_user_agent;
    }
    std::thread([=]() {
        int status = 0;
        std::string body;
        const bool ok = http && http(url.c_str(), has_post ? post.c_str() : nullptr,
                                     ctype.empty() ? nullptr : ctype.c_str(), ua.c_str(), &status, &body);
        rc_api_server_response_t r{};
        if (ok) {
            r.body = body.c_str();
            r.body_length = body.size();
            r.http_status_code = status;
        } else {
            r.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        }
        callback(&r, callback_data);
    }).detach();
}

void RC_CCONV log_message(const char* message, const rc_client_t*) {
    std::fprintf(stderr, "rcheevos: %s\n", message);
}

bool show_popup(const char* title, unsigned points, const std::string& badge);
void prefetch_badges();

void RC_CCONV on_event(const rc_client_event_t* e, rc_client_t*) {
    switch (e->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            if (e->achievement && e->achievement->id < kWarningId) {
                if (show_popup(e->achievement->title, e->achievement->points, e->achievement->badge_name))
                    break;
                char buf[256];
                std::snprintf(buf, sizeof buf, "Achievement unlocked: %s (%u)", e->achievement->title,
                              e->achievement->points);
                notify(buf);
            }
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            notice("Game Mastered!", "Every achievement unlocked.");
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            if (e->server_error && e->server_error->error_message)
                notice("RetroAchievements", e->server_error->error_message);
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            notice("RetroAchievements", "Offline: unlocks sent later.");
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            notice("RetroAchievements", "Back online.");
            break;
        default:
            break;
    }
    bump();
}

// "Welcome, <name>" with the account's avatar and `line` under it.
void welcome(rc_client_t* client, const std::string& line) {
    const rc_client_user_t* u = rc_client_get_user_info(client);
    if (!u) return;
    std::string name = u->display_name ? u->display_name : u->username;
    if (name.size() > 16) name = name.substr(0, 15) + "...";
    // The avatar's URL from the logged-in account itself: rc_client builds it
    // from the account's fields (a made-up rc_client_user_t crashed it).
    const std::string key = std::string(kAvatarPrefix) + u->username;
    char url[256] = {};
    if (rc_client_user_get_image_url(u, url, sizeof url) == RC_OK && url[0]) {
        std::lock_guard<std::mutex> lock(g_badge_mu);
        g_avatar_urls[key] = url;
    }
    notice("Welcome, " + name + "!", line, {}, key);
}

void RC_CCONV on_game_loaded(int result, const char* error, rc_client_t* client, void*) {
    if (result == RC_OK) {
        rc_client_user_game_summary_t s{};
        rc_client_get_user_game_summary(client, &s);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_game_loaded = true;
            g_message.clear();
        }
        prefetch_badges();   // so an unlock's popup has its badge
        char buf[160];
        std::snprintf(buf, sizeof buf, "%u of %u unlocked, %u pts",
                      s.num_unlocked_achievements, s.num_core_achievements, s.points_unlocked);
        welcome(client, buf);
    } else {
        std::lock_guard<std::mutex> lock(g_mu);
        g_game_loaded = false;
        g_message = error ? error : "the game could not be loaded";
        std::fprintf(stderr, "cheevos: load failed: %s\n", g_message.c_str());
    }
    bump();
}

// The game's set, asked for once after a login; false if it was already
// (or no one is logged in). Only with the bus up: once the set is loaded,
// rc_client reads every address it uses (on the answer's thread) to disable
// the unreadable ones. A mod (a patched ROM) loads none: RetroAchievements'
// set is made for the stock game; the page says why.
bool begin_game_load(rc_client_t* c) {
    bool mod = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state != CheevosState::LoggedIn || g_game_requested) return false;
        g_game_requested = true;
        if (rom_patch_name()) {
            g_message = "Not available with a mod loaded.";
            bump();
            mod = true;
        }
    }
    if (mod) welcome(c, "Achievements are off with a mod.");
    else rc_client_begin_load_game(c, kRomMd5, &on_game_loaded, nullptr);
    return true;
}

void RC_CCONV on_login(int result, const char* error, rc_client_t* client, void* userdata) {
    const bool with_password = userdata != nullptr;
    if (result == RC_OK) {
        const rc_client_user_t* u = rc_client_get_user_info(client);
        if (u && with_password) token_write(u->username, u->token);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_state = CheevosState::LoggedIn;
            g_message.clear();
            g_game_requested = false;
        }
        // The welcome comes when the game's set is loaded (on_game_loaded),
        // with the count; a mod loads none (begin_game_load says so).
        if (u) std::fprintf(stderr, "cheevos: logged in as %s\n", u->display_name);
        // The game's set: at once if the game is running, else on its first
        // frame (cheevos_frame). A login from the options menu (2026-11-24,
        // the Thor): the menu holds the game, so no frame came and the
        // Achievements page said "Loading the game's achievements..." until
        // the menu was closed.
        if (gbarecomp::active_bus()) begin_game_load(client);
    } else {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_state = CheevosState::Failed;
            // Short, for the menu's two-line help; the server's words go to the log.
            if (result == RC_INVALID_CREDENTIALS) g_message = "Wrong name or password.";
            else if (result == RC_EXPIRED_TOKEN) g_message = "The saved login expired.";
            else if (result == RC_NO_RESPONSE) g_message = "No answer from the server.";
            else g_message = error ? error : "Login failed.";
        }
        notice("RetroAchievements", std::string("Login failed: ") + (error ? error : "no answer"));
        // A token the server refuses is no use any more (password changed).
        if (!with_password && (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN))
            std::remove(token_path().c_str());
    }
    bump();
}

// The client itself, made once.
rc_client_t* client() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_client) return g_client;
    g_client = rc_client_create(&read_memory, &server_call);
    if (!g_client) return nullptr;
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_WARN, &log_message);
    rc_client_set_event_handler(g_client, &on_event);
    // Softcore: our app is not on the server's list, so hardcore unlocks would
    // be turned into casual ones anyway; asking for them is the honest choice.
    rc_client_set_hardcore_enabled(g_client, 0);
    rc_client_set_unofficial_enabled(g_client, 0);
    char clause[64] = {};
    rc_client_get_user_agent_clause(g_client, clause, sizeof clause);
    g_user_agent = std::string(kClientName) + "/" + kClientVersion;
    if (!g_os.empty()) g_user_agent += " (" + g_os + ")";
    if (clause[0]) g_user_agent += std::string(" ") + clause;
    return g_client;
}

}  // namespace

bool http_get(const char* url, int* status, std::string* body) {
    CheevosHttp http;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        http = g_http;
    }
    return http && http(url, nullptr, nullptr, "KnightOfLodisRecomp", status, body);
}

void cheevos_set_http(CheevosHttp fn) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_http = fn;
}

void cheevos_set_notify(CheevosNotify fn) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_notify = fn;
}

void cheevos_set_popup(CheevosPopup fn) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_popup = fn;
}

void cheevos_set_platform(const char* os) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_os = os ? os : "";
}

void cheevos_start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::string user, token;
    if (!token_read(&user, &token)) return;
    rc_client_t* c = client();
    if (!c) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_http) return;   // no way to reach the server on this build
        g_state = CheevosState::LoggingIn;
        g_message = "Logging in...";
    }
    bump();
    rc_client_begin_login_with_token(c, user.c_str(), token.c_str(), &on_login, nullptr);
}

namespace {
// One queued notice a frame, in the game's window (Android: Cheevos.showPopup
// queues and slides them in turn); without a popup backend, a notice line.
void show_notices(unsigned long long frame_count) {
    Notice n;
    CheevosPopup fn;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_notices.empty()) return;
        Notice& front = g_notices.front();
        if (front.since == 0) front.since = frame_count ? frame_count : 1;
        n = front;
        fn = g_popup;
    }
    std::vector<unsigned> pic;
    if (fn && !n.avatar.empty()) {
        pic = cheevos_badge(n.avatar, 32, false);
        if (pic.empty() && frame_count - n.since < kAvatarWait) return;   // still on its way
    }
    bool shown = false;
    if (fn) {
        int W = 0, H = 0;
        const std::vector<std::uint32_t> px = menu_notice_popup(
            n.heading.c_str(), n.corner.c_str(), n.line.c_str(),
            pic.empty() ? nullptr : reinterpret_cast<const std::uint32_t*>(pic.data()), &W, &H);
        if (px.empty()) return;   // the ROM's art is not up yet: next frame
        fn(reinterpret_cast<const unsigned*>(px.data()), W, H);
        shown = true;
    }
    if (shown) std::fprintf(stderr, "cheevos: %s: %s\n", n.heading.c_str(), n.line.c_str());
    else notify(n.heading + ": " + n.line);
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_notices.empty()) g_notices.pop_front();
}
}  // namespace

void cheevos_frame(unsigned long long frame_count) {
    show_notices(frame_count);
    rc_client_t* c;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        c = g_client;
        if (!c || g_state != CheevosState::LoggedIn) return;
    }
    if (begin_game_load(c)) {
        g_last_frame = frame_count;
        return;
    }
    // The per-frame policy can run twice in one frame (after a live resize).
    if (frame_count == g_last_frame) return;
    // A state loaded (the frame count jumped): every achievement waits for its
    // conditions to be false again, so none fires on the jump itself.
    if (g_last_frame != ~0ull && frame_count != g_last_frame + 1) rc_client_reset(c);
    g_last_frame = frame_count;
    rc_client_do_frame(c);
}

void cheevos_login(const std::string& user, const std::string& password) {
    rc_client_t* c = client();
    if (!c) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_http) {
            g_state = CheevosState::Failed;
            g_message = "no network on this build";
            bump();
            return;
        }
        g_state = CheevosState::LoggingIn;
        g_message = "Logging in...";
    }
    bump();
    // userdata non-null: a password login, whose token is saved.
    static int kWithPassword = 1;
    rc_client_begin_login_with_password(c, user.c_str(), password.c_str(), &on_login, &kWithPassword);
}

void cheevos_logout() {
    rc_client_t* c;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        c = g_client;
        g_state = CheevosState::LoggedOut;
        g_message.clear();
        g_game_loaded = false;
        g_game_requested = false;
    }
    if (c) {
        rc_client_unload_game(c);
        rc_client_logout(c);
    }
    std::remove(token_path().c_str());
    bump();
    notice("RetroAchievements", "Logged out.");
}

namespace { CheevosLoginForm g_login_form_fn = nullptr; }
void cheevos_set_login_form(CheevosLoginForm fn) { g_login_form_fn = fn; }
void cheevos_request_login_form() {
    if (g_login_form_fn) g_login_form_fn();
    else g_login_form.store(true);
}
bool cheevos_take_login_form_request() { return g_login_form.exchange(false); }

CheevosStatus cheevos_status() {
    CheevosStatus s;
    rc_client_t* c;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s.state = g_state;
        s.message = g_message;
        s.game_loaded = g_game_loaded;
        c = g_client;
    }
    if (!c) return s;
    if (const rc_client_user_t* u = rc_client_get_user_info(c)) {
        s.user = u->display_name ? u->display_name : u->username;
        s.score = u->score_softcore;
    }
    if (s.game_loaded) {
        if (const rc_client_game_t* g = rc_client_get_game_info(c)) s.game_title = g->title ? g->title : "";
        rc_client_user_game_summary_t sum{};
        rc_client_get_user_game_summary(c, &sum);
        s.unlocked = sum.num_unlocked_achievements;
        s.total = sum.num_core_achievements;
        s.points_unlocked = sum.points_unlocked;
        s.points_total = sum.points_core;
    }
    return s;
}

std::vector<CheevosEntry> cheevos_list() {
    std::vector<CheevosEntry> out;
    rc_client_t* c;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        c = g_game_loaded ? g_client : nullptr;
    }
    if (!c) return out;
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        c, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return out;
    // Unlocked first, then the rest in the server's order.
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t b = 0; b < list->num_buckets; ++b) {
            const rc_client_achievement_bucket_t& bk = list->buckets[b];
            for (uint32_t i = 0; i < bk.num_achievements; ++i) {
                const rc_client_achievement_t* a = bk.achievements[i];
                if (a->id >= kWarningId) continue;
                const bool got = a->unlocked != 0;
                if (got != (pass == 0)) continue;
                CheevosEntry e;
                e.title = a->title ? a->title : "";
                e.description = a->description ? a->description : "";
                e.progress = a->measured_progress;
                e.points = a->points;
                e.badge = a->badge_name;
                e.unlocked = got;
                out.push_back(std::move(e));
            }
        }
    rc_client_destroy_achievement_list(list);
    return out;
}

int cheevos_version() { return g_version.load(); }

// ── Badges ───────────────────────────────────────────────────────────────
// One worker fetches them in turn: from the disk cache, else from the media
// server (the URL rcheevos builds; the same for everyone, nothing personal).
namespace {

struct Badge {
    bool queued = false, failed = false;
    int w = 0, h = 0;
    std::vector<unsigned> argb;                  // as decoded
    std::map<int, std::vector<unsigned>> scaled; // size*2 + locked
};
std::condition_variable g_badge_cv;
std::map<std::string, Badge> g_badges;
std::deque<std::string> g_badge_queue;
bool g_badge_worker = false;

std::string badge_dir() {
    const std::string dir = settings_dir();
    return (dir.empty() ? std::string(".") : dir) + "/badges";
}

bool read_file(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    std::size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    std::fclose(f);
    return !out->empty();
}

bool decode_png(const std::string& bytes, int* w, int* h, std::vector<unsigned>* argb) {
    int n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                              static_cast<int>(bytes.size()), w, h, &n, 4);
    if (!px) return false;
    argb->resize(static_cast<std::size_t>(*w) * *h);
    for (std::size_t i = 0; i < argb->size(); ++i) {
        const unsigned char* p = px + i * 4;
        (*argb)[i] = (static_cast<unsigned>(p[3]) << 24) | (static_cast<unsigned>(p[0]) << 16) |
                     (static_cast<unsigned>(p[1]) << 8) | p[2];
    }
    stbi_image_free(px);
    return true;
}

void badge_worker() {
    for (;;) {
        std::string name;
        {
            std::unique_lock<std::mutex> lock(g_badge_mu);
            g_badge_cv.wait(lock, [] { return !g_badge_queue.empty(); });
            name = g_badge_queue.front();
            g_badge_queue.pop_front();
        }
        const std::string path = badge_dir() + "/" + name + ".png";
        std::string bytes;
        bool ok = read_file(path, &bytes);
        if (!ok) {
            CheevosHttp http;
            std::string ua;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                http = g_http;
                ua = g_user_agent;
            }
            char url[256] = {};
            if (name.rfind(kAvatarPrefix, 0) == 0) {   // an account's avatar: its URL from welcome()
                std::lock_guard<std::mutex> lock(g_badge_mu);
                const auto it = g_avatar_urls.find(name);
                if (it != g_avatar_urls.end())
                    std::snprintf(url, sizeof url, "%s", it->second.c_str());
            } else {
                rc_client_achievement_t probe{};
                std::snprintf(probe.badge_name, sizeof probe.badge_name, "%s", name.c_str());
                rc_client_achievement_get_image_url(&probe, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof url);
            }
            int status = 0;
            ok = http && url[0] && http(url, nullptr, nullptr, ua.c_str(), &status, &bytes) && status == 200;
            if (ok) {
                const std::string dir = badge_dir();
                std::error_code ec;
                std::filesystem::create_directory(dir, ec);
                if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), f);
                    std::fclose(f);
                }
            }
        }
        Badge done;
        done.queued = true;
        done.failed = !(ok && decode_png(bytes, &done.w, &done.h, &done.argb));
        if (done.failed) std::fprintf(stderr, "cheevos: badge %s not loaded\n", name.c_str());
        {
            std::lock_guard<std::mutex> lock(g_badge_mu);
            g_badges[name] = std::move(done);
        }
        bump();
    }
}

// Box filter to size x size; locked: grey, at 60% brightness.
std::vector<unsigned> scale_badge(const Badge& b, int size, bool locked) {
    std::vector<unsigned> out(static_cast<std::size_t>(size) * size, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int x0 = x * b.w / size, x1 = std::max(x0 + 1, (x + 1) * b.w / size);
            const int y0 = y * b.h / size, y1 = std::max(y0 + 1, (y + 1) * b.h / size);
            unsigned sr = 0, sg = 0, sb = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const unsigned v = b.argb[static_cast<std::size_t>(yy) * b.w + xx];
                    sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF; ++n;
                }
            unsigned r = sr / n, g = sg / n, bl = sb / n;
            if (locked) {
                const unsigned l = (r * 77 + g * 150 + bl * 29) >> 8;
                r = g = bl = l * 6 / 10;
            }
            out[static_cast<std::size_t>(y) * size + x] = 0xFF000000u | (r << 16) | (g << 8) | bl;
        }
    return out;
}

}  // namespace

std::vector<unsigned> cheevos_badge(const std::string& badge, int size, bool locked) {
    if (badge.empty() || size <= 0) return {};
    std::lock_guard<std::mutex> lock(g_badge_mu);
    Badge& b = g_badges[badge];
    if (!b.queued) {
        b.queued = true;
        g_badge_queue.push_back(badge);
        if (!g_badge_worker) {
            g_badge_worker = true;
            std::thread(&badge_worker).detach();
        }
        g_badge_cv.notify_one();
        return {};
    }
    if (b.failed || b.argb.empty()) return {};
    const int key = size * 2 + (locked ? 1 : 0);
    auto it = b.scaled.find(key);
    if (it == b.scaled.end()) it = b.scaled.emplace(key, scale_badge(b, size, locked)).first;
    return it->second;
}

namespace {

// The popup for one unlock, with the game's confirm sound. False when there is
// no popup backend (the caller then writes a notice line).
bool show_popup(const char* title, unsigned points, const std::string& badge) {
    CheevosPopup fn;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        fn = g_popup;
    }
    if (!fn) return false;
    const std::vector<unsigned> b = cheevos_badge(badge, 32, false);
    int W = 0, H = 0;
    const std::vector<std::uint32_t> px = menu_cheevo_popup(
        title, points, b.empty() ? nullptr : reinterpret_cast<const std::uint32_t*>(b.data()), &W, &H);
    if (px.empty()) return false;
    std::fprintf(stderr, "cheevos: unlocked %s (%u)\n", title ? title : "", points);
    menu_confirm_sound();
    fn(reinterpret_cast<const unsigned*>(px.data()), W, H);
    return true;
}

void prefetch_badges() {
    for (const CheevosEntry& e : cheevos_list()) cheevos_badge(e.badge, 32, false);
}

}  // namespace

bool cheevos_notice(const char* heading, const char* line) {
    CheevosPopup fn;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        fn = g_popup;
    }
    if (!fn) return false;
    int W = 0, H = 0;
    const std::vector<std::uint32_t> px = menu_notice_popup(heading, "", line, nullptr, &W, &H);
    if (px.empty()) return false;
    fn(reinterpret_cast<const unsigned*>(px.data()), W, H);
    return true;
}

void cheevos_mod_notice_frame(unsigned long long frame_count) {
    // Once, a second into the session (the ROM's art is up by then and the
    // picture is on screen): which mod is playing.
    static bool done = false;
    if (done) return;
    const char* mod = rom_patch_name();
    if (!mod) { done = true; return; }
    static unsigned long long first = frame_count;
    if (frame_count - first < 60) return;
    std::string name = mod;
    if (const auto dot = name.rfind('.'); dot != std::string::npos && dot > 0) name.resize(dot);
    if (cheevos_notice("Mod Loaded", name.c_str())) {
        done = true;
        return;
    }
    if (frame_count - first > 600) {   // no popup backend (the desktop): a line instead
        done = true;
        std::fprintf(stderr, "mod: playing %s\n", name.c_str());
    }
}

void cheevos_popup_test() {
    static bool done = false;
    if (done) return;
    const char* path = std::getenv("KOL_RA_POPUP_PPM");
    if (!path) { done = true; return; }
    const char* name = std::getenv("KOL_RA_POPUP_BADGE");
    std::vector<unsigned> b;
    if (name && *name) {
        b = cheevos_badge(name, 32, false);
        if (b.empty()) return;   // still on its way: next frame
    }
    // KOL_RA_POPUP_MOD=name: the mod notice instead; KOL_RA_POPUP_NOTICE=
    // "heading|line": any notice, with the badge if one is named.
    const char* mod = std::getenv("KOL_RA_POPUP_MOD");
    const char* any = std::getenv("KOL_RA_POPUP_NOTICE");
    std::string head, rest;
    if (any) {
        head = any;
        const auto bar = head.find('|');
        if (bar != std::string::npos) { rest = head.substr(bar + 1); head.resize(bar); }
    }
    const std::uint32_t* pic = b.empty() ? nullptr : reinterpret_cast<const std::uint32_t*>(b.data());
    int W = 0, H = 0;
    const std::vector<std::uint32_t> px =
        any ? menu_notice_popup(head.c_str(), "", rest.c_str(), pic, &W, &H)
        : mod ? menu_notice_popup("Mod Loaded", "", mod, nullptr, &W, &H)
              : menu_cheevo_popup("A Knight's First Victory", 10, pic, &W, &H);
    if (px.empty()) return;   // the ROM's art is not up yet
    done = true;
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6 %d %d 255\n", W, H);
        for (std::uint32_t v : px) {
            // Clear pixels (outside the window's rounded corners) as magenta.
            const unsigned char c[3] = {static_cast<unsigned char>(v >> 24 ? v >> 16 : 255),
                                        static_cast<unsigned char>(v >> 24 ? v >> 8 : 0),
                                        static_cast<unsigned char>(v >> 24 ? v : 255)};
            std::fwrite(c, 1, 3, f);
        }
        std::fclose(f);
        std::fprintf(stderr, "cheevos: wrote %s\n", path);
    }
}

}  // namespace kol
