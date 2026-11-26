// kol_unit_data.cpp — see kol_unit_data.h.
//
// Every address here was found by watching what the guest actually READ while
// it drew a status window (GBARECOMP_READ_WATCH), not by pattern-matching the
// ROM. That matters: two plausible constant offsets fitted the first samples
// and were both wrong — equipment looked like `name_id = id + 40` and talents
// like `id + 234`, and both are really tables whose first entries happen to
// line up. Anchor on a read, not on an offset that fits two points.

#include "kol_unit_data.h"
#include "kol_actions.h"
#include "kol_sheet.h"
#include "kol_focus.h"
#include "kol_perspective.h"
#include "kol_widescreen.h"
#include "rom_patch.h"   // runtime_rom_patch_changed

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "runtime.h"
#include "runtime_arm.h"
#include "kol_region.h"
#include "kol_jtext.h"

namespace kol {
namespace {

constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kEwramEnd  = 0x02040000u;
constexpr std::uint32_t kHighlightedUnitPtr = usa(0x02028960u);
constexpr std::uint32_t kRomBase = 0x08000000u;

constexpr std::uint32_t kRecSize = 0x68u;

// Record offsets, all confirmed against the rendered window on eight units.
constexpr std::uint32_t kOffName   = 0x30u;
constexpr std::uint32_t kOffHp     = 0x40u;   // u16 hp, hpmax, mp, mpmax, str, int, agi
constexpr std::uint32_t kOffStr    = 0x48u;
constexpr std::uint32_t kOffClass  = 0x50u;
constexpr std::uint32_t kOffLevel  = 0x52u;
constexpr std::uint32_t kOffExp    = 0x53u;
constexpr std::uint32_t kOffEquip  = 0x60u;   // four item ids
constexpr std::uint32_t kOffTalent = 0x64u;   // four talent ids, 0xFF empty

// ROM tables. Each holds a u16 name-pool id at entry offset 0.
constexpr std::uint32_t kEmblemClassOf = usa(0x086148A0u);   // 20 bytes per kind (+0x4E)
constexpr std::uint32_t kClassTable  = usa(0x08615228u); constexpr std::uint32_t kClassStride  = 88u;
constexpr std::uint32_t kItemTable   = usa(0x08616F60u); constexpr std::uint32_t kItemStride   = 36u;
constexpr std::uint32_t kTalentTable = usa(0x0861A8F0u); constexpr std::uint32_t kTalentStride = 64u;
constexpr std::uint32_t kNamePtrs    = usa(0x08623678u); constexpr std::uint32_t kNameCount    = 489u;

// Class entry fields.
constexpr std::uint32_t kClsBaseId   = 0x09u;  // female entries are sparse and name the male one here
constexpr std::uint32_t kClsDmgMult  = 0x4Cu;  // damage TAKEN, percent
constexpr std::uint32_t kClsInnate   = 0x53u;  // unarmed attack; 5 for humans, 50/55 for beasts
constexpr std::uint32_t kClsWeaponPc = 0x54u;  // weapon power, percent
constexpr std::uint32_t kClsMagicPc  = 0x4Du;  // six elemental damage-taken %, to +0x52
constexpr std::uint32_t kClsFlags    = 0x18u;  // u16; 0x100 adds innate attack to DEF (beasts)

// Item entry fields.
constexpr std::uint32_t kItemKind = 0x06u;   // above 10 is armour
constexpr std::uint32_t kItemPhys = 0x0Cu;
constexpr std::uint32_t kItemSpll = 0x0Du;
constexpr std::uint32_t kItemDefPc = 0x16u;  // s8, added (halved) to the class dmg%
constexpr std::uint32_t kItemElemPc = 0x17u; // six s8, averaged in with the class's
constexpr std::uint32_t kItemStr  = 0x13u;
constexpr std::uint32_t kItemInt  = 0x14u;
constexpr std::uint32_t kItemAgi  = 0x15u;

// The `0x81 NN` bitmap escape: a table of (u16 tile offset, u16 cell count),
// and 4bpp pixels at 64 bytes per 8x16 character cell.
constexpr std::uint32_t kEscTable = usa_only(0x0860D088u);
constexpr std::uint32_t kEscPixels = usa_only(0x086A2AB0u);
constexpr int kMaxCells = 24;
constexpr int kCellW = 8, kCellH = 16;

const std::uint8_t* g_rom = nullptr;
std::uint32_t g_rom_size = 0;

bool rom_ok(std::uint32_t addr, std::uint32_t len) {
    if (!g_rom || addr < kRomBase) return false;
    const std::uint32_t off = addr - kRomBase;
    return off + len <= g_rom_size;
}
std::uint8_t rom8(std::uint32_t addr) {
    return rom_ok(addr, 1) ? g_rom[addr - kRomBase] : 0u;
}
std::uint16_t rom16(std::uint32_t addr) {
    if (!rom_ok(addr, 2)) return 0u;
    const std::uint32_t o = addr - kRomBase;
    return static_cast<std::uint16_t>(g_rom[o] | (g_rom[o + 1] << 8));
}
std::uint32_t rom32(std::uint32_t addr) {
    if (!rom_ok(addr, 4)) return 0u;
    const std::uint32_t o = addr - kRomBase;
    return static_cast<std::uint32_t>(g_rom[o] | (g_rom[o + 1] << 8) |
                                      (g_rom[o + 2] << 16) | (g_rom[o + 3] << 24));
}

// Female class entries carry only a name and kClsBaseId; the real stats live
// in the male entry it names. A sparse entry has a zero weapon power, which is
// how one is recognised without hardcoding which ids are which.
std::uint32_t resolve_class(std::uint32_t cls) {
    if (cls > 255u) return cls;
    const std::uint32_t e = kClassTable + cls * kClassStride;
    const std::uint8_t base = rom8(e + kClsBaseId);
    if (rom8(e + kClsWeaponPc) == 0u && base != 0u && base != cls && base < 256u)
        return base;
    return cls;
}

// Decode one pool string. Text is 'A'-'Z' = 0x00.., 'a'-'z' = 0x1A.., 0x89 is a
// space and 0xFF terminates. A leading 0x81 makes the whole entry a bitmap.
// ── Names the cartridge stores as pixels ────────────────────────────────
// 193 of the 489 pool strings begin with the `0x81 NN` escape, which makes
// the whole entry a pre-rendered strip and leaves NO text form in the ROM at
// all. The panel therefore showed a weapon called "Short Bow" at full size
// beside an armour drawn as a tiny two-line image.
//
// This table is those names, transcribed from the strips themselves. They
// were rasterised straight out of `kEscPixels` and read: 105 of the 188
// referenced strips are two 8px lines stacked in one 16px cell run, the other
// 83 are a single 16px line, and telling them apart is whether rows 7-8 are
// blank. Whole words were read rather than glyphs matched, because the font
// has 147 distinct 8px shapes with real ambiguity between I/l/1 and O/0/D,
// and English item names are self-correcting in context where single glyphs
// are not.
//
// Transcription, so treat a wrong name as a transcription error and check the
// strip before assuming the lookup is broken:
// `tools/dump_name_strips.py totkol.gba <dir>` regenerates the sheets these
// were read from. Nothing decodes pixels at runtime — this is a static array
// and one index. An id with no entry keeps the old behaviour and is drawn as
// its strip, so this fails soft.
const char* const kStripText[] = {
    "Firedrake Sword",   // 0
    "Sword of Tiamat",   // 1
    "Dragon Gem Sword",   // 2
    "Peridot Sword",   // 3
    "Needle of Light",   // 4
    "Earth Dragon Axe",   // 5
    "Bloody Cleaver",   // 6
    "Hammer of Tears",   // 7
    "Mystic Hammer",   // 8
    "Hyacinth Fan",   // 9
    "Gypsy Queen",   // 10
    "Scipplay's Staff",   // 11
    "Ripple's Staff",   // 12
    "Earth Javelin",   // 13
    "Osric's Spear",   // 14
    "Leviathan Mail",   // 15
    "Leather Whip",   // 16
    "Rapture Rose",   // 17
    "Thunder Bow",   // 18
    "Sandstorm Bow",   // 19
    "Composite Bow",   // 20
    "Tower Shield",   // 21
    "Dragon Shield",   // 22
    "Thunder Shield",   // 23
    "Flame Shield",   // 24
    "Saint's Shield",   // 25
    "Chocolate Shield",   // 26
    "Hard Leather",   // 27
    "Thunder Chain",   // 28
    "Flame Leather",   // 29
    "Earth Leather",   // 30
    "Saint's Garb",   // 31
    "Cursed Garment",   // 32
    "Heavy Armor",   // 33
    "Peregrine Mail",   // 34
    "Phoenix Mail",   // 35
    "Nathalork Mail",   // 36
    "Dragon Armor",   // 37
    "Grincer Coat",   // 38
    "Candy Armor",   // 39
    "Robe of the Wise",   // 40
    "Cloak of Oath",   // 41
    "Robe of Abyss",   // 42
    "Pure-White Dress",   // 43
    "Cloak of Authority",   // 44
    "Leather Hat",   // 45
    "Plumed Headband",   // 46
    "Sherwood Hat",   // 47
    "Winged Shoes",   // 48
    "Circlet of Wisdom",   // 49
    "Greasy Boots",   // 50
    "Forest Boots",   // 51
    "Ring of Flight",   // 52
    "Ring of Flotation",   // 53
    "Armlet of Agility",   // 54
    "Firedrake Ring",   // 55
    "Dragon Eyes",   // 56
    "Ring of the Dead",   // 57
    "Necklace of Resist",   // 58
    "Pearl Necklace",   // 59
    "Cassowary Feather",   // 60
    "Glass Pumpkin",   // 61
    "Sacrificial Doll",   // 62
    "Sacred Stone of Bliss",   // 63
    "Transferring Stone",   // 64
    "Healing Leaf",   // 65
    "Healing Seed",   // 66
    "Healing Salve",   // 67
    "Healing Essence",   // 68
    "Magic Leaf",   // 69
    "Magic Seed",   // 70
    "Magic Salve",   // 71
    "Magic Essence",   // 72
    "Revive Stone",   // 73
    "Savage Bugle",   // 74
    nullptr,   // 75
    "Reincarnation",   // 76
    "Sword Emblem",   // 77
    "Crown of Intellect",   // 78
    "Stone of Swiftness",   // 79
    "Tome of Discipline",   // 80
    "Urn of Chaos",   // 81
    "Mirror of the Gods",   // 82
    "Sorcerer's Cup",   // 83
    "Altar of Resurrection",   // 84
    "Dragon Tamer",   // 85
    "Beast Tamer",   // 86
    "Angel Knight",   // 87
    "Thunder Dragon",   // 88
    "Red Dragon",   // 89
    "Earth Dragon",   // 90
    "Blue Dragon",   // 91
    "Dragon Zombie",   // 92
    "Undead Soldier",   // 93
    "Undead Wizard",   // 94
    "Undead Knight",   // 95
    "Dark Stalker",   // 96
    "Hell Gigantes",   // 97
    nullptr,   // 98
    "??? (Unknown)",   // 99
    "Fallen Angel",   // 100
    "Sacred Demon",   // 101
    nullptr,   // 102
    "Ignis Fatuus",   // 103
    "Lightning Bow",   // 104
    "Thunder Arrow",   // 105
    "Magic Missile",   // 106
    nullptr,   // 107
    "Shuriken Barrage",   // 108
    "Pelting Fury",   // 109
    "Summon Darkness",   // 110
    "Thunder Flare",   // 111
    nullptr,   // 112
    "Magic Barrage",   // 113
    "Summon Golem",   // 114
    "Ray of Paralysis",   // 115
    "Molten Blade",   // 116
    "Petrifying Cloud",   // 117
    "Slumber Mist",   // 118
    "Poison Squall",   // 119
    nullptr,   // 120
    "Resurrection",   // 121
    nullptr,   // 122
    "Poignant Melody",   // 123
    "Fairy's Embrace",   // 124
    "Energy Transfer",   // 125
    "Maximum Heal",   // 126
    "Maximum Charge",   // 127
    "Maximum Restore",   // 128
    "Reincarnation",   // 129
    "Sage's Wisdom",   // 130
    "Enlightenment",   // 131
    "Summon Element",   // 132
    "Summon Tempest",   // 133
    "Harnella's Influence",   // 134
    "Zoshonel's Influence",   // 135
    "Berthe's Influence",   // 136
    "Grueza's Influence",   // 137
    "Thunder Breath",   // 138
    "Poison Breath",   // 139
    "Rotten Breath",   // 140
    "Petrifying Breath",   // 141
    "Strangling Tentacles",   // 142
    "Swallow's Daze",   // 143
    "Day of Reckoning",   // 144
    "Transmigration",   // 145
    "Dragon's Scale",   // 146
    "Animal Hunter",   // 147
    "The Pen and the Sword",   // 148
    "Gibe of Fallen Angel",   // 149
    "Philosopher's Stone",   // 150
    "Self Preservation",   // 151
    nullptr,   // 152
    "Vixen's Whisper",   // 153
    "Broken Heart",   // 154
    "Knight's Certificate",   // 155
    "Book of Initiation",   // 156
    "Heavenly Spirit",   // 157
    "Embodiment of Desires",   // 158
    "Archangel's Feather",   // 159
    "The Cycle of Life",   // 160
    "Mark of the Elite",   // 161
    "Lucky Soldier",   // 162
    "Mark of Valor",   // 163
    "Veteran Soldier",   // 164
    "Relix's Emblem",   // 165
    "Ripple's Emblem",   // 166
    "Grozz Nuy's Emblem",   // 167
    "Ostorea South",   // 168
    "Ostorea West",   // 169
    "Throne Room",   // 170
    "Angel's Headstone",   // 171
    "Hall of Conviction",   // 172
    "Garden of Memories",   // 173
    nullptr,   // 174
    "Seraph's Plume",   // 175
    "Sombra",   // 176
    "Branca",   // 177
    "Trueno",   // 178
    "Trevas",   // 179
    "Oceano",   // 180
    "The End of Summer",   // 181
    "After the Sunset",   // 182
    "The Last Battle",   // 183
    "Numida Chronology",   // 184
    "Epic of Tinea",   // 185
    "Cave Exploration",   // 186
    "Batraal Chronology",   // 187
    "Tundra Geology",   // 188
    "Southern Cross",   // 189
    "Armlet of Wisdom",   // 190
    nullptr,   // 191
    nullptr,   // 192
    nullptr,   // 193
    nullptr,   // 194
    nullptr,   // 195
    nullptr,   // 196
    nullptr,   // 197
    nullptr,   // 198
    nullptr,   // 199
    nullptr,   // 200
    nullptr,   // 201
    nullptr,   // 202
    "Sherwood Bow",   // 203
    "Earth Shield",   // 204
    "Divine Radiance",   // 205
    "Cursed Existence",   // 206
    "Hall of Corruption",   // 207
    "Lesser Daemon",   // 208
};
constexpr int kStripTextCount =
    static_cast<int>(sizeof kStripText / sizeof kStripText[0]);

// Strip `id`'s transcription, or null. A mod (a ROM patch) that redrew the
// strip or moved it gets none: the strip is then drawn from its own pixels.
const char* strip_text(int id) {
    if (id < 0 || id >= kStripTextCount || !kStripText[id]) return nullptr;
    if (g_runtime_rom_patch_name) {
        const std::uint32_t e = kEscTable + static_cast<std::uint32_t>(id) * 4u;
        if (runtime_rom_patch_changed(e, 4u)) return nullptr;
        const std::uint32_t cells = rom16(e + 2u);
        if (runtime_rom_patch_changed(kEscPixels + rom16(e) * 32u, cells * 64u)) return nullptr;
    }
    return kStripText[id];
}

void read_name(std::uint16_t name_id, UnitName* out) {
    *out = UnitName{};
    if (name_id >= kNameCount) return;
    // The Japanese ROM: where usa() puts kNamePtrs are its name records, 8
    // codes each, zero padded, the same ids (kol_jtext.h). No strips.
    if (kRegion == Region::Japan) {
        const std::uint32_t rec = kNamePtrs + name_id * 16u;
        if (!rom_ok(rec, 16)) return;
        int len = 8;
        while (len > 0 && rom16(rec + static_cast<std::uint32_t>(len - 1) * 2u) == 0u) --len;
        for (int i = 0; i < len; ++i) out->codes[out->ncodes++] = rom16(rec + static_cast<std::uint32_t>(i) * 2u);
        jtext::show(out->codes, out->ncodes, out->text, static_cast<int>(sizeof out->text));
        return;
    }
    const std::uint32_t ptr = rom32(kNamePtrs + name_id * 4u);
    if (!rom_ok(ptr, 1)) return;
    std::size_t n = 0;
    for (std::uint32_t a = ptr; n + 1 < sizeof out->text; ++a) {
        const std::uint8_t b = rom8(a);
        if (b == 0xFFu) break;
        if (b == 0x81u) {                 // a pre-rendered strip, not text
            const int id = rom8(a + 1u);
            out->strip = id;
            if (const char* t = strip_text(id)) {
                std::snprintf(out->text, sizeof out->text, "%s", t);
                out->bitmap = -1;         // text now exists; draw it as text
            } else {
                out->bitmap = id;         // unknown strip: fall back to pixels
                out->text[0] = '\0';
            }
            return;
        }
        if (out->ncodes < static_cast<int>(sizeof out->codes / sizeof out->codes[0]))
            out->codes[out->ncodes++] = b;
        char c = '?';
        if (b < 0x1Au) c = static_cast<char>('A' + b);
        else if (b < 0x34u) c = static_cast<char>('a' + (b - 0x1Au));
        else if (b == 0x89u) c = ' ';
        out->text[n++] = c;
    }
    out->text[n] = '\0';
}

void table_name(std::uint32_t table, std::uint32_t stride, std::uint32_t idx,
                UnitName* out) {
    read_name(rom16(table + idx * stride), out);
}

// An item as the game's accessor 0x081437AC reads it. Ids up to 0xC7 are the
// ROM table. Above that is a MADE item: a 28-byte RAM record at 0x02000D30 +
// (id - 0xC8) * 28, or at 0x02001C60 on side 1 (made_side1()), holding
// its name (+0x00, codes up to 0xFF), STR/INT/AGI (u16 +0x10/+0x12/+0x14),
// ROM +0x0B's field (+0x16), its support type (+0x17), and the item it was
// made from (+0x18), which supplies every other field, its picture included.
constexpr std::uint32_t kMadeItems = usa(0x02000D30u);
constexpr std::uint32_t kMadeItemsOther = usa(0x02001C60u);   // accessor side 1 (0x081437E2)
constexpr std::uint32_t kMadeStride = 28u;
constexpr unsigned kMaxRomItem = 0xC7u;
struct Item {
    bool ok = false;
    std::uint32_t rom = 0;    // ROM record: the item's, or a made item's base's
    std::uint32_t made = 0;   // the made item's RAM record, else 0
    unsigned base = 0;        // the ROM id; also the picture index (field 30)
    int str = 0, intel = 0, agi = 0;
    unsigned type = 0;        // support type (field 15)
};
// The side the game passes for a unit's own items. The status window, its
// HELP texts and the ATK/DEF routines (0x08139D1C, 0x08139FCC, 0x0802AFAE,
// 0x0803DD96) pass record +0x5D bit 5; the support gauge's routines
// (0x0813AAB4, 0x0813B44C, 0x0813B618) pass 1 for an owner with bit 7, the
// other team, instead. No state here has bit 5 set; enemies carrying a made
// item (BS, CH: 0xC9) read an empty side-1 record, as the game's gauge does.
bool made_side1(std::uint8_t b5d) { return (b5d & 0x20u) != 0u; }
bool made_side1_gauge(std::uint8_t b5d) { return (b5d & 0x80u) != 0u; }
Item item_at(const std::uint8_t* ew, std::uint32_t esz, unsigned id, bool side1) {
    Item it;
    if (id <= kMaxRomItem) {
        it.base = id;
        it.rom = kItemTable + id * kItemStride;
        it.str = rom8(it.rom + kItemStr);
        it.intel = rom8(it.rom + kItemInt);
        it.agi = rom8(it.rom + kItemAgi);
        it.type = rom8(it.rom + 0x12u);
        it.ok = true;
        return it;
    }
    const std::uint32_t a = (side1 ? kMadeItemsOther : kMadeItems) +
                            (id - (kMaxRomItem + 1u)) * kMadeStride;
    if (!ew || a - kEwramBase + kMadeStride > esz) return it;
    const std::uint8_t* m = ew + (a - kEwramBase);
    it.made = a;
    it.base = m[0x18];
    if (it.base > kMaxRomItem) return it;
    it.rom = kItemTable + it.base * kItemStride;
    it.str = m[0x10] | (m[0x11] << 8);
    it.intel = m[0x12] | (m[0x13] << 8);
    it.agi = m[0x14] | (m[0x15] << 8);
    it.type = m[0x17];
    it.ok = true;
    return it;
}
// A made item's name, from its RAM record, in the same codes as a unit's.
void made_name(const std::uint8_t* m, UnitName* out) {
    *out = UnitName{};
    if (kRegion == Region::Japan) {   // as a unit's there: 8 codes, zero padded (not yet seen in a game)
        int len = 8;
        while (len > 0 && (m[(len - 1) * 2] | (m[(len - 1) * 2 + 1] << 8)) == 0) --len;
        for (int i = 0; i < len; ++i)
            out->codes[out->ncodes++] = static_cast<font::Code>(m[i * 2] | (m[i * 2 + 1] << 8));
        jtext::show(out->codes, out->ncodes, out->text, static_cast<int>(sizeof out->text));
        return;
    }
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < 16u; ++i) {
        const std::uint8_t b = m[i];
        if (b == 0xFFu) break;
        out->codes[out->ncodes++] = b;
        char c = '?';
        if (b < 0x1Au) c = static_cast<char>('A' + b);
        else if (b < 0x34u) c = static_cast<char>('a' + (b - 0x1Au));
        else if (b == 0x89u) c = ' ';
        out->text[n++] = c;
    }
    out->text[n] = '\0';
}
// An equipment slot's name as the window shows it.
void item_name(const std::uint8_t* ew, const Item& it, unsigned id, UnitName* out) {
    if (it.made) made_name(ew + (it.made - kEwramBase), out);
    else table_name(kItemTable, kItemStride, id, out);
}

// The unit record the cursor names, or 0. Fails closed.
std::uint32_t highlighted_record(const std::uint8_t* ew, std::uint32_t esz) {
    const std::uint32_t off = kHighlightedUnitPtr - kEwramBase;
    if (!ew || off + 4u > esz) return 0u;
    const std::uint32_t rec = static_cast<std::uint32_t>(
        ew[off] | (ew[off + 1] << 8) | (ew[off + 2] << 16) | (ew[off + 3] << 24));
    if (rec < kEwramBase || rec + kRecSize > kEwramEnd) return 0u;
    if ((rec - kEwramBase) + kRecSize > esz) return 0u;
    return rec;
}

// ── The support/fear gauge ──────────────────────────────────────────────
// A transcription of 0x0813B7C8 and the routines it calls; see README "The
// gauge under the portrait". Allies within Manhattan distance 3 add support
// through the type codes their class and gear carry; enemies within 3 take it
// away through the "fear" type 0x1C; the sum is clamped to [-2, +2].
namespace gauge {

constexpr std::uint32_t kRoster = usa(0x02001420u);
constexpr std::uint32_t kEntries = kRoster + 0x680u;   // battle entries
constexpr std::uint32_t kEntryStride = 0x1Cu;          // one per record index
constexpr std::uint32_t kSideRecords = 8u * 0x68u;     // 0x340
constexpr std::uint32_t kSideEntries = 8u * 0x1Cu;     // 0xE0
constexpr std::uint32_t kGaugeOff = kRoster + 0xDA4u;  // nonzero: no gauge
constexpr std::uint32_t kStoryFlags = usa(0x020011CCu);     // bit 0: gauge applies (0x08142F50(0))
constexpr std::uint8_t kFear = 0x1C;                   // enemy type: -1
constexpr std::uint8_t kFearless = 0x21;               // unit type: immune

struct Ctx {
    const std::uint8_t* ew;
    std::uint32_t size;
    bool known = true;
    bool ok(std::uint32_t a, std::uint32_t n) const {
        return a >= kEwramBase && a - kEwramBase + n <= size;
    }
    std::uint8_t u8(std::uint32_t a) const { return ok(a, 1) ? ew[a - kEwramBase] : 0; }
    std::uint16_t u16(std::uint32_t a) const {
        return static_cast<std::uint16_t>(u8(a) | (u8(a + 1) << 8));
    }
    std::uint32_t u32(std::uint32_t a) const {
        return static_cast<std::uint32_t>(u16(a)) | (static_cast<std::uint32_t>(u16(a + 2)) << 16);
    }
};

// Class field with the accessor's fallback (0x0813FA44): a zero field is
// looked up again on the second class byte when +0x09 is 0xFF, else on the
// base class +0x09 names, else it is 0.
unsigned class_field(unsigned c1, unsigned c2, unsigned off, bool wide, int depth = 0) {
    if (c1 > 255u || depth > 4) return 0;
    const std::uint32_t e = kClassTable + c1 * kClassStride;
    const unsigned v = wide ? rom16(e + off) : rom8(e + off);
    if (v != 0) return v;
    const unsigned base = rom8(e + 0x09u);
    if (base == 0xFFu) return c2 != c1 ? class_field(c2, c2, off, wide, depth + 1) : 0;
    if (base != 0u && base != c1) return class_field(base, c2, off, wide, depth + 1);
    return 0;
}
// The same walk for a u32 field, as 0x0813FA44 reads the emblem mask (+0x04).
std::uint32_t class_word(unsigned c1, unsigned c2, unsigned off, int depth = 0) {
    if (c1 > 255u || depth > 4) return 0;
    const std::uint32_t e = kClassTable + c1 * kClassStride;
    const std::uint32_t v = rom32(e + off);
    if (v != 0) return v;
    const unsigned base = rom8(e + 0x09u);
    if (base == 0xFFu) return c2 != c1 ? class_word(c2, c2, off, depth + 1) : 0;
    if (base != 0u && base != c1) return class_word(base, c2, off, depth + 1);
    return 0;
}
unsigned cls8(const Ctx& c, std::uint32_t rec, unsigned off) {
    return class_field(c.u8(rec + 0x4Fu), c.u8(rec + 0x50u), off, false);
}
unsigned cls16(const Ctx& c, std::uint32_t rec, unsigned off) {
    return class_field(c.u8(rec + 0x4Fu), c.u8(rec + 0x50u), off, true);
}
// The item's support type: ROM +0x12, or a made item's own +0x17, read on
// the side the gauge passes for the item's owner.
unsigned item_type(Ctx& c, unsigned id, std::uint32_t owner) {
    const Item it = item_at(c.ew, c.size, id, made_side1_gauge(c.u8(owner + 0x5Du)));
    if (!it.ok) { c.known = false; return 0; }
    return it.type;
}
// 0x08139818: all four of one particular set of equipment.
bool full_set(const Ctx& c, std::uint32_t rec) {
    unsigned bits = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned id = c.u8(rec + kOffEquip + i);
        if (id == 0x53u) bits |= 1u;
        else if (id == 0x6Cu) bits |= 2u;
        else if (id == 0x83u) bits |= 4u;
        else if (id == 0x98u) bits |= 8u;
    }
    return bits == 0xFu;
}
// 0x0813B37C: what one support type is worth to this unit (0 or 1).
int weight(const Ctx& c, unsigned type, std::uint32_t unit) {
    switch (type) {
        case 0x19: return (cls8(c, unit, 0x0Au) == 6u || full_set(c, unit)) ? 1 : 0;
        case 0x1A: return cls8(c, unit, 0x0Au) == 5u ? 1 : 0;
        case 0x1D: return c.u8(unit + 0x5Eu) == 1u ? 1 : 0;
        case 0x1E: return (cls16(c, unit, 0x18u) & 8u) ? 1 : 0;
        case 0x1F: return 1;
        default:   return 0;
    }
}
// 0x0813B44C: one ally's support for the unit.
int ally(Ctx& c, std::uint32_t unit, std::uint32_t a) {
    int s = weight(c, cls8(c, a, 0x38u), unit) + weight(c, cls8(c, a, 0x39u), unit);
    for (unsigned i = 0; i < 4; ++i)
        s += weight(c, item_type(c, c.u8(a + kOffEquip + i), a), unit);
    const std::uint32_t f = c.u32(a + 0x08u);
    if (f & 0x2000000u) ++s;
    if ((f & 0x400000u) && (cls16(c, unit, 0x18u) & 8u)) ++s;
    if (full_set(c, a) && (cls8(c, unit, 0x0Au) == 6u || full_set(c, unit))) ++s;
    return s;
}
// 0x0813AAB4: whether the unit carries a type, in its class or its gear.
bool has_type(Ctx& c, std::uint32_t unit, unsigned type) {
    if (cls8(c, unit, 0x38u) == type || cls8(c, unit, 0x39u) == type) return true;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned id = c.u8(unit + kOffEquip + i);
        if (id != 0u && item_type(c, id, unit) == type) return true;
    }
    return (c.u32(unit + 0x08u) & 0x100u) && type == 0x0Eu;
}
// 0x0813B618: one enemy's fear, against a unit that is not immune to it.
int enemy(Ctx& c, std::uint32_t unit, std::uint32_t e) {
    if (has_type(c, unit, kFearless)) return 0;
    int s = 0;
    if (cls8(c, e, 0x38u) == kFear) --s;
    if (cls8(c, e, 0x39u) == kFear) --s;
    for (unsigned i = 0; i < 4; ++i)
        if (item_type(c, c.u8(e + kOffEquip + i), e) == kFear) --s;
    const std::uint32_t f = c.u32(e + 0x08u);
    if (f & 1u) --s;
    if ((f & 8u) && (cls16(c, unit, 0x18u) & 8u)) --s;
    return s;
}
bool present(const Ctx& c, std::uint32_t rec) {
    return c.u8(rec + 0x4Eu) != 0u && c.u16(rec + kOffHp) != 0u;
}

