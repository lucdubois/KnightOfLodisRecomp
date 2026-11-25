# Android JNI build for Knight of Lodis Recomp.
#
# Layout follows SDL2's own android-project convention: SDL is vendored here
# and built first so that SDL2::SDL2 exists, which is exactly what
# gbarecomp/CMakeLists.txt looks for:
#
#     if(ANDROID AND TARGET SDL2::SDL2)
#         # Android applications provide SDL from the parent Gradle/CMake
#         # project. Reusing that target keeps SDLActivity's libSDL2.so as
#         # the single backend.
#
# So the ordering below is load-bearing: SDL must be added before gbarecomp.
cmake_minimum_required(VERSION 3.20)
project(KnightOfLodisAndroid CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KOL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(GBARECOMP_ROOT "${KOL_ROOT}/../gbarecomp")

# 1. SDL2 — produces the SDL2::SDL2 target and libSDL2.so that SDLActivity
#    loads. Built shared, which is what the Java side expects.
# 16 KB memory pages (Android 15+ devices): every shared library's LOAD
# segments aligned to 16384, which 4 KB devices load as well. Before the
# subdirectories, so SDL2 gets it too. (The NDK's
# ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES does the same; as a CMake argument it
# would rebuild all the generated code.)
add_link_options("-Wl,-z,max-page-size=16384")

set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL)

# 2. gbarecomp framework. Sees ANDROID + SDL2::SDL2 and takes its Android path.
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp-build)

# 3. The recompiled game itself. Same generated sources as the desktop build;
#    only the entry point differs (SDL_main instead of main). One library a
#    cartridge (kol_game_library, below): libmain.so is the USA game, and
#    libmain_jp.so the Japanese one when its recompiled code is there
#    (generated_atoj, src/kol_region.h). MainActivity loads the one whose ROM
#    the player gave.

# map_settings.cfg built into the game (src/kol_map_settings.h): its text
# as a C++ raw string; an edit of the file reconfigures.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KOL_ROOT}/map_settings.cfg")
file(READ "${KOL_ROOT}/map_settings.cfg" KOL_MAP_SETTINGS_TEXT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kol_map_settings_baked.inc"
     "R\"KOLMAP(${KOL_MAP_SETTINGS_TEXT})KOLMAP\"\n")
# map_ext.cfg (the map editor's extensions, src/kol_map_source.cpp) built in the
# same way: what a device with no file of its own, and every public build, draws.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KOL_ROOT}/map_ext.cfg")
if(EXISTS "${KOL_ROOT}/map_ext.cfg")
    file(READ "${KOL_ROOT}/map_ext.cfg" KOL_MAP_EXT_TEXT)
else()
    set(KOL_MAP_EXT_TEXT "")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kol_map_ext_baked.inc"
     "R\"KOLEXT(${KOL_MAP_EXT_TEXT})KOLEXT\"\n")

include("${KOL_ROOT}/third_party/rcheevos.cmake")

set(KOL_SOURCES
    main_android.cpp
    "${KOL_ROOT}/src/kol_cheevos.cpp"
    "${KOL_ROOT}/src/kol_crash.cpp"
    "${KOL_ROOT}/src/kol_shaders.cpp"
    "${KOL_ROOT}/src/kol_widescreen.cpp"
    "${KOL_ROOT}/src/kol_terrain_probe.cpp"
    "${KOL_ROOT}/src/kol_highlight.cpp"
    "${KOL_ROOT}/src/kol_unit_data.cpp"
    "${KOL_ROOT}/src/kol_a7.cpp"
    "${KOL_ROOT}/src/kol_sheet.cpp"
    "${KOL_ROOT}/src/kol_unit_list.cpp"
    "${KOL_ROOT}/src/kol_shop_buy.cpp"
    "${KOL_ROOT}/src/kol_scene_strip.cpp"
    "${KOL_ROOT}/src/kol_actions.cpp"
    "${KOL_ROOT}/src/kol_desktop.cpp"
    "${KOL_ROOT}/src/kol_fast_load.cpp"
    "${KOL_ROOT}/src/kol_fast_actions.cpp"
    "${KOL_ROOT}/src/kol_ghost.cpp"
    "${KOL_ROOT}/src/kol_gulls.cpp"
    "${KOL_ROOT}/src/kol_rain.cpp"
    "${KOL_ROOT}/src/kol_snow.cpp"
    "${KOL_ROOT}/src/kol_rumble.cpp"
    "${KOL_ROOT}/src/kol_states.cpp"
    "${KOL_ROOT}/src/kol_vs_bot.cpp"
    "${KOL_ROOT}/src/kol_vs_cpu.cpp"
    "${KOL_ROOT}/src/kol_vs_net.cpp"
    "${KOL_ROOT}/src/kol_stick_cursor.cpp"
    "${KOL_ROOT}/src/kol_auto_win.cpp"
    "${KOL_ROOT}/src/kol_map_source.cpp"
    "${KOL_ROOT}/src/kol_settings.cpp"
    "${KOL_ROOT}/src/kol_sound.cpp"
    "${KOL_ROOT}/src/kol_hd_audio.cpp"
    "${KOL_ROOT}/src/kol_menu.cpp"
    "${KOL_ROOT}/src/kol_tile_edit.cpp"
    "${KOL_ROOT}/src/kol_edit_panel.cpp"
    "${KOL_ROOT}/src/kol_map_settings.cpp"
    "${KOL_ROOT}/src/kol_obj_cull.cpp"
    "${KOL_ROOT}/src/kol_focus.cpp"
    "${KOL_ROOT}/src/kol_focus_atlas.cpp"
    "${KOL_ROOT}/src/kol_focus_cutouts.cpp"
    "${KOL_ROOT}/src/kol_focus_lamps.cpp"
    "${KOL_ROOT}/src/kol_focus_models.cpp"
    "${KOL_ROOT}/src/kol_focus_present.cpp"
    "${KOL_ROOT}/src/kol_focus_queries.cpp"
    "${KOL_ROOT}/src/kol_focus_roofs.cpp"
    "${KOL_ROOT}/src/kol_focus_shadows.cpp"
    "${KOL_ROOT}/src/kol_focus_sprites.cpp"
    "${KOL_ROOT}/src/kol_focus_windows.cpp"
    "${KOL_ROOT}/src/kol_perspective.cpp"
    "${KOL_ROOT}/src/kol_workers.cpp")
