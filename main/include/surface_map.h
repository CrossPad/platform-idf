// SPDX-License-Identifier: GPL-3.0.0-or-later
#pragma once

/**
 * @file surface_map.h
 * @brief The SysEx control surface's address space and the decisions in it, with no
 *        board in sight: what a frame may name, what a value means, and when a
 *        "give it back" is allowed to touch a pad at all.
 *
 * The shape follows how Roland and Novation put a control surface on the wire -- an
 * address space written in blocks -- because that is what keeps the traffic cheap for
 * the thing a surface actually does: many related values at once. One frame paints
 * sixteen pads from a palette the host loaded once (`SEC_WRITE`, 24 bytes on the
 * wire); the same sixteen pads as direct colour is 56 bytes; one pad per frame is 16
 * frames of 9 bytes = 144; the same with NRPN would be 192 bytes of Control Changes
 * and could not carry colour at all. Numbers like that belong to the transport, not to
 * taste, so the byte counts are stated in docs/subsystems/surface-sysex.md with the
 * frames written out, and the frame lengths are pinned by main/tests here.
 *
 * NRPN is not the wrong answer in general -- it is the right answer for one parameter
 * a DAW automates, because it needs no vendor framing, is per channel, is 14-bit, and
 * shows up in an automation lane as something a person can pick. For a grid of pads it
 * is four messages per pad. This surface therefore keeps both doors: the Control Change
 * map (main/host_midi.cpp) is what a DAW rides one fader at a time, and the address
 * writes here are what an animation or a control page paints at once. A value written
 * through either is the same value at the same place -- the level sections name the
 * same targets the CC map points at -- so a host is not choosing between two
 * representations of a parameter.
 *
 * Restoring is where a surface most often lies. A host that painted pads asks for its
 * write undone, and the honest answer is not "put back what I saw before" but "put back
 * what I saw before *if nothing else has touched it since*": the board's own apps light
 * pads for what is playing (docs/subsystems/midi-input.md), so a restore after a
 * fill landed would otherwise stamp a colour over a pad that is telling the player
 * something. `Slot`/`handBack` below encode that rule, and the host test pins the case
 * that matters: a pad the board repainted is left alone and counted, not restored.
 *
 * Sections the board cannot answer honestly are marked not restorable rather than
 * faked: a pad's live mixer level has no getter (`crosspad-core`'s `IPadMix` offers
 * `setPadMix` and no read-back), so there is nothing to compare against and nothing to
 * put back, and a "restore" of it would be a guess.
 *
 * Threading: pure functions over caller-owned bytes. Nothing here allocates, locks,
 * touches the strip, the mixer or the card. The applying half lives in
 * main/host_midi.cpp, whose contract (park on the MIDI task, act on the worker) the
 * frames this decodes are handed to.
 */

#include <cstddef>
#include <cstdint>

