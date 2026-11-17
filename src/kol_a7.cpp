// kol_a7.cpp — the game's asset decoder, transcribed from ROM 0x08004990.
// See the header. Verified against the game's own output: 327 of 327 entries
// the status sheet decodes, captured with a test-only hook on the routine.

#include "kol_a7.h"

namespace kol {
namespace {

constexpr std::uint32_t kRomBase = 0x08000000u;

struct Rom {
    const std::uint8_t* p;
    std::uint32_t size;
    bool ok(std::uint32_t addr, std::uint32_t len) const {
        return p && addr >= kRomBase && addr - kRomBase + len <= size;
    }
    std::uint8_t u8(std::uint32_t addr) const { return p[addr - kRomBase]; }
    std::uint16_t be16(std::uint32_t addr) const {
        return static_cast<std::uint16_t>((u8(addr) << 8) | u8(addr + 1));
    }
    std::uint32_t be32(std::uint32_t addr) const {
        return (static_cast<std::uint32_t>(be16(addr)) << 16) | be16(addr + 2);
    }
};

}  // namespace

bool a7_decode(const std::uint8_t* rom, std::uint32_t rom_size,
               std::uint32_t stream, std::uint32_t dict,
               std::vector<std::uint8_t>* out) {
    const Rom r{rom, rom_size};
    if (!out || !r.ok(dict, 4)) return false;
    const unsigned mode = r.u8(dict);
    const unsigned k = r.u8(dict + 1);
    if (k > 8) return false;
    std::uint32_t dp = dict + 2;
    std::uint32_t s = stream;
    unsigned size = 0;
    if (mode <= 2) { size = r.be16(dp); dp += 2; }
    std::uint16_t mask;
    if (mode == 0 || mode == 3) mask = static_cast<std::uint16_t>(0xFFFFu >> (k + 1));
    else if (mode == 1 || mode == 4) mask = static_cast<std::uint16_t>(0xFFFFu >> k);
    else return false;   // modes 2/5 are not transcribed; fail closed
    if (mode >= 3) {
        if (!r.ok(s, 2)) return false;
        size = r.be16(s);
        s += 2;
    }

    std::vector<std::uint8_t> o;
    o.reserve(size);
    auto copy_ref = [&](unsigned len, unsigned off) {
        if (!r.ok(dp + off, len)) return false;
        for (unsigned i = 0; i < len; ++i) o.push_back(r.u8(dp + off + i));
        return true;
    };
    if (mode == 0 || mode == 3) {
        // Control byte: bit 7 set is a literal run of (b & 0x7F) + 1 bytes;
        // clear is a dictionary reference, length in the top k of its low
        // seven bits (+3), offset in the rest of that byte and the next.
        while (o.size() < size) {
            if (!r.ok(s, 1)) return false;
            const unsigned b = r.u8(s);
            if (b & 0x80u) {
                const unsigned n = (b & 0x7Fu) + 1u;
                ++s;
                if (!r.ok(s, n)) return false;
                for (unsigned i = 0; i < n; ++i) o.push_back(r.u8(s + i));
                s += n;
            } else {
                if (!r.ok(s, 2)) return false;
                const unsigned len = (((b << 1) >> (8 - k)) & 0xFFFFu) + 3u;
                const unsigned off = r.be16(s) & mask;
                s += 2;
                if (!copy_ref(len, off)) return false;
            }
        }
    } else {
        // LZSS: a flag byte, most significant bit first, 1 = one literal
        // byte, 0 = a two-byte reference (length in the top k bits, +3).
        unsigned flags = 0;
        while (o.size() < size) {
            flags = (flags << 1) & 0xFFFFu;
            if ((flags & 0xFFu) == 0u) {
                if (!r.ok(s, 1)) return false;
                flags = (static_cast<unsigned>(r.u8(s)) << 8) | 0xFFu;
                ++s;
            }
            if (flags & 0x8000u) {
                if (!r.ok(s, 1)) return false;
                o.push_back(r.u8(s));
                ++s;
            } else {
                if (!r.ok(s, 2)) return false;
                const unsigned b = r.u8(s);
                const unsigned len = (b >> (8 - k)) + 3u;
                const unsigned off = r.be16(s) & mask;
                s += 2;
                if (!copy_ref(len, off)) return false;
            }
        }
    }
    // The guest treats running past the size as an error; so does this.
    if (o.size() != size) return false;
    *out = std::move(o);
    return true;
}

int a7_count(const std::uint8_t* rom, std::uint32_t rom_size,
             std::uint32_t archive) {
    const Rom r{rom, rom_size};
    if (!r.ok(archive, 8)) return -1;
    if (r.u8(archive) != 0x41u || r.u8(archive + 1) != 0x37u) return -1;
    const int n = r.be16(archive + 2);
    if (!r.ok(archive + 8, static_cast<std::uint32_t>(n) * 4u)) return -1;
    return n;
}

bool a7_entry(const std::uint8_t* rom, std::uint32_t rom_size,
              std::uint32_t archive, int index,
              std::vector<std::uint8_t>* out) {
    const int n = a7_count(rom, rom_size, archive);
    if (n < 0 || index < 0 || index >= n) return false;
    const Rom r{rom, rom_size};
    const std::uint32_t dict = archive + r.be32(archive + 4);
    const std::uint32_t stream =
        archive + r.be32(archive + 8 + static_cast<std::uint32_t>(index) * 4u);
    return a7_decode(rom, rom_size, stream, dict, out);
}

}  // namespace kol