// Where a unit stands now: the cell of its map object (X = cx*32+16,
// Z = cy*32+16; list [0x0202A238], 26 x 0xD0, record at +0). The battle
// entry's +0x14/+0x15 keep the cell the unit's turn began on until its
// action is over (S_move_highlight: Alphonse walks 5,5 -> 7,5 and the entry
// shows 7,5 for one frame, then 5,5 again), while the game's own gauge
// follows the walk: in CO Ivanna's entry says 15,11, she stands on 13,10,
// and the unit bar shows +2, which is the gauge at 13,10 (+1 at 15,11).
// Falls back to the entry for a unit with no object.
void cell(const Ctx& c, std::uint32_t rec, std::uint32_t entry, int* x, int* y) {
    constexpr std::uint32_t kObjects = usa(0x0202A238u);
    const std::uint32_t list = c.u32(kObjects);
    for (int i = 0; list && i < 26; ++i) {
        const std::uint32_t o = list + static_cast<std::uint32_t>(i) * 0xD0u;
        if (!c.ok(o, 0x18) || c.u32(o) != rec) continue;
        const int ox = c.u16(o + 0x10u), oz = c.u16(o + 0x14u);
        if (ox < 16 || oz < 16) break;
        *x = (ox - 16) / 32;
        *y = (oz - 16) / 32;
        return;
    }
    *x = c.u8(entry + 0x14u);
    *y = c.u8(entry + 0x15u);
}

// The persuasion chance, 0x0813E30C(actor, actor entry, target, target
// entry), as the confirm box's "% HIT" shows it (0x080709AE): 0 for a
// leader, a unit not on the field (+0x4E > 1), a class without +0x18 bit 1,
// an entry with +0x0C bits 3-7, or a battle of type 0x30/0x31 (+0x13BC);
// else 0, 80, 50 or 5 as the actor class's +0x1C, +0x1A or +0x1B matches the
// target class's +0x1D, +30 with the actor's +0x08 bit 10, -50 each with its
// bit 1 against a target class kind (+0x0A) 6 and bit 2 against 5, clamped
// to 0-100, then that share of the HP the target has lost (rounded toward
// zero, as 0x08159B1C divides). State 0036: Allos on Sandy at 73/148, 25.
int persuade_chance(const Ctx& c, std::uint32_t actor, std::uint32_t target, std::uint32_t target_entry) {
    const unsigned t1 = c.u8(target + 0x4Fu), t2 = c.u8(target + 0x50u);
    const unsigned a1 = c.u8(actor + 0x4Fu), a2 = c.u8(actor + 0x50u);
    const unsigned kind = class_field(t1, t2, 0x1Du, false);
    if (static_cast<std::uint8_t>(c.u8(usa(0x020013BCu)) - 0x30u) <= 1u) return 0;
    if ((c.u8(target_entry + 0x17u) & 1u) || c.u8(target + 0x4Eu) > 1u) return 0;
    if (!(class_field(t1, t2, 0x18u, true) & 2u) || (c.u8(target_entry + 0x0Cu) & 0xF8u)) return 0;
    int chance = 5;
    if (class_field(a1, a2, 0x1Cu, false) == kind) chance = 0;
    else if (class_field(a1, a2, 0x1Au, false) == kind) chance = 80;
    else if (class_field(a1, a2, 0x1Bu, false) == kind) chance = 50;
    const std::uint32_t flags = c.u32(actor + 0x08u);
    if (flags & 0x400u) chance += 30;
    if ((flags & 2u) && class_field(t1, t2, 0x0Au, false) == 6u) chance -= 50;
    if ((flags & 4u) && class_field(t1, t2, 0x0Au, false) == 5u) chance -= 50;
    chance = std::clamp(chance, 0, 100);
    const int hp = c.u16(target + 0x40u), hp_max = c.u16(target + 0x42u);
    if (hp_max == 0) return 0;
    return std::clamp((hp_max - hp) * chance / hp_max, 0, 100);
}

}  // namespace gauge

// ── Portraits (0x0814308C for the layers, 0x08143170 for the palettes) ──
namespace portrait {
constexpr std::uint32_t kMode = usa(0x020012E0u);        // == 4 enables a variant rule
constexpr std::uint32_t kVariants = usa(0x0200122Cu);    // story variant per character
constexpr std::uint32_t kCharTable = usa(0x086148A0u);   // 20 bytes per character
constexpr std::uint32_t kPortraitRecs = usa(0x08613C5Cu);   // 20 bytes per portrait
}  // namespace portrait

void unit_portrait(const gauge::Ctx& c, std::uint32_t rec, int tiles[2], int pal[2]) {
    using namespace portrait;
    tiles[0] = tiles[1] = pal[0] = pal[1] = 0;
    const unsigned who = c.u8(rec + 0x4Eu);
    const unsigned side_bits = c.u8(rec + 0x5Du);
    const bool other = (side_bits & 0x80u) != 0;
    if (who <= 1u) {
        // A generic unit: the class's own portrait, one layer. Class fields
        // 0x0A/0x0B/0x0C are u16 +0x10 (portrait), +0x12 / +0x14 (palette
        // for the player's side / the other side), with the usual fallback.
        tiles[0] = static_cast<int>(gauge::cls16(c, rec, 0x10u));
        pal[0] = static_cast<int>(gauge::cls16(c, rec, other ? 0x14u : 0x12u));
        return;
    }
    // A named character: a story-dependent variant picks its portrait.
    unsigned variant = c.u8(kVariants + who);
    if (c.u8(kMode) == 4u) {
        const bool flag14 = (c.u8(gauge::kStoryFlags + 2u) & 0x10u) != 0;   // story flag 0x14
        const bool odd = (side_bits & 1u) != 0;
        if (flag14 ? (odd && other) : (odd && !other)) variant = 5u;
    }
    const unsigned idx = rom8(kCharTable + 0x0Bu + who * 20u + variant);
    const std::uint32_t p = kPortraitRecs + idx * 20u;
    tiles[0] = rom16(p + 0x08u);
    tiles[1] = rom16(p + 0x0Eu);
    pal[0] = rom16(p + (other ? 0x0Cu : 0x0Au));
    pal[1] = rom16(p + (other ? 0x12u : 0x10u));
}