namespace crosspad {
namespace surface {

/// Why a frame was refused. Each of these is worth a counter and a console line: the
/// failure mode of a surface like this is a host writing into silence.
enum class Bad : uint8_t {
    Ok = 0,
    TooShort,        ///< the frame does not hold what the sub says it does
    UnknownSection,  ///< no such address space on this board
    BadCount,        ///< zero or more than one block's worth of values
    BadRange,        ///< start + count runs past the end of the section
    BadValue,        ///< a value outside what the section accepts
};

inline const char *badName(Bad b)
{
    switch (b) {
    case Bad::Ok:         return "ok";
    case Bad::TooShort:   return "too_short";
    case Bad::UnknownSection: return "unknown_section";
    case Bad::BadCount:   return "bad_count";
    case Bad::BadRange:   return "bad_range";
    default:              return "bad_value";
    }
}

/**
 * The address spaces. Values are 7-bit (0..127) unless the section says otherwise;
 * a level is scaled by the same curve the Control Change map uses (`levelFrom7`), so
 * the number a host writes means the same thing whichever door it came in by.
 */
enum class Sec : uint8_t {
    PadMode = 0x00,   ///< per pad: 0 = the board's own colours again, 1 = held host colour
    PadColour = 0x01, ///< per pad: a palette index (see PalLoad), 0..31
    PadMix = 0x02,    ///< per pad: live level, no read-back (not restorable)
    ChVolume = 0x03,  ///< per mixer channel: volume, 0 at addr 0
    ChPan = 0x04,     ///< per mixer channel: balance, 64 = centre
    OutVolume = 0x05, ///< per mixer output bus: volume
    HwLevel = 0x06,   ///< per hardware output: the level after the DAC
    Anim = 0x07,      ///< addr 0 = which (0 off, 1 chase, 2 ring, 3 strobe); addr 1 = period/10 ms
    Kit = 0x08,       ///< addr 0 = bank MSB, addr 1 = program: the pair `KIT_PC MAP` prints
    Lease = 0x09,     ///< addr 0 = pad-LED lease timeout in 100 ms units (0 = hold until told)
    PalLoad = 0x0A,   ///< palette entries, written as triples by their own sub
    Count = 0x0B,
};

/// How many addresses a section has. 16 for the pads and the mixer's channels, which
/// is what the board can hold; the outputs and the one-value spaces are smaller on
/// purpose, so a frame naming address 20 of `HwLevel` is refused rather than ignored.
inline int secAddrs(Sec s)
{
    switch (s) {
    case Sec::PadMode: case Sec::PadColour: case Sec::PadMix:
    case Sec::ChVolume: case Sec::ChPan:
        return 16;
    case Sec::OutVolume: case Sec::HwLevel:
        return 8;
    case Sec::Anim: case Sec::Kit:
        return 2;
    case Sec::Lease:
        return 1;
    default:
        return 0;
    }
}

inline const char *secName(Sec s)
{
    switch (s) {
    case Sec::PadMode:    return "pad_mode";
    case Sec::PadColour:  return "pad_colour";
    case Sec::PadMix:     return "pad_mix";
    case Sec::ChVolume:   return "ch_volume";
    case Sec::ChPan:      return "ch_pan";
    case Sec::OutVolume:  return "out_volume";
    case Sec::HwLevel:    return "hw_level";
    case Sec::Anim:       return "anim";
    case Sec::Kit:        return "kit";
    case Sec::Lease:      return "lease";
    case Sec::PalLoad:    return "palette";
    default:              return "?";
    }
}

inline bool secKnown(uint8_t wire) { return wire < static_cast<uint8_t>(Sec::Count); }

/**
 * Whether a section can be given back honestly. A pad's live mixer level cannot: the
 * mixer offers no way to read it, so there is nothing to compare against and nothing to
 * put back -- a restore would be the board inventing a value. Everything else here has
 * either a getter (the levels) or a read of the pad's own colour (`getPadColor`), which
 * is what makes the "still mine?" rule possible at all.
 */
inline bool secRestorable(Sec s)
{
    /* Kit is on this list for a threading reason, not a honesty-about-values reason: the
     * board knows which kit it is on, but undoing a kit means loading the previous one,
     * which needs the kit listing -- and the listing belongs to the UI task, while a
     * restore is applied on the worker. Rather than accept a frame and quietly do
     * nothing with it, the grammar refuses it: the pair is writable, and a host that
     * wants its setlist undone keeps its own record of the pair it came from, which is
     * the same information in one byte less than a round trip would cost. */
    return s != Sec::PadMix && s != Sec::PalLoad && s != Sec::Lease && s != Sec::Kit
           && s != Sec::Count;
}

inline int secMaxValue(Sec s)
{
    switch (s) {
    case Sec::PadMode:   return 1;   ///< 0 = back to the board, 1 = host holds it
    case Sec::PadColour: return 31;  ///< palette entries
    case Sec::Anim:      return 127; ///< `which` is 0..3; checked by the caller per address
    default:             return 127;
    }
}

/// The largest block one frame carries. 16 because that is a grid of pads, and a frame
/// that wanted more would be a different design, not a bigger one.
constexpr int kMaxBlock = 16;

/// Palette entries a host may load and address pads by. 32 entries is what a
/// Launchpad-style colour table holds and what one load frame carries (32 x 3 = 96
/// seven-bit bytes, half of a MIDI-SysEx-sized frame).
constexpr int kPalette = 32;

/// One decoded block write.
struct Write {
    Sec sec = Sec::PadMode;
    int start = 0;
    int n = 0;
    uint8_t v[kMaxBlock] = {0};
};

/**
 * What a decoded frame becomes for the parking side: one block write, one restore,
 * or one direct-colour block. Plain fixed data, because the SysEx handler fills it in
 * on the MIDI task and the worker reads it later -- a struct defined inside one file and
 * copied in by another is how a field insertion becomes a silent misread, so the layout
 * lives here where the host test can pin its size and a byte round trip.
 */
enum class Kind : uint8_t { Write = 0, Restore = 1, Rgb = 2 };

struct Action {
    Kind kind = Kind::Write;
    Sec sec = Sec::PadMode;
    uint8_t start = 0;
    uint8_t n = 0;
    uint8_t v[kMaxBlock * 3] = {0};   ///< values, or r,g,b triples for Kind::Rgb
};

/// A range a restore is asked for.
struct Range {
    Sec sec = Sec::PadMode;
    int start = 0;
    int n = 0;
};

/// `a` is the frame's payload after `F0 <manufacturer> <cmd> <sub>`; `len` counts it
/// including the sub byte, the way main/audio_route_control.cpp:383-449 does for every
/// other sub of that command, so the length checks read the same everywhere.
inline Bad decodeWrite(const uint8_t *a, unsigned len, Write *out)
{
    if (!a || !out || len < 4) return Bad::TooShort;   // sub, sec, start, count
    if (!secKnown(a[1])) return Bad::UnknownSection;
    const Sec sec = static_cast<Sec>(a[1]);
    const int count = a[3] & 0x7F;
    if (count < 1 || count > kMaxBlock) return Bad::BadCount;
    const int start = a[2] & 0x7F;
    if (start + count > secAddrs(sec)) return Bad::BadRange;
    if (len < 4u + static_cast<unsigned>(count)) return Bad::TooShort;
    out->sec = sec;
    out->start = start;
    out->n = count;
    for (int i = 0; i < count; ++i) {
        const int v = a[4 + i] & 0x7F;
        if (sec == Sec::Anim) {
            // `which` is a small enum, the period is in 10 ms units up to 1270 ms;
            // both are checked against their own address because one frame writes
            // both at once.
            const int addr = start + i;
            if (addr == 0 && v > 3) return Bad::BadValue;
        } else if (v > secMaxValue(sec)) {
            return Bad::BadValue;
        }
        out->v[i] = static_cast<uint8_t>(v);
    }
    return Bad::Ok;
}

inline Bad decodeRestore(const uint8_t *a, unsigned len, Range *out)
{
    if (!a || !out || len < 4) return Bad::TooShort;
    if (!secKnown(a[1])) return Bad::UnknownSection;
    const Sec sec = static_cast<Sec>(a[1]);
    if (!secRestorable(sec)) return Bad::UnknownSection;   // no honest answer for this one
    const int count = a[3] & 0x7F;
    if (count < 1 || count > kMaxBlock) return Bad::BadCount;
    const int start = a[2] & 0x7F;
    if (start + count > secAddrs(sec)) return Bad::BadRange;
    out->sec = sec;
    out->start = start;
    out->n = count;
    return Bad::Ok;
}

/// A palette load: `index, count, (r,g,b) x count` -- 7-bit triples, which the board
/// doubles into the 8-bit the strip wants (the same convention as the existing
/// `LED_PAD`/`LED_GRID` frames: main/audio_route_control.cpp:432-446).
inline Bad decodePalette(const uint8_t *a, unsigned len, int *index, int *n,
                         uint8_t *rgb /* 3 * kMaxBlock */)
{
    if (!a || !index || !n || !rgb || len < 4) return Bad::TooShort;
    const int start = a[1] & 0x7F;
    const int count = a[2] & 0x7F;
    if (count < 1 || count > kMaxBlock) return Bad::BadCount;
    if (start + count > kPalette) return Bad::BadRange;
    if (len < 3u + 3u * static_cast<unsigned>(count)) return Bad::TooShort;
    for (int i = 0; i < 3 * count; ++i) rgb[i] = a[3 + i] & 0x7F;
    *index = start;
    *n = count;
    return Bad::Ok;
}

/// A direct-colour write: `start, count, (r,g,b) x count` for pads. 7-bit per
/// component, doubled by the board -- half the resolution of the pads' physical
/// colour, and the price of a 7-bit wire.
inline Bad decodeRgb(const uint8_t *a, unsigned len, int *start, int *n,
                     uint8_t *rgb /* 3 * kMaxBlock */)
{
    if (!a || !start || !n || !rgb || len < 3) return Bad::TooShort;
    const int s = a[1] & 0x7F;
    const int count = a[2] & 0x7F;
    if (count < 1 || count > kMaxBlock) return Bad::BadCount;
    if (s + count > 16) return Bad::BadRange;
    if (len < 3u + 3u * static_cast<unsigned>(count)) return Bad::TooShort;
    for (int i = 0; i < 3 * count; ++i) rgb[i] = a[3 + i] & 0x7F;
    *start = s;
    *n = count;
    return Bad::Ok;
}

// ── byte counts, counted the way the wire counts them, so the documentation
// cannot drift. Every one is the whole message: F0, the manufacturer and command
// bytes, the sub, the addressing, the payload, F7. (USB-MIDI carries messages in
// four-byte packets, so on USB the packets round these up; a TRS line carries the
// bytes themselves, at 31250 baud, and the ratios below are what decide which form
// an animation host can afford at a given frame rate.)
//
// Paint sixteen pads by palette index: F0 7D 1D 11 <sec> <start> <n> x16 F7
constexpr int kBytesGridFromPalette = 3 + 1 + 3 + 16 + 1;   // 24
// The same sixteen pads as direct colour: F0 7D 1D 12 <start> <n> x48 F7
constexpr int kBytesGridDirectRgb = 3 + 1 + 2 + 48 + 1;     // 55
// One pad per frame (`LED_PAD`, the form that predates this address space):
// F0 7D 1D 0D <pad> <r> <g> <b> F7, sixteen times.
constexpr int kBytesOnePadPerFrame = 9 * 16;                // 144
// NRPN, four Control Changes of three bytes (101, 102, 6 data-MSB, 38 data-LSB) for
// each of sixteen pads. It cannot carry a colour at all -- 14 bits is not RGB -- which
// is the other half of why the grid is written by SysEx here and ridden by CC one
// fader at a time.
constexpr int kBytesNrpnPerGrid = 4 * 3 * 16;               // 192

// ── the restore bookkeeping ─────────────────────────────────────────────────
//
// One slot per (section, address): what the surface left the value at, and what the
// board stood at before the surface first touched it. A second write in the same run
// does not move `before`, so giving back is the board's value from before the surface
// started, not wherever the last frame happened to stop -- the same reasoning as
// `hostmidi::Control` for the Control Change map, kept as its own type because the
// surface addresses a parameter directly while the CC map addresses it through a rule.
constexpr int16_t kNoSlot = -32768;

struct Slot {
    int16_t wrote = kNoSlot;   ///< the value the surface left here, in wire units
    int16_t before = kNoSlot;  ///< what the board stood at before the first write
};

inline void claim(Slot &s, int16_t value, int16_t was)
{
    if (s.wrote == kNoSlot) s.before = was;
    s.wrote = value;
}

/// Still what the surface left it at? The board's own apps, another host, or a verb
/// at the panel can all have moved it since, and a restore that ignores that is a
/// surface talking over the board.
inline bool stillMine(const Slot &s, int16_t currentNow)
{
    return s.wrote != kNoSlot && currentNow == s.wrote;
}

/// Hand the address back if it is still the surface's, and only then. Returns false
/// and changes nothing when the value has been taken over -- the caller counts that
/// as a skip and says so, because silence is what makes this class of bug invisible.
inline bool handBack(Slot &s, int16_t currentNow, int16_t *into)
{
    if (!stillMine(s, currentNow)) return false;
    if (into) *into = s.before;
    s.wrote = kNoSlot;
    s.before = kNoSlot;
    return true;
}

inline void clearSlot(Slot &s)
{
    s.wrote = kNoSlot;
    s.before = kNoSlot;
}

}  // namespace surface
}  // namespace crosspad
