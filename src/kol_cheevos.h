#pragma once

// RetroAchievements (rcheevos rc_client, third_party/rcheevos): softcore only.
//
// The player logs in once with a username and password; the server answers
// with a token, which is all that is kept (settings_dir()/retroachievements.cfg,
// never the password). With a token the game is loaded by our ROM's MD5 (one
// ROM is accepted: SetupActivity / RunOptions) and every guest frame is
// checked (cheevos_frame, from the per-frame policy). Hardcore is off: the
// server does not know this client and would refuse it anyway.
//
// The platform supplies two things: an HTTP POST (desktop: libcurl,
// kol_cheevos_curl.cpp; Android: Java, Cheevos.java through main_android.cpp)
// and, optionally, a way to show a line to the player (an unlock).

#include <string>
#include <vector>

namespace kol {

// A blocking HTTP request, run on a worker thread of ours. `post` null: GET.
// Returns false when no answer came at all (then *status is ignored).
using CheevosHttp = bool (*)(const char* url, const char* post, const char* content_type,
                             const char* user_agent, int* status, std::string* body);
void cheevos_set_http(CheevosHttp fn);
// A plain GET through the same function (blocking: a worker thread's), for
// other downloads (Display > Shader > Download Shader Pack). False: no HTTP
// on this build, or no answer.
bool http_get(const char* url, int* status, std::string* body);
// A line for the player: an unlock, a login, a lost connection.
using CheevosNotify = void (*)(const char* text);
void cheevos_set_notify(CheevosNotify fn);
// An unlock's popup, drawn in the game's window style (menu_cheevo_popup_render):
// ARGB, w x h. Android shows it over the game (Cheevos.showPopup), the desktop
// as an overlay at the top right (kol_desktop desktop_popup); without a
// backend the unlock is a notice line instead.
using CheevosPopup = void (*)(const unsigned* argb, int w, int h);
void cheevos_set_popup(CheevosPopup fn);
// The same popup with any heading and one line (no points, no badge); false
// without a popup backend or before the ROM's art is up.
bool cheevos_notice(const char* heading, const char* line);
// Each frame: once a mod (a ROM patch) is playing, "Mod Loaded" and its name
// in that popup, a second after start.
void cheevos_mod_notice_frame(unsigned long long frame_count);
// Desktop check: KOL_RA_POPUP_PPM=path [KOL_RA_POPUP_BADGE=name] writes a
// sample popup once the ROM's art is up (called each frame).
void cheevos_popup_test();
// "Android 14", "macOS": the user agent's operating-system part.
void cheevos_set_platform(const char* os);

// Start: reads the saved token and logs in with it. Safe to call again.
void cheevos_start();
// The desktop's setup (kol_cheevos_curl.cpp): libcurl, notices on stderr, and
// a login from KOL_RA_USER + KOL_RA_PASSWORD (no text entry there).
void cheevos_desktop_init();
// Each guest frame, on the guest thread (kol_extended_view_frame).
void cheevos_frame(unsigned long long frame_count);

// Log in with a password (from the login form); the token is saved on success.
void cheevos_login(const std::string& user, const std::string& password);
void cheevos_logout();

// The menu's Achievements page asks for the login form: the platform's form
// opens at once (Android: a dialog, Cheevos.java), else the request waits to
// be taken (the desktop prints how to log in).
using CheevosLoginForm = void (*)();
void cheevos_set_login_form(CheevosLoginForm fn);
void cheevos_request_login_form();
bool cheevos_take_login_form_request();

enum class CheevosState { LoggedOut, LoggingIn, LoggedIn, Failed };
struct CheevosStatus {
    CheevosState state = CheevosState::LoggedOut;
    std::string user;        // the display name
    std::string message;     // the last error, or what is under way
    bool game_loaded = false;
    std::string game_title;
    unsigned unlocked = 0, total = 0, points_unlocked = 0, points_total = 0;
    unsigned score = 0;      // the account's softcore points
};
CheevosStatus cheevos_status();

struct CheevosEntry {
    std::string title, description, progress;   // progress: "3/10", or empty
    std::string badge;                          // the badge's name ("12345"), for cheevos_badge
    unsigned points = 0;
    bool unlocked = false;
};
// The game's achievements, the unlocked ones first. Empty before a load.
std::vector<CheevosEntry> cheevos_list();
// Changes each time the status or the list may have changed (the menu redraws).
int cheevos_version();

// An achievement's badge, `size` x `size` ARGB (box-filtered from the 64x64
// PNG), greyed and dimmed when `locked`. Empty until it has arrived: the first
// ask queues it (downloaded once, kept in settings_dir()/badges/), and
// cheevos_version() changes when it is ready.
std::vector<unsigned> cheevos_badge(const std::string& badge, int size, bool locked);

}  // namespace kol
