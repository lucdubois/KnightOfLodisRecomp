// kol_fast_load.cpp — see the header.
#include "kol_dev.h"
#include "kol_fast_load.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bios_hle.h"             // gba::bios_hle_service_decompress
#include "kol_a7.h"
#include "kol_settings.h"
#include "kol_unit_data.h"       // unit_data_rom
#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"          // g_cpu, g_bios_hle_hook, bus_*
#include "gba_bus.h"
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus
#include <algorithm>

extern "C" void runtime_pc_watch_set(std::uint32_t pc, void (*cb)(void*), void* ctx);
extern "C" void runtime_rw_record(int on, std::uint32_t depth);
extern "C" void runtime_rw_clear(void);
extern "C" const std::uint8_t* runtime_rw_reads(void);
extern "C" const std::uint8_t* runtime_rw_writes(void);
extern "C" std::size_t runtime_rw_bytes(void);
extern "C" std::uint32_t runtime_rw_addr(std::size_t k);
extern "C" int runtime_rw_io_touched(void);
extern "C" std::uint32_t g_irq_nest_depth;

namespace kol {
namespace {

// The game's asset decoder 0x08004990(stream, dict, out_count*) writes each
// byte at [0x030029F4] + [0x030029F0] when [0x030029F0] + 1 < [0x030029F8]
// (unsigned; the reader at 0x08004E2C sets the base, -1 and 0), then counts
// it; returns 1, and the bytes decoded to *out_count when it is not null.
constexpr std::uint32_t kA7Decode = 0x08004990u;
constexpr std::uint32_t kA7Count = 0x030029F0u, kA7Base = 0x030029F4u, kA7Limit = 0x030029F8u;
constexpr char kA7HookId[] = "kol.fast-load-a7";
// memset8(dst, count, value) at 0x08002F92: strb; r0++; r1--; while (r1 > 0)
// (signed), so at least one byte. Leaves r0 past the bytes, r1 = count - n,
// the flags of the last cmp r1, #0. Only clears of kMemsetMin bytes or more
// are taken: a loading screen's (47 and 53 KB on the way back from View
// List); the many small ones every frame (32 to 968 bytes) are left to the
// game, so its timing within a frame, and so its pictures, stay the same
// (sped up, one frame of FN showed a band of rows a game write reached
// earlier).
constexpr std::uint32_t kMemset = 0x08002F92u;
constexpr std::uint32_t kMemsetMin = 4096u;
constexpr char kMemsetHookId[] = "kol.fast-load-memset";

// ── Memoised pure routines ────────────────────────────────────────────────
// Three steps of the rebuild are pure functions of memory: they read EWRAM
// (and a little IWRAM and the ROM), write EWRAM (and their own stack frame),
// and touch no I/O, VRAM or DMA (measured with the runtime's read/write-set
// recorder, README). 0x080221B4, 16 frames, reads ~13 KB and writes ~12 KB
// (a per-cell table over the map's diamond); 0x0802714C, 3 frames, builds
// the height grid and the map-source arrays from 4 KB; 0x0805D6E4, 4 frames,
// 8 KB from 219 bytes of the units. Each is called from one place
// (0x080485xx), whose next instruction is a function entry of its own.
//   A run is recorded: the bytes it reads before writing them (its inputs,
// with their values at entry; the stack below its entry sp left out) and
// the bytes it writes (their values at return), plus r0-r3 and the flags at
// return. At a later call with the same r0-r3 and sp, if every input byte
// holds its recorded value the routine would run the same path and write
// the same bytes: they are written and the call returns at once. Else the
// call runs and is recorded. A recording that touched I/O is dropped.
//   The call sites: the battle screen's rebuild (0x080485xx: from View List)
// and its first build (0x080481xx, 0x08047Dxx; the units' table also from
// 0x0805FE3A, at every change of the deployment). Each site's next
// instruction is a function entry of its own, where a recording ends. A
// call from a site not listed runs as the game's.
struct MemoFn { std::uint32_t entry; const char* name; const char* id; };
constexpr MemoFn kMemoFns[] = {
    {0x080221B4u, "composer", "kol.fast-load-memo-composer"},
    {0x0802714Cu, "grid", "kol.fast-load-memo-grid"},
    {0x0805D6E4u, "units", "kol.fast-load-memo-units"},
};
constexpr int kMemoFnCount = 3;
struct MemoSite { int fn; std::uint32_t ret; const char* id; };
constexpr MemoSite kMemoSites[] = {
    {0, 0x0804862Cu, "kol.fast-load-memo-ret-0"}, {0, 0x08048198u, "kol.fast-load-memo-ret-1"},
    {1, 0x08048552u, "kol.fast-load-memo-ret-2"}, {1, 0x08048152u, "kol.fast-load-memo-ret-3"},
    {2, 0x08048658u, "kol.fast-load-memo-ret-4"}, {2, 0x08047D0Au, "kol.fast-load-memo-ret-5"},
    {2, 0x08047D1Cu, "kol.fast-load-memo-ret-6"}, {2, 0x0805FE3Au, "kol.fast-load-memo-ret-7"},
};
constexpr int kMemoSiteCount = 8;
constexpr std::size_t kMemoKeep = 64;
struct MemoByte { std::uint32_t addr; std::uint8_t value; };
struct MemoEntry {
    int fn;
    std::uint32_t args[4], sp;
    bool stack_dep;   // reads or writes the caller's stack: then sp must match too
    std::vector<MemoByte> in, out;
    std::uint32_t ret_r[4], ret_flags;
    unsigned long last_used;
};
std::vector<MemoEntry> g_memo;
unsigned long g_memo_clock = 0, g_memo_hits = 0, g_memo_misses = 0;
// Kept on disk (files/fast_load_memo.bin beside the settings; the desktop
// runner: KOL_FAST_LOAD_MEMO=<path>), so a map recorded once serves every
// later session: the first return in a battle hits too. A stale entry can
// only miss, never answer wrong, since its inputs are its key.
std::string g_memo_path;
bool g_memo_dirty = false;
long g_memo_dirty_frames = 0;
constexpr std::size_t kMemoMinOut = 2048;   // smaller results are not worth keeping
constexpr std::uint32_t kMemoMagic = 0x4D4C4F4Bu;   // 'KOLM'
constexpr std::uint32_t kMemoVersion = 1;
void memo_save() {
    if (g_memo_path.empty()) return;
    std::FILE* f = std::fopen((g_memo_path + ".tmp").c_str(), "wb");
    if (!f) return;
    auto w32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    w32(kMemoMagic); w32(kMemoVersion); w32(static_cast<std::uint32_t>(g_memo.size()));
    for (const MemoEntry& e : g_memo) {
        w32(static_cast<std::uint32_t>(e.fn));
        for (int i = 0; i < 4; ++i) w32(e.args[i]);
        w32(e.sp); w32(e.stack_dep ? 1u : 0u);
        for (int i = 0; i < 4; ++i) w32(e.ret_r[i]);
        w32(e.ret_flags);
        w32(static_cast<std::uint32_t>(e.in.size()));
        std::fwrite(e.in.data(), sizeof(MemoByte), e.in.size(), f);
        w32(static_cast<std::uint32_t>(e.out.size()));
        std::fwrite(e.out.data(), sizeof(MemoByte), e.out.size(), f);
    }
    std::fclose(f);
    std::rename((g_memo_path + ".tmp").c_str(), g_memo_path.c_str());
}
void memo_load() {
    if (g_memo_path.empty()) return;
    std::FILE* f = std::fopen(g_memo_path.c_str(), "rb");
    if (!f) return;
    auto r32 = [&](std::uint32_t* v) { return std::fread(v, 4, 1, f) == 1; };
    std::uint32_t magic = 0, version = 0, count = 0;
    bool ok = r32(&magic) && r32(&version) && r32(&count) && magic == kMemoMagic && version == kMemoVersion &&
              count <= 256;
    std::vector<MemoEntry> loaded;
    for (std::uint32_t n = 0; ok && n < count; ++n) {
        MemoEntry e;
        std::uint32_t fn = 0, dep = 0, nin = 0, nout = 0;
        ok = r32(&fn) && fn < static_cast<std::uint32_t>(kMemoFnCount);
        for (int i = 0; ok && i < 4; ++i) ok = r32(&e.args[i]);
        ok = ok && r32(&e.sp) && r32(&dep);
        for (int i = 0; ok && i < 4; ++i) ok = r32(&e.ret_r[i]);
        ok = ok && r32(&e.ret_flags) && r32(&nin) && nin <= 0x80000;
        if (ok) { e.in.resize(nin); ok = std::fread(e.in.data(), sizeof(MemoByte), nin, f) == nin; }
        ok = ok && r32(&nout) && nout <= 0x80000;
        if (ok) { e.out.resize(nout); ok = std::fread(e.out.data(), sizeof(MemoByte), nout, f) == nout; }
        if (!ok) break;
        e.fn = static_cast<int>(fn);
        e.stack_dep = dep != 0;
        e.last_used = 0;
        loaded.push_back(std::move(e));
    }
    std::fclose(f);
    if (ok) g_memo = std::move(loaded);
    std::fprintf(stderr, "fast_load: memo %s: %zu entries%s\n", g_memo_path.c_str(), g_memo.size(), ok ? "" : " (file not read)");
}
int g_rec_fn = -1, g_rec_site = -1;     // a recording in progress
std::uint32_t g_rec_sp = 0, g_rec_args[4];
std::vector<std::uint8_t> g_rec_snapshot;   // memory at entry, the recorder's layout
const MemoEntry* g_rec_expect = nullptr;    // check mode: the entry this run should reproduce

bool g_installed = false;
bool g_check = false;
// Check mode: what a hook would have written, compared once the game has
// done the same work.
struct Pending {
    const char* what;
    std::uint32_t at;
    std::vector<std::uint8_t> bytes;
    std::uint32_t count_at = 0, count = 0;   // A7: [kA7Count] expected
    std::uint32_t in0 = 0, in1 = 0;          // the routine's inputs, for the log
};
std::vector<Pending> g_pending;
unsigned long g_match = 0, g_mismatch = 0;
unsigned long g_runs[3] = {0, 0, 0};   // swi, a7, memset (served or checked)
void check_pending();
// The check at the moment the routine returns (the trace armed:
// GBARECOMP_INSN_TRACE=1), before the game can touch the result and after it
// has written all of it.
unsigned long long g_watch_cycles = 0;
std::uint32_t g_watch_at = 0;
void on_return(void*) {
    if (std::getenv("KOL_FAST_LOAD_WATCH"))
        std::fprintf(stderr, "fast_load watch: fired at %08X after %llu cycles\n", g_watch_at,
                     g_runtime_cycles - g_watch_cycles);
    check_pending();
}
void check_at_return(std::uint32_t pc) {
    g_watch_cycles = g_runtime_cycles;
    g_watch_at = pc & ~1u;
    runtime_pc_watch_set(pc & ~1u, &on_return, nullptr);
}

bool enabled() {
    return g_installed && setting_on(Setting::FastLoading) && !gbarecomp::runtime_excursion_active();
}

void check_pending() {
    for (const Pending& p : g_pending) {
        bool ok = true;
        std::size_t first = 0;
        for (std::size_t i = 0; i < p.bytes.size(); ++i)
            if (bus_read_u8(p.at + static_cast<std::uint32_t>(i)) != p.bytes[i]) { ok = false; first = i; break; }
        // The A7 count, unless the reader has since started over (it zeroes it).
        if (ok && p.count_at) {
            const std::uint32_t c = bus_read_u32(p.count_at);
            if (c >= p.count - p.bytes.size() && c != p.count) { ok = false; first = p.bytes.size(); }
        }
        if (ok) ++g_match;
        else {
            ++g_mismatch;
            std::fprintf(stderr, "fast_load check: %s at 0x%08X, %zu bytes: MISMATCH at +%zu (inputs %08X %08X)\n",
                         p.what, p.at, p.bytes.size(), first, p.in0, p.in1);
            if (std::getenv("KOL_FAST_LOAD_WATCH")) {
                std::fprintf(stderr, "   ours: ");
                for (std::size_t i = 0; i < p.bytes.size() && i < 64; ++i) std::fprintf(stderr, "%02X", p.bytes[i]);
                std::fprintf(stderr, "\n   game: ");
                for (std::size_t i = 0; i < p.bytes.size() && i < 64; ++i) std::fprintf(stderr, "%02X", bus_read_u8(p.at + static_cast<std::uint32_t>(i)));
                std::fprintf(stderr, "\n");
                static bool traced = false;
                if (!traced) {   // the game's own run of the routine, block by block
                    traced = true;
                    const std::uint32_t n = runtime_fp_save_tail_csv("fast_load_trace.csv", 4000);
                    std::fprintf(stderr, "   trace: fast_load_trace.csv (%u blocks)\n", n);
                }
                // Where in EWRAM/IWRAM the game put them, if anywhere.
                if (gba::GbaBus* bus = gbarecomp::active_bus()) {
                    const std::uint8_t* ew = bus->ewram_ptr();
                    const std::uint8_t* iw = bus->iwram_ptr();
                    const std::size_t n = std::min<std::size_t>(p.bytes.size(), 32);
                    for (std::size_t a = 0; a + n <= 0x40000; ++a)
                        if (std::memcmp(ew + a, p.bytes.data(), n) == 0) std::fprintf(stderr, "   found in EWRAM at %08zX\n", 0x02000000 + a);
                    for (std::size_t a = 0; a + n <= 0x8000; ++a)
                        if (std::memcmp(iw + a, p.bytes.data(), n) == 0) std::fprintf(stderr, "   found in IWRAM at %08zX\n", 0x03000000 + a);
                }
            }
        }
    }
    g_pending.clear();
}

// ── The BIOS decompressors ────────────────────────────────────────────────
int on_swi(std::uint32_t swi) {
    if (swi < 0x11u || swi > 0x13u || !enabled()) return 0;
    if (g_check) {
        // The destination's extent from the header (bytes), saved, written
        // natively, read back and put back; then the BIOS runs.
        const std::uint32_t src = g_cpu.R[0] & ~3u, dst = g_cpu.R[1];
        const std::uint32_t size = bus_read_u32(src) >> 8;
        if (size == 0 || size > 0x40000u) return 0;
        // Saved and put back as halfwords: a byte write to VRAM fills both
        // bytes of its halfword.
        const std::uint32_t a0 = dst & ~1u, a1 = (dst + size + 1u) & ~1u;
        std::vector<std::uint16_t> before((a1 - a0) / 2u);
        for (std::uint32_t a = a0; a < a1; a += 2u) before[(a - a0) / 2u] = bus_read_u16(a);
        const std::uint32_t r[4] = {g_cpu.R[0], g_cpu.R[1], g_cpu.R[2], g_cpu.R[3]};
        if (!gba::bios_hle_service_decompress(swi)) return 0;
        Pending p{swi == 0x13u ? "huffman" : "lz77", dst, std::vector<std::uint8_t>(size)};
        for (std::uint32_t i = 0; i < size; ++i) p.bytes[i] = bus_read_u8(dst + i);
        for (std::uint32_t a = a0; a < a1; a += 2u) bus_write_u16(a, before[(a - a0) / 2u]);
        for (int i = 0; i < 4; ++i) g_cpu.R[i] = r[i];
        g_pending.push_back(std::move(p));
        check_at_return(g_cpu.R[15]);   // the instruction after the swi
        ++g_runs[0];
        return 0;
    }
    if (!gba::bios_hle_service_decompress(swi)) return 0;   // a Huffman width it cannot do
    ++g_runs[0];
    return 1;
}

int (*g_prev_swi_hook)(std::uint32_t) = nullptr;

int on_swi_chained(std::uint32_t swi) {
    if (on_swi(swi)) return 1;
    return g_prev_swi_hook ? g_prev_swi_hook(swi) : 0;
}

// ── The game's A7 decoder ─────────────────────────────────────────────────
int on_a7(std::uint32_t, int, ArmCpuState* cpu) {
    if (!enabled()) return 0;
    std::uint32_t rom_size = 0;
    const std::uint8_t* rom = unit_data_rom(&rom_size);
    if (!rom) return 0;
    std::vector<std::uint8_t> out;
    if (!a7_decode(rom, rom_size, cpu->R[0], cpu->R[1], &out)) return 0;   // the game handles it
    const std::uint32_t base = bus_read_u32(kA7Base), limit = bus_read_u32(kA7Limit);
    std::uint32_t count = bus_read_u32(kA7Count);
    if (g_check) {
        Pending p{"a7", base + count, {}, kA7Count, count, cpu->R[0], cpu->R[1]};
        if (std::getenv("KOL_FAST_LOAD_WATCH"))
            std::fprintf(stderr, "fast_load a7: stream %08X dict %08X out %08X base %08X count %u limit %08X decoded %zu r2 %08X\n",
                         cpu->R[0], cpu->R[1], cpu->R[2], base, count, limit, out.size(), cpu->R[2]);
        for (std::uint8_t b : out) {
            if (p.count + 1u >= limit) break;
            p.bytes.push_back(b);
            ++p.count;
        }
        g_pending.push_back(std::move(p));
        check_at_return(cpu->R[14]);
        ++g_runs[1];
        return 0;
    }
    for (std::uint8_t b : out) {
        if (count + 1u >= limit) break;   // unsigned, as the routine compares
        bus_write_u8(base + count, b);
        ++count;
    }
    bus_write_u32(kA7Count, count);
    if (cpu->R[2]) bus_write_u32(cpu->R[2], static_cast<std::uint32_t>(out.size()));
    cpu->R[0] = 1u;
    cpu->R[15] = cpu->R[14] & ~1u;
    ++g_runs[1];
    return 1;
}

// ── memset8 ───────────────────────────────────────────────────────────────
int on_memset(std::uint32_t, int, ArmCpuState* cpu) {
    if (!enabled()) return 0;
    const std::uint32_t dst = cpu->R[0];
    const std::int32_t count = static_cast<std::int32_t>(cpu->R[1]);
    const std::uint8_t value = static_cast<std::uint8_t>(cpu->R[2]);
    const std::uint32_t n = count >= 1 ? static_cast<std::uint32_t>(count) : 1u;
    if (n < kMemsetMin) return 0;
    if (g_check) {
        g_pending.push_back({"memset", dst, std::vector<std::uint8_t>(n, value)});
        check_at_return(cpu->R[14]);
        ++g_runs[2];
        return 0;
    }
    for (std::uint32_t i = 0; i < n; ++i) bus_write_u8(dst + i, value);
    const std::int32_t left = count - static_cast<std::int32_t>(n);
    cpu->R[0] = dst + n;
    cpu->R[1] = static_cast<std::uint32_t>(left);
    // cmp r1, #0: N from the result, Z when zero, no borrow, no overflow.
    cpu->cpsr = (cpu->cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT)) |
                (left < 0 ? CPSR_N_BIT : 0u) | (left == 0 ? CPSR_Z_BIT : 0u) | CPSR_C_BIT;
    cpu->R[15] = cpu->R[14] & ~1u;
    ++g_runs[2];
    return 1;
}

void memo_snapshot() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::size_t n = runtime_rw_bytes();
    g_rec_snapshot.resize(n);
    if (!bus) return;
    std::memcpy(g_rec_snapshot.data(), bus->ewram_ptr(), 0x40000);
    std::memcpy(g_rec_snapshot.data() + 0x40000, bus->iwram_ptr(), 0x8000);
    std::memcpy(g_rec_snapshot.data() + 0x48000, bus->vram_ptr(), 0x18000);
    std::memcpy(g_rec_snapshot.data() + 0x60000, bus->pal_ptr(), 0x400);
    std::memcpy(g_rec_snapshot.data() + 0x60400, bus->oam_ptr(), 0x400);
}
bool memo_matches(const MemoEntry& e, int fn, const ArmCpuState* cpu) {
    if (e.fn != fn || (e.stack_dep && e.sp != cpu->R[13])) return false;
    for (int i = 0; i < 4; ++i)
        if (e.args[i] != cpu->R[i]) return false;
    for (const MemoByte& b : e.in)
        if (bus_read_u8(b.addr) != b.value) return false;
    return true;
}
void memo_apply(const MemoEntry& e, ArmCpuState* cpu, std::uint32_t ret) {
    for (const MemoByte& b : e.out) bus_write_u8(b.addr, b.value);
    for (int i = 0; i < 4; ++i) cpu->R[i] = e.ret_r[i];
    cpu->cpsr = (cpu->cpsr & 0x0FFFFFFFu) | e.ret_flags;
    cpu->R[15] = ret;
}
int memo_entry(int fn, ArmCpuState* cpu) {
    const MemoFn& f = kMemoFns[fn];
    if (!enabled() || g_rec_fn >= 0) return 0;
    int site = -1;
    for (int i = 0; i < kMemoSiteCount; ++i)
        if (kMemoSites[i].fn == fn && kMemoSites[i].ret == (cpu->R[14] & ~1u)) site = i;
    if (site < 0) {
        if (std::getenv("KOL_FAST_LOAD_WATCH"))
            std::fprintf(stderr, "fast_load: memo %s called from a site not listed (lr %08X); the game runs it\n", f.name, cpu->R[14]);
        return 0;
    }
    ++g_memo_clock;
    for (MemoEntry& e : g_memo)
        if (memo_matches(e, fn, cpu)) {
            e.last_used = g_memo_clock;
            if (!g_check) {
                ++g_memo_hits;
                memo_apply(e, cpu, kMemoSites[site].ret);
                if constexpr (kol::kDevTools)
                    std::fprintf(stderr, "fast_load: memo %s hit (%zu bytes written back)\n", f.name, e.out.size());
                return 1;
            }
            g_rec_expect = &e;   // run and record anyway; compare at the return
            break;
        }
    ++g_memo_misses;
    if (std::getenv("KOL_FAST_LOAD_WATCH"))
        for (const MemoEntry& e : g_memo) {
            if (e.fn != fn) continue;
            if (e.sp != cpu->R[13] || e.args[0] != cpu->R[0] || e.args[1] != cpu->R[1] || e.args[2] != cpu->R[2] || e.args[3] != cpu->R[3]) {
                std::fprintf(stderr, "fast_load: memo %s miss: sp/args differ (sp %08X vs %08X, r0 %08X vs %08X, r1 %08X vs %08X)\n",
                             f.name, e.sp, cpu->R[13], e.args[0], cpu->R[0], e.args[1], cpu->R[1]);
                continue;
            }
            std::size_t diff = 0;
            std::uint32_t first = 0, last = 0;
            for (const MemoByte& b : e.in)
                if (bus_read_u8(b.addr) != b.value) { if (!diff) first = b.addr; last = b.addr; ++diff; }
            std::fprintf(stderr, "fast_load: memo %s miss: %zu of %zu input bytes differ (%08X..%08X)\n", f.name, diff,
                         e.in.size(), first, last);
        }
    memo_snapshot();
    runtime_rw_clear();
    runtime_rw_record(1, g_irq_nest_depth);
    g_rec_fn = fn;
    g_rec_site = site;
    g_rec_sp = cpu->R[13];
    for (int i = 0; i < 4; ++i) g_rec_args[i] = cpu->R[i];
    return 0;
}
int memo_return(int site, ArmCpuState* cpu) {
    if (g_rec_site != site) return 0;
    const int fn = g_rec_fn;
    runtime_rw_record(0, 0);
    g_rec_fn = -1;
    g_rec_site = -1;
    const bool io = runtime_rw_io_touched() != 0;
    MemoEntry e;
    e.fn = fn;
    e.sp = g_rec_sp;
    e.stack_dep = false;
    for (int i = 0; i < 4; ++i) { e.args[i] = g_rec_args[i]; e.ret_r[i] = cpu->R[i]; }
    e.ret_flags = cpu->cpsr & 0xF0000000u;
    e.last_used = g_memo_clock;
    const std::uint8_t* rd = runtime_rw_reads();
    const std::uint8_t* wr = runtime_rw_writes();
    const std::size_t n = runtime_rw_bytes();
    for (std::size_t k = 0; k < n; ++k) {
        if (!rd[k] && !wr[k]) continue;
        const std::uint32_t a = runtime_rw_addr(k);
        if ((a >> 24) == 3u && a < g_rec_sp) continue;   // its own stack frame
        if ((a >> 24) == 3u && a >= 0x03007000u) e.stack_dep = true;   // the caller's
        if (rd[k]) e.in.push_back({a, g_rec_snapshot[k]});
        if (wr[k]) e.out.push_back({a, bus_read_u8(a)});
    }
    runtime_rw_clear();
    if (g_rec_expect) {   // check mode: the recorded entry against this run
        const MemoEntry& x = *g_rec_expect;
        g_rec_expect = nullptr;
        bool ok = x.out.size() == e.out.size() && x.in.size() == e.in.size();
        for (std::size_t i = 0; ok && i < e.out.size(); ++i)
            ok = x.out[i].addr == e.out[i].addr && x.out[i].value == e.out[i].value;
        for (int i = 0; ok && i < 4; ++i) ok = x.ret_r[i] == e.ret_r[i];
        ok = ok && x.ret_flags == e.ret_flags;
        if (ok) ++g_match;
        else {
            ++g_mismatch;
            std::fprintf(stderr, "fast_load check: memo %s: MISMATCH (recorded %zu in %zu out; this run %zu in %zu out)\n",
                         kMemoFns[fn].name, x.in.size(), x.out.size(), e.in.size(), e.out.size());
        }
        return 0;
    }
    if (io) {
        std::fprintf(stderr, "fast_load: memo %s touched I/O; not kept\n", kMemoFns[fn].name);
        return 0;
    }
    if (e.out.size() < kMemoMinOut) return 0;
    if (g_memo.size() >= kMemoKeep) {
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < g_memo.size(); ++i)
            if (g_memo[i].last_used < g_memo[oldest].last_used) oldest = i;
        g_memo.erase(g_memo.begin() + static_cast<long>(oldest));
    }
    if constexpr (kol::kDevTools)
        std::fprintf(stderr, "fast_load: memo %s recorded: %zu input bytes, %zu output bytes\n", kMemoFns[fn].name,
                     e.in.size(), e.out.size());
    g_memo.push_back(std::move(e));
    g_memo_dirty = true;   // saved from the frame hook, a few seconds on
    g_memo_dirty_frames = 0;
    return 0;
}
template <int F> int on_memo(std::uint32_t, int, ArmCpuState* cpu) { return memo_entry(F, cpu); }
template <int S> int on_memo_ret(std::uint32_t, int, ArmCpuState* cpu) { return memo_return(S, cpu); }

}  // namespace

