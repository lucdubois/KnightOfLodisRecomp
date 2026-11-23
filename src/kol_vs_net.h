#pragma once

// VS Mode over the local network (desktop; Options > Gameplay > VS Mode =
// Network). kol_vs_cpu.cpp runs the session; this is its network part and
// the button timeline both machines share.
//
// Each machine runs BOTH games: its player's, seen, and an unseen copy of
// the other player's, linked to it locally as two GBAs on a cable (the 14
// transfers a frame the game makes stay inside the machine). Only buttons
// cross the network: each game's buttons for frame f are its player's from
// frame f - kDelay, sent to the other machine, where the copy of that game
// takes them at the same frame. Both copies of a game start from the same
// snapshot (taken on VS's Connecting screen, sent with its save and its
// settings) and take the same buttons at the same frames, so they stay the
// same game; a hash of each game's memory every 64 frames, compared across
// the machines, says if they do not.
//
// Finding each other: while held on Connecting, each game says so on the
// local network (UDP broadcast, port 19873, every 0.5 s) with a TCP port of
// its own; the one with the smaller random number connects to the other.
// They swap hellos (ROM, BIOS and mod fingerprints must match), snapshots,
// saves and settings; the smaller number's game is the link's parent.

#include <cstdint>
#include <string>
#include <vector>

namespace kol {

// This process is the unseen copy of the other player's game
// (KOL_VS_MIRROR=1, set by the session).
bool vs_mirror_child();

namespace vsnet {

// What each side sends the other.
struct Side {
    std::uint64_t nonce = 0;   // random, per try: the smaller one's game is the link's parent
    std::string kind = "vs";   // the link menu's choice, "vs" or "exchange": both must have made the same
    std::string name;          // the computer's name, for the notices
    std::string rom, bios, mod;   // fingerprints: they must match
    std::uint64_t frame = 0;   // the snapshot's PPU frame count
    std::uint64_t clock = 0;   // and its guest cycle
    unsigned panel_w = 0, panel_h = 0;   // the screen (the copy's view sizes follow it)
    std::string cfg;           // its enhancements.cfg ("" none)
    std::vector<std::uint8_t> state, save;
};

// A file's fingerprint (FNV-1a 64, hex), "none" for an empty path.
std::string file_fingerprint(const std::string& path);

enum class Status { Idle, Searching, Exchanging, Ready, Failed };

// Starts looking for another game held on Connecting on this network, in
// the background. False when no socket could be had.
bool start(Side mine);
Status status();
const std::string& failure();   // why it failed (Failed)
const Side& peer();             // the other side (Ready)
// The channel to the copy of the peer's game (a local TCP socket): the
// peer's buttons go to it as they come, those that came before it first,
// and the copy's hashes come back on it (play_tick). The session owns it
// from here: closed when replaced, stopped, or when the peer goes (the
// copy's wait for buttons then ends, and so does the copy).
void set_key_sink(int fd);
// During the match: this game's buttons for a frame, its hash at a frame.
void send_keys(std::uint64_t frame, std::uint16_t keys);
void send_hash(std::uint64_t frame, std::uint64_t hash);
// The peer's hash of its own game at `frame`, taken (once).
bool take_peer_hash(std::uint64_t frame, std::uint64_t* hash);
// The other side closed, said goodbye, or went silent for 15 s.
bool peer_gone(std::string* why);
// Ends it (a goodbye to the peer), any time; then Idle.
void stop(const char* why);
// Called from the session's thread when the other side goes (closed, a
// goodbye, or 15 s of silence): async-signal-safe work only.
void set_on_gone(void (*fn)());

// The seen game's pad reads (the main loop's, right before it reads
// KEYINPUT), on the guest thread: fn runs at each one, before the frame's
// logic. kol_vs_cpu takes VS's and Exchange's snapshots there: the same
// instruction of the game on every machine (2026-11-23).
void set_pad_read_hook(void (*fn)());

// The button timeline, the seen game's side (numbered by the game's pad
// reads: kol_vs_net.cpp). From the snapshot on, the game sees only what
// this sets (the key filter returns it): begin() at the snapshot with
// KEYINPUT then; reset_calls() as the snapshot is loaded back (the match
// starts there, as its copy over there does); tick() each frame (the copy's
// hashes compared); end() lets the player's buttons through again.
void play_begin(std::uint16_t keyinput, const std::string& session_dir);
void play_start_keys();   // the first reads' buttons (none), once the peer is there
void play_reset_calls();
void play_tick(std::uint64_t frame);
std::uint16_t play_filter(std::uint16_t keys);
bool play_active();
// A desync found (the two machines' copies of a game differ), with where.
bool play_desync(std::string* what);
void play_end();

// The unseen copy: its settings from the session folder (main, before the
// runtime starts), its key reader, filter and pad hook (install).
void mirror_settings();
void mirror_install();
void mirror_tick(std::uint64_t frame);

// The memory both copies of a game must agree on: EWRAM and IWRAM (taken
// at a pad read: the same instruction in both).
std::uint64_t game_hash();

}  // namespace vsnet
}  // namespace kol