# Developer tools (src/kol_dev.h): the Gradle build type sets it (debug ON,
# release OFF).
option(KOL_DEV_TOOLS "Build the developer tools (Debug page, height editor, dev logs)" ON)
# The version in the session log's first line (kol_crash.cpp).
file(STRINGS "${KOL_ROOT}/android/app/build.gradle" KOL_VERSION_LINE REGEX "versionName \"")
string(REGEX REPLACE ".*versionName \"([^\"]*)\".*" "\\1" KOL_VERSION "${KOL_VERSION_LINE}")
set_source_files_properties("${KOL_ROOT}/src/kol_crash.cpp" PROPERTIES COMPILE_DEFINITIONS "KOL_VERSION=\"${KOL_VERSION}\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KOL_ROOT}/android/app/build.gradle")

# One cartridge's library: our sources and that ROM's recompiled code.
function(kol_game_library target generated_dir)
    file(GLOB shards CONFIGURE_DEPENDS "${generated_dir}/recompiled_*.cpp")
    set(generated ${shards} "${generated_dir}/dispatch_table.cpp")
    if(EXISTS "${generated_dir}/symbol_map.cpp")
        list(APPEND generated "${generated_dir}/symbol_map.cpp")
    endif()
    add_library(${target} SHARED ${KOL_SOURCES} ${generated})
    target_compile_definitions(${target} PRIVATE KOL_DEV_TOOLS=$<IF:$<BOOL:${KOL_DEV_TOOLS}>,1,0>)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${generated_dir}"
        "${KOL_ROOT}/src"
        "${KOL_ROOT}/third_party/stb")
    # `log` provides __android_log_write, used by main_android.cpp to route the
    # runtime's stdout/stderr onto logcat.
    target_link_libraries(${target} PRIVATE SDL2::SDL2 log kol_rcheevos)
    target_link_gbarecomp_runtime_stack(${target})
    # Bind libmain's own symbols inside libmain. Without it every call from the
    # recompiled code into the runtime (runtime_tick, the flag helpers, bus reads)
    # went through a PLT stub, as if another library could replace them: 2.6% of
    # the guest thread on the Thor (2026-10-06, simpleperf showed it as
    # unw_getcontext, the last named symbol before .plt). Nothing interposes:
    # SDLActivity only dlsym()s SDL_main, which stays exported, and overlays get
    # the runtime through GbaOverlayCallbacks, not by symbol.
    target_link_options(${target} PRIVATE "-Wl,-Bsymbolic")
endfunction()

# SDLActivity dlopen()s the last library MainActivity names and calls
# SDL_main: each is SHARED, and "main" stays the USA game's name.
kol_game_library(main "${KOL_ROOT}/generated")
if(EXISTS "${KOL_ROOT}/generated_atoj/dispatch_table.cpp")
    kol_game_library(main_jp "${KOL_ROOT}/generated_atoj")
    target_compile_definitions(main_jp PRIVATE KOL_REGION_ATOJ=1)
endif()

