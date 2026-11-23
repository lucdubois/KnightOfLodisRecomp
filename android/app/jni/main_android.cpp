// Android entry point for Knight of Lodis Recomp.
//
// Differs from src/main.cpp in exactly two ways:
//   1. SDL_main.h renames main -> SDL_main, which is what SDLActivity calls
//      after loading libmain.so.
//   2. The ROM and BIOS are not next to an executable here. They live in the
//      app's internal storage, which the Java side provisions on first run
//      (the file-picker setup flow) and hands to us via SDL's own
//      "internal storage path" query. Nothing is bundled in the APK.
#include <SDL.h>
#include <SDL_main.h>

#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "runtime.h"
#include "rom_patch.h"
#include <dirent.h>
#include <fstream>
#include <iterator>

#include "kol_vs_cpu.h"
#include "kol_vs_net.h"
#include "kol_dev.h"
#include "kol_settings.h"
#include "kol_rumble.h"
#include "kol_cheevos.h"
#include <sys/system_properties.h>
#include "kol_menu.h"
#include "kol_sheet.h"
#include "kol_widescreen.h"
#include "kol_terrain_probe.h"
#include "kol_unit_data.h"
#include "kol_unit_list.h"
#include "kol_scene_strip.h"
#include "kol_sheet.h"
#include "kol_actions.h"
#include "kol_auto_win.h"
#include "kol_edit_panel.h"

namespace {

// The runtime reports everything through stdout/stderr — the coverage banner,
// dispatch misses, save-state results, the present-in-place mode line. On
// Android nothing reads those, so the single most useful diagnostic surface in
// the whole engine is silently discarded. Pump both through a pipe onto logcat
// so `adb logcat -s KoL` shows exactly what a desktop terminal would.
int g_log_pipe[2] = {-1, -1};

void* log_pump_thread(void*) {
    char buf[512];
    std::string line;
    ssize_t n;
    while ((n = read(g_log_pipe[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, "KoL", line.c_str());
                line.clear();
            } else if (line.size() < 1024) {
                line.push_back(buf[i]);
            }
        }
    }
    return nullptr;
}

void route_stdio_to_logcat() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (pipe(g_log_pipe) != 0) return;
    dup2(g_log_pipe[1], STDOUT_FILENO);
    dup2(g_log_pipe[1], STDERR_FILENO);
    pthread_t t;
    pthread_create(&t, nullptr, log_pump_thread, nullptr);
    pthread_detach(t);
}

}  // namespace

namespace {

// ── Second screen (AYN Thor) ────────────────────────────────────────────────
// Every page of the device's second display is drawn natively from the
// cartridge's own art and handed to Java as a picture: the unit's status
// sheet (kol_sheet), the battle's unit list (kol_unit_list) and the options
// menu (kol_menu). Strictly a present-time shadow of guest memory: it only
// ever reads, so it cannot perturb timing, IRQs or game logic. (It began as a
// hand-drawn panel fed with values read here; that panel is gone.)

jclass    g_screen2_class = nullptr;
jmethodID g_screen2_enabled = nullptr;
jmethodID g_screen2_available = nullptr;
jmethodID g_screen2_sheetimg = nullptr;
jmethodID g_screen2_menuimg = nullptr;
jmethodID g_screen2_peekimg = nullptr;
jmethodID g_screen2_list = nullptr;
jmethodID g_screen2_strip = nullptr;
std::atomic<bool> g_sheet_peek{false};   // a finger is on the sheet's lower card
jmethodID g_screen2_autowin = nullptr;
jmethodID g_screen2_bar = nullptr;
jmethodID g_screen2_pagehelp = nullptr;
jmethodID g_screen2_edit = nullptr;
jmethodID g_screen2_single = nullptr;
bool      g_screen2_ready = false;

bool screen2_bind(JNIEnv* env) {
    if (g_screen2_ready) return true;
    if (!env) return false;
    jclass local = env->FindClass("com/koldecomp/game/SecondScreen");
    if (!local) { env->ExceptionClear(); return false; }
    g_screen2_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    // Required: an APK without the sheet has nothing to show.
    g_screen2_sheetimg = env->GetStaticMethodID(g_screen2_class, "pushSheetImage",
                                               "([III)V");
    if (!g_screen2_sheetimg) { env->ExceptionClear(); return false; }
    g_screen2_enabled = env->GetStaticMethodID(g_screen2_class, "setEnabled",
                                              "(Z)V");
    if (!g_screen2_enabled) env->ExceptionClear();
    g_screen2_available = env->GetStaticMethodID(g_screen2_class, "available", "()Z");
    if (!g_screen2_available) env->ExceptionClear();
    g_screen2_menuimg = env->GetStaticMethodID(g_screen2_class, "pushMenuImage", "([I)V");
    if (!g_screen2_menuimg) env->ExceptionClear();
    g_screen2_peekimg = env->GetStaticMethodID(g_screen2_class, "pushSheetPeek", "([I)V");
    if (!g_screen2_peekimg) env->ExceptionClear();
    g_screen2_list = env->GetStaticMethodID(g_screen2_class, "pushUnitList", "([III)V");
    if (!g_screen2_list) env->ExceptionClear();
    g_screen2_strip = env->GetStaticMethodID(g_screen2_class, "pushSceneStrip", "([IIII)V");
    if (!g_screen2_strip) env->ExceptionClear();
    g_screen2_autowin = env->GetStaticMethodID(g_screen2_class, "setAutoWin", "(Z)V");
    if (!g_screen2_autowin) env->ExceptionClear();
    g_screen2_bar = env->GetStaticMethodID(g_screen2_class, "barChanged", "()V");
    if (!g_screen2_bar) env->ExceptionClear();
    g_screen2_pagehelp = env->GetStaticMethodID(g_screen2_class, "pushPageHelp", "([I)V");
    if (!g_screen2_pagehelp) env->ExceptionClear();
    g_screen2_edit = env->GetStaticMethodID(g_screen2_class, "pushEditPanel", "([I)V");
    if (!g_screen2_edit) env->ExceptionClear();
    g_screen2_single = env->GetStaticMethodID(g_screen2_class, "setSingle", "(Z)V");
    if (!g_screen2_single) env->ExceptionClear();
    g_screen2_ready = true;
    return true;
}

// Rumble (kol_rumble.h): the device's vibrator through Rumble.play(int[]
// ms, int[] levels 0..255). Resolved on SDLThread like SecondScreen (the
// app's classloader); called on the thread an effect starts on (the guest's,
// the menu's), each attaching through SDL_AndroidGetJNIEnv.
jclass    g_rumble_class = nullptr;
jmethodID g_rumble_play = nullptr;

bool rumble_bind(JNIEnv* env) {
    if (!env) return false;
    jclass local = env->FindClass("com/koldecomp/game/Rumble");
    if (!local) { env->ExceptionClear(); return false; }
    g_rumble_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_rumble_play = env->GetStaticMethodID(g_rumble_class, "play", "([I[I)V");
    if (!g_rumble_play) { env->ExceptionClear(); return false; }
    return true;
}

// VS Mode's copies (kol_vs_cpu.h VsPlatform): VsCopy.start(argv, env, log,
// dir) runs one in VsCopyService, a process of its own (":vscopy": an app's
// process cannot start the native game twice); VsCopy.wifiLock holds the
// Wi-Fi's multicast lock while a game looks for another on the network
// (Android drops broadcasts without it). Resolved on SDLThread like Rumble.
jclass    g_vs_class = nullptr;
jmethodID g_vs_start = nullptr;
jmethodID g_vs_wifi = nullptr;

bool vs_bind(JNIEnv* env) {
    if (!env) return false;
    jclass local = env->FindClass("com/koldecomp/game/VsCopy");
    if (!local) { env->ExceptionClear(); return false; }
    g_vs_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_vs_start = env->GetStaticMethodID(g_vs_class, "start",
        "([Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
    g_vs_wifi = env->GetStaticMethodID(g_vs_class, "wifiLock", "(Z)V");
    if (!g_vs_start || !g_vs_wifi) { env->ExceptionClear(); return false; }
    return true;
}

jobjectArray vs_strings(JNIEnv* env, const std::vector<std::string>& v) {
    jclass str = env->FindClass("java/lang/String");
    jobjectArray a = env->NewObjectArray(static_cast<jsize>(v.size()), str, nullptr);
    for (std::size_t i = 0; i < v.size(); ++i) {
        jstring s = env->NewStringUTF(v[i].c_str());
        env->SetObjectArrayElement(a, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(str);
    return a;
}

bool vs_spawn_android(const std::vector<std::string>& argv, const std::vector<std::string>& envs,
                      const std::string& log, const std::string& dir) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_vs_start) return false;
    jobjectArray a = vs_strings(env, argv), e = vs_strings(env, envs);
    jstring l = env->NewStringUTF(log.c_str()), d = env->NewStringUTF(dir.c_str());
    const bool ok = env->CallStaticBooleanMethod(g_vs_class, g_vs_start, a, e, l, d);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(a); env->DeleteLocalRef(e); env->DeleteLocalRef(l); env->DeleteLocalRef(d);
    return ok;
}

void vs_wifi_lock_android(bool on) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_vs_wifi) return;
    env->CallStaticVoidMethod(g_vs_class, g_vs_wifi, on ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// RetroAchievements (kol_cheevos.h): Cheevos.java's HTTP, toast and login
// form. Resolved on SDLThread like Rumble; post() is called on the native
// worker each request runs on, attached through SDL_AndroidGetJNIEnv.
jclass    g_cheevos_class = nullptr;
jmethodID g_cheevos_post = nullptr;
jmethodID g_cheevos_notify = nullptr;
jmethodID g_cheevos_login = nullptr;
jmethodID g_cheevos_popup = nullptr;

bool cheevos_bind(JNIEnv* env) {
    if (!env) return false;
    jclass local = env->FindClass("com/koldecomp/game/Cheevos");
    if (!local) { env->ExceptionClear(); return false; }
    g_cheevos_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_cheevos_post = env->GetStaticMethodID(g_cheevos_class, "post",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[B");
    g_cheevos_notify = env->GetStaticMethodID(g_cheevos_class, "notify", "(Ljava/lang/String;)V");
    g_cheevos_login = env->GetStaticMethodID(g_cheevos_class, "showLogin", "()V");
    g_cheevos_popup = env->GetStaticMethodID(g_cheevos_class, "showPopup", "([III)V");
    if (!g_cheevos_post || !g_cheevos_notify || !g_cheevos_login || !g_cheevos_popup) { env->ExceptionClear(); return false; }
    return true;
}

bool cheevos_http_android(const char* url, const char* post, const char* content_type,
                          const char* user_agent, int* status, std::string* body) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_cheevos_post) return false;
    jstring ju = env->NewStringUTF(url);
    jstring jp = post ? env->NewStringUTF(post) : nullptr;
    jstring jc = content_type ? env->NewStringUTF(content_type) : nullptr;
    jstring ja = env->NewStringUTF(user_agent);
    auto out = static_cast<jbyteArray>(env->CallStaticObjectMethod(g_cheevos_class, g_cheevos_post, ju, jp, jc, ja));
    if (env->ExceptionCheck()) { env->ExceptionClear(); out = nullptr; }
    bool ok = false;
    if (out) {
        const jsize n = env->GetArrayLength(out);
        if (n >= 4) {
            std::vector<jbyte> b(static_cast<std::size_t>(n));
            env->GetByteArrayRegion(out, 0, n, b.data());
            const auto u = [&](int i) { return static_cast<unsigned>(static_cast<std::uint8_t>(b[static_cast<std::size_t>(i)])); };
            *status = static_cast<int>((u(0) << 24) | (u(1) << 16) | (u(2) << 8) | u(3));
            body->assign(reinterpret_cast<const char*>(b.data()) + 4, static_cast<std::size_t>(n - 4));
            ok = true;
        }
        env->DeleteLocalRef(out);
    }
    env->DeleteLocalRef(ju);
    if (jp) env->DeleteLocalRef(jp);
    if (jc) env->DeleteLocalRef(jc);
    env->DeleteLocalRef(ja);
    return ok;
}

void cheevos_notify_android(const char* text) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_cheevos_notify) return;
    jstring t = env->NewStringUTF(text);
    env->CallStaticVoidMethod(g_cheevos_class, g_cheevos_notify, t);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(t);
}

