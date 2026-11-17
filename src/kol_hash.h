#pragma once

// FNV-1a, the hash of the cache keys (kol_focus.cpp, kol_perspective.cpp).
// A key is only ever compared with itself, but each site keeps its own way
// of feeding a value in (its bias, scale and order), so the keys, and what
// the KOL_*_CACHE_CHECK modes compare, are as they were.

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace kol {

constexpr std::uint32_t kFnvBasis = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

// One value folded in.
inline void fnv_mix(std::uint32_t& h, std::uint32_t v) { h = (h ^ v) * kFnvPrime; }
// Places, sizes: each int with `bias` added (65536, or 4096), so a negative
// one stays apart from the positives.
inline void fnv_ints(std::uint32_t& h, std::initializer_list<int> vs, int bias = 65536) {
    for (int v : vs) fnv_mix(h, static_cast<std::uint32_t>(v + bias));
}
// A float in quarters, 7 added (exact: v * 4 rounds nothing, so it is the
// same fused or not).
inline void fnv_q4(std::uint32_t& h, float v) { fnv_mix(h, static_cast<std::uint32_t>(v * 4.0f + 7.0f)); }
// The bytes of a vector's (or an array's) elements.
template <class V>
std::uint32_t fnv_bytes(const V& v, std::uint32_t h = kFnvBasis) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(v.data());
    for (std::size_t i = 0; i < v.size() * sizeof(v[0]); ++i) h = (h ^ p[i]) * kFnvPrime;
    return h;
}
// The 64-bit form, with the xorshift the roof and BG1 caches add.
inline void fnv64x_mix(std::uint64_t& h, std::uint64_t v) {
    h = (h ^ v) * 1099511628211ull;
    h ^= h >> 29;
}

}  // namespace kol