// 0x0813B7C8 for the unit whose record is `unit`.
bool support_gauge(const std::uint8_t* ew, std::uint32_t esz, std::uint32_t unit,
                   int* out) {
    using namespace gauge;
    Ctx c{ew, esz};
    if (unit < kRoster || (unit - kRoster) % kRecSize != 0u) return false;
    const std::uint32_t idx = (unit - kRoster) / kRecSize;
    if (idx >= 16u) return false;
    *out = 0;
    // In record state +0x4E == 0x18 the sheet draws all five bars instead of
    // one (0x0803E64A); not modelled, so reported unknown.
    if (c.u8(unit + 0x4Eu) == 0x18u) return false;
    if (!(c.u8(kStoryFlags) & 1u)) return true;
    if (!present(c, unit) || c.u8(kGaugeOff) != 0u) return true;
    if (static_cast<std::int32_t>(c.u32(unit + 0x08u)) < 0) { *out = 2; return true; }
    const unsigned side = c.u8(unit + 0x5Du) >> 7;
    const std::uint32_t me = kEntries + idx * kEntryStride;
    int x = 0, y = 0;
    cell(c, unit, me, &x, &y);
    int sum = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const unsigned s = pass == 0 ? side : 1u - side;
        for (unsigned j = 0; j < 8; ++j) {
            const std::uint32_t r = kRoster + s * kSideRecords + j * kRecSize;
            const std::uint32_t e = kEntries + s * kSideEntries + j * kEntryStride;
            if (!present(c, r)) continue;
            // Itself. The game compares entry cells here; with live cells
            // the unit's own entry may lie elsewhere, so it goes by record.
            if (r == unit) continue;
            int ex = 0, ey = 0;
            cell(c, r, e, &ex, &ey);
            const int dx = ex - x, dy = ey - y;
            if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) > 3) continue;
            sum += pass == 0 ? ally(c, unit, r) : enemy(c, unit, r);
        }
    }
    if (sum > 2) sum = 2;
    if (sum < -2) sum = -2;
    *out = sum;
    return c.known;
}

// ── The item under the status window's cursor, and its HELP text ────────
// What SELECT would describe, found the way the game finds it; see README
// "Item descriptions on the second screen". The window's control block is
// [0x02002B1C], naming its unit at +0x5854.
//
// With the equipment list open (sub-state +0x61A4 == 0x1D, or 0x1F for the
// spellbook list) the item is the list's (0x08046C20): the list object [ctl+0x582C] is an item list when its
// kind (+0x19) is 0, 2 or 4, and the item under the hand is byte 0 of entry
// [+0x1C] + 4 * list[+0x38 + scroll + cursor], where scroll and cursor are
// list[+0x20 + tab] and list[+0x28 + tab] for tab list[+0x30 + list[+0x1A]]
// (0x08046EB0, 0x08046F04, 0x08046F78). Otherwise it is the cursor's row, 12
// bytes at [ctl+0x6188] + 12 * u16[ctl+0x618E], whose (id, kind) the help
// builder 0x0802ABE0 switches on: kind 5 is the equipment slot the row index
// names (0x0802AF38), kind 6 the slot its id names (0x0802AF70); kinds 7 and
// 8 are the talent slots the same way (0x0802AFD4, 0x0802B00C). A list of
// kind 1 or 3 holds talents, and a talent's help id is its record's +0x06.
// The other kinds are transcribed below; kind 0, 3 and 4 rows are their own
// help id. See README "Every HELP text on the second screen".
constexpr std::uint32_t kWinCtl = usa(0x02002B1Cu);
constexpr std::uint32_t kCtlUnit = usa_ctl(0x5854u);
constexpr std::uint32_t kCtlList = usa_ctl(0x582Cu);
constexpr std::uint32_t kCtlRows = usa_ctl(0x6188u);
constexpr std::uint32_t kCtlEmblems = usa_ctl(0x6198u);  // the EMBLEM page's earned set
constexpr std::uint32_t kCtlRow = usa_ctl(0x618Eu);
constexpr std::uint32_t kCtlSub = usa_ctl(0x61A4u);
constexpr unsigned kSubList = 0x1Du;       // the equipment list
constexpr unsigned kSubTalentList = 0x1Fu; // the spellbook list
constexpr unsigned kSubClassChange = 0x21u; // the class-change grid
// Organize's R menu screens: the army's item stock, spellbook stock and
// class list (0x080402E0(4 / 9 / 15, 4)), their lists the same list object.
constexpr unsigned kSubItemStock = 0x04u, kSubSpellStock = 0x09u, kSubClassList = 0x0Fu;
constexpr std::uint32_t kCtlClassCursor = usa_ctl(0x58C3u);  // its highlighted class
// The help text banks (0x08015D50 is handed one and an index): ids up to
// 0x1FF in the first, the rest in the second, less 0x200 (0x08046DB8). An
// entry is at bank + u16[bank + 2 * index]; two header bytes, then the text.
constexpr std::uint32_t kHelpBank = usa_only(0x0874B6DCu);
constexpr std::uint32_t kHelpBank2 = usa_only(0x0875976Cu);
// The Japanese ROM has them as one "m7" bank (kol_jtext.h) under the same
// ids, 696 of them: 0x298 is "this emblem has not been earned" in both, and
// the numbers, figures and character names of 629 entries agree outright
// (regionmap/jhelp.py check; the rest word a number differently).
constexpr std::uint32_t kHelpBankJapan = japan_only(0x08737B78u);
// The NAVI prompts, same layout. Entry 0x22 is the move choice's "Choose where
// you want to move this character." (fetched at 0x080662C4).
constexpr std::uint32_t kNaviBank = usa(0x0875D99Au);
constexpr unsigned kNaviMoveChoice = 0x22u;
constexpr int kNaviHelpItem = 0x3000 + static_cast<int>(kNaviMoveChoice);
// The prompt's card shows once per move choice, for at most this long; so
// does a message the move choice put up.
constexpr double kNaviShowSeconds = 4.0;
constexpr int kMessageHelpItem = 0x4000;
// u32 per character; in the Japanese ROM a record of 8 codes, zero padded.
constexpr std::uint32_t kCharNames = usa(0x086254D8u);

// The unit field accessor 0x08143B70 reads plain record bytes for every field
// used here: 0x10 +0x4E, 0x11 +0x4F (class), 0x12 +0x50 (second class), 0x16
// +0x54 (element), 0x17 +0x55 (alignment), 0x18/0x19 +0x56/+0x57 (birth month
// and day), 0x1A/0x1B +0x58/+0x59, 0x1C u16 +0x5A, 0x1D +0x5C, 0x1E +0x5D, 0x1F +0x5E (sex).
// Field 0x28 is the battle entry's signed +3, 0 in the roster.
constexpr std::uint32_t kCtlEntry = usa_ctl(0x5860u);   // battle entry, 0 in the roster
constexpr std::uint32_t kCtlBattle = usa_ctl(0x6195u);  // 1 in battle
constexpr std::uint32_t kCtlHelp = usa_ctl(0x61A6u);    // 1 SELECT's cursor, 3 a box up
constexpr unsigned kSubEquipSlots = 0x1Cu, kSubTalentSlots = 0x1Eu;
constexpr std::uint32_t kStrTable = usa(0x08623678u);  // u32 string pointers
constexpr std::uint32_t kMoveTable = usa(0x0861C8C4u); // movement types, 8 bytes
constexpr std::uint32_t kMonthDays = usa(0x0861C8B4u);
constexpr std::uint32_t kStoryFlags = usa(0x020011CCu);

// What the cursor is on: an item id, a talent id, another help id, or
// nothing (all 0). `fig` are the figures a `0x80 NN` code names.
struct CursorThing {
    unsigned item = 0, talent = 0, help = 0;
    font::Code fig[3][24] = {};
    int fig_len[3] = {0, 0, 0};
};

// Story flag n (0x08142F50).
bool story_flag(const gauge::Ctx& c, unsigned n) {
    return ((c.u8(kStoryFlags + n / 8u) >> (n % 8u)) & 1u) != 0u;
}

// An item's field 0x1D (0x081437AC): +0x20 of its ROM record, or of a made
// item's base. Side 1 reads made items from 0x02001C60 instead of 0x02000D30.
unsigned item_field_1d(const gauge::Ctx& c, unsigned id, bool side1) {
    unsigned base = id;
    if (id > kMaxRomItem)
        base = c.u8((side1 ? usa(0x02001C60u) : kMadeItems) +
                    ((id + 0x38u) & 0xFFu) * kMadeStride + 0x18u);
    return base <= kMaxRomItem ? rom8(kItemTable + base * kItemStride + 0x20u) : 0u;
}

// The unit's movement-type record (0x08139B74): the class's type (class
// field 0x22, +0x2D), stepped through the table by each equipped item's
// field 0x1D, then by the battle entry's +3 and a snowfield rule
// (0x08020BF8: story flag 0x122 without 0x119, a unit whose +0x4E is not 13
// or 14, and IWRAM 0x03001029 clear). +0 is the land type (1-5), +1 the
// water type, +2 set for a snow-footed variant.
std::uint32_t move_record(const gauge::Ctx& c, std::uint32_t unit, std::uint32_t entry) {
    const bool side1 = (c.u8(unit + 0x5Du) & 0x20u) != 0u;
    int mt = static_cast<int>(gauge::class_field(c.u8(unit + 0x4Fu), c.u8(unit + 0x50u), 0x2Du, false));
    for (unsigned i = 0; i < 4u; ++i) {
        const unsigned id = c.u8(unit + kOffEquip + i);
        if (id == 0u) continue;
        const unsigned t = item_field_1d(c, id, side1);
        if (t >= 1u && t <= 4u) mt = rom8(kMoveTable + mt * 8u + 2u + t);
    }
    if (c.u8(unit + 0x5Du) & 1u) {
        const int v = c.ok(entry, 4) ? static_cast<std::int8_t>(c.u8(entry + 3u)) : 0;
        if (v > 3) { if (mt > 40) mt -= 20; else if (mt > 30) mt -= 10; }
        else if (v >= 1) { if (mt > 30) mt -= 10; }
        else if (v <= -1 && v >= -3) { if (mt >= 21 && mt <= 40) mt += 10; }
        else if (v <= -4) {
            if (mt >= 21 && mt <= 30) mt += 20;
            else if (mt >= 31 && mt <= 40) mt += 10;
        }
    }
    const unsigned kind = c.u8(unit + 0x4Eu);
    if (story_flag(c, 0x122u) && !story_flag(c, 0x119u) &&
        static_cast<std::uint8_t>(kind - 13u) > 1u && bus_read_u8(usa(0x03001029u)) == 0u) {
        if (mt <= 10) mt += 20;
        else if (mt <= 20) mt += 10;
    }
    return kMoveTable + static_cast<std::uint32_t>(mt) * 8u;
}

// A unit's battle entry: the one for its record index in the battle roster,
// 0 for a record outside it (the army roster). The status window's own
// (control block +0x5860) is the same one.
std::uint32_t battle_entry(std::uint32_t unit) {
    if (unit < gauge::kRoster || (unit - gauge::kRoster) % kRecSize != 0u) return 0u;
    const std::uint32_t idx = (unit - gauge::kRoster) / kRecSize;
    return idx < 16u ? gauge::kEntries + idx * gauge::kEntryStride : 0u;
}

// Persuasion (the move task 0x0202A5B0 in its state 0x2E, the command's
// task 0x080893DC with its block at task +0x1A0: +0 the actor's {record,
// entry}): the chance on `target`, -1 when not persuading or not a battle
// unit.
int persuade_on(std::uint32_t target) {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew) return -1;
    const gauge::Ctx c{ew, esz};
    if (c.u8(usa(0x0202A5B0u) + 0x27Eu) != 0x2Eu) return -1;
    const std::uint32_t block = c.u32(usa(0x0202A5B0u) + 0x1A0u);
    const std::uint32_t actor = c.u32(c.u32(block));
    const std::uint32_t entry = battle_entry(target);
    if (!battle_entry(actor) || !entry || actor == target) return -1;
    // Its own side: the game refuses before any chance (message 0x3B, 0x08089356).
    if ((c.u8(actor + 0x5Du) & 0x80u) == (c.u8(target + 0x5Du) & 0x80u)) return -1;
    return gauge::persuade_chance(c, actor, target, entry);
}

// An item's field 8 (0x081437AC): s8 ROM +0x0B, or a made item's byte +0x16
// (side 1 reads made items from 0x02001C60).
int item_field_8(const gauge::Ctx& c, unsigned id, bool side1) {
    if (id <= kMaxRomItem) return static_cast<std::int8_t>(rom8(kItemTable + id * kItemStride + 0x0Bu));
    return c.u8((side1 ? usa(0x02001C60u) : kMadeItems) + ((id + 0x38u) & 0xFFu) * kMadeStride + 0x16u);
}

// The unit's speed x10, as the status window prints it after MOVE
// (0x08139920). In 16.16 fixed point:
//   class field 0x21 (+0x2C, with the accessor's fallback)
// + 2 ln(unit field 0x0F + 80) - 9.1, field 0x0F being u16 +0x4C, plus 50
//   when the battle entry's +0x0E is set; ln from the table at 0x085F5EC4
//   (0x08008090), 0x0FFF6E667 the constant
// - the equipment's field 8, summed, / 60
// + 2 with exactly the four items 0x4E, 0x6D, 0x84, 0x8C (0x0813989C)
// + a step on the entry's signed +3 when +0x5D bit 0 is set: +2 above 3, +1
//   for 1-3, -1 for -3 to -1, -2 below that
// then times 2^-16 and 10 as doubles, truncated, clamped to 30-119.
int unit_speed(const gauge::Ctx& c, std::uint32_t unit, std::uint32_t entry) {
    const bool side1 = (c.u8(unit + 0x5Du) & 0x20u) != 0u;
    std::int32_t v = static_cast<std::int32_t>(
        gauge::class_field(c.u8(unit + 0x4Fu), c.u8(unit + 0x50u), 0x2Cu, false) << 16);
    unsigned f = c.u16(unit + 0x4Cu);
    if (entry && c.ok(entry, 0x10u) && c.u8(entry + 0x0Eu) != 0u) f += 50u;
    const std::int32_t ln = static_cast<std::int32_t>(rom32(usa(0x085F5EC4u) + ((f + 80u) & 0xFFFFu) * 4u));
    v += ln * 2 + static_cast<std::int32_t>(0xFFF6E667u);
    int weight = 0;
    unsigned set = 0;
    for (unsigned i = 0; i < 4u; ++i) {
        const unsigned id = c.u8(unit + kOffEquip + i);
        if (id == 0x4Eu) set |= 1u;
        else if (id == 0x6Du) set |= 2u;
        else if (id == 0x84u) set |= 4u;
        else if (id == 0x8Cu) set |= 8u;
        if (id != 0u) weight += item_field_8(c, id, side1);
    }
    v -= static_cast<std::int32_t>(static_cast<std::uint32_t>(weight) << 16) / 60;
    if (set == 0xFu) v += 0x20000;
    if (c.u8(unit + 0x5Du) & 1u) {
        const int t = entry && c.ok(entry, 4) ? static_cast<std::int8_t>(c.u8(entry + 3u)) : 0;
        v += t > 3 ? 0x20000 : t >= 1 ? 0x10000 : t == 0 ? 0 : t >= -3 ? -0x10000 : -0x20000;
    }
    const int s = static_cast<int>(static_cast<double>(v) * (1.0 / 65536.0) * 10.0);
    return s < 30 ? 30 : s > 119 ? 119 : s;
}

// The campaign's day count (0x0813AEB4), from the month and day at
// 0x0200118B/C and the month lengths at 0x0861C8B4.
int day_count(const gauge::Ctx& c) {
    const int m = c.u8(usa(0x0200118Bu)), d = c.u8(usa(0x0200118Cu));
    auto len = [](int i) { return static_cast<int>(rom8(kMonthDays + static_cast<std::uint32_t>(i))); };
    int s10 = 0;
    for (int i = 5; i <= 14; ++i) s10 += len(i);
    if (m <= 4 || (m == 5 && d <= 24)) {
        int r = s10 - 24 + len(4);
        for (int i = 0; i < m - 1; ++i) r += len(i);
        return r + d;
    }
    if (m == 5) return d - 25;
    int r = d;
    for (int i = 0; i < m - 6; ++i) r += len(5 + i);
    return r - 24 + len(4);
}

// Which of the 36 fortunes the birthday row names (0x081406AC): a cycle of
// +0x59 days, phased by u16 +0x5A, in four bands, moved on by +0x58 and +0x5C.
unsigned fortune(const gauge::Ctx& c, std::uint32_t unit) {
    const int period = c.u8(unit + 0x59u);
    const int x = period ? (static_cast<std::int16_t>(c.u16(unit + 0x5Au) + day_count(c)) * 100 / period) % 100 : 0;
    unsigned f = x <= 13 ? 0u : x <= 36 ? 9u : x <= 63 ? 18u : x <= 86 ? 27u : 0u;
    const unsigned a = c.u8(unit + 0x58u), b = c.u8(unit + 0x5Cu);
    if (a > 5u) f += a <= 10u ? 3u : 6u;
    if (b > 40u) f += b <= 59u ? 1u : 2u;
    return f;
}

// An ASCII string in the text codes, for the strips kStripText names.
int codes_from_ascii(const char* s, font::Code* out, int cap) {
    int n = 0;
    if (kRegion != Region::Usa) {   // that font's own codes
        for (; *s && n < cap; ++s)
            if (const font::Code c = font::ascii(*s); c != font::kNone) out[n++] = c;
        return n;
    }
    for (; *s && n < cap; ++s) {
        const char ch = *s;
        if (ch >= 'A' && ch <= 'Z') out[n++] = static_cast<font::Code>(ch - 'A');
        else if (ch >= 'a' && ch <= 'z') out[n++] = static_cast<font::Code>(0x1A + ch - 'a');
        else if (ch >= '0' && ch <= '9') out[n++] = static_cast<font::Code>(0x34 + ch - '0');
        else if (ch == '\'') out[n++] = 0x46u;
        else if (ch == ' ') out[n++] = 0x89u;
    }
    return n;
}

// A record of 8 codes, zero padded, as the Japanese ROM keeps its names
// (kol_jtext.h): the codes it holds.
int japan_record(std::uint32_t rec, font::Code* out, int cap) {
    if (!rom_ok(rec, 16)) return 0;
    int len = 8;
    while (len > 0 && rom16(rec + static_cast<std::uint32_t>(len - 1) * 2u) == 0u) --len;
    int n = 0;
    for (int i = 0; i < len && n < cap; ++i) out[n++] = rom16(rec + static_cast<std::uint32_t>(i) * 2u);
    return n;
}