void cheevos_popup_android(const unsigned* argb, int w, int h) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_cheevos_popup || w <= 0 || h <= 0) return;
    jintArray a = env->NewIntArray(w * h);
    if (!a) { env->ExceptionClear(); return; }
    env->SetIntArrayRegion(a, 0, w * h, reinterpret_cast<const jint*>(argb));
    env->CallStaticVoidMethod(g_cheevos_class, g_cheevos_popup, a, w, h);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(a);
}

void cheevos_login_form_android() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_cheevos_login) return;
    env->CallStaticVoidMethod(g_cheevos_class, g_cheevos_login);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Options > Mod and Debug > Unlink ROM (kol_menu MenuPlatform): the game asks
// in its own dialog; these do the work in Java. Bound on SDLThread.
jclass    g_mods_class = nullptr;
jclass    g_setup_class = nullptr;
jmethodID g_mods_pick = nullptr, g_mods_remove = nullptr, g_mods_restart = nullptr, g_mods_licenses = nullptr;
jmethodID g_mods_pick_bios = nullptr;
jmethodID g_mods_pick_shaders = nullptr;
jmethodID g_setup_unlink = nullptr;

bool platform_bind(JNIEnv* env) {
    if (!env) return false;
    auto global = [&](const char* name) -> jclass {
        jclass local = env->FindClass(name);
        if (!local) { env->ExceptionClear(); return nullptr; }
        auto g = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        return g;
    };
    g_mods_class = global("com/koldecomp/game/Mods");
    g_setup_class = global("com/koldecomp/game/SetupActivity");
    if (!g_mods_class || !g_setup_class) return false;
    g_mods_pick = env->GetStaticMethodID(g_mods_class, "pick", "()V");
    g_mods_remove = env->GetStaticMethodID(g_mods_class, "remove", "()V");
    g_mods_restart = env->GetStaticMethodID(g_mods_class, "restart", "()V");
    g_mods_licenses = env->GetStaticMethodID(g_mods_class, "showLicenses", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_mods_pick_bios = env->GetStaticMethodID(g_mods_class, "pickBios", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_mods_pick_shaders = env->GetStaticMethodID(g_mods_class, "pickShaders", "()V");
    g_setup_unlink = env->GetStaticMethodID(g_setup_class, "unlinkRom", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    return g_mods_pick && g_mods_remove && g_mods_restart && g_setup_unlink;
}

void call_static(jclass cls, jmethodID m) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !cls || !m) return;
    env->CallStaticVoidMethod(cls, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
}
void mods_pick_android() { call_static(g_mods_class, g_mods_pick); }
void mods_remove_android() { call_static(g_mods_class, g_mods_remove); }
void restart_android() { call_static(g_mods_class, g_mods_restart); }
void unlink_rom_android() { call_static(g_setup_class, g_setup_unlink); }
void licenses_android() { call_static(g_mods_class, g_mods_licenses); }
void bios_pick_android() { call_static(g_mods_class, g_mods_pick_bios); }
void shaders_pick_android() { call_static(g_mods_class, g_mods_pick_shaders); }

// The mod to play: the one file in files/mod/ (Mods.java puts it there), or
// "". It is checked here first, as the runtime will (rom_patch.h): one that
// does not apply, or changes the game's code, would stop the game at start,
// so it is set aside in files/mod_refused/ instead, the game starts as the
// original, and *why says what happened, for the player.
std::string pick_mod(const std::string& base, const std::string& rom_path, std::string* why) {
    const std::string dir = base + "/mod";
    std::string path, name;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            name = e->d_name;
            path = dir + "/" + name;
            break;
        }
        closedir(d);
    }
    if (path.empty()) return {};
    std::ifstream f(rom_path, std::ios::binary);
    const std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> out;
    std::string summary, err;
    if (!rom.empty() && gbarecomp::rom_patch_load_and_check(path, rom, &out, &summary, &err)) {
        std::printf("android: mod %s: %s\n", name.c_str(), summary.c_str());
        std::fflush(stdout);
        return path;
    }
    if (rom.empty()) err = "the ROM could not be read";
    const std::string refused = base + "/mod_refused";
    mkdir(refused.c_str(), 0700);
    std::rename(path.c_str(), (refused + "/" + name).c_str());
    *why = "The mod " + name + " can't be played: " + err +
           ".\n\nThe original game starts instead. Choose another mod in Options > Mod.";
    std::fprintf(stderr, "android: mod %s refused: %s\n", name.c_str(), err.c_str());
    return {};
}

