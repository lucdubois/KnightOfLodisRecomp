# rcheevos (RetroAchievements' client library, MIT, third_party/rcheevos):
# v12.5.0, the parts rc_client needs. No hashing (our ROM's MD5 is fixed,
# src/kol_cheevos.cpp), no RAIntegration, no libretro helpers.
# Included by the desktop CMakeLists.txt and android/app/jni/CMakeLists.txt.
enable_language(C)
set(KOL_RCHEEVOS_DIR "${CMAKE_CURRENT_LIST_DIR}/rcheevos")
file(GLOB KOL_RCHEEVOS_SOURCES CONFIGURE_DEPENDS
    "${KOL_RCHEEVOS_DIR}/src/*.c"
    "${KOL_RCHEEVOS_DIR}/src/rapi/*.c"
    "${KOL_RCHEEVOS_DIR}/src/rcheevos/*.c"
    "${KOL_RCHEEVOS_DIR}/src/rhash/md5.c")
add_library(kol_rcheevos STATIC ${KOL_RCHEEVOS_SOURCES})
target_include_directories(kol_rcheevos PUBLIC "${KOL_RCHEEVOS_DIR}/include")
set_target_properties(kol_rcheevos PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