// String `i` of the table at 0x08623678, as codes; a `0x81 NN` strip becomes
// its transcription.
int table_string(unsigned i, font::Code* out, int cap) {
    // The Japanese ROM: the name records, under the same ids (0x19E on the
    // months, 0x1AE and 0x1AF the sexes: read off the font).
    if (kRegion == Region::Japan) return japan_record(kStrTable + i * 16u, out, cap);
    std::uint32_t a = rom32(kStrTable + i * 4u);
    int n = 0;
    for (int k = 0; k < 24 && n < cap && rom_ok(a, 1); ++k) {
        const std::uint8_t b = rom8(a++);
        if (b == 0xFFu) break;
        if (b == 0x81u) {
            const unsigned id = rom8(a);
            if (const char* t = strip_text(static_cast<int>(id)))
                n += codes_from_ascii(t, out + n, cap - n);
            break;
        }
        out[n++] = b;
    }
    return n;
}

// The birthday row (case 2 of 0x0802ABE0): text 434 + the fortune, or 502
// with no birthday; figures 0-2 are the month's name, the day and the sex.
void birthday(const gauge::Ctx& c, std::uint32_t unit, CursorThing* t) {
    const unsigned month = c.u8(unit + 0x56u), day = c.u8(unit + 0x57u);
    const unsigned sex = c.u8(unit + 0x5Eu);
    t->help = (month == 0u && day == 0u) ? 502u : 434u + fortune(c, unit);
    t->fig_len[0] = table_string(0x19Eu + month, t->fig[0], 24);
    char num[8];
    std::snprintf(num, sizeof num, "%u", day);
    t->fig_len[1] = codes_from_ascii(num, t->fig[1], 24);
    if (sex == 0u) {
        // "--" (0x0860D045). The Japanese ROM has no such string where the
        // USA one does: two of its dashes (not seen in that game yet).
        t->fig[2][0] = t->fig[2][1] = kRegion == Region::Usa ? font::Code{0x4Cu} : font::ascii('-');
        t->fig_len[2] = 2;
    } else {
        t->fig_len[2] = table_string(0x1ADu + sex, t->fig[2], 24);
    }
}

CursorThing cursor_thing(const gauge::Ctx& c, std::uint32_t unit) {
    CursorThing t;
    const std::uint32_t ctl = c.u32(kWinCtl);
    if (!c.ok(ctl, kCtlSub + 4u) || c.u32(ctl + kCtlUnit) != unit) return t;
    const unsigned sub = c.u8(ctl + kCtlSub);
    if (sub == kSubClassChange) {
        // The class-change grid: the class under its cursor, whose help is
        // class +0x30 as the class row's is (a class with none falls back
        // through +0x09, as the female variants do to the male ones).
        const unsigned cls = c.u8(ctl + kCtlClassCursor);
        if (cls) t.help = gauge::class_field(cls, cls, 0x30u, true);
        return t;
    }
    if (sub == kSubList || sub == kSubTalentList) {
        const std::uint32_t l = c.u32(ctl + kCtlList);
        if (!c.ok(l, 0x60u) || c.u8(l + 0x13u) == 0u) return t;
        const unsigned kind = c.u8(l + 0x19u);
        const unsigned tab = c.u8(l + 0x30u + c.u8(l + 0x1Au));
        const unsigned row = c.u8(l + 0x20u + tab) + c.u8(l + 0x28u + tab);
        const unsigned id = c.u8(c.u32(l + 0x1Cu) + 4u * c.u8(l + 0x38u + row));
        if (kind == 0u || kind == 2u || kind == 4u) t.item = id;
        else if ((kind == 1u || kind == 3u) && id != 0xFFu) t.talent = id;
        return t;
    }
    // The slot cursors describe what they are on as they move; any other
    // cursor only while SELECT's help cursor is up, since the row it names
    // stays behind when that ends.
    if (sub != kSubEquipSlots && sub != kSubTalentSlots && c.u8(ctl + kCtlHelp) == 0u)
        return t;
    const std::uint32_t rows = c.u32(ctl + kCtlRows);
    const unsigned idx = c.u16(ctl + kCtlRow);
    const std::uint32_t row = rows + idx * 12u;
    unsigned id = 0, kind = 0;
    if (rom_ok(row, 4)) {
        id = rom16(row);
        kind = rom16(row + 2u);
    } else if (c.ok(row, 4)) {
        id = c.u16(row);
        kind = c.u16(row + 2u);
    } else {
        return t;
    }
    const std::uint32_t entry = c.u32(ctl + kCtlEntry);
    const bool battle = c.u8(ctl + kCtlBattle) == 1u;
    const unsigned entry_flags = c.ok(entry, 0x18u) ? c.u8(entry + 0x17u) : 0u;
    switch (kind) {
        case 5: case 6: case 7: case 8: {
            const unsigned slot = (kind == 5u || kind == 7u) ? idx : id;
            if (slot >= 4u) return t;
            if (kind <= 6u) {
                t.item = c.u8(unit + kOffEquip + slot);
            } else {
                const unsigned tid = c.u8(unit + kOffTalent + slot);
                if (tid != 0xFFu) t.talent = tid;
            }
            return t;
        }
        case 1:                                  // No. (0x0802AC74)
            t.help = 400u;
            if (battle && (entry_flags & 1u)) t.help = 503u;       // leader
            else if (battle && (entry_flags & 2u)) t.help = 504u;  // guest
            return t;
        case 2:                                  // birthday (0x0802B160)
            birthday(c, unit, &t);
            return t;
        case 11: case 12: {                      // the two MOVE icons (0x0802AE46)
            const std::uint32_t m = move_record(c, unit, entry);
            const unsigned v = rom8(m + (kind == 11u ? 0u : 1u));
            unsigned k = (v >= 1u && v <= 5u) ? v - 1u : 0u;
            if (kind == 11u && rom8(m + 2u) == 1u) k += 5u;
            t.help = (kind == 11u ? 414u : 424u) + k;
            return t;
        }
        case 13: {                               // class (0x0802ACF6)
            const unsigned who = c.u8(unit + 0x4Eu);
            if ((who == 4u || who == 5u || who == 10u || who == 11u) && battle &&
                c.u8(usa(0x020012E0u)) != 4u && (entry_flags & 1u)) {
                t.help = who == 4u ? 364u
                       : who == 5u ? (c.u8(usa(0x020013BCu)) == 0x24u ? 365u : 366u)
                       : who == 10u ? 368u : 370u;
            } else {
                t.help = gauge::class_field(c.u8(unit + 0x4Fu), c.u8(unit + 0x50u), 0x30u, true);
            }
            return t;
        }
        case 17: {                               // element (0x0802B068)
            unsigned v = c.u8(unit + 0x54u);
            if (gauge::class_field(c.u8(unit + 0x4Fu), c.u8(unit + 0x50u), 0x16u, false) == 0u) {
                v = c.u8(unit + 0x55u);
                if (v == 1u) v = 5u;
                else if (v == 3u) v = 6u;
            }
            static constexpr unsigned kIds[7] = {402u, 496u, 497u, 498u, 499u, 500u, 501u};
            t.help = v <= 6u ? kIds[v] : v;
            return t;
        }
        case 10: {                               // an EMBLEM page slot (0x0802ADB6)
            const bool earned = id < 32u && ((c.u32(ctl + kCtlEmblems) >> id) & 1u);
            t.help = emblem_help(static_cast<int>(id), earned, c.u8(unit + 0x5Eu));
            return t;
        }
        case 9: case 14: case 15: case 16:
            return t;                            // in no status window table
        default:                                 // 0, 3, 4: the row's own text
            t.help = id;                         // 0 is "dummy": nothing
            return t;
    }
}

// An item's help id: its accessor's field 2 (0x081438B8), the u16 at +0x04
// of its ROM record, or a made item's base's. The made items' bases (ids
// 17-22) all say 0x3A, which 0x0801C004 replaces by the item's type (field
// 15); a type it has no text for keeps 0x3A.
unsigned item_help_id(const Item& it) {
    const unsigned h = rom16(it.rom + 0x04u);
    if (h != 0x3Au) return h;
    switch (it.type) {
        case 3: return 0x2A5;
        case 15: return 0x2AD;
        case 18: return 0x2A7;
        case 23: return 0x2AA;
        case 25: return 0x2A8;
        case 26: return 0x2A9;
        case 29: return 0x2A6;
        case 30: return 0x2AB;
        case 31: return 0x2AC;
        case 32: return 0x2A4;
        case 33: return 0x2A3;
        default: return h;
    }
}

// ── The battle's weapon and spell lists ─────────────────────────────────
// After Attack or Magic the game lists the acting unit's weapons (ARMS) or
// talents (TALENT), and SELECT shows the HELP text of the row under the
// cursor. Found by hooking that help's text fetch (0x08015D50) in CO and CN:
// - The battle's work block is [0x0202B26C] (it moves between runs). Its
//   byte +0x2047 is 3 while such a list is open (5 in the command menu,
//   the move and the target choice), and +0x203C is the acting record.
// - The list is the bytes at +0x1E94: item ids for ARMS (Ivanna's 09,
//   Balmung; Alphonse's C8, Kaukown) or talent ids for TALENT (Alphonse's
//   17 57, Ice Javelin and Atropos). 0 ends it; 0xFF is a row the item
//   path (0x0807B52C) resolves some other way, not handled here.
// - The list's window object is at +0x206C: u16 +0x0E its title (0x22F
//   ARMS, 0x255 TALENT), and the row is u16 +0x16 plus u16 +0x1A (Down
//   moves +0x1A), as 0x0807B4FC and 0x0807C92C index it.
// - The help ids then are the item's (0x081437AC(item, 2), ROM +0x04:
//   Balmung 0x32) and the talent's +0x06 (Heal 0x10A), as the game passes.
struct BattleRow {
    std::uint32_t actor = 0;
    unsigned item = 0, talent = 0;
};

BattleRow battle_list_row(const gauge::Ctx& c) {
    BattleRow r;
    const std::uint32_t b = c.u32(usa(0x0202B26Cu));
    // (usa_ctl: the Japanese build has each of the four 0x54 lower, as the
    // rest of the block; checked in both games' memory with either list
    // open, regionmap/battle_list_evidence.py.)
    constexpr std::uint32_t kMode = usa_ctl(0x2047u), kActor = usa_ctl(0x203Cu);
    constexpr std::uint32_t kIds = usa_ctl(0x1E94u), kWindow = usa_ctl(0x206Cu);
    if (!c.ok(b, 0x2090u) || c.u8(b + kMode) != 3u) return r;
    const std::uint32_t m = b + kWindow;
    const unsigned row = c.u16(m + 0x16u) + c.u16(m + 0x1Au);
    if (row >= 16u) return r;
    const unsigned id = c.u8(b + kIds + row);
    if (id == 0u || id == 0xFFu) return r;
    switch (c.u16(m + 0x0Eu)) {
        case 0x22Fu: r.item = id; break;     // ARMS
        case 0x255u: r.talent = id; break;   // TALENT
        default: return r;
    }
    r.actor = c.u32(b + kActor);
    return r;
}

// Help text `h` into out->help_codes. `0x87 NN` is character NN's name;
// `0x80 NN` is figure NN, which the game fills in at run time: from `t` when
// it has one, else left out.
void help_text_at(std::uint32_t bank, unsigned index, UnitInfo* out,
                  const CursorThing* t = nullptr);

void help_text(unsigned h, UnitInfo* out, const CursorThing* t = nullptr) {
    if (kRegion == Region::Japan) help_text_at(kHelpBankJapan, h, out, t);
    else if (h <= 0x1FFu) help_text_at(kHelpBank, h, out, t);
    else help_text_at(kHelpBank2, h - 0x200u, out, t);
}

// The same from the Japanese ROM: entry `index` of an "m7" bank, as the game
// lays it out. The lines stay the game's own (font::kNewline between them,
// their padding blanks dropped), two to a page; colours stay as their
// controls (font::is_colour) for the card to draw; a figure (0xF4nn) comes
// from `t` and a character's name (0xFEnn) from its record.
void help_text_japan(std::uint32_t bank, unsigned index, UnitInfo* out, const CursorThing* t) {
    std::vector<std::uint16_t> codes;
    if (!g_rom || !jtext::entry(g_rom, g_rom_size, bank, index, &codes, false)) return;
    const int cap = static_cast<int>(sizeof out->help_codes / sizeof out->help_codes[0]);
    int n = 0;
    auto trim = [&] {   // the blanks a line was padded with
        while (n > 0 && out->help_codes[n - 1] == font::kSpace) --n;
    };
    for (const std::uint16_t c : codes) {
        if (n >= cap) break;
        if (c == font::kNewline) {
            trim();
            out->help_codes[n++] = c;
        } else if (font::is_figure(c)) {
            const unsigned k = c & 0xFFu;
            for (int i = 0; t && k < 3u && i < t->fig_len[k] && n < cap; ++i)
                out->help_codes[n++] = t->fig[k][i];
        } else if (font::is_character(c)) {
            n += japan_record(kCharNames + (c & 0xFFu) * 16u, out->help_codes + n, cap - n);
        } else if (font::is_colour(c) || c < 0xF000u) {
            out->help_codes[n++] = c;
        }
    }
    // No blank lines at the end (a text that ends its last line, and the
    // second line of a one-line page).
    for (trim(); n > 0 && out->help_codes[n - 1] == font::kNewline; trim()) --n;
    out->help_len = n;
}

// Entry `index` of a text bank laid out as the HELP banks are (a u16 offset
// table, two header bytes, the text): the HELP banks and the NAVI bank.
void help_text_at(std::uint32_t bank, unsigned index, UnitInfo* out, const CursorThing* t) {
    // The USA ROM's layout and one-byte codes below; the Japanese ROM keeps
    // these texts in "m7" banks of 16-bit codes (kol_jtext.h).
    if (kRegion != Region::Usa) {
        help_text_japan(bank, index, out, t);
        return;
    }
    std::uint32_t a = bank + rom16(bank + 2u * index) + 2u;
    const int cap = static_cast<int>(sizeof out->help_codes / sizeof out->help_codes[0]);
    int n = 0;
    for (int guard = 0; guard < 1024 && n < cap; ++guard) {
        const std::uint8_t b = rom8(a++);
        if (b == 0xFFu || !rom_ok(a, 1)) break;
        if (b == 0x80u) {
            const unsigned k = rom8(a++);
            for (int i = 0; t && k < 3u && i < t->fig_len[k] && n < cap; ++i)
                out->help_codes[n++] = t->fig[k][i];
            continue;
        }
        if (b == 0x81u) {   // an escape and its argument (the message box's own)
            ++a;
            continue;
        }
        if (b == 0x87u) {
            std::uint32_t s = rom32(kCharNames + rom8(a++) * 4u);
            for (int k = 0; k < 16 && n < cap; ++k) {
                const std::uint8_t ch = rom8(s++);
                if (ch == 0xFFu) break;
                if (ch == 0x81u) { ++s; continue; }
                out->help_codes[n++] = ch;
            }
            continue;
        }
        out->help_codes[n++] = b;
    }
    out->help_len = n;
}

std::uint16_t rec16(const std::uint8_t* r, std::uint32_t o) {
    return static_cast<std::uint16_t>(r[o] | (r[o + 1] << 8));
}

}  // namespace

int persuade_chance(std::uint32_t target) { return persuade_on(target); }

bool stock_help(UnitInfo* out) {
    if (!out || !g_rom) return false;
    *out = UnitInfo{};
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew) return false;
    const gauge::Ctx c{ew, esz};
    const std::uint32_t ctl = c.u32(kWinCtl);
    if (!c.ok(ctl, kCtlSub + 4u)) return false;
    const unsigned sub = c.u8(ctl + kCtlSub);
    if (sub != kSubItemStock && sub != kSubSpellStock && sub != kSubClassList) return false;
    // The list object, read as the equipment list's (cursor_thing).
    const std::uint32_t l = c.u32(ctl + kCtlList);
    if (!c.ok(l, 0x60u) || c.u8(l + 0x13u) == 0u) return false;
    const unsigned kind = c.u8(l + 0x19u);
    const unsigned tab = c.u8(l + 0x30u + c.u8(l + 0x1Au));
    const unsigned row = c.u8(l + 0x20u + tab) + c.u8(l + 0x28u + tab);
    if (sub == kSubClassList) {
        // The class list holds the classes themselves (+0x38 on, +0x13 of
        // them; no entry table): the class under the hand, and its help as
        // the class-change grid's, class +0x30.
        if (row >= c.u8(l + 0x13u)) return false;
        const unsigned cls = c.u8(l + 0x38u + row);
        const unsigned h = cls ? gauge::class_field(cls, cls, 0x30u, true) : 0u;
        if (!h) return false;
        out->help_item = 0x2000 + static_cast<int>(cls);
        help_text(h, out);
        return out->help_len > 0;
    }
    const std::uint32_t entries = c.u32(l + 0x1Cu);
    if (!c.ok(entries + 4u * c.u8(l + 0x38u + row), 1)) return false;
    const unsigned id = c.u8(entries + 4u * c.u8(l + 0x38u + row));
    if (kind == 0u || kind == 2u || kind == 4u) {
        const Item it = id ? item_at(ew, esz, id, false) : Item{};
        if (!it.ok) return false;
        out->help_item = static_cast<int>(id);
        help_text(item_help_id(it), out);
    } else if ((kind == 1u || kind == 3u) && id != 0u && id != 0xFFu) {
        out->help_item = 0x1000 + static_cast<int>(id);
        help_text(rom16(kTalentTable + id * kTalentStride + 0x06u), out);
    }
    return out->help_len > 0;
}

bool help_text_for(unsigned id, UnitInfo* out) {
    if (!out || !rom_ok(kRegion == Region::Japan ? kHelpBankJapan : kHelpBank, 2)) return false;
    help_text(id, out);
    return out->help_len > 0;
}

int emblem_picture(int slot, int kind) {
    if (slot < 0 || slot > 31) return -1;
    if (slot <= 10) return slot;
    if (slot == 11) return kind == 2 ? 11 : 12;          // Vixen's Whisper / Broken Heart
    if (slot <= 30) return slot + 1;
    return kind == 0 ? 34 : kind == 2 ? 33 : 32;       // Grozz Nuy's / Ripple's / Relix's
}