void rumble_android(const int* ms, const float* level, int n) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_rumble_play || n <= 0) return;
    std::vector<jint> lv(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        lv[static_cast<std::size_t>(i)] = static_cast<jint>(level[i] * 255.0f + 0.5f);
    jintArray a = env->NewIntArray(n);
    jintArray b = env->NewIntArray(n);
    if (a && b) {
        env->SetIntArrayRegion(a, 0, n, reinterpret_cast<const jint*>(ms));
        env->SetIntArrayRegion(b, 0, n, lv.data());
        env->CallStaticVoidMethod(g_rumble_class, g_rumble_play, a, b);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (a) env->DeleteLocalRef(a);
    if (b) env->DeleteLocalRef(b);
}

// The bar's Save and Load (kol::kZoneWorld...): none with the options menu
// open; greyed while the game's save or load screen is up; on, on the world
// map (kol::world_map_scene); none elsewhere.
int bar_world_mode() {
    if (kol::menu_is_open()) return kol::kZoneWorldNone;
    if (kol::action_file_screen_open())   // in Organize's grid, Item, Spell and Class stay live
        return kol::kZoneWorldGreyed | (kol::action_organize_ready() ? kol::kZoneOrganizeOn : 0);
    if (!kol::world_map_scene()) return kol::kZoneWorldNone;
    const int ok = kol::action_world_items_ok();   // as the game's menu lists them here
    return kol::kZoneWorldOn | ((ok & 1) ? 0 : kol::kZoneNoTraining) | ((ok & 2) ? 0 : kol::kZoneNoShop);
}

void* screen2_thread(void*) {
    // Class and method were resolved on SDLThread (see main): FindClass on a
    // natively-attached thread uses the system classloader, which cannot see
    // application classes. Only the JNIEnv is per-thread.
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env || !g_screen2_ready) {
        std::fprintf(stderr, "screen2: not bound; thread exiting\n");
        return nullptr;
    }
    std::fprintf(stderr, "screen2: bound\n");

    bool screen2_was_on = true;
    for (;;) {
        // Debug > No Second Screen (dev builds): Java then treats the device
        // as one-screened: the panel hidden, BACK pausing the game with the
        // options menu over it (SecondScreen.java).
        if (g_screen2_single) {
            static int single_shown = -1;
            const int single = kol::setting_on(kol::Setting::DebugNoSecondScreen) ? 1 : 0;
            if (single != single_shown) {
                env->CallStaticVoidMethod(g_screen2_class, g_screen2_single, single ? JNI_TRUE : JNI_FALSE);
                if (env->ExceptionCheck()) env->ExceptionClear();
                else single_shown = single;
            }
        }
        // Is the presentation up? The game's unit box on the map is left out
        // while it shows the unit (kol::second_screen_shown).
        if (g_screen2_available) {
            const jboolean up =
                env->CallStaticBooleanMethod(g_screen2_class, g_screen2_available);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else kol::set_second_screen_present(up == JNI_TRUE);
        }
        // The player can switch the panel off from the settings menu. Stop
        // pushing and tell Java once, so the panel clears rather than freezing
        // on whatever unit was last hovered.
        if (!kol::second_screen_wanted()) {
            if (screen2_was_on) {
                screen2_was_on = false;
                if (g_screen2_enabled) {
                    env->CallStaticVoidMethod(g_screen2_class,
                                              g_screen2_enabled, JNI_FALSE);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
            }
            SDL_Delay(120);
            continue;
        }
        if (!screen2_was_on) {
            screen2_was_on = true;
            if (g_screen2_enabled) {
                env->CallStaticVoidMethod(g_screen2_class, g_screen2_enabled,
                                          JNI_TRUE);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        // The opening's mountain and the title's clouds, carrying on below
        // the top screen (kol_scene_strip): sent when they move, null once
        // neither scene is up.
        if (g_screen2_strip) {
            static std::vector<std::uint32_t> cimg(kol::kStripW * kol::kStripH), clast;
            static bool con_last = false;
            int scene = 0;
            const bool con = kol::compose_scene_strip(cimg.data(), &scene);
            static int scene_last = 0;
            if (con ? (!con_last || scene != scene_last || cimg != clast) : con_last) {
                jintArray ia = nullptr;
                if (con && (ia = env->NewIntArray(kol::kStripW * kol::kStripH)))
                    env->SetIntArrayRegion(ia, 0, kol::kStripW * kol::kStripH,
                                           reinterpret_cast<const jint*>(cimg.data()));
                env->CallStaticVoidMethod(g_screen2_class, g_screen2_strip, ia,
                                          kol::kStripW, kol::kStripH, con ? scene : 0);
                if (ia) env->DeleteLocalRef(ia);
                if (env->ExceptionCheck()) {
                    env->ExceptionDescribe(); env->ExceptionClear();
                } else {
                    con_last = con;
                    scene_last = con ? scene : 0;
                    if (con) clast = cimg;
                }
            }
        }

        // Debug > Tile Info's editor page (kol_edit_panel): sent when it
        // changes, null once Tile Info is off.
        if (g_screen2_edit) {
            static std::vector<std::uint32_t> eimg(kol::kSheetW * kol::kSheetH), elast;
            static bool eon_last = false;
            const bool eon = kol::compose_edit_panel(eimg.data());
            if (eon ? (!eon_last || eimg != elast) : eon_last) {
                jintArray ia = nullptr;
                if (eon && (ia = env->NewIntArray(kol::kSheetW * kol::kSheetH)))
                    env->SetIntArrayRegion(ia, 0, kol::kSheetW * kol::kSheetH,
                                           reinterpret_cast<const jint*>(eimg.data()));
                env->CallStaticVoidMethod(g_screen2_class, g_screen2_edit, ia);
                if (ia) env->DeleteLocalRef(ia);
                if (env->ExceptionCheck()) {
                    env->ExceptionDescribe(); env->ExceptionClear();
                } else {
                    eon_last = eon;
                    if (eon) elast = eimg;
                }
            }
        }

        // The battle's units while the player browses the map (kol_unit_list).
        // Sent before the sheet: the second screen waits for the sheet of the
        // unit the list says the cursor is on, so it must learn that first.
        // the list's picture, its height, and the record of the unit on the
        // cursor's cell (0 on empty ground), sent when any of them changes;
        // null once there is no list. The units animate, so while it is up
        // it changes most ticks.
        if (g_screen2_list) {
            static std::vector<std::uint32_t> limg(kol::kListW * kol::kListMaxH), llast;
            static int lh_last = -1;
            static std::uint32_t lon_last = ~0u;
            std::uint32_t on = 0;
            const int lh = kol::compose_unit_list(limg.data(), &on);
            const std::size_t n = static_cast<std::size_t>(kol::kListW) * (lh > 0 ? lh : 0);
            // (A shop's flag too: the world map's army list is the same picture as the
            // shop menu's, and without it the bottom bar never came back.)
            static bool lshop_last = false;
            const bool lshop = kol::unit_list_in_shop();
            const bool same = lh == lh_last && on == lon_last && lshop == lshop_last &&
                              (lh <= 0 || std::equal(limg.begin(), limg.begin() + n, llast.begin()));
            if (!same) {
                jintArray ia = nullptr;
                if (lh > 0 && (ia = env->NewIntArray(static_cast<jsize>(n))))
                    env->SetIntArrayRegion(ia, 0, static_cast<jsize>(n),
                                           reinterpret_cast<const jint*>(limg.data()));
                env->CallStaticVoidMethod(g_screen2_class, g_screen2_list, ia,
                                          lh > 0 ? lh : 0, static_cast<jint>(on));
                if (ia) env->DeleteLocalRef(ia);
                if (env->ExceptionCheck()) {
                    env->ExceptionDescribe(); env->ExceptionClear();
                } else {
                    lh_last = lh;
                    lon_last = on;
                    lshop_last = lshop;
                    llast.assign(limg.begin(), limg.begin() + n);
                }
            }
        }

        // The hovered unit, decoded from its record and the ROM tables it
        // indexes. No fork, no captured window, no waiting: this is exact for
        // every unit on the map the instant the cursor reaches it.
        if (g_screen2_sheetimg) {
            kol::UnitInfo u;
            const bool have_unit = kol::unit_for_highlighted(&u) && u.valid;
            // No unit for half a second (off the battle map with no status
            // window up): the sheet is taken down, rather than left on the
            // last unit shown. The wait rides out a frame or two without one.
            static int none_ticks = 0;
            static bool sheet_up = false;
            if (have_unit) {
                none_ticks = 0;
            } else if (sheet_up && ++none_ticks >= 15) {
                env->CallStaticVoidMethod(g_screen2_class, g_screen2_sheetimg,
                                          static_cast<jintArray>(nullptr), 0, 0);
                if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
                else sheet_up = false;
            }
            if (have_unit) {
                // One line per unit the cursor reaches, and only when it
                // changes. This is the device-side check for a panel that
                // lives on a screen no screenshot tool can reach: Screen-2 is
                // FLAG_SECURE, so `screencap -d 4` comes back empty.
                {
                    static std::uint32_t last_rec = 0;
                    static int last_hp = -1;
                    if (u.record != last_rec || u.hp != last_hp) {
                        last_rec = u.record; last_hp = u.hp;
                        std::printf("screen2: %s %s  Lv%d EXP%d  HP %d/%d "
                                    "MP %d/%d  STR %d INT %d AGI %d  "
                                    "ATK %d M.ATK %d  DEF %d M.DEF %d  "
                                    "element %d alignment %d\n",
                                    u.name,
                                    u.cls.bitmap >= 0 ? "<strip>" : u.cls.text,
                                    u.level, u.exp, u.hp, u.hp_max,
                                    u.mp, u.mp_max, u.str_, u.intel, u.agi,
                                    u.atk_melee, u.atk_magic, u.def, u.mdef,
                                    u.element, u.alignment);
                        std::fflush(stdout);
                    }
                }
                // The whole sheet, drawn from ROM art (kol_sheet.cpp). Sent
                // only when a pixel of it changes, not every tick. While the
                // cursor chooses a move or target it is the acting unit's,
                // with the card of the unit under the cursor below; a target
                // that dies flashes red and fades out (compose_unit_sheet).
                if (g_screen2_sheetimg) {
                    static std::vector<std::uint32_t> img(kol::kSheetW * kol::kSheetH);
                    static std::vector<std::uint32_t> last;
                    if (!sheet_up) last.clear();   // taken down: send it again
                    if (kol::compose_unit_sheet(u, img.data(), SDL_GetTicks() / 1000.0) &&
                        img != last) {
                        jintArray ia = env->NewIntArray(kol::kSheetW * kol::kSheetH);
                        if (ia) {
                            env->SetIntArrayRegion(ia, 0, kol::kSheetW * kol::kSheetH,
                                                   reinterpret_cast<const jint*>(img.data()));
                            env->CallStaticVoidMethod(g_screen2_class, g_screen2_sheetimg,
                                                      ia, kol::kSheetW, kol::kSheetH);
                            env->DeleteLocalRef(ia);
                            if (env->ExceptionCheck()) {
                                env->ExceptionDescribe(); env->ExceptionClear();
                            } else {
                                last = img;
                                sheet_up = true;
                            }
                        }
                    }
                }

                // While a finger is on the lower card, its other page too, so
                // the swipe can show it beside the current one (null: the
                // two-card sheet, which has no pages).
                static bool peek_was = false;
                const bool peek = g_sheet_peek.load();
                if (g_screen2_peekimg && peek) {
                    static std::vector<std::uint32_t> other(kol::kSheetW * kol::kSheetH);
                    static std::vector<std::uint32_t> other_last;
                    const bool have = kol::compose_other_page(u, other.data());
                    if (!peek_was || (have ? other != other_last : !other_last.empty())) {
                        jintArray ia = nullptr;
                        if (have && (ia = env->NewIntArray(kol::kSheetW * kol::kSheetH)))
                            env->SetIntArrayRegion(ia, 0, kol::kSheetW * kol::kSheetH,
                                                   reinterpret_cast<const jint*>(other.data()));
                        env->CallStaticVoidMethod(g_screen2_class, g_screen2_peekimg, ia);
                        if (ia) env->DeleteLocalRef(ia);
                        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
                        if (have) other_last = other; else other_last.clear();
                    }
                }
                peek_was = peek;
            }
        }

        // The options menu after the pad changed it (a touch gets its image
        // back directly): the new image, or null once B has closed it.
        if (g_screen2_menuimg && kol::menu_take_dirty()) {
            jintArray ia = nullptr;
            if (kol::menu_is_open()) {
                static std::vector<std::uint32_t> mimg(kol::kSheetW * kol::kSheetH);
                if (kol::menu_render(mimg.data())) {
                    ia = env->NewIntArray(kol::kSheetW * kol::kSheetH);
                    if (ia) env->SetIntArrayRegion(ia, 0, kol::kSheetW * kol::kSheetH,
                                                   reinterpret_cast<const jint*>(mimg.data()));
                }
            }
            env->CallStaticVoidMethod(g_screen2_class, g_screen2_menuimg, ia);
            if (ia) env->DeleteLocalRef(ia);
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        }

        // The debug win waiting for its answer: "auto-win" in the corner.
        static int autowin_shown = -1;
        const int autowin = kol::auto_win_pending() ? 1 : 0;
        if (g_screen2_autowin && autowin != autowin_shown) {
            env->CallStaticVoidMethod(g_screen2_class, g_screen2_autowin,
                                      autowin ? JNI_TRUE : JNI_FALSE);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else autowin_shown = autowin;
        }

        // In Organize's item or spellbook stock, the HELP card of the one
        // under the list's cursor, over the page (kol::stock_help); null when
        // the cursor leaves the lists. Sent when it changes.
        if (g_screen2_pagehelp) {
            static int help_shown = -1;
            kol::UnitInfo hu;
            const int item = kol::stock_help(&hu) ? hu.help_item : 0;
            if (item != help_shown) {
                jintArray ia = nullptr;
                if (item) {
                    static std::vector<std::uint32_t> hp(kol::kSheetW * kol::kSheetH);
                    const int h = kol::render_help_card(nullptr, 0, hu, hp.data());
                    if (h > 0 && (ia = env->NewIntArray(kol::kSheetW * h)))
                        env->SetIntArrayRegion(ia, 0, kol::kSheetW * h,
                                               reinterpret_cast<const jint*>(hp.data()));
                }
                env->CallStaticVoidMethod(g_screen2_class, g_screen2_pagehelp, ia);
                if (ia) env->DeleteLocalRef(ia);
                if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
                else help_shown = item;
            }
        }

        // The bar's Save and Load (on, greyed, none) changed: draw it again.
        // Nothing else on the second screen need change when the game's save
        // screen opens over the world map.
        static int bar_shown = -1;
        const int bar = bar_world_mode();
        if (g_screen2_bar && bar != bar_shown) {
            env->CallStaticVoidMethod(g_screen2_class, g_screen2_bar);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else bar_shown = bar;
        }

        // 30 Hz. Nothing here forks or captures any more, so a tick is a
        // record read plus a few ROM lookups.
        SDL_Delay(33);
    }
    return nullptr;
}

// Reads files/widescreen.cfg. Returns 240 (faithful, the default) when the
// file is absent, empty or unparseable.
//
// "auto" picks the widest view that still scales by a WHOLE number: the
// largest integer scale whose 160 lines fit the panel, then the width that
// fills it. On 1920x1080 that is 6x -> 320x160, presented as 1920x960 with
// 60px bars, every guest pixel exactly 6x6 device pixels.
//
// Snapping to an integer beats matching the aspect exactly. Exact 16:9 at 160
// lines is 284.4, which needs a fractional 6.75x scale: it fills the height
// but scales unevenly, and some guest pixels come out larger than others.
// 320 also shows a third more battlefield than 284.
int requested_view_width(const char* base) {
    const std::string path = std::string(base) + "/widescreen.cfg";
    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return 240;
    char buf[32] = {0};
    const bool got = std::fgets(buf, sizeof(buf), fp) != nullptr;
    std::fclose(fp);
    if (!got) return 240;

    std::string v(buf);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
        v.pop_back();
    if (v.empty()) return 240;

    int width = 0;
    if (v == "auto") {
        // SDL_GetDisplayBounds needs the video subsystem, and run_game() does
        // not bring it up until much later. SDL_Init is refcounted, so
        // starting it here is safe and the runtime's own init still works.
        const bool video_ready =
            SDL_WasInit(SDL_INIT_VIDEO) ||
            SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
        SDL_Rect bounds{};
        if (!video_ready || SDL_GetDisplayBounds(0, &bounds) != 0 ||
            bounds.h <= 0) {
            std::printf("android: widescreen.cfg=auto but display bounds "
                        "unavailable; staying at 240x160\n");
            return 240;
        }
        // Long edge is the horizontal one in the landscape orientation the
        // game runs in, whichever way the panel reports itself.
        // The panel's 16:9 box: the view is always 16:9 (kol_settings'
        // ladder), barred at the sides on a wider phone.
        const int long_edge = bounds.w > bounds.h ? bounds.w : bounds.h;
        const int h = bounds.w > bounds.h ? bounds.h : bounds.w;
        const int w = std::min(long_edge, h * 16 / 9);
        const int scale = h / 160;
        if (scale < 1) {
            std::printf("android: display %dx%d is under 160 lines; "
                        "staying at 240x160\n", bounds.w, bounds.h);
            return 240;
        }
        width = (w + scale - 1) / scale;   // ceil: fill the width, never crop
        std::printf("android: display %dx%d -> integer scale %dx -> "
                    "logical view %dx160 (%dx%d, %dpx bars)\n",
                    bounds.w, bounds.h, scale, width,
                    width * scale, 160 * scale, (h - 160 * scale) / 2);
    } else {
        width = std::atoi(v.c_str());
    }
    if (width < 240) return 240;
    if (width > 480) return 480;  // GbaPpu::kMaxRenderWidth
    return width;
}

// SDL runs SDL_main on its own "SDLThread", whose stack is far smaller than a
// desktop main thread's 8 MB. The runtime's save-state path overflowed it and
// took the process down with SIGSEGV ("stack pointer is not in a rw map"), so
// run the engine on a thread we size ourselves.
constexpr std::size_t kGuestStackBytes = 16u * 1024u * 1024u;

struct GuestArgs {
    int argc;
    char** argv;
    int result;
};

void* guest_thread(void* p) {
    auto* a = static_cast<GuestArgs*>(p);
    gbarecomp::RunOptions opts;
    opts.builtin_game_name = "Tactics Ogre: The Knight of Lodis";
    opts.builtin_rom_sha1  = "85a46320a31f16f5c6dda27fc094edd078dbe43d";
    opts.show_fps_by_default = true;
    // Rewind on L2 (gbarecomp's default binding): 20 s kept, a state every
    // 15 frames; each press goes back a second, held it repeats. Off while
    // Debug > Tile Info or L2 Moves Sun has L2 (kol_widescreen).
    opts.rewind_history_seconds = 20;
    kol::configure_extended_view(opts);
    a->result = gbarecomp::run_game(a->argc, a->argv, opts);
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    // SDL_AndroidGetInternalStoragePath() is the app's private files dir
    // (/data/data/<pkg>/files). The Java setup flow copies the user-picked
    // ROM and BIOS there under fixed names before starting this activity.
    route_stdio_to_logcat();

    const char* base = SDL_AndroidGetInternalStoragePath();
    if (!base) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "no internal storage path; cannot locate ROM/BIOS");
        return 1;
    }
    const std::string rom  = std::string(base) + "/totkol.gba";
    const std::string bios = std::string(base) + "/gba_bios.bin";
    const std::string save = std::string(base) + "/totkol.sav";
    // VS Mode's session folders (kol_vs_cpu: std::filesystem's temporary
    // folder): an app has no /tmp; files/vs is its own.
    {
        const std::string tmp = std::string(base) + "/vs";
        mkdir(tmp.c_str(), 0700);
        SDL_setenv("TMPDIR", tmp.c_str(), 1);
    }

    // Android has no runtime C toolchain and blocks dlopen() of app-written
    // .so files, so the self-heal recompile tier cannot run. The interpreter
    // bridge still covers the IWRAM PCs that never compile to native; it is
    // logged loudly by the runtime, as the project rules require.
    SDL_setenv("GBARECOMP_SELFHEAL_RECOMPILE", "0", 1);

    // So what the static build misses on the device reaches the desktop for
    // review: each session's coverage summary (every bridged PC, its entries
    // and interpreted instructions) in files/misses/, one file per launch so a
    // short session cannot overwrite a long one, rewritten every 600 frames
    // (~10 s) once something was bridged; the app is killed, not exited, so
    // the exit-time write alone never happened. Pull with
    //   adb exec-out "run-as com.koldecomp.game tar c files/misses" | tar x
    // Not in a public build (kol_dev.h).
    if constexpr (kol::kDevTools) {
        const std::string dir = std::string(base) + "/misses";
        mkdir(dir.c_str(), 0700);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&now));
        const std::string cov = dir + "/coverage_" + stamp + ".json";
        const std::string frag = dir + "/misses_" + stamp + ".toml.frag";
        SDL_setenv("GBARECOMP_COVERAGE_JSON", cov.c_str(), 1);
        SDL_setenv("GBARECOMP_MISS_FRAG", frag.c_str(), 1);
        SDL_setenv("GBARECOMP_MISS_FLUSH_FRAMES", "600", 0);
    }

    // On-screen controls only while no game controller is connected (USB,
    // Bluetooth, or a handheld's own pad, as the Thor's): a phone shows them
    // until one is paired, and they go as soon as it is (gbarecomp's auto
    // mode follows SDL's controller events). Asked 2026-11-17; before, they
    // were always off ("this build targets Android handhelds").
    SDL_setenv("GBARECOMP_TOUCH_CONTROLS", "auto", 1);

    // Developer switches on the device: files/env.cfg, one KEY=VALUE per
    // line ('#' comments), applied before the runtime starts. The desktop
    // takes these from the shell; a handheld has no shell for the app, so
    // this is how an env-gated probe or A/B switch reaches it. Absent on a
    // player's device, so nothing changes there. Not read by a public build.
    if constexpr (kol::kDevTools) {
        const std::string env_path = std::string(base) + "/env.cfg";
        if (std::FILE* f = std::fopen(env_path.c_str(), "r")) {
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                std::size_t n = std::strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
                    line[--n] = '\0';
                char* eq = std::strchr(line, '=');
                if (line[0] == '#' || !eq) continue;
                *eq = '\0';
                SDL_setenv(line, eq + 1, 1);
                std::fprintf(stderr, "env.cfg: %s=%s\n", line, eq + 1);
            }
            std::fclose(f);
        }
    }

    // Play without a BIOS: files/no_bios, written by the setup screen's
    // "Play without a BIOS (experimental)" switch. The runtime then takes the
    // built-in replacements (--no-bios) and no BIOS file at all, so
    // gba_bios.bin may be absent. Without the marker the real BIOS is used,
    // as before. Options > Enhancement Options > Technical > BIOS writes or
    // removes the same marker, for the next start (2026-11-23); with no
    // gba_bios.bin the built-in one is the only choice.
    const bool no_bios = access((std::string(base) + "/no_bios").c_str(), F_OK) == 0 ||
                         access(bios.c_str(), R_OK) != 0;
    if (no_bios) {
        std::printf("android: files/no_bios set; playing without a BIOS "
                    "(built-in replacements)\n");
        std::fflush(stdout);
    }

    std::vector<std::string> args = {
        "KnightOfLodisRecomp",
        "--rom",  rom,
    };
    // A mod (Options > Mod): a patch over the ROM, in memory. The runtime
    // gives it its own save and slots; KoL its own memo and numbered states.
    std::string mod_refused;
    const std::string mod = pick_mod(base, rom, &mod_refused);
    if (!mod.empty()) {
        args.push_back("--rom-patch");
        args.push_back(mod);
    } else if (!mod_refused.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Mod", mod_refused.c_str(), nullptr);
    }
    if (no_bios) {
        args.push_back("--no-bios");
    } else {
        args.push_back("--bios");
        args.push_back(bios);
    }
    args.push_back("--save-path");
    args.push_back(save);
    args.push_back("--window");
    // No Game Boy logo and chime at start (asked 2026-10-18): gbarecomp's
    // boot skip alone, the BIOS's SWIs still the recompiled ones. --no-bios
    // implies it; passed either way.
    args.push_back("--skip-bios-intro");

    // Extended view stays OFF unless a marker file asks for it, so the default
    // Android build renders the faithful 240x160 exactly as before (gbarecomp
    // ENHANCEMENTS.md Rule 1: enhancements are opt-in, default-off). There is
    // no CLI on Android, hence the file:
    //
    //   adb shell "run-as com.koldecomp.game sh -c 'echo auto > files/widescreen.cfg'"
    //
    // Contents: "auto" to follow the panel's own aspect at the GBA's 160
    // lines, or an explicit logical width (284 is true 16:9). Delete the file
    // to go back to 240.
    const int resolved_width = requested_view_width(base);
    if (resolved_width > 240) {
        std::printf("android: extended view requested: %dx160\n",
                    resolved_width);
        std::fflush(stdout);
        args.push_back("--view-width");
        args.push_back(std::to_string(resolved_width));
    }
    // The player-facing toggles. Seeded from what the host just resolved, then
    // overridden by anything the player chose last session. The widescreen
    // switch flips between this width and 240 while running; widescreen.cfg
    // still decides what "on" means.
    // The panel's own geometry, for the menu's width ladder. SDL's video
    // subsystem is already up by here whenever widescreen.cfg said "auto";
    // starting it is refcounted and harmless when it is not.
    unsigned panel_w = 0, panel_h = 0;
    {
        const bool video_ready = SDL_WasInit(SDL_INIT_VIDEO) ||
                                 SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
        SDL_Rect bounds{};
        if (video_ready && SDL_GetDisplayBounds(0, &bounds) == 0 &&
            bounds.w > 0 && bounds.h > 0) {
            panel_w = static_cast<unsigned>(bounds.w);
            panel_h = static_cast<unsigned>(bounds.h);
        }
    }
    kol::settings_init(base, static_cast<unsigned>(resolved_width),
                       panel_w, panel_h);
    kol::bios_in_use_set(!no_bios);   // Technical > BIOS: what this start plays with
    kol::vs_cpu_args(args);   // VS Mode's copies start with the same ROM, BIOS and mod
    std::vector<char*> argv_storage;
    argv_storage.reserve(args.size() + 1);
    for (auto& a : args) argv_storage.push_back(a.data());
    argv_storage.push_back(nullptr);

    {
        pthread_attr_t self_attr;
        std::size_t self_stack = 0;
        if (pthread_getattr_np(pthread_self(), &self_attr) == 0) {
            pthread_attr_getstacksize(&self_attr, &self_stack);
            pthread_attr_destroy(&self_attr);
        }
        std::printf("android: SDLThread stack=%zu bytes; running guest on a "
                    "%zu byte stack\n", self_stack, kGuestStackBytes);
        std::fflush(stdout);
    }

    // Resolve SecondScreen on THIS thread: main() is SDL_main, running on
    // SDLThread, which Java created and which therefore carries the app
    // classloader. A plain pthread does not.
    if (screen2_bind(static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()))) {
        pthread_t s2;
        if (pthread_create(&s2, nullptr, screen2_thread, nullptr) == 0)
            pthread_detach(s2);
    } else {
        std::fprintf(stderr, "screen2: bind failed on SDLThread\n");
    }
    if (cheevos_bind(static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()))) {
        char release[PROP_VALUE_MAX] = {};
        __system_property_get("ro.build.version.release", release);
        kol::cheevos_set_platform((std::string("Android ") + release).c_str());
        kol::cheevos_set_http(&cheevos_http_android);
        kol::cheevos_set_notify(&cheevos_notify_android);
        kol::cheevos_set_login_form(&cheevos_login_form_android);
        kol::cheevos_set_popup(&cheevos_popup_android);
        kol::cheevos_start();   // logs in with the saved token, if there is one
    } else {
        std::fprintf(stderr, "cheevos: Cheevos.java not bound; no RetroAchievements\n");
    }
    if (platform_bind(static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()))) {
        kol::MenuPlatform p;
        p.pick_mod = &mods_pick_android;
        p.remove_mod = &mods_remove_android;
        p.restart = &restart_android;
        if (g_mods_licenses) p.show_licenses = &licenses_android;
        if (g_mods_pick_bios) p.pick_bios = &bios_pick_android;   // Technical > BIOS
        if (g_mods_pick_shaders) p.pick_shader_folder = &shaders_pick_android;   // Display > Shader
        if constexpr (kol::kDevTools) p.unlink_rom = &unlink_rom_android;   // Debug > Unlink ROM
        kol::menu_set_platform(p);
    } else {
        std::fprintf(stderr, "mods: Mods / SetupActivity not bound; no Options > Mod\n");
    }
    if (vs_bind(static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()))) {
        kol::VsPlatform vp;
        vp.spawn = &vs_spawn_android;
        vp.wifi_lock = &vs_wifi_lock_android;
        kol::vs_set_platform(vp);
    } else {
        std::fprintf(stderr, "vs: VsCopy not bound; no VS against the computer or the network\n");
    }
    if (rumble_bind(static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv())))
        kol::rumble_set_backend(&rumble_android);
    else
        std::fprintf(stderr, "rumble: Rumble.play not bound; the controller is tried instead\n");

    GuestArgs ga{static_cast<int>(argv_storage.size() - 1),
                 argv_storage.data(), 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kGuestStackBytes);
    pthread_t guest;
    if (pthread_create(&guest, &attr, guest_thread, &ga) != 0) {
        pthread_attr_destroy(&attr);
        std::fprintf(stderr, "android: could not create guest thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_join(guest, nullptr);
    return ga.result;
}


// ── VS Mode's copy, in VsCopyService's process ──────────────────────────────
// The other player's (or the computer's) game, unseen: the session's switches
// set first (they are what the desktop passes as the copy's environment),
// its output to the session's log, its settings from its owner's file, then
// the runtime headless on a thread of our own size, as main() runs it.
extern "C" JNIEXPORT jint JNICALL Java_com_koldecomp_game_VsCopyService_nativeRun(JNIEnv* env, jclass,
                                                                               jobjectArray jargv, jobjectArray jenv,
                                                                               jstring jlog, jstring jdir) {
    auto strings = [env](jobjectArray a) {
        std::vector<std::string> v;
        const jsize n = a ? env->GetArrayLength(a) : 0;
        for (jsize i = 0; i < n; ++i) {
            auto s = static_cast<jstring>(env->GetObjectArrayElement(a, i));
            const char* c = s ? env->GetStringUTFChars(s, nullptr) : nullptr;
            v.emplace_back(c ? c : "");
            if (c) env->ReleaseStringUTFChars(s, c);
            if (s) env->DeleteLocalRef(s);
        }
        return v;
    };
    auto string = [env](jstring s) {
        const char* c = s ? env->GetStringUTFChars(s, nullptr) : nullptr;
        std::string out = c ? c : "";
        if (c) env->ReleaseStringUTFChars(s, c);
        return out;
    };
    std::vector<std::string> args = strings(jargv);
    const std::vector<std::string> sets = strings(jenv);
    const std::string log = string(jlog), dir = string(jdir);
    if (!log.empty()) {
        std::freopen(log.c_str(), "w", stdout);
        std::freopen(log.c_str(), "a", stderr);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    if (!dir.empty() && chdir(dir.c_str()) != 0) std::fprintf(stderr, "vs: no folder %s\n", dir.c_str());
    for (const std::string& kv : sets) {
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
    }
    if (kol::vs_mirror_child()) kol::vsnet::mirror_settings();
    std::vector<char*> argv_storage;
    for (auto& a : args) argv_storage.push_back(a.data());
    argv_storage.push_back(nullptr);
    GuestArgs ga{static_cast<int>(argv_storage.size() - 1), argv_storage.data(), 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kGuestStackBytes);
    pthread_t guest;
    if (pthread_create(&guest, &attr, guest_thread, &ga) != 0) {
        pthread_attr_destroy(&attr);
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_join(guest, nullptr);
    std::fflush(stdout);
    std::fflush(stderr);
    return ga.result;
}

// ── The settings menu's bridge ───────────────────────────────────────────────
// Java builds the menu from this list rather than carrying its own copy, so a
// new toggle needs no Java change: add it to kol::Setting and it appears.
extern "C" {

// The mod picker's answer (Mods.nativePicked, from SetupActivity's pick_mod
// mode): the game says so in its own dialog.
extern "C" JNIEXPORT void JNICALL Java_com_koldecomp_game_Mods_nativePicked(JNIEnv* env, jclass,
                                                                         jstring name, jstring error) {
    const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    const char* e = error ? env->GetStringUTFChars(error, nullptr) : nullptr;
    kol::menu_mod_picked(n, e);
    if (n) env->ReleaseStringUTFChars(name, n);
    if (e) env->ReleaseStringUTFChars(error, e);
}

// Display > Shader > Import Folder's answer (Mods.nativeShadersImported).
extern "C" JNIEXPORT void JNICALL Java_com_koldecomp_game_Mods_nativeShadersImported(JNIEnv* env, jclass, jint presets,
                                                                                  jstring name, jstring error) {
    const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    const char* e = error ? env->GetStringUTFChars(error, nullptr) : nullptr;
    kol::menu_shaders_imported(presets, n, e);
    if (n) env->ReleaseStringUTFChars(name, n);
    if (e) env->ReleaseStringUTFChars(error, e);
}

// Options > Technical > BIOS's picker (Mods.nativeBiosPicked): kept and
// chosen for the next start, or why not; the game says so in its dialog.
extern "C" JNIEXPORT void JNICALL Java_com_koldecomp_game_Mods_nativeBiosPicked(JNIEnv* env, jclass,
                                                                             jstring error) {
    const char* e = error ? env->GetStringUTFChars(error, nullptr) : nullptr;
    kol::menu_bios_picked(e);
    if (e) env->ReleaseStringUTFChars(error, e);
}

// The login form's Log In (Cheevos.showLogin): the password goes to the server
// once, from a native worker; only the token comes back to be kept.
JNIEXPORT void JNICALL
Java_com_koldecomp_game_Cheevos_nativeLogin(JNIEnv* env, jclass, jstring user, jstring password) {
    const char* u = env->GetStringUTFChars(user, nullptr);
    const char* p = env->GetStringUTFChars(password, nullptr);
    if (u && p) kol::cheevos_login(u, p);
    if (u) env->ReleaseStringUTFChars(user, u);
    if (p) env->ReleaseStringUTFChars(password, p);
}

// Options > Display > Bottom Screen: StatsView's size, and how it draws.
JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_nativeBottomSize(JNIEnv*, jclass, jint w, jint h) {
    kol::bottom_screen_size_set(w, h);
}
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_SecondScreen_nativeBottomScaling(JNIEnv*, jclass) {
    return kol::bottom_scaling_mode();
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeSettingCount(JNIEnv*, jclass) {
    return static_cast<jint>(kol::Setting::Count);
}

JNIEXPORT jstring JNICALL
Java_com_koldecomp_game_MainActivity_nativeSettingLabel(JNIEnv* env, jclass,
                                                        jint index) {
    if (index < 0 || index >= static_cast<jint>(kol::Setting::Count))
        return env->NewStringUTF("");
    return env->NewStringUTF(
        kol::setting_label(static_cast<kol::Setting>(index)));
}

JNIEXPORT jboolean JNICALL
Java_com_koldecomp_game_MainActivity_nativeSettingOn(JNIEnv*, jclass,
                                                     jint index) {
    if (index < 0 || index >= static_cast<jint>(kol::Setting::Count))
        return JNI_FALSE;
    return kol::setting_on(static_cast<kol::Setting>(index)) ? JNI_TRUE
                                                             : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_MainActivity_nativeSettingSet(JNIEnv*, jclass,
                                                      jint index,
                                                      jboolean on) {
    if (index < 0 || index >= static_cast<jint>(kol::Setting::Count)) return;
    kol::setting_set(static_cast<kol::Setting>(index), on == JNI_TRUE);
}

// The extended view's width. A choice rather than a toggle, so it is its own
// short list behind the dialog's "Width" button; the ladder is built natively
// from the panel, so Java never carries a list of widths either.
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeWidthCount(JNIEnv*, jclass) {
    return static_cast<jint>(kol::width_choice_count());
}

JNIEXPORT jstring JNICALL
Java_com_koldecomp_game_MainActivity_nativeWidthLabel(JNIEnv* env, jclass,
                                                      jint index) {
    if (index < 0) return env->NewStringUTF("");
    return env->NewStringUTF(
        kol::width_choice_label(static_cast<unsigned>(index)));
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeWidthCurrent(JNIEnv*, jclass) {
    const unsigned cur = kol::width_choice_current();
    // One past the end means the live width is not on the ladder; report -1 so
    // the dialog shows nothing selected rather than selecting the wrong row.
    return cur < kol::width_choice_count() ? static_cast<jint>(cur) : -1;
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_MainActivity_nativeWidthSet(JNIEnv*, jclass,
                                                    jint index) {
    if (index < 0) return;
    kol::width_choice_set(static_cast<unsigned>(index));
}

// The depth of field's strength, the same kind of short native list.
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeBlurCount(JNIEnv*, jclass) {
    return static_cast<jint>(kol::blur_choice_count());
}

JNIEXPORT jstring JNICALL
Java_com_koldecomp_game_MainActivity_nativeBlurLabel(JNIEnv* env, jclass,
                                                     jint index) {
    if (index < 0) return env->NewStringUTF("");
    return env->NewStringUTF(kol::blur_choice_label(static_cast<unsigned>(index)));
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeBlurCurrent(JNIEnv*, jclass) {
    return static_cast<jint>(kol::blur_choice_current());
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_MainActivity_nativeBlurSet(JNIEnv*, jclass, jint index) {
    if (index < 0) return;
    kol::blur_choice_set(static_cast<unsigned>(index));
}

// The sound: hardware mix or the verified high-quality re-render.
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeSoundCount(JNIEnv*, jclass) {
    return static_cast<jint>(kol::sound_choice_count());
}

JNIEXPORT jstring JNICALL
Java_com_koldecomp_game_MainActivity_nativeSoundLabel(JNIEnv* env, jclass,
                                                      jint index) {
    if (index < 0) return env->NewStringUTF("");
    return env->NewStringUTF(kol::sound_choice_label(static_cast<unsigned>(index)));
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeSoundCurrent(JNIEnv*, jclass) {
    return static_cast<jint>(kol::sound_choice_current());
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_MainActivity_nativeSoundSet(JNIEnv*, jclass, jint index) {
    if (index < 0) return;
    kol::sound_choice_set(static_cast<unsigned>(index));
}

// Music and effects volumes (which: 0 music, 1 effects), 0..100% by 10.
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeVolumeCount(JNIEnv*, jclass) {
    return static_cast<jint>(kol::volume_choice_count());
}

JNIEXPORT jstring JNICALL
Java_com_koldecomp_game_MainActivity_nativeVolumeLabel(JNIEnv* env, jclass,
                                                       jint index) {
    if (index < 0) return env->NewStringUTF("");
    return env->NewStringUTF(kol::volume_choice_label(static_cast<unsigned>(index)));
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_MainActivity_nativeVolumeCurrent(JNIEnv*, jclass, jint which) {
    return static_cast<jint>(kol::volume_choice_current(which != 0));
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_MainActivity_nativeVolumeSet(JNIEnv*, jclass, jint which,
                                                     jint index) {
    if (index < 0) return;
    kol::volume_choice_set(which != 0, static_cast<unsigned>(index));
}

// The options menu on the second screen (kol_menu.h). Each call returns the
// menu's new image (kSheetW x kSheetH ARGB), or null once it is closed.
namespace {
jintArray menu_image(JNIEnv* env) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(kol::kSheetW) * kol::kSheetH);
    if (!kol::menu_render(px.data())) return nullptr;
    jintArray out = env->NewIntArray(static_cast<jsize>(px.size()));
    if (out) env->SetIntArrayRegion(out, 0, static_cast<jsize>(px.size()),
                                    reinterpret_cast<const jint*>(px.data()));
    return out;
}
}  // namespace

JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuToggle(JNIEnv* env, jclass) {
    return kol::menu_toggle() ? menu_image(env) : nullptr;
}

// The game held still while the options menu is up over it on a device with
// one screen (gbarecomp's g_runtime_pause_request); the pad still reaches the
// menu through the key filter.
JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_nativePause(JNIEnv*, jclass, jboolean on) {
    gbarecomp::g_runtime_pause_request = on == JNI_TRUE ? 1 : 0;
}

JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuTouch(JNIEnv* env, jclass, jint action,
                                               jfloat x, jfloat y) {
    kol::menu_touch(action, x, y);
    return kol::menu_is_open() ? menu_image(env) : nullptr;
}

// The Save States page's grid (kol_menu.h): the UI thread scrolls its rows.
JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuStatesInfo(JNIEnv* env, jclass) {
    jint info[7] = {};
    int got[7] = {};
    if (!kol::menu_states_info(got)) return nullptr;
    for (int i = 0; i < 7; ++i) info[i] = got[i];
    jintArray a = env->NewIntArray(7);
    if (a) env->SetIntArrayRegion(a, 0, 7, info);
    return a;
}

JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuStatesRow(JNIEnv* env, jclass, jint row, jint h) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(kol::kSheetW) * h);
    if (!kol::menu_states_row(row, px.data())) return nullptr;
    jintArray a = env->NewIntArray(static_cast<jsize>(px.size()));
    if (a) env->SetIntArrayRegion(a, 0, static_cast<jsize>(px.size()),
                                  reinterpret_cast<const jint*>(px.data()));
    return a;
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_menuStatesScroll(JNIEnv*, jclass, jfloat scroll) {
    kol::menu_states_scroll(scroll);
}

// The sheet's lower card: a swipe flips it between EQUIP./TALENT and the
// EMBLEM page; a tap selects an emblem there (true when something changed).
// The screen2 thread sends the new picture on its next tick.
// The editor page: a touch in its canvas pixels (0 down, 1 move, 2 up, 3
// cancel). The screen2 thread sends the new picture on its next tick.
JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_editTouch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    kol::edit_panel_touch(action, x, y);
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_sheetFlipPage(JNIEnv*, jclass) {
    kol::sheet_flip_lower_page();
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_sheetPeek(JNIEnv*, jclass, jboolean on) {
    g_sheet_peek.store(on == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_koldecomp_game_SecondScreen_sheetTap(JNIEnv*, jclass, jfloat x, jfloat y) {
    return kol::sheet_tap(x, y) ? JNI_TRUE : JNI_FALSE;
}

// A tap on the unit list, in its canvas pixels: the unit there is selected on
// the battlefield (the map cursor glides to it). Its record, 0 for none.
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_SecondScreen_unitListTap(JNIEnv*, jclass, jfloat x, jfloat y) {
    return static_cast<jint>(kol::unit_list_tap(x, y));
}

// The top rows of the list's picture that stay when it scrolls.
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_SecondScreen_unitListPin(JNIEnv*, jclass) {
    return static_cast<jint>(kol::unit_list_pin());
}

// The zone under the page, kSheetW x h, with the button inside it, pushed
// in while `pressed` (kol::menu_zone_render); null before the ROM art or for
// h < the button.
JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuZone(JNIEnv* env, jclass, jint h, jint pressed,
                                              jint world) {
    if (h < kol::kMenuButtonSize || h > 1024) return nullptr;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(kol::kSheetW) * h);
    if (!kol::menu_zone_render(px.data(), h, pressed, world)) return nullptr;
    jintArray a = env->NewIntArray(static_cast<jsize>(px.size()));
    if (a) env->SetIntArrayRegion(a, 0, static_cast<jsize>(px.size()),
                                  reinterpret_cast<const jint*>(px.data()));
    return a;
}

// The bar's Save and Load buttons (bar_world_mode).
JNIEXPORT jint JNICALL
Java_com_koldecomp_game_SecondScreen_menuWorld(JNIEnv*, jclass) {
    return bar_world_mode();
}

// Save and Load: the game's own screens, as its system menu opens them, with
// its confirm sound (kol::action_world_save / _load).
// Item (0), Spell (1), Class (2): Organize's R menu screens; Training (3),
// Shop (4): the main menu's; the same way.
JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_worldItem(JNIEnv*, jclass, jint item) {
    kol::menu_confirm_sound();
    kol::action_world_item(item);
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_worldSave(JNIEnv*, jclass) {
    kol::menu_confirm_sound();
    kol::action_world_save();
}
JNIEXPORT void JNICALL
Java_com_koldecomp_game_SecondScreen_worldLoad(JNIEnv*, jclass) {
    kol::menu_confirm_sound();
    kol::action_world_load();
}

// The bar's button's help card (kol::menu_zone_help_render), kSheetW wide and
// as tall as the card (length / kSheetW); null for none.
JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_barHelp(JNIEnv* env, jclass, jint button) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(kol::kSheetW) * kol::kSheetH);
    const int h = kol::menu_zone_help_render(button, px.data());
    if (h <= 0) return nullptr;
    const jsize n = static_cast<jsize>(kol::kSheetW * h);
    jintArray a = env->NewIntArray(n);
    if (a) env->SetIntArrayRegion(a, 0, n, reinterpret_cast<const jint*>(px.data()));
    return a;
}

// The game's pointing hand, kHandW x kHandH (kol::menu_hand_render).
JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuHand(JNIEnv* env, jclass) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(kol::kHandW) * kol::kHandH);
    if (!kol::menu_hand_render(px.data())) return nullptr;
    jintArray a = env->NewIntArray(static_cast<jsize>(px.size()));
    if (a) env->SetIntArrayRegion(a, 0, static_cast<jsize>(px.size()),
                                  reinterpret_cast<const jint*>(px.data()));
    return a;
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_SecondScreen_unitListPress(JNIEnv*, jclass, jfloat x, jfloat y, jint phase) {
    return static_cast<jint>(kol::unit_list_press(x, y, phase));
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_SecondScreen_unitListShop(JNIEnv*, jclass) {
    return kol::unit_list_in_shop() ? 1 : 0;
}

JNIEXPORT jint JNICALL
Java_com_koldecomp_game_SecondScreen_unitListFaces(JNIEnv*, jclass) {
    return static_cast<jint>(kol::unit_list_faces());
}

// The menu's scroll arrow, 8 x 8 (kol::menu_arrow_render).
JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuArrow(JNIEnv* env, jclass, jboolean down) {
    std::vector<std::uint32_t> px(8 * 8);
    if (!kol::menu_arrow_render(px.data(), down != JNI_FALSE)) return nullptr;
    jintArray a = env->NewIntArray(static_cast<jsize>(px.size()));
    if (a) env->SetIntArrayRegion(a, 0, static_cast<jsize>(px.size()),
                                  reinterpret_cast<const jint*>(px.data()));
    return a;
}

// The corner button, kMenuButtonSize square.
JNIEXPORT jintArray JNICALL
Java_com_koldecomp_game_SecondScreen_menuButton(JNIEnv* env, jclass) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(kol::kMenuButtonSize) * kol::kMenuButtonSize);
    if (!kol::menu_button_render(px.data())) return nullptr;
    jintArray out = env->NewIntArray(static_cast<jsize>(px.size()));
    if (out) env->SetIntArrayRegion(out, 0, static_cast<jsize>(px.size()),
                                    reinterpret_cast<const jint*>(px.data()));
    return out;
}

// Save states by slot. The pad combos only reach slot 1, which is also where
// stray combos land, so the menu is the way to keep a state somewhere safe.
// Both go through the runtime's single save/load path at a safe boundary.
JNIEXPORT void JNICALL
Java_com_koldecomp_game_MainActivity_nativeStateSave(JNIEnv*, jclass,
                                                     jint slot) {
    gbarecomp::runtime_request_state_save(static_cast<int>(slot));
}

JNIEXPORT void JNICALL
Java_com_koldecomp_game_MainActivity_nativeStateLoad(JNIEnv*, jclass,
                                                     jint slot) {
    gbarecomp::runtime_request_state_load(static_cast<int>(slot));
}

}  // extern "C"
