#pragma once

// The KOL_* switches' reading rules, one function a rule (kol_focus.cpp,
// kol_perspective.cpp). Each switch stays a function-local static built
// from one call, with its comment where it is read: these only parse.
//
// The rules differ on purpose and are kept as they were:
//   env_set       set at all, "0" and "" too (the presence switches);
//   env_true      set and neither "" nor starting with '0';
//   env_not_zero  on unless set to something starting with '0';
//   env_num       atof of the value, "" reads 0; `fallback` when unset;
//   env_num_or    the same, but "" reads as unset (`fallback`).
// The _min and _clamp forms bound a value that is set and leave `fallback`
// as it is (a -1 "none" stays -1).

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace kol {

inline bool env_set(const char* name) { return std::getenv(name) != nullptr; }
inline bool env_true(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}
inline bool env_not_zero(const char* name) {
    const char* v = std::getenv(name);
    return !(v && v[0] == '0');
}
inline float env_num(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v ? static_cast<float>(std::atof(v)) : fallback;
}
inline float env_num_or(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}
inline float env_num_min(const char* name, float lo, float fallback) {
    const char* v = std::getenv(name);
    return v ? std::max(lo, static_cast<float>(std::atof(v))) : fallback;
}
inline float env_num_clamp(const char* name, float lo, float hi, float fallback) {
    const char* v = std::getenv(name);
    return v ? std::clamp(static_cast<float>(std::atof(v)), lo, hi) : fallback;
}
inline int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : fallback;
}
inline int env_int_clamp(const char* name, int lo, int hi, int fallback) {
    const char* v = std::getenv(name);
    return v ? std::clamp(std::atoi(v), lo, hi) : fallback;
}
// Each item of a ';'-separated list (KOL_MAP_LAMP, KOL_DOOR_LIGHT, ...):
// f(item), the item's text running on to the string's end (sscanf stops at
// its ';'). An empty item ends the list.
template <class F>
void env_items(const char* name, F f) {
    for (const char* p = std::getenv(name); p && *p;) {
        f(p);
        p = std::strchr(p, ';');
        if (p) ++p;
    }
}

}  // namespace kol