unsigned emblem_help(int slot, bool earned, int kind) {
    if (slot < 0 || slot > 31) return 0;
    if (!earned) return 0x298u;                         // "This emblem has not been earned."
    if (slot == 31) return kind == 0 ? 0x17Fu : kind == 1 ? 0x169u : 0x16Au;
    return 0x149u + static_cast<unsigned>(emblem_picture(slot, kind));
}

void unit_data_set_rom(const std::uint8_t* rom, std::uint32_t size) {
    g_rom = rom;
    g_rom_size = size;
    // KOL_TALENT_LIST=1: every talent's id, kind (+0x02), the bytes after it
    // and its name, once.
    if (std::getenv("KOL_TALENT_LIST"))
        for (std::uint32_t id = 1; id < 127; ++id) {
            UnitName n;
            table_name(kTalentTable, kTalentStride, id, &n);
            const std::uint32_t e = kTalentTable + id * kTalentStride;
            std::fprintf(stderr, "talent %3u kind %2u +03..+0B:", id, rom8(e + 2u));
            for (std::uint32_t k = 3; k < 12; ++k) std::fprintf(stderr, " %02X", rom8(e + k));
            std::fprintf(stderr, " +1C..+23:");
            for (std::uint32_t k = 0x1C; k < 0x24; ++k) std::fprintf(stderr, " %02X", rom8(e + k));
            std::fprintf(stderr, "  %s\n", n.text[0] ? n.text : "(bitmap)");
        }
}

// Guest frames the map cursor has been out (unit_dump_tick, every frame).
// The game moves its highlighted pointer the frame AFTER the cursor comes
// out (BL_enemy_turn: the cursor at 4899, the pointer at 4900), so the
// cursor counts as out from its second frame.
std::atomic<int> g_cursor_out_frames{0};

bool unit_for_highlighted(UnitInfo* out) {
    if (!out) return false;
    *out = UnitInfo{};
    if (!g_rom) return false;

    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    // With a status window up, the unit it shows: the "Next unit" button
    // can move it away from the one the map cursor names.
    std::uint32_t rec_addr = status_window_unit();
    if (rec_addr < kEwramBase || rec_addr + kRecSize > kEwramEnd ||
        (rec_addr - kEwramBase) + kRecSize > esz) {
        // Off the battle map only a status window names a unit. The game's
        // highlighted pointer outlives the battle: back at the title, and
        // into a New Game, it still named the last unit the cursor was on,
        // and the second screen kept showing it (the user, 10-12).
        if (!is_battle_scene()) return false;
        // Nor in a story event, though it plays on a map (the user, 10-20, asked
        // for no cards outside battle; IC, map 59,
        // Eleanor in a house). Preparation keeps its card: its NAVI script
        // sets the same flag.
        if (story_event() && !battle_preparation()) return false;
        rec_addr = highlighted_record(ew, esz);
        // On the enemy's turn the game leaves that pointer on the player's
        // last unit (BL_enemy_turn: Allos, 0x02001488, through all eight
        // enemy turns and the ~470 frames after them until the cursor comes
        // out on Alphonse at 4900). With the cursor put away and no unit of
        // the player's acting: the unit taking its turn when it is not the
        // player's, and no unit at all between turns.
        // An allied army the game plays (Rictor's on map 2) takes its own
        // phase after the enemy's: its acting unit too (IA, the user, 10-20).
        const std::uint32_t a = acting_record();
        if (!battle_preparation() && g_cursor_out_frames.load(std::memory_order_relaxed) < 2 &&
            !unit_in_action()) {
            if (!a) return false;
            if (ai_phase() ||
                (a - kEwramBase + kRecSize <= esz && (ew[a - kEwramBase + 0x5Du] & 0x80u)))
                rec_addr = a;
        }
    }
    return unit_for_record(rec_addr, out);
}

bool sheet_units(const UnitInfo& hovered, UnitInfo* top, UnitInfo* bottom) {
    *top = hovered;
    // The enemy's turn (unit_for_highlighted names the acting unit): the
    // unit its attack or spell is aimed at below it, once the game has
    // named one (the user, 10-12).
    // The game clears the attack's pair after the blow (GC: Persace's at
    // 400, his turn running on to 539), so the target stays for the rest of
    // that enemy's action.
    if (!unit_in_action() && (hovered.other_team || ai_phase()) &&
        hovered.record == acting_record() &&
        g_cursor_out_frames.load(std::memory_order_relaxed) < 2) {
        static std::uint32_t actor = 0, target = 0;
        if (hovered.record != actor) { actor = hovered.record; target = 0; }
        // From a talent's number on, the unit it hit (ID: not its target).
        const std::uint32_t hit = sheet_hit_target();
        const std::uint32_t t = hit ? hit : acting_target();
        if (t && t != hovered.record) target = t;
        return target && unit_for_record(target, bottom) && bottom->valid;
    }
    // Looking around during a turn is the plain sheet of the unit looked at.
    if (!unit_in_action() || cursor_browsing()) return false;
    const std::uint32_t a = acting_record();
    UnitInfo actor;
    if (!a || !unit_for_record(a, &actor) || !actor.valid) return false;
    *top = actor;
    std::uint32_t t = cursor_unit_record();
    // The confirmation as saved in BV and BW has the cursor put away; the
    // game's highlighted unit is then the target, the one its forecast shows.
    if (!t && !cursor_on_map() && target_confirming()) t = hovered.record;
    if (const std::uint32_t hit = sheet_hit_target()) t = hit;   // a talent's hit
    // Between the target's choice and its confirmation the game has the
    // cursor put away a frame or two before the confirmation is up
    // (states/thor/0075, A: frames 22-23 named no target), and the actor's
    // full sheet flashed between the two cards (the user saw the
    // attacker's or healer's full sheet flash on pressing A). The
    // last target of the same actor is kept for kTargetHold seconds.
    constexpr double kTargetHold = 0.1;
    static std::uint32_t last_actor = 0, last_target = 0;
    static std::chrono::steady_clock::time_point last_at;
    const auto now = std::chrono::steady_clock::now();
    if (t && t != a) {
        last_actor = a; last_target = t; last_at = now;
    } else if (!t && a == last_actor && last_target &&
               std::chrono::duration<double>(now - last_at).count() < kTargetHold) {
        t = last_target;
    }
    if (!t || t == a) return false;   // an empty cell, or the actor itself
    return unit_for_record(t, bottom) && bottom->valid;
}

namespace {

// The HP the sheet shows, held until the blow lands (the user, 2026-10-20,
// asked to see the target on the bottom screen before its HP drops, and the
// bar dropping as the attack lands). The game settles an action's result
// before it plays it: on IA (map 2, Stan on Alphonse) the attack's pair
// names the target at 1564, his HP goes 84 -> 42 at 1565, the strike starts
// at 1567 and the damage number pops at 1685, two seconds later. So a
// battle unit's HP that changes during an action is held on its card until
// a damage number comes up for it, and then drains: the bar's solid part drops at once, the part lost stays
// white for a moment, then red, and shrinks with the figure. A heal rises.
// A change with no number for it (none comes, or no unit acts) is shown as
// it is, at once.
// Which unit a number is for: 0x0202AFCC points at the number's own sprite
// block (0x0202C118 for every target on IA), with no unit in it. So it goes
// by the action: the attack's target, if its HP waits; else the acting unit
// (a counterattack); else every unit whose HP waits (an area technique).
// The 16 battle units' records, 0x02001420 on, stride 0x68 (8 a side).
constexpr std::uint32_t kBattleRecs = usa(0x02001420u);
constexpr int kBattleUnits = 16;
constexpr long kHpHoldMax = 12 * 60;   // frames a change waits for its number
constexpr long kHpPause = 15;          // the lost part held, white for kHpFlash
constexpr long kHpFlash = 6;
constexpr long kHpDrain = 30;          // then shrinking

struct HpTrack {
    std::atomic<int> max{-1}, settled{-1}, from{-1};
    std::atomic<long> held_since{-1}, drain_at{-1};
};
HpTrack g_hp[kBattleUnits];
std::atomic<long> g_hp_frame{0};
// The unit a talent or an attack hit, from its number on, and whose action
// it was.
std::atomic<std::uint32_t> g_hit_record{0}, g_hit_actor{0};

// The figure a tracked unit's card shows at `frame`, and its bar.
struct HpShown { int number, bar, ghost; bool flash, busy, heal; };
HpShown hp_shown(const HpTrack& t, long frame) {
    const int s = t.settled.load(std::memory_order_relaxed);
    HpShown r{s, s, -1, false, t.held_since.load(std::memory_order_relaxed) >= 0, false};
    const long at = t.drain_at.load(std::memory_order_relaxed);
    const long d = frame - at;
    if (at < 0 || d < 0 || d >= kHpPause + kHpDrain) return r;
    const int f = t.from.load(std::memory_order_relaxed);
    double k = d < kHpPause ? 0.0 : static_cast<double>(d - kHpPause) / kHpDrain;
    k = 1.0 - (1.0 - k) * (1.0 - k);   // ease out
    const int roll = static_cast<int>(std::lround(f + (s - f) * k));
    r.number = roll;
    r.busy = true;
    r.flash = d < kHpFlash;
    if (s < f) {
        r.ghost = roll;      // the part lost, shrinking to the bar
    } else {
        r.bar = roll;        // the bar filling the part gained
        r.ghost = s;
        r.heal = true;
    }
    return r;
}

// The unit record a map object stands for (its +0x00), 0 when out of EWRAM.
std::uint32_t rd_rec_of(const std::uint8_t* ew, std::uint32_t esz, std::uint32_t obj) {
    const std::uint32_t o = obj - kEwramBase;
    if (obj < kEwramBase || o + 4u > esz) return 0u;
    return static_cast<std::uint32_t>(ew[o] | ew[o + 1] << 8 | ew[o + 2] << 16 | ew[o + 3] << 24);
}

int hp_slot(std::uint32_t record) {
    if (record < kBattleRecs || (record - kBattleRecs) % kRecSize) return -1;
    const std::uint32_t i = (record - kBattleRecs) / kRecSize;
    return i < kBattleUnits ? static_cast<int>(i) : -1;
}

void hp_track_tick() {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew) return;
    const long frame = g_hp_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    static unsigned epoch = ~0u;
    const bool reload = epoch != g_runtime_state_epoch;
    epoch = g_runtime_state_epoch;
    const bool battle = is_battle_scene();
    const std::uint32_t actor = acting_record();
    const bool acting = actor != 0u;
    // The action's target, kept for the action: the game clears the
    // attack's pair after the blow, sometimes before the number is up.
    static std::uint32_t action = 0, target = 0;
    static long action_no = 0;           // counts actions, for `numbered`
    static long numbered[kBattleUnits];  // the action a unit's number came in
    static bool predicted[kBattleUnits]; // the card is at a talent's result
    static int taken[kBattleUnits];      // the talent's result entries its numbers took
    const bool new_action = actor != action;
    if (new_action) {
        action = actor; target = 0; ++action_no;
        g_hit_record.store(0, std::memory_order_relaxed);
        std::fill(std::begin(taken), std::end(taken), 0);
    }
    if (const std::uint32_t t = acting_target()) target = t;
    static std::uint32_t number_was = 0, number_unit_was = 0;
    const std::uint32_t number = damage_number_object();
    // The same block may serve the next unit's number: a new place is a new
    // number too.
    const std::uint32_t number_unit = number ? damage_number_unit() : 0u;
    const bool new_number = number != 0u && (number != number_was || number_unit != number_unit_was);
    number_was = number;
    number_unit_was = number_unit;
    for (int i = 0; i < kBattleUnits; ++i) {
        HpTrack& t = g_hp[i];
        const std::uint32_t o = kBattleRecs + i * kRecSize - kEwramBase;
        if (o + kRecSize > esz) break;
        const int hp = ew[o + kOffHp] | ew[o + kOffHp + 1] << 8;
        const int max = ew[o + kOffHp + 2] | ew[o + kOffHp + 3] << 8;
        auto snap = [&] {
            t.settled.store(max > 0 ? hp : -1, std::memory_order_relaxed);
            t.max.store(max, std::memory_order_relaxed);
            t.held_since.store(-1, std::memory_order_relaxed);
            t.drain_at.store(-1, std::memory_order_relaxed);
        };
        const int s = t.settled.load(std::memory_order_relaxed);
        if (reload || !battle || max <= 0 || s < 0 || t.max.load(std::memory_order_relaxed) != max) {
            predicted[i] = false;
            snap();
            continue;
        }
        if (hp == s) {
            t.held_since.store(-1, std::memory_order_relaxed);
            predicted[i] = false;
            continue;
        }
        // At a talent's result the game has not written yet: kept there
        // until it does, or until the next unit acts.
        if (predicted[i]) {
            if (!new_action && acting) continue;
            predicted[i] = false;
            snap();
            continue;
        }
        static const bool probe = std::getenv("KOL_SHEET_PROBE") != nullptr;
        if (probe && t.held_since.load(std::memory_order_relaxed) < 0)
            std::fprintf(stderr, "hp-change: %ld %08X %d -> %d acting=%d\n", frame,
                         kBattleRecs + i * kRecSize, s, hp, acting ? 1 : 0);
        // Its number already came in this action (a talent's: the game
        // writes the HP after the number has gone, IB 870 after 749-833):
        // the card moves now, unless the number's figure already took it
        // there (a talent's result, below).
        if (numbered[i] == action_no) {
            t.from.store(hp_shown(t, frame).number, std::memory_order_relaxed);
            t.settled.store(hp, std::memory_order_relaxed);
            t.held_since.store(-1, std::memory_order_relaxed);
            t.drain_at.store(frame, std::memory_order_relaxed);
            continue;
        }
        const long held = t.held_since.load(std::memory_order_relaxed);
        // A change still waiting when the next unit acts is shown as it is.
        if (acting && held >= 0 && new_action) snap();
        else if (acting && held < 0) t.held_since.store(frame, std::memory_order_relaxed);
        else if (!acting || frame - held >= kHpHoldMax) snap();
    }
    if (!new_number || !battle || reload) return;
    auto waits = [&](int i) {
        return i >= 0 && g_hp[i].held_since.load(std::memory_order_relaxed) >= 0;
    };
    auto drain_to = [&](int i, int to) {
        HpTrack& t = g_hp[i];
        numbered[i] = action_no;
        t.from.store(hp_shown(t, frame).number, std::memory_order_relaxed);
        t.settled.store(to, std::memory_order_relaxed);
        t.held_since.store(-1, std::memory_order_relaxed);
        t.drain_at.store(frame, std::memory_order_relaxed);
    };
    auto drain = [&](int i) {
        const std::uint32_t o = kBattleRecs + i * kRecSize - kEwramBase;
        drain_to(i, ew[o + kOffHp] | ew[o + kOffHp + 1] << 8);
    };
    // The unit a number is for: the one standing where the number's block
    // says it is (damage_number_unit: its world X and Z). IF, Crag Crush on
    // four units, numbers one at a time, not in the result's order: each
    // drains its own unit, and becomes the lower card in turn. Not the
    // attacker: a counterattack's number drains the top card in place.
    int hit = hp_slot(rd_rec_of(ew, esz, number_unit));
    // Else by the action, as before the place was known: the attack's
    // target if its HP waits, else the attacker, else every waiting unit.
    if (hit < 0) {
        if (waits(hp_slot(target))) hit = hp_slot(target);
        else if (waits(hp_slot(actor))) hit = hp_slot(actor);
        else
            for (int i = 0; i < kBattleUnits; ++i)
                if (waits(i)) {
                    if (hit < 0) hit = i;
                    else drain(i);
                }
    }
    if (hit < 0) return;
    const std::uint32_t rec = kBattleRecs + static_cast<std::uint32_t>(hit) * kRecSize;
    if (rec != actor) {
        g_hit_actor.store(actor, std::memory_order_relaxed);
        g_hit_record.store(rec, std::memory_order_relaxed);
    }
    if (waits(hit)) {
        drain(hit);
        return;
    }
    numbered[hit] = action_no;
    // Its HP not written yet (a talent writes after its numbers: ID 691,
    // after 563-652): the talent's result for it, the figure the number
    // shows; the game's write then lands on it.
    // Each of its numbers takes the next entry for it, and a number past its
    // entries takes nothing: states/thor/0090, the dragon's Cold Breath on
    // Flauros (one entry, 58) pops two, the damage and then her sleep, and
    // the card went 112 -> 54 -> 0; the game leaves her at 54.
    // KOL_RESULT_EVERY_NUMBER=1: every number takes the first entry, as before.
    static const bool every = std::getenv("KOL_RESULT_EVERY_NUMBER") != nullptr;
    TalentResult res[8];
    const int n = talent_results(res, 8);
    const std::uint32_t obj = object_of_record(rec);
    int skip = every ? 0 : taken[hit]++;
    for (int k = 0; k < n; ++k) {
        if (!res[k].known || res[k].object != obj) continue;
        if (skip-- > 0) continue;
        const int s = g_hp[hit].settled.load(std::memory_order_relaxed);
        const int max = g_hp[hit].max.load(std::memory_order_relaxed);
        if (s < 0) break;
        const int to = res[k].heal ? std::min(max, s + res[k].amount) : std::max(0, s - res[k].amount);
        if (to != s) {
            drain_to(hit, to);
            predicted[hit] = true;
        }
        break;
    }
}

}  // namespace

void sheet_hp(UnitInfo* u) {
    if (!u || !u->valid) return;
    const int i = hp_slot(u->record);
    if (i < 0) return;
    const HpTrack& t = g_hp[i];
    if (t.settled.load(std::memory_order_relaxed) < 0 ||
        t.max.load(std::memory_order_relaxed) != u->hp_max)
        return;
    const HpShown r = hp_shown(t, g_hp_frame.load(std::memory_order_relaxed));
    u->hp = r.number;
    u->hp_bar = r.bar;
    u->hp_ghost = r.ghost;
    u->hp_flash = r.flash;
    u->hp_heal = r.heal;
}

std::uint32_t sheet_hit_target() {
    const std::uint32_t hit = g_hit_record.load(std::memory_order_relaxed);
    return hit && g_hit_actor.load(std::memory_order_relaxed) == acting_record() ? hit : 0u;
}

bool sheet_hp_busy(std::uint32_t record) {
    const int i = hp_slot(record);
    return i >= 0 && hp_shown(g_hp[i], g_hp_frame.load(std::memory_order_relaxed)).busy;
}