void fast_load_install() {
    if (g_installed) return;
    const char* off = std::getenv("KOL_NO_FAST_LOAD");
    if (off && off[0] && off[0] != '0') return;
    const char* chk = std::getenv("KOL_FAST_LOAD_CHECK");
    g_check = chk && chk[0] && chk[0] != '0';
    if (g_check && !std::getenv("GBARECOMP_INSN_TRACE")) {
        std::fprintf(stderr, "fast_load: CHECK mode needs GBARECOMP_INSN_TRACE=1 (the compare runs at each return); off\n");
        g_check = false;
    }
    if (!gba_mod_register_function_entry_plugin(kA7HookId, kA7Decode, 1, &on_a7) ||
        !gba_mod_set_function_hook_enabled(kA7HookId, 1) ||
        !gba_mod_register_function_entry_plugin(kMemsetHookId, kMemset, 1, &on_memset) ||
        !gba_mod_set_function_hook_enabled(kMemsetHookId, 1)) {
        std::fprintf(stderr, "fast_load: hooks NOT installed\n");
        return;
    }
    using Handler = int (*)(std::uint32_t, int, ArmCpuState*);
    const Handler entries[kMemoFnCount] = {&on_memo<0>, &on_memo<1>, &on_memo<2>};
    const Handler returns[kMemoSiteCount] = {&on_memo_ret<0>, &on_memo_ret<1>, &on_memo_ret<2>, &on_memo_ret<3>,
                                             &on_memo_ret<4>, &on_memo_ret<5>, &on_memo_ret<6>, &on_memo_ret<7>};
    for (int i = 0; i < kMemoFnCount; ++i)
        if (!gba_mod_register_function_entry_plugin(kMemoFns[i].id, kMemoFns[i].entry, 1, entries[i]) ||
            !gba_mod_set_function_hook_enabled(kMemoFns[i].id, 1)) {
            std::fprintf(stderr, "fast_load: memo hooks NOT installed (%s)\n", kMemoFns[i].name);
            return;
        }
    for (int i = 0; i < kMemoSiteCount; ++i)
        if (!gba_mod_register_function_entry_plugin(kMemoSites[i].id, kMemoSites[i].ret, 1, returns[i]) ||
            !gba_mod_set_function_hook_enabled(kMemoSites[i].id, 1)) {
            std::fprintf(stderr, "fast_load: memo return hooks NOT installed (%08X)\n", kMemoSites[i].ret);
            return;
        }
    // A BIOS HLE hook already installed (--bios-hle, --no-bios) keeps every SWI
    // this one declines.
    g_prev_swi_hook = g_bios_hle_hook;
    g_bios_hle_hook = &on_swi_chained;
    g_installed = true;
    if (const char* e = std::getenv("KOL_FAST_LOAD_MEMO")) g_memo_path = e;
    // The memo keys on RAM only, not on the ROM it also reads: a mod (a
    // patched ROM, gbarecomp --rom-patch) keeps its own file.
    else if (!settings_dir().empty())
        g_memo_path = settings_dir() + "/fast_load_memo" + kol::rom_file_tag() + ".bin";
    memo_load();
    if (g_check) std::fprintf(stderr, "fast_load: CHECK mode (the game does the work; results compared%s)\n",
                              std::getenv("GBARECOMP_INSN_TRACE") ? " as each routine returns" : " later: set GBARECOMP_INSN_TRACE=1");
}

