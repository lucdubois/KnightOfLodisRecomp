cmake_minimum_required(VERSION 3.20)
project(KnightOfLodisRecomp CXX)
if(APPLE)
    enable_language(OBJCXX)   # src/kol_setup_mac.mm
endif()
if(WIN32)
    enable_language(RC)       # src/kol_windows.rc: the program's icon
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sibling checkout of https://github.com/mstan/gbarecomp — pinned locally at
# ed9824b70aa350cd9e1653894beaf6b1b6b27787 as of this writing. Pulling it in
# via add_subdirectory (rather than a prebuilt release lib) is the pattern
# gbarecomp's own CMakeLists documents for a game project: see the comment
# above `target_compile_definitions(gbarecomp_runtime PRIVATE
# GBARECOMP_SRC_DIR=...)` in gbarecomp/CMakeLists.txt.
set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../gbarecomp"
    CACHE PATH "Path to the gbarecomp framework checkout")

if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT (${GBARECOMP_ROOT}) does not contain gbarecomp. "
        "Clone https://github.com/mstan/gbarecomp.git as a sibling of this "
        "project, or pass -DGBARECOMP_ROOT=<path>.")
endif()

add_subdirectory("${GBARECOMP_ROOT}" gbarecomp-build)

# gba_recompile writes a variable number of recompiled_NNN.cpp shards
# (currently 16) plus dispatch_table.cpp and symbol_map.cpp. Glob so the
# project regenerating with a different shard count doesn't need editing.
file(GLOB KOL_GENERATED_SHARDS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_*.cpp")
set(KOL_GENERATED_SOURCES
    ${KOL_GENERATED_SHARDS}
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/dispatch_table.cpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
    list(APPEND KOL_GENERATED_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
endif()
if(KOL_GENERATED_SHARDS STREQUAL "")
    message(WARNING
        "generated/recompiled_*.cpp not found. Run "
        "'${GBARECOMP_ROOT}/build/gba_recompile --rom totkol.gba "
        "--config game.toml --out generated' first.")
endif()

# map_settings.cfg built into the game (src/kol_map_settings.h): its text
# as a C++ raw string; an edit of the file reconfigures.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/map_settings.cfg")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/map_settings.cfg" KOL_MAP_SETTINGS_TEXT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kol_map_settings_baked.inc"
     "R\"KOLMAP(${KOL_MAP_SETTINGS_TEXT})KOLMAP\"\n")
# map_ext.cfg (the map editor's extensions, src/kol_map_source.cpp) built in the
# same way: what a device with no file of its own, and every public build, draws.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/map_ext.cfg")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/map_ext.cfg")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/map_ext.cfg" KOL_MAP_EXT_TEXT)
else()
    set(KOL_MAP_EXT_TEXT "")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kol_map_ext_baked.inc"
     "R\"KOLEXT(${KOL_MAP_EXT_TEXT})KOLEXT\"\n")

# The desktop builds' third-party notices (tools/make_licenses.py), built into
# the game for Options > Licenses (src/kol_desktop.cpp).
set(KOL_LICENSES_TXT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/NOTICES-desktop.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KOL_LICENSES_TXT}")
file(READ "${KOL_LICENSES_TXT}" KOL_LICENSES_TEXT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kol_licenses_baked.inc"
     "R\"KOLLIC(${KOL_LICENSES_TEXT})KOLLIC\"\n")

# The setup screen's typeface (src/kol_setup.cpp), built into the game: each
# .ttf as a list of bytes for an array's initialiser.
foreach(KOL_FONT Regular Bold)
    set(KOL_FONT_TTF "${CMAKE_CURRENT_SOURCE_DIR}/third_party/roboto/Roboto-${KOL_FONT}.ttf")
    set(KOL_FONT_INC "${CMAKE_CURRENT_BINARY_DIR}/kol_font_roboto_${KOL_FONT}.inc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KOL_FONT_TTF}")
    if(NOT EXISTS "${KOL_FONT_INC}" OR "${KOL_FONT_TTF}" IS_NEWER_THAN "${KOL_FONT_INC}")
        file(READ "${KOL_FONT_TTF}" KOL_FONT_HEX HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," KOL_FONT_HEX "${KOL_FONT_HEX}")
        file(WRITE "${KOL_FONT_INC}" "${KOL_FONT_HEX}\n")
    endif()
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos.cmake")
# RetroAchievements' HTTP: libcurl, but on Windows the system's WinHTTP
# (src/kol_cheevos_winhttp.cpp; tools/package_windows.sh).
if(NOT WIN32)
    find_package(CURL REQUIRED)
endif()

add_executable(KnightOfLodisRecomp
    src/main.cpp
    src/kol_desktop.cpp
    src/kol_setup.cpp
    $<$<PLATFORM_ID:Darwin>:${CMAKE_CURRENT_SOURCE_DIR}/src/kol_setup_mac.mm>
    src/kol_cheevos.cpp
    $<IF:$<PLATFORM_ID:Windows>,${CMAKE_CURRENT_SOURCE_DIR}/src/kol_cheevos_winhttp.cpp,${CMAKE_CURRENT_SOURCE_DIR}/src/kol_cheevos_curl.cpp>
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/src/kol_windows.rc>
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/src/kol_setup_win.cpp>
    src/kol_widescreen.cpp
    src/kol_obj_cull.cpp
    src/kol_focus.cpp
    src/kol_focus_atlas.cpp
    src/kol_focus_cutouts.cpp
    src/kol_focus_lamps.cpp
    src/kol_focus_models.cpp
    src/kol_focus_present.cpp
    src/kol_focus_queries.cpp
    src/kol_focus_roofs.cpp
    src/kol_focus_shadows.cpp
    src/kol_focus_sprites.cpp
    src/kol_focus_windows.cpp
    src/kol_perspective.cpp
    src/kol_workers.cpp
    src/kol_terrain_probe.cpp
    src/kol_highlight.cpp
    src/kol_unit_data.cpp
    src/kol_a7.cpp
    src/kol_sheet.cpp
    src/kol_unit_list.cpp
    src/kol_shop_buy.cpp
    src/kol_scene_strip.cpp
    src/kol_actions.cpp
    src/kol_fast_load.cpp
    src/kol_fast_actions.cpp
    src/kol_ghost.cpp
    src/kol_gulls.cpp
    src/kol_rain.cpp
    src/kol_snow.cpp
    src/kol_rumble.cpp
    src/kol_stick_cursor.cpp
    src/kol_auto_win.cpp
    src/kol_map_source.cpp
    src/kol_settings.cpp
    src/kol_sound.cpp
    src/kol_hd_audio.cpp
    src/kol_menu.cpp
    src/kol_states.cpp
    src/kol_vs_bot.cpp
    src/kol_vs_cpu.cpp
    src/kol_vs_net.cpp
    src/kol_tile_edit.cpp
    src/kol_edit_panel.cpp
    src/kol_map_settings.cpp
    ${KOL_GENERATED_SOURCES}
)
# Developer tools (src/kol_dev.h): OFF for a public build.
option(KOL_DEV_TOOLS "Build the developer tools (Debug page, height editor, dev logs)" ON)
target_compile_definitions(KnightOfLodisRecomp PRIVATE KOL_DEV_TOOLS=$<IF:$<BOOL:${KOL_DEV_TOOLS}>,1,0>)
target_include_directories(KnightOfLodisRecomp PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

# Defined by gbarecomp/CMakeLists.txt: links gbarecomp_armv4t, gbarecomp_gba,
# gbarecomp_runtime, gbarecomp_debug in the order/grouping each platform's
# linker needs (the four archives are mutually recursive).
target_link_gbarecomp_runtime_stack(KnightOfLodisRecomp)
target_link_libraries(KnightOfLodisRecomp PRIVATE kol_rcheevos)
if(WIN32)
    # WinHTTP (achievements), the Open dialog (the setup screen); a window
    # program, no console (-mwindows: gbarecomp's static SDL2 flags add it).
    target_link_libraries(KnightOfLodisRecomp PRIVATE winhttp comdlg32)
else()
    target_link_libraries(KnightOfLodisRecomp PRIVATE CURL::libcurl)
endif()
if(APPLE)
    # The setup screen's file picker (src/kol_setup_mac.mm).
    target_link_libraries(KnightOfLodisRecomp PRIVATE "-framework AppKit")
endif()

set_target_properties(KnightOfLodisRecomp PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES)