namespace {

// The target's card when it dies: once the game removes the dead unit, the
// card flashes red, then fades into the attacker's full sheet, and stays away
// for the rest of that action.
constexpr double kDeathFlash = 0.6;        // three flashes
constexpr double kDeathFlashPeriod = 0.2;
constexpr double kDeathFade = 0.4;

UnitInfo g_last_bottom;          // the last living unit shown in the lower card
UnitInfo g_dying;                // its card as it was last read (HP 0)
std::uint32_t g_gone = 0;        // its record: no lower card for it any more
double g_death_start = -1.0;

std::uint32_t mix_argb(std::uint32_t a, std::uint32_t b, double t) {
    auto ch = [&](int sh) {
        const double x = ((a >> sh) & 0xFFu) * (1.0 - t) + ((b >> sh) & 0xFFu) * t;
        return static_cast<std::uint32_t>(x + 0.5) << sh;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

}  // namespace

bool compose_unit_sheet(const UnitInfo& hovered, std::uint32_t* argb, double now_s) {
    if (!argb) return false;
    // A new action starts clean: out of an action, or another unit acting.
    // So does a gone unit that is back alive: loading a state from before the
    // blow brings Mulinne back while Ivanna's action flag stays up (it stays
    // up after an attack, since she may still move), and her card was hidden.
    static std::uint32_t actor_seen = 0;
    const std::uint32_t actor =
        unit_in_action() ||
                ((hovered.other_team || ai_phase()) && hovered.record == acting_record())
            ? acting_record() : 0u;   // the player's action, or an AI army's
    UnitInfo back;
    if (actor != actor_seen ||
        (g_gone && unit_for_record(g_gone, &back) && back.hp > 0)) {
        actor_seen = actor;
        g_last_bottom = UnitInfo{};
        g_gone = 0;
        g_death_start = -1.0;
    }
    UnitInfo top, bottom;
    bool pair = sheet_units(hovered, &top, &bottom);
    sheet_hp(&top);
    if (pair) sheet_hp(&bottom);

    // The move prompt's card (see unit_for_record) shows once per move
    // choice: from when it appears until four seconds have passed or the
    // cursor leaves the unit, whichever comes first. Coming back to the unit
    // does not bring it back; the next move choice does.
    static bool navi_done = false;
    static double navi_since = -1.0;
    if (!move_choice_active()) {
        navi_done = false;
        navi_since = -1.0;
    }
    if (top.help_item == kNaviHelpItem) {
        if (!navi_done && navi_since < 0.0) navi_since = now_s;
        if (!navi_done && now_s - navi_since >= kNaviShowSeconds) navi_done = true;
        if (navi_done) {
            top.help_len = 0;
            top.help_item = 0;
        }
    } else if (navi_since >= 0.0) {
        navi_done = true;   // the cursor left the unit
    }

    // A message the move choice put up and the game no longer shows (Elrik's
    // cursor on Eleanor, A: "Can't move to that spot.", kol_actions.cpp):
    // on the top card for four seconds, or until the cursor leaves the cell
    // it was given on.
    {
        static unsigned seen = 0;
        static double since = -1.0;
        static int at_x = -1, at_z = -1;
        std::uint32_t bank = 0;
        unsigned index = 0;
        const unsigned serial = move_message(&bank, &index);
        int cx = -1, cz = -1;
        const bool cursor = cursor_on_map() && map_cursor_cell(&cx, &cz);
        if (serial != seen) {
            seen = serial;
            since = now_s;
            at_x = at_z = -1;
        }
        // The map cursor is put away for a few frames as the message fires
        // (CT: frames 12-18), its slot left stale, so the cell is taken the
        // first time the cursor is out again, and only then compared.
        if (since >= 0.0 && cursor && at_x < 0) { at_x = cx; at_z = cz; }
        // Also gone when the move choice ends (B), once the cursor is back.
        if (since >= 0.0 && (now_s - since >= kNaviShowSeconds ||
                             (cursor && (cx != at_x || cz != at_z)) ||
                             (at_x >= 0 && !move_choice_active())))
            since = -1.0;
        if (since >= 0.0 && bank != 0u && top.valid) {
            top.help_item = kMessageHelpItem;
            help_text_at(bank, index, &top);
            pair = false;   // the two-card sheet has no room for the card
        }
    }

    // The NAVI box battle preparation opens and the game no longer shows
    // ("To form an Attack Team...", kol_actions.cpp): on the top card for
    // four seconds, or until any button is pressed.
    {
        static unsigned seen = 0, presses = 0;
        static double since = -1.0;
        std::uint32_t bank = 0;
        unsigned index = 0;
        int number = -1;
        const unsigned serial = navi_message(&bank, &index, &number);
        if (serial != seen) {
            seen = serial;
            since = now_s;
            presses = key_presses();
        }
        if (since >= 0.0 && (now_s - since >= kNaviShowSeconds || key_presses() != presses))
            since = -1.0;
        if (since >= 0.0 && top.valid) {
            CursorThing t;   // its figure 0 (0x80 0x00): the team size
            if (number >= 0) {
                char num[8];
                std::snprintf(num, sizeof num, "%d", number);
                t.fig_len[0] = codes_from_ascii(num, t.fig[0], 24);
            }
            top.help_item = kMessageHelpItem;
            help_text_at(bank, index, &top, &t);
            pair = false;   // the two-card sheet has no room for the card
        }
    }

    // The last target shown dies. The flash waits for the damage number the
    // game pops above the sprite (the user's choice): in CO Ivanna's blow
    // takes Mulinne's HP to 0 at frame 182, the number appears at 265 and
    // leaves at 354, her death animates until about 420, and the game clears
    // her record at 422. Until the number shows the card reads HP 0. Should
    // the number be missed, the record's clearing starts the flash instead.
    if (g_death_start < 0.0 && g_last_bottom.valid) {
        UnitInfo now;
        const bool present = unit_for_record(g_last_bottom.record, &now);
        if (!present ||
            (now.hp <= 0 && damage_number_shown() && !sheet_hp_busy(g_last_bottom.record))) {
            g_dying = g_last_bottom;
            g_dying.hp = 0;
            g_dying.hp_bar = g_dying.hp_ghost = -1;
            g_dying.hp_flash = g_dying.hp_heal = false;
            g_gone = g_last_bottom.record;
            g_last_bottom = UnitInfo{};
            g_death_start = now_s;
        }
    }
    if (pair && bottom.record == g_gone) pair = false;
    if (pair) g_last_bottom = bottom;

    // A tapped slot is the unit's it was tapped on: another unit on the sheet
    // (the shoulders, a tap in the list, the game's own cursor) closes its
    // card. It stayed, showing the new unit's item in that slot (the user
    // expected the description box to close on moving to another
    // unit). KOL_SHEET_TAP_KEEPS=1: it stays.
    static const bool tap_keeps = std::getenv("KOL_SHEET_TAP_KEEPS") != nullptr;
    static std::uint32_t tapped_on = 0;
    if (sheet_selected_slot() < 0) {
        tapped_on = 0;
    } else if (tapped_on == 0) {
        tapped_on = top.record;
    } else if (top.record != tapped_on && !tap_keeps) {
        sheet_clear_selection();
        tapped_on = 0;
    }

    // A slot tapped on the second screen's lower card: its HELP text, unless
    // the game has one of its own up. An empty EQUIP. or TALENT row has none.
    if (!pair && top.help_len == 0) {
        const int s = sheet_selected_slot();
        unsigned h = 0;
        int what = 0;
        if (s < 0) {
        } else if (sheet_lower_page() == 1) {
            h = emblem_help(s, (top.emblems >> s) & 1u, top.emblem_kind);
            what = 0x2000 + static_cast<int>(h);
        } else if (s < 4 && top.equip_id[s] != 0) {
            std::uint32_t esz = 0;
            const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
            const std::uint32_t o = top.record - kEwramBase + 0x5Du;
            const bool side1 = ew && o < esz && made_side1(ew[o]);
            const Item it = item_at(ew, esz, static_cast<unsigned>(top.equip_id[s]), side1);
            if (it.ok) { h = item_help_id(it); what = top.equip_id[s]; }
        } else if (s >= 4 && top.talent_id[s - 4] > 0) {
            const unsigned tid = static_cast<unsigned>(top.talent_id[s - 4]);
            h = rom16(kTalentTable + tid * kTalentStride + 0x06u);
            what = 0x1000 + static_cast<int>(tid);
        }
        if (h) {
            help_text(h, &top);
            top.help_item = what;
        } else if (s >= 0) {
            sheet_clear_selection();
        }
    }

    const double t = g_death_start < 0.0 ? -1.0 : now_s - g_death_start;
    if (t < 0.0 || t >= kDeathFlash + kDeathFade) {
        if (!render_unit_sheet(top, argb, pair ? &bottom : nullptr)) return false;
        // Persuading: the chance on the lower card's unit between the cards.
        if (pair) {
            const int chance = persuade_chance(bottom.record);
            draw_sheet_forecast(argb, now_s, chance >= 0);   // (the confirmation; kol_sheet)
            if (chance >= 0) draw_sheet_chance(argb, chance);
        }
        return true;
    }

    if (!render_unit_sheet(top, argb, &g_dying)) return false;
    constexpr std::uint32_t kRed = 0xFFFF2020u;
    const int y0 = kSheetLowerRow * 8;
    std::uint32_t* lower = argb + y0 * kSheetW;
    const int n = (kSheetH - y0) * kSheetW;
    if (t < kDeathFlash) {
        if (std::fmod(t, kDeathFlashPeriod) < kDeathFlashPeriod / 2)
            for (int i = 0; i < n; ++i) lower[i] = mix_argb(lower[i], kRed, 0.6);
        return true;
    }
    static std::uint32_t full[kSheetW * kSheetH];
    if (!render_unit_sheet(top, full)) return true;
    const double f = (t - kDeathFlash) / kDeathFade;
    const std::uint32_t* to = full + y0 * kSheetW;
    for (int i = 0; i < n; ++i)
        lower[i] = mix_argb(mix_argb(lower[i], kRed, 0.6 * (1.0 - f)), to[i], f);
    return true;
}

bool compose_other_page(const UnitInfo& hovered, std::uint32_t* argb) {
    UnitInfo top, bottom;
    if (!argb || sheet_units(hovered, &top, &bottom)) return false;
    top.help_len = 0;
    return render_unit_sheet(top, argb, nullptr, sheet_lower_page() ^ 1);
}

// A copy of EWRAM unit_for_record reads instead of the live one while set:
// the shop's comparisons, the item on sale put on a unit (unit_with_item).
// Per thread: the second screen's other pictures read the live one.
thread_local const std::uint8_t* t_ew_what_if = nullptr;
thread_local std::uint32_t t_ew_what_if_size = 0;

bool unit_for_record(std::uint32_t rec_addr, UnitInfo* out) {
    if (!out) return false;
    *out = UnitInfo{};
    if (!g_rom) return false;
    std::uint32_t esz = t_ew_what_if_size;
    const std::uint8_t* ew = t_ew_what_if ? t_ew_what_if : gbarecomp::runtime_ewram_view(&esz);
    if (!ew || rec_addr < kEwramBase || rec_addr + kRecSize > kEwramEnd ||
        (rec_addr - kEwramBase) + kRecSize > esz)
        return false;
    const std::uint8_t* r = ew + (rec_addr - kEwramBase);
    out->record = rec_addr;

    std::size_t n = 0;
    if (kRegion == Region::Japan) {
        // 8 codes, zero padded (a unit hired and named in the Japanese shop,
        // 2026-11-25: "あいう" is 0E 00 0F 00 10 00 and ten zero bytes).
        int len = 8;
        while (len > 0 && (r[kOffName + (len - 1) * 2] | (r[kOffName + (len - 1) * 2 + 1] << 8)) == 0) --len;
        for (int i = 0; i < len; ++i)
            out->name_codes[out->name_ncodes++] =
                static_cast<font::Code>(r[kOffName + i * 2] | (r[kOffName + i * 2 + 1] << 8));
        jtext::show(out->name_codes, out->name_ncodes, out->name, static_cast<int>(sizeof out->name));
        n = std::strlen(out->name);
    } else
    for (std::uint32_t i = 0; i < 15u; ++i) {
        const std::uint8_t b = r[kOffName + i];
        if (b == 0xFFu) break;
        out->name_codes[out->name_ncodes++] = b;
        if (b < 0x1Au) out->name[n++] = static_cast<char>('A' + b);
        else if (b < 0x34u) out->name[n++] = static_cast<char>('a' + (b - 0x1Au));
        else break;
    }
    out->name[n] = '\0';

    out->hp     = rec16(r, kOffHp);
    out->hp_max = rec16(r, kOffHp + 2u);
    out->mp     = rec16(r, kOffHp + 4u);
    out->mp_max = rec16(r, kOffHp + 6u);
    out->sp = (gauge::class_field(r[0x4F], r[0x50], 0x18u, true) & 0x80u) != 0u;
    {
        // The move row: the icons from the movement type (0x0803BDC0 reads
        // 0x08139B74's record: land 2 (type - 1), + 1 on snow; water type - 1).
        const gauge::Ctx mc{ew, esz};
        const std::uint32_t entry = battle_entry(rec_addr);
        const std::uint32_t m = move_record(mc, rec_addr, entry);
        const unsigned land = rom8(m), water = rom8(m + 1u), snow = rom8(m + 2u);
        if (land >= 1u && land <= 5u) out->move_land = static_cast<int>(2u * (land - 1u) + (snow == 1u ? 1u : 0u));
        if (water >= 1u && water <= 5u) out->move_water = static_cast<int>(water - 1u);
        out->speed = unit_speed(mc, rec_addr, entry);
    }
    out->str_base = rec16(r, kOffStr);
    out->int_base = rec16(r, kOffStr + 2u);
    out->agi_base = rec16(r, kOffStr + 4u);
    out->level = r[kOffLevel];
    out->exp   = r[kOffExp];

    // Equipment: the adjustment the window shows is a plain sum, and the
    // figures it produces are the ones this project long believed were
    // "computed at draw time and stored nowhere".
    const bool side1 = made_side1(r[0x5Du]);
    int dstr = 0, dint = 0, dagi = 0;
    for (int s = 0; s < 4; ++s) {
        const std::uint8_t id = r[kOffEquip + s];
        const Item it = item_at(ew, esz, id, side1);
        out->equip_id[s] = id;
        if (!it.ok) continue;
        item_name(ew, it, id, &out->equip[s]);
        if (id == 0u) continue;            // "None" — an empty slot, not an item
        out->equip_pic[s] = static_cast<int>(it.base);
        dstr += it.str;
        dint += it.intel;
        dagi += it.agi;
    }
    out->str_  = out->str_base + dstr;
    out->intel = out->int_base + dint;
    out->agi   = out->agi_base + dagi;

    for (int s = 0; s < 4; ++s) {
        const std::uint8_t id = r[kOffTalent + s];
        if (id == 0xFFu) continue;         // slot never filled
        out->talent_id[s] = id;
        table_name(kTalentTable, kTalentStride, id, &out->talent[s]);
        const std::uint32_t e = kTalentTable + id * kTalentStride;
        out->talent_kind[s] = rom8(e + 0x02u);
        out->talent_sub[s] = rom8(e + 0x08u);
        out->talent_pow[s] = rom8(e + 0x22u);
        out->talent_mp[s] = rom8(e + 0x0Fu);
    }

    const std::uint32_t cls = r[kOffClass];
    table_name(kClassTable, kClassStride, cls, &out->cls);

    // ATK as the guest's routine at 0x08139D1C computes it for the sheet
    // (through the 999 cap at 0x0813B094): STR, equipment included, plus a
    // weapon term. The weapons are the first two slots, in any position,
    // whose item +0x08 is 1 or 2 (0x0813BEC0). Each one's power is its
    // +0x0C, or the class's innate attack when its kind +0x06 is 0x0B. The
    // stronger of the two counts, and with no weapon the innate attack
    // does; either is scaled by the class's weapon percent. Deneb's wands
    // sit in slots 3 and 4 behind a hat and a cloak: slot 1 alone gave her
    // 215, not the window's 230 (states/CS_deneb_attack.state).
    const std::uint32_t ce = kClassTable + resolve_class(cls) * kClassStride;
    const int innate = rom8(ce + kClsInnate);
    const int wpc    = rom8(ce + kClsWeaponPc);
    int power = -1, weapons = 0;
    for (int s = 0; s < 4 && weapons < 2; ++s) {
        const std::uint8_t id = r[kOffEquip + s];
        if (id == 0u) continue;
        const Item it = item_at(ew, esz, id, side1);
        if (!it.ok) continue;
        const unsigned hands = rom8(it.rom + 0x08u);
        if (hands != 1u && hands != 2u) continue;
        ++weapons;
        const int p = rom8(it.rom + kItemKind) == 0x0Bu ? innate : rom8(it.rom + kItemPhys);
        if (p > power) power = p;
    }
    if (weapons == 0) power = innate;
    out->atk_melee = out->str_ + power * wpc / 100;
    out->atk_magic = out->intel;

    // Defence, exactly as the guest's routine at 0x08139FCC computes it for
    // the sheet: a stat plus equipment, times 100, over the class's own
    // damage-taken percent shifted by half the equipment's. The class term
    // DIVIDES the whole sum, STR included, which is why a Cleric's DEF (120%
    // taken) can sit below his STR. Signed halving truncates toward zero, as
    // the THUMB (x + (x >> 31)) >> 1 does.
    auto half = [](int v) { return v / 2; };
    int armour = 0, def_pc = 0, elem_pc = 0;
    for (int s = 0; s < 4; ++s) {
        const std::uint8_t id = r[kOffEquip + s];
        if (id == 0u) continue;
        const Item it = item_at(ew, esz, id, side1);
        if (!it.ok) continue;
        const std::uint32_t e = it.rom;
        if (rom8(e + kItemKind) > 10u) armour += rom8(e + kItemPhys);
        def_pc += static_cast<std::int8_t>(rom8(e + kItemDefPc));
        for (std::uint32_t k = 0; k < 6u; ++k)
            elem_pc += static_cast<std::int8_t>(rom8(e + kItemElemPc + k));
    }
    // A class with flag 0x100 — the beasts and dragons — adds its innate
    // attack to the DEF sum; that is the community's "monster DEF = (STR +
    // 50) * ..." as a special case. A class WITHOUT the flag adds it too for
    // character 0x18 (record +0x4E, Shaher) when his story variant is set
    // (0x0813A108: byte 0x0200122C + 0x18 through 0x08142FAC, the same
    // variant table the portraits read). This was left blank as "a game flag
    // this decode cannot see" until Thor state 0302 (the user found Shaher's
    // DEF blank on the bottom screen): variant 0 there, so no bonus. Classes
    // 64/65, whose fields resolve through a second id, stay unknown.
    const bool odd_class = rom8(ce + kClsDmgMult) == 0u;
    const bool beast = (rom16(ce + kClsFlags) & 0x100u) != 0u;
    bool shaher_bonus = false;
    if (!beast && r[0x4Eu] == 0x18u) {
        const std::uint32_t v = usa(0x0200122Cu) + 0x18u - kEwramBase;
        shaher_bonus = v < esz && ew[v] != 0u;
    }
    if (!odd_class) {
        int div = rom8(ce + kClsDmgMult) + half(def_pc);
        if (div <= 0) div = 1;
        const int innate_def = beast || shaher_bonus ? rom8(ce + kClsInnate) : 0;
        out->def = 100 * (out->str_ + armour + innate_def) / div;
        int mdiv = 0;
        for (std::uint32_t k = 0; k < 6u; ++k) mdiv += rom8(ce + kClsMagicPc + k);
        mdiv = (mdiv + half(elem_pc)) / 6;
        if (mdiv <= 0) mdiv = 1;
        // A bug in the guest, reproduced because the sheet shows its result.
        // The magic path's equipment loop (0x0813A234) starts from a pointer
        // the previous loop left at sp+4 — the unit's base INT — and never
        // advances it, so instead of the equipment's +0x0D it adds four times
        // the +0x0D of the item whose id is the low byte of base INT. Checked
        // by poking Alberich's base INT to 50 (his staff, +0x0D = 5): the
        // sheet read 84, not the 63 a correct loop would give.
        // The loop reads the field through the item accessor 0x081437AC(id,
        // 10, side). An id above 0xC7 is made item id - 0xC8, in the table
        // at 0x02000D30, or at 0x02001C60 when record +0x5D bit 5 is set (the
        // routine's side flag at 0x08139FE6); field 10 is then the +0x0D of
        // its base item (+0x18) in ROM (0x08143960). Monsters with a base
        // INT of 200 or more (Radam's 216, states/CR_mdef_missing.state)
        // land here, and their M.DEF was left blank.
        const std::uint32_t bug_id = static_cast<std::uint32_t>(out->int_base) & 0xFFu;
        int bug = 0;
        bool bug_known = true;
        std::uint32_t bug_base = bug_id;
        if (bug_id > kMaxRomItem) {
            const std::uint32_t table = side1 ? kMadeItemsOther : kMadeItems;
            const std::uint32_t a = table + (bug_id - (kMaxRomItem + 1u)) * kMadeStride;
            if (a - kEwramBase + kMadeStride <= esz) bug_base = ew[a - kEwramBase + 0x18u];
            else bug_known = false;
        }
        if (bug_known && bug_base != 0u)
            bug = 4 * rom8(kItemTable + bug_base * kItemStride + kItemSpll);
        if (bug_known) out->mdef = 100 * (out->intel + bug) / mdiv;
    }
    // The window reads all four through wrappers that cap them at 999:
    // 0x0813B094 over the attack routine, 0x0813B0B0 over the defence one.
    int* const capped[] = {&out->atk_melee, &out->atk_magic, &out->def, &out->mdef};
    for (int* v : capped)
        if (*v > 999) *v = 999;

    // Element and alignment, read where the game reads them (record +0x54
    // via its field accessor from 0x0803B29C, +0x55 from 0x0803B4BE). A class
    // whose field 0x0D (byte +0x16) is 0 has no element of its own: the icon
    // shows its alignment instead, 1 as virtue (5), 3 as bane (6), and no
    // alignment letter is drawn (0x0803B230, 0x0803B478; a Ghost in
    // states/CE_move_greyed.state).
    const bool elemental = gauge::class_field(r[0x4Fu], r[0x50u], 0x16u, false) != 0u;
    const int al = r[0x55u];
    const int el = elemental ? r[0x54u] : al == 1 ? 5 : al == 3 ? 6 : al;
    out->element = (el >= 1 && el <= 6) ? el : -1;
    // Record +0x5D bit 7: set for every unit not on the player's side, whose
    // status sheet the game draws in green (palette bank 5) instead of cream.
    // Consistent across three battle states: the party clear, enemies and
    // guests set, and the game's own windows green exactly for the latter.
    out->other_team = (r[0x5Du] & 0x80u) != 0;
    {
        // The EMBLEM page's set (0x0803C01C): the emblem word ANDed with a
        // mask from the class table, found through the table at 0x086148A0
        // for the kinds (+0x4E above 1) that name a class there, else through
        // the record's own +0x4F/+0x50.
        const unsigned who = r[0x4Eu];
        const unsigned alt = who > 1u ? rom8(kEmblemClassOf + who * 20u + 2u) : 0u;
        const std::uint32_t mask = alt ? gauge::class_word(alt, alt, 0x04u)
                                       : gauge::class_word(r[0x4Fu], r[0x50u], 0x04u);
        out->emblems = (r[0x08] | (r[0x09] << 8) | (r[0x0A] << 16) |
                        (static_cast<std::uint32_t>(r[0x0B]) << 24)) & mask;
        out->emblem_kind = r[0x5Eu];
    }
    out->support_known = support_gauge(ew, esz, rec_addr, &out->support);
    {
        gauge::Ctx pc{ew, esz};
        unit_portrait(pc, rec_addr, out->portrait_tiles, out->portrait_pal);
    }
    out->alignment = (elemental && al >= 1 && al <= 3) ? al : -1;

    // With the status window's cursor on something the game has a HELP text
    // for, that text, which the second screen shows in place of the unit card.
    if (status_window_unit() == rec_addr) {
        const gauge::Ctx hc{ew, esz};
        const CursorThing t = cursor_thing(hc, rec_addr);
        const Item it = t.item ? item_at(ew, esz, t.item, side1) : Item{};
        if (it.ok) {
            out->help_item = static_cast<int>(t.item);
            help_text(item_help_id(it), out);
        } else if (t.talent) {
            out->help_item = 0x1000 + static_cast<int>(t.talent);
            help_text(rom16(kTalentTable + t.talent * kTalentStride + 0x06u), out);
        } else if (t.help) {
            out->help_item = 0x2000 + static_cast<int>(t.help);
            help_text(t.help, out, &t);
        }
    } else {
        // In battle, the weapon or spell under the ARMS / TALENT list's
        // cursor, for the unit choosing it.
        const gauge::Ctx hc{ew, esz};
        const BattleRow row = battle_list_row(hc);
        const Item it = row.item ? item_at(ew, esz, row.item, side1) : Item{};
        if (row.actor == rec_addr && it.ok) {
            out->help_item = static_cast<int>(row.item);
            help_text(item_help_id(it), out);
        } else if (row.actor == rec_addr && row.talent) {
            out->help_item = 0x1000 + static_cast<int>(row.talent);
            help_text(rom16(kTalentTable + row.talent * kTalentStride + 0x06u), out);
        } else if (move_choice_on_actor() && acting_record() == rec_addr) {
            // The move choice's NAVI prompt, which the game no longer puts up
            // (kol_actions.cpp): here, until the cursor leaves the unit.
            out->help_item = kNaviHelpItem;
            help_text_at(kNaviBank, kNaviMoveChoice, out);
        }
    }

    // A unit that dies has its record cleared (CO: Mulinne's, about four
    // seconds after her HP reaches 0), and a cleared record reads as a name of
    // "AAAA…" (code 0 is 'A') with no HP. Nothing is drawn from it.
    out->valid = out->hp_max > 0;
    return out->valid;
}

void unit_dump_tick() {
    if (!gbarecomp::runtime_excursion_active()) {   // real frames only
        hp_track_tick();
        const int n = g_cursor_out_frames.load(std::memory_order_relaxed);
        g_cursor_out_frames.store(cursor_on_map() ? std::min(n + 1, 1000) : 0,
                                  std::memory_order_relaxed);
    }
    // KOL_SHEET_PROBE=1: one line whenever what the second screen's sheet
    // is chosen from changes (the scene, the game's unit box, the
    // highlighted and acting units, the status window, the cursor).
    if (static const bool probe = std::getenv("KOL_SHEET_PROBE") != nullptr; probe) {
        std::uint32_t esz = 0;
        const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
        UnitInfo shown, bottom, top;
        const bool have = unit_for_highlighted(&shown) && shown.valid;
        auto ew32 = [&](std::uint32_t a) {
            const std::uint32_t o = a - kEwramBase;
            return ew && o + 4u <= esz ? static_cast<std::uint32_t>(ew[o] | ew[o + 1] << 8 |
                                                                    ew[o + 2] << 16 | ew[o + 3] << 24)
                                       : 0u;
        };
        // The lower card's unit and its HP, as the sheet would show them.
        const bool two = have && sheet_units(shown, &top, &bottom);
        sheet_hp(&bottom);
        const int target_hp = two ? bottom.hp : -1;
        char line[480];
        std::snprintf(line, sizeof line,
                      "shown=%08X %-10s battle=%d status=%d box=%d prep=%d hl=%08X win=%08X act=%08X "
                      "cursor=%d in_action=%d ai=%d tgt=%08X bot=%08X thp=%d obj=%08X blow=%08X:%08X ai_pick=%08X:%08X strike=%d dmg=%08X act_blk=%08X:%08X n=%d t0=%08X res=%d:%08X:%X:%d ev=%d",
                      have ? shown.record : 0u, have ? shown.name : "-", is_battle_scene() ? 1 : 0, is_status_scene() ? 1 : 0,
                      unit_panel_rect(nullptr, nullptr, nullptr, nullptr) ? 1 : 0,
                      battle_preparation() ? 1 : 0, highlighted_record(ew, esz),
                      status_window_unit(), acting_record(), cursor_on_map() ? 1 : 0,
                      unit_in_action() ? 1 : 0, ai_phase() ? 1 : 0, acting_target(), two ? bottom.record : 0u, target_hp,
                      acting_object(), ew32(usa(0x0202B710u)), ew32(usa(0x0202B714u)), ew32(usa(0x0203C734u)),
                      ew32(usa(0x0203C738u)), perspective_combat_camera() ? 1 : 0,
                      damage_number_object(), ew32(usa(0x02029D80u)), ew32(usa(0x02029D84u)),
                      static_cast<int>(ew32(usa(0x02029DA0u))), ew32(usa(0x02029DA8u)),
                      static_cast<int>(ew32(usa(0x02029EC0u))), ew32(usa(0x02029EC4u)), ew32(usa(0x02029EC8u)),
                      static_cast<int>(ew32(usa(0x02029ECCu))), static_cast<int>(ew32(usa(0x020013D0u)) & 0xFFu));
        static std::string last;
        static long frame = 0;
        ++frame;
        if (last != line) {
            last = line;
            std::fprintf(stderr, "sheet-probe: %ld %s\n", frame, line);
        }
    }
    static const bool on = [] {
        const char* v = std::getenv("KOL_UNIT_DUMP");
        return v && v[0] && v[0] != '0';
    }();
    if (!on) return;
    UnitInfo u;
    if (!unit_for_highlighted(&u)) return;
    // KOL_SHEET_DUMP=<file.ppm> also writes the second screen's sheet, as
    // rendered from ROM, so its layout can be checked without a device. It
    // is written whenever the picture changes; a "%d" in the name takes the
    // frame number, to keep every picture of an animation (the dying card).
    if (const char* path = std::getenv("KOL_SHEET_DUMP")) {
        static int frame = 0;
        ++frame;
        // KOL_TEST_PRESS_AT=n presses "a button" n frames in (messages that
        // go away on one).
        if (const char* e = std::getenv("KOL_TEST_PRESS_AT"))
            if (std::atoi(e) == frame) note_key_press();
        // KOL_SHEET_EMBLEMS=1 shows the EMBLEM page, KOL_SHEET_TAP="x:y"
        // taps the sheet there.
        if (frame == 1) {
            if (std::getenv("KOL_SHEET_EMBLEMS")) sheet_flip_lower_page();
            float tx = 0, ty = 0;
            if (const char* e = std::getenv("KOL_SHEET_TAP"))
                if (std::sscanf(e, "%f:%f", &tx, &ty) == 2) sheet_tap(tx, ty);
        }
        static std::uint32_t px[kSheetW * kSheetH], last_px[kSheetW * kSheetH];
        if (compose_unit_sheet(u, px, frame / 59.73) &&
            std::memcmp(px, last_px, sizeof px) != 0) {
            std::memcpy(last_px, px, sizeof px);
            char name[512];
            std::snprintf(name, sizeof name, path, frame);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fprintf(f, "P6 %d %d 255\n", kSheetW, kSheetH);
                for (int i = 0; i < kSheetW * kSheetH; ++i) {
                    const unsigned char rgb[3] = {
                        static_cast<unsigned char>(px[i] >> 16),
                        static_cast<unsigned char>(px[i] >> 8),
                        static_cast<unsigned char>(px[i])};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
        }
    }
    static std::uint32_t last = 0, last_target = 0;
    static int last_hp = -1, last_help = -1;
    const std::uint32_t target = cursor_unit_record();
    if (u.record == last && u.hp == last_hp && u.help_item == last_help &&
        target == last_target)
        return;
    last = u.record; last_hp = u.hp; last_help = u.help_item; last_target = target;
    if (target) std::printf("cursor unit @%08X (acting @%08X)\n", target, acting_record());
    auto show = [](const UnitName& n) {
        static char buf[48];
        if (n.bitmap >= 0) std::snprintf(buf, sizeof buf, "<bitmap %02X>", n.bitmap);
        else std::snprintf(buf, sizeof buf, "%s", n.text);
        return buf;
    };
    std::printf("unit @%08X %s  %s  Lv%d EXP%d  HP %d/%d  MP %d/%d\n",
                u.record, u.name, show(u.cls), u.level, u.exp,
                u.hp, u.hp_max, u.mp, u.mp_max);
    std::printf("     move land %d water %d speed %d%s\n", u.move_land, u.move_water,
                u.speed, u.sp ? "  (SP)" : "");
    std::printf("     STR %d(%d) INT %d(%d) AGI %d(%d)  ATK melee %d magic %d"
                "  DEF %d M.DEF %d  element %d alignment %d  support %d%s\n",
                u.str_, u.str_base, u.intel, u.int_base, u.agi, u.agi_base,
                u.atk_melee, u.atk_magic, u.def, u.mdef, u.element, u.alignment,
                u.support, u.support_known ? "" : "?");
    for (int i = 0; i < 4; ++i) std::printf("     equip%d  %s\n", i + 1, show(u.equip[i]));
    for (int i = 0; i < 4; ++i)
        std::printf("     talent%d %s (id %d, kind %d/%d, POW %d, MP %d)\n", i + 1,
                    show(u.talent[i]), u.talent_id[i], u.talent_kind[i],
                    u.talent_sub[i], u.talent_pow[i], u.talent_mp[i]);
    if (u.help_len > 0) {
        // The HELP text in ASCII, near enough to read: '/' ends a line, '|'
        // a page; codes with no letter show as <NN>.
        std::printf("     help (item %d) ", u.help_item);
        for (int i = 0; kRegion != Region::Usa && i < u.help_len; ++i) {
            // (Japan: Latin and digits as themselves, the rest as <code>.)
            const font::Code b = u.help_codes[i];
            char one[12];
            if (b == font::kNewline) std::putchar('/');
            else if (font::is_colour(b)) std::printf("{%u}", b & 0xFFu);
            else { jtext::show(&b, 1, one, sizeof one); std::fputs(one, stdout); }
        }
        for (int i = 0; kRegion == Region::Usa && i < u.help_len; ++i) {
            const font::Code b = u.help_codes[i];
            if (b < 0x1Au) std::putchar('A' + b);
            else if (b < 0x34u) std::putchar('a' + (b - 0x1Au));
            else if (b < 0x3Eu) std::putchar('0' + (b - 0x34u));
            else if (b == 0x89u) std::putchar(' ');
            else if (b == 0x88u) std::putchar('/');
            else if (b == 0x8Eu) std::putchar('|');
            else if (b == 0x8Au || b == 0x8Du) {}
            else std::printf("<%02X>", b);
        }
        std::putchar('\n');
    }
    std::fflush(stdout);
}

int name_bitmap_max_pixels() { return kMaxCells * kCellW * kCellH; }

// ── The status sheet's frame, straight from the cartridge ───────────────
// The sheet is a plain 9-slice on BG0 in palette bank 4: four corners, four
// edges, and a field of four hatched tiles laid in a 4-wide pattern that
// shifts by two every row. Twelve tiles in all, and every one of them is in
// the ROM.
//
// They are NOT stored as a tileset. Consecutive tiles sit 0x20 then 0x21
// apart, so the run has control bytes interleaved -- a bespoke codec, not
// LZ77 (every aligned 0x10 block in the image was decompressed and searched)
// and not RLE. What makes reading them by address sound anyway is that each
// of the twelve occurs EXACTLY ONCE in the 8 MiB image and is fully
// non-blank: these are the assets, not a coincidental byte match. See README
// "Can the artwork come from the ROM?".
//
// Verified by rebuilding the whole 30x20 frame from these twelve plus the
// palette and comparing it against the frame the game draws.
namespace {

constexpr std::uint32_t kSheetPal = usa(0x0861DFE8u);   // BG palette bank 4
// Order: corner TL/TR/BL/BR, edge top/bottom/left/right, field a/b/c/d.
const std::uint32_t kSheetTileRom[12] = {
    usa(0x086FCD51u), usa(0x086FCD71u), usa(0x086FCD92u), usa(0x086FCDB2u),
    usa(0x086FCDD3u), usa(0x086FCDF3u), usa(0x086FCE14u), usa(0x086FCE34u),
    usa(0x086B7834u), usa(0x086B7855u), usa(0x086B7875u), usa(0x086B7896u),
};
constexpr int kSheetTiles = 12;

std::uint32_t bgr555_argb(std::uint16_t c) {
    const std::uint32_t r = (c & 0x1Fu) << 3;
    const std::uint32_t g = ((c >> 5) & 0x1Fu) << 3;
    const std::uint32_t b = ((c >> 10) & 0x1Fu) << 3;
    return 0xFF000000u | ((r | (r >> 5)) << 16) | ((g | (g >> 5)) << 8) |
           (b | (b >> 5));
}

}  // namespace

int sheet_frame_atlas_pixels() { return kSheetTiles * 8 * 8; }


bool sheet_frame_atlas(std::uint32_t* argb, int* w, int* h) {
    if (!argb || !w || !h || !g_rom) return false;
    if (!rom_ok(kSheetPal, 32)) return false;
    std::uint32_t pal[16];
    for (int i = 0; i < 16; ++i) pal[i] = bgr555_argb(rom16(kSheetPal + i * 2u));
    *w = kSheetTiles * 8;
    *h = 8;
    for (int t = 0; t < kSheetTiles; ++t) {
        const std::uint32_t src = kSheetTileRom[t];
        if (!rom_ok(src, 32)) return false;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; x += 2) {
                const std::uint8_t b = rom8(src + static_cast<std::uint32_t>(y * 4 + x / 2));
                argb[y * (*w) + t * 8 + x]     = pal[b & 0xFu];
                argb[y * (*w) + t * 8 + x + 1] = pal[b >> 4];
            }
    }
    return true;
}

const std::uint8_t* unit_data_rom(std::uint32_t* size) {
    if (size) *size = g_rom_size;
    return g_rom;
}

int name_strip_raw(int id, std::uint32_t* rom_addr) {
    if (id < 0 || id > 255 || !g_rom || !rom_addr) return 0;
    const std::uint32_t e = kEscTable + static_cast<std::uint32_t>(id) * 4u;
    const int cells = rom16(e + 2u);
    if (cells <= 0 || cells > kMaxCells) return 0;
    const std::uint32_t base = kEscPixels + rom16(e) * 32u;
    if (!rom_ok(base, static_cast<std::uint32_t>(cells) * 64u)) return 0;
    *rom_addr = base;
    return cells;
}

bool name_bitmap(int id, std::uint32_t* argb, int* w, int* h) {
    if (!argb || !w || !h || id < 0 || id > 255 || !g_rom) return false;
    const std::uint32_t e = kEscTable + static_cast<std::uint32_t>(id) * 4u;
    const std::uint32_t off = rom16(e);
    const int cells = rom16(e + 2u);
    if (cells <= 0 || cells > kMaxCells) return false;
    const std::uint32_t base = kEscPixels + off * 32u;
    const std::uint32_t bytes = static_cast<std::uint32_t>(cells) * 64u;
    if (!rom_ok(base, bytes)) return false;

    const int width = cells * kCellW;
    for (int i = 0; i < width * kCellH; ++i) argb[i] = 0u;
    for (int c = 0; c < cells; ++c) {
        const std::uint32_t cell = base + static_cast<std::uint32_t>(c) * 64u;
        for (int y = 0; y < kCellH; ++y) {
            for (int x = 0; x < kCellW; ++x) {
                const std::uint8_t byte = rom8(cell + y * 4u + x / 2);
                const std::uint8_t idx = (x & 1) ? (byte >> 4) : (byte & 0x0Fu);
                if (!idx) continue;
                // A mask, not the game's colours: the status window's palette
                // is not even loaded while the battle map is up, and a cream
                // parchment strip pasted into this panel would look pasted.
                // The panel tints the alpha in its own text colour.
                const std::uint32_t a = (idx >= 3u) ? 0xFFu : 0x88u;
                argb[y * width + c * kCellW + x] = (a << 24) | 0x00FFFFFFu;
            }
        }
    }
    *w = width; *h = kCellH;
    return true;
}

// ── The shop: who can wear the item on sale, and what it would change ────
// README "The army list in the shop: who can equip it".

// The Buy list's work block, its pointer at 0x02029CAC (0 while no list is
// open: the shop's menu): the open tab +0x498, the tab's item count +0x49A,
// each tab's cursor at +0x1F0 + tab, the tab's item ids from +0x208 (found
// by moving the cursor and the tab on states/thor/0019 and 0023).
//
// +0x49F bit 0 is set in the magic shop (0x08131BA0; some 40 more routines read it):
// its six tabs are the elements and its ids number the talent table, not the
// item table (states/thor/0122: Air Blade is 3, which as an item is a two-handed
// weapon, so a spell showed that weapon's ATK). The tab kinds, +0x200, do not tell
// them apart: kind 0 is the hats in an item shop (states/thor/0020).
// The block outlives the list: its pointer stays after B, with the tab and ids
// as they were, so nothing in it says the list has closed. But +0x49B counts up
// every frame the list runs, from 0 as it opens, through the buy prompt; it
// stops the frame after B (states/thor/0123: A, then B, 443 frames still). It
// skips one frame at a tab change, hence the few frames' grace.
namespace {
std::atomic<unsigned> g_shop_still{0xFFFFu};   // frames since +0x49B moved
constexpr unsigned kShopListGrace = 3u;
}  // namespace
void shop_list_frame() {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    constexpr std::uint32_t kShopPtr = usa(0x02029CACu) - kEwramBase;
    if (!ew || kShopPtr + 4u > esz) return;
    const std::uint32_t p = static_cast<std::uint32_t>(ew[kShopPtr]) | (ew[kShopPtr + 1] << 8) |
                            (ew[kShopPtr + 2] << 16) | (static_cast<std::uint32_t>(ew[kShopPtr + 3]) << 24);
    static int last = -1;
    const int now = (p >= kEwramBase && p - kEwramBase + 0x4A0u <= esz) ? ew[p - kEwramBase + 0x49Bu] : -1;
    const unsigned still = g_shop_still.load(std::memory_order_relaxed);
    g_shop_still.store(now != last && now >= 0 ? 0u : (still < 0xFFFFu ? still + 1u : still), std::memory_order_relaxed);
    last = now;
}

namespace {
// The open list: its block's EWRAM offset, its tab and item count; false with
// none open.
bool shop_list(const std::uint8_t** ew_out, std::uint32_t* o_out, unsigned* tab_out, unsigned* count_out) {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    constexpr std::uint32_t kShopPtr = usa(0x02029CACu) - kEwramBase;
    if (!ew || kShopPtr + 4u > esz) return false;
    const std::uint32_t p = static_cast<std::uint32_t>(ew[kShopPtr]) | (ew[kShopPtr + 1] << 8) |
                            (ew[kShopPtr + 2] << 16) | (static_cast<std::uint32_t>(ew[kShopPtr + 3]) << 24);
    if (p < kEwramBase || p - kEwramBase + 0x4A0u > esz) return false;
    if (g_shop_still.load(std::memory_order_relaxed) > kShopListGrace) return false;
    const std::uint32_t o = p - kEwramBase;
    const unsigned tab = ew[o + 0x498u], count = ew[o + 0x49Au];
    if (tab >= 8u || count == 0u || count > 0x80u) return false;
    *ew_out = ew; *o_out = o; *tab_out = tab; *count_out = count;
    return true;
}
// The row under the cursor: its place in the window, +0x1F0 + tab, and the tab's
// scroll, +0x1F8 + tab (the list object's +0x1A and +0x16 at block +0x994, as the
// game adds them at 0x08133D50). Checked on 0020 (cursor 2, scroll 11: 1000 goth),
// after ten Downs (5, 11: Trident, 1100), 0122. The window alone named the wrong item
// on any list longer than six rows.
unsigned shop_row(const std::uint8_t* ew, std::uint32_t o, unsigned tab) {
    return static_cast<unsigned>(ew[o + 0x1F0u + tab]) + ew[o + 0x1F8u + tab];
}
int shop_tab_entry(bool spells) {
    const std::uint8_t* ew = nullptr;
    std::uint32_t o = 0;
    unsigned tab = 0, count = 0;
    if (!shop_list(&ew, &o, &tab, &count)) return -1;
    if (((ew[o + 0x49Fu] & 1u) != 0u) != spells) return -1;
    const unsigned cur = shop_row(ew, o, tab);
    if (cur >= count) return -1;
    const unsigned id = ew[o + 0x208u + cur];
    return id != 0u ? static_cast<int>(id) : -1;
}
}  // namespace

int shop_cursor_item() { return shop_tab_entry(false); }
int shop_cursor_spell() { return shop_tab_entry(true); }

// Whether the unit can use a spell, as the game's status list colours it and
// its equip refuses it (0x0803D2C0, 0x08032692, the battle menu 0x0807C2BC):
// class field 0x27 (+0x33, through the base class) is a bit of the spell's
// mask, +0x20; then with the byte at 0x020012E0 at 3 or 4, +0x1D bit 2 or 3.
// Only spell kinds 1-6 and 11 (0x0803EE28 answers 2-8) go through this.
bool unit_can_use_spell(std::uint32_t record, int id) {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || record < kEwramBase || record - kEwramBase + kRecSize > esz || id <= 0 || id >= 0xFF) return false;
    const std::uint8_t* r = ew + (record - kEwramBase);
    const std::uint32_t e = kTalentTable + static_cast<std::uint32_t>(id) * kTalentStride;
    const unsigned kind = rom8(e + 0x02u);
    if (!((kind >= 1u && kind <= 6u) || kind == 11u)) return true;
    const unsigned group = gauge::class_field(r[0x4F], r[0x50], 0x33u, false);
    if (group > 15u || !((rom16(e + 0x20u) >> group) & 1u)) return false;
    constexpr std::uint32_t kEwMode = usa(0x020012E0u) - kEwramBase;   // the game mode (kol_region.h)
    const unsigned mode = kEwMode < esz ? ew[kEwMode] : 0u;
    if (mode == 4u && !(rom8(e + 0x1Du) & 8u)) return false;
    if (mode == 3u && !(rom8(e + 0x1Du) & 4u)) return false;
    return true;
}

// A spell's HELP text.
bool spell_help(int id, UnitInfo* out) {
    if (!out || !g_rom) return false;
    *out = UnitInfo{};
    if (id <= 0 || id >= 0xFF) return false;
    out->help_item = 0x1000 + id;
    help_text(rom16(kTalentTable + static_cast<std::uint32_t>(id) * kTalentStride + 0x06u), out);
    return out->help_len > 0;
}
void spell_name(int id, UnitName* out) {
    if (out && id > 0) table_name(kTalentTable, kTalentStride, static_cast<std::uint32_t>(id), out);
}
int spell_price(int id) { return rom16(kTalentTable + static_cast<std::uint32_t>(id) * kTalentStride + 0x04u); }
int spell_icon_kind(int id) { return rom8(kTalentTable + static_cast<std::uint32_t>(id) * kTalentStride + 0x02u); }
int spell_icon_sub(int id) { return rom8(kTalentTable + static_cast<std::uint32_t>(id) * kTalentStride + 0x08u); }
std::uint32_t war_funds() {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || 0x1188u > esz) return 0;
    return static_cast<std::uint32_t>(ew[0x1184]) | (ew[0x1185] << 8) | (ew[0x1186] << 16) |
           (static_cast<std::uint32_t>(ew[0x1187]) << 24);
}
bool shop_spell_free() {
    const std::uint8_t* ew = nullptr;
    std::uint32_t o = 0;
    unsigned tab = 0, count = 0;
    if (!shop_list(&ew, &o, &tab, &count)) return false;
    const unsigned cur = shop_row(ew, o, tab);
    if (cur >= count) return false;
    const unsigned index = ew[o + 0x2ACu + cur];
    if (index == 0xFFu) return false;
    const std::uint32_t table = static_cast<std::uint32_t>(ew[o + 0x0Cu]) | (ew[o + 0x0Du] << 8) | (ew[o + 0x0Eu] << 16) |
                                (static_cast<std::uint32_t>(ew[o + 0x0Fu]) << 24);
    std::uint32_t esz = 0;
    gbarecomp::runtime_ewram_view(&esz);
    const std::uint32_t e = table + index * 4u;
    if (e < kEwramBase || e - kEwramBase + 4u > esz) return false;
    return ew[e - kEwramBase + 1u] > ew[e - kEwramBase + 2u];
}
bool shop_spell_available(int spell) {
    return shop_spell_free() || war_funds() >= static_cast<std::uint32_t>(spell_price(spell));
}
bool shop_item_available(int id) {
    return shop_spell_free() || war_funds() >= static_cast<std::uint32_t>(shop_item_price(id));
}
void shop_item_name(int id, UnitName* out) {
    if (out && id > 0) table_name(kItemTable, kItemStride, static_cast<std::uint32_t>(id), out);
}
int shop_item_price(int id) { return rom16(kItemTable + static_cast<std::uint32_t>(id) * kItemStride + 0x02u); }
bool shop_item_help(int id, UnitInfo* out) {
    if (!out || id <= 0) return false;
    *out = UnitInfo{};
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew) return false;
    const Item it = item_at(ew, esz, static_cast<unsigned>(id), false);
    if (!it.ok) return false;
    out->help_item = id;
    help_text(item_help_id(it), out);
    return out->help_len > 0;
}
// 0x0814165C with the slot's item off: one-handed, at most two weapons and no
// two-handed one; two-handed, the others come off with it (the routine itself
// refuses them); a shield, armour or shoes, no other of the kind; accessories any.
// Same item: nothing to gain. Item 0x7A and the class's rule: unit_can_equip.
bool item_slot_takes(std::uint32_t record, int slot, int id) {
    if (slot < 0 || slot > 3 || !unit_can_equip(record, id)) return false;
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew) return false;
    if (ew[record - kEwramBase + kOffEquip + static_cast<unsigned>(slot)] == static_cast<unsigned>(id)) return false;
    const int cat = item_category(id);
    int one = 0, two = 0, same = 0;
    for (int s = 0; s < 4; ++s) {
        if (s == slot) continue;
        const int held = unit_slot_category(record, s);
        one += held == 1;
        two += held == 2;
        same += held == cat;
    }
    if (cat == 1) return two == 0 && one <= 1;
    if (cat == 2 || cat == 5) return true;
    return same == 0;
}
// IDEAL TYPE (0x08036608) fills only slots at 0, and A on a crossed box does
// nothing: 255 is no slot. Innate talents are the kinds the status list keeps
// out of the spell masks (0x0803EE28 above 8), in its locked colour.
bool spell_slot_takes(std::uint32_t record, int slot, int spell) {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || record < kEwramBase || record - kEwramBase + kRecSize > esz || slot < 0 || slot > 3) return false;
    const unsigned v = ew[record - kEwramBase + 0x64u + static_cast<unsigned>(slot)];
    if (v == 0xFFu || (spell > 0 && v == static_cast<unsigned>(spell))) return false;
    if (v == 0u) return true;
    const unsigned kind = rom8(kTalentTable + v * kTalentStride + 0x02u);
    return (kind >= 1u && kind <= 6u) || kind == 11u;
}
bool unit_has_spell_slot(std::uint32_t record) {
    for (int s = 0; s < 4; ++s)
        if (spell_slot_takes(record, s, 0)) return true;
    return false;
}
bool shop_spell_help(UnitInfo* out) { return spell_help(shop_cursor_spell(), out); }