void fast_load_present() {
    if constexpr (!kol::kDevTools) return;   // a measuring log, not in a public build
    // Every hold, whatever the setting, for measuring loads on the device
    // ("hold:" in logcat): more than 3 game frames between two presents
    // (g_runtime_presented_frame, the guest frame of the last one), and the
    // real time between them, as this hook (each VBlank start) first sees each.
    static std::uint64_t last_present = 0;
    static auto last_t = std::chrono::steady_clock::now();
    const std::uint64_t p = gbarecomp::g_runtime_presented_frame;
    if (p == last_present) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_present && p > last_present + 3u && p - last_present < 100000u)
        std::fprintf(stderr, "hold: %llu frames (%llu..%llu), %.0f ms, fast loading %s, fast actions %s\n",
                     static_cast<unsigned long long>(p - last_present),
                     static_cast<unsigned long long>(last_present),
                     static_cast<unsigned long long>(p),
                     std::chrono::duration<double, std::milli>(now - last_t).count(),
                     setting_on(Setting::FastLoading) ? "on" : "off",
                     setting_on(Setting::FastActions) ? "on" : "off");
    last_present = p;
    last_t = now;
}

void fast_load_frame() {
    if (!g_installed) return;
    if (g_memo_dirty && ++g_memo_dirty_frames >= 300) {   // ~5 s after the last recording
        g_memo_dirty = false;
        memo_save();
    }
    if (g_check) {
        // (The compares run only as each routine returns: a present, and so
        // this hook, can fall between two blocks of a routine at IRQ depth 0,
        // and compared then the result is half written.)
        static unsigned long last = 0;
        const unsigned long runs = g_runs[0] + g_runs[1] + g_runs[2] + g_memo_hits + g_memo_misses;
        if (runs != last && (runs - last >= 50 || g_mismatch)) {
            last = runs;
            std::fprintf(stderr, "fast_load check: swi %lu a7 %lu memset %lu memo %lu/%lu: %lu match, %lu MISMATCH\n",
                         g_runs[0], g_runs[1], g_runs[2], g_memo_hits, g_memo_hits + g_memo_misses, g_match, g_mismatch);
        }
    }
}

}  // namespace kol