// An item's slot kind, field 5 (+0x08 of its ROM record): 1 a one-handed
// weapon, 2 a two-handed one, 3 a shield, 4 armour, 5 an accessory, 6 shoes;
// 0 not worn (consumables).
int item_category(int id) {
    if (id <= 0 || static_cast<unsigned>(id) > kMaxRomItem) return 0;
    return rom8(kItemTable + static_cast<std::uint32_t>(id) * kItemStride + 0x08u);
}

// Whether the unit can wear the item at all, as 0x0814165C decides it with
// its slots aside (that routine also refuses a third weapon, a second shield
// and the like, which a swap gets round): class field 0x24 (+0x2F, through
// the base class) 2 wears accessories only, 3 no shoes (the mermaids, the
// user noted, cannot wear shoes), and item 0x7A only with record
// +0x5E == 2. Checked on states/thor/0022 (Aerial, Mermaid: 3; a unit with
// 2 wears four accessories).
bool unit_can_equip(std::uint32_t record, int id) {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || record < kEwramBase || record - kEwramBase + kRecSize > esz) return false;
    const std::uint8_t* r = ew + (record - kEwramBase);
    const int cat = item_category(id);
    if (cat < 1 || cat > 6) return false;
    const unsigned body = gauge::class_field(r[0x4F], r[0x50], 0x2Fu, false);
    if (body == 2u && cat != 5) return false;
    if (body == 3u && cat == 6) return false;
    if (id == 0x7A && cat == 3 && r[0x5E] != 2u) return false;
    return true;
}

// The kind of what the unit wears in a slot (item_category; 0 an empty
// slot, -1 none such).
int unit_slot_category(std::uint32_t record, int slot) {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || slot < 0 || slot > 3 || record < kEwramBase || record - kEwramBase + kRecSize > esz) return -1;
    const std::uint32_t o = record - kEwramBase;
    const unsigned eid = ew[o + kOffEquip + static_cast<unsigned>(slot)];
    if (eid == 0u) return 0;
    const Item it = item_at(ew, esz, eid, made_side1(ew[o + 0x5Du]));
    return it.ok ? item_category(static_cast<int>(it.base)) : -1;
}

// The same with the item in a given slot (a two-handed weapon still clears
// the other weapons).
bool unit_with_item_at(std::uint32_t record, int id, int slot, UnitInfo* out) {
    std::uint32_t esz = 0;
    const std::uint8_t* ew = gbarecomp::runtime_ewram_view(&esz);
    if (!ew || !out || slot < 0 || slot > 3 || record < kEwramBase || record - kEwramBase + kRecSize > esz)
        return false;
    const int cat = item_category(id);
    if (cat < 1 || cat > 6) return false;
    const std::uint32_t o = record - kEwramBase;
    int cats[4] = {0, 0, 0, 0};
    for (int s = 0; s < 4; ++s) cats[s] = unit_slot_category(record, s);
    static thread_local std::vector<std::uint8_t> copy;
    copy.assign(ew, ew + esz);
    copy[o + kOffEquip + static_cast<unsigned>(slot)] = static_cast<std::uint8_t>(id);
    if (cat == 2)
        for (int s = 0; s < 4; ++s)
            if (s != slot && (cats[s] == 1 || cats[s] == 2)) copy[o + kOffEquip + static_cast<unsigned>(s)] = 0u;
    t_ew_what_if = copy.data();
    t_ew_what_if_size = esz;
    const bool ok = unit_for_record(record, out);
    t_ew_what_if = nullptr;
    t_ew_what_if_size = 0;
    return ok;
}

}  // namespace kol
