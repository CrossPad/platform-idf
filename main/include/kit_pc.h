// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file kit_pc.h
 * @brief What a host's MIDI Program Change means for the board's kit: the
 *        decision, with no board in sight.
 *
 * A Program Change is 7 bits, so one message can name 128 slots; Bank Select
 * (CC 0 = MSB, CC 32 = LSB, latched by the caller) extends that. Two ways to
 * read the pair, both pinned by main/tests/test_kit_pc.cpp:
 *
 *  - FLAT:  the program is the slot's position in the board's own kit listing,
 *           in the order and with the ids `KIT_LIST` prints, the bank MSB
 *           selecting the next block of 128. What the host sends is therefore
 *           readable straight off the listing, and says nothing about the kind
 *           of kit.
 *  - GROUP: the bank MSB picks a kind (0 performance, 1 drums, 2 slice,
 *           3 pitch, 4 the user's own) and the program is the position inside
 *           that kind -- the buckets the kit browser itself uses, so a setlist
 *           can be written per kind without knowing how many kits of each kind
 *           a particular card holds.
 *
 * A slot that exists but is not loadable (a deleted kit keeps its id slot; a kit
 * the boot scan has not parsed yet) is refused with a reason rather than
 * skipped: skipping would move every later program up one and let the host's
 * setlist drift under itself. In GROUP the position counts only the slots that
 * are kits at all, because "the third drum kit on the card" cannot mean a hole
 * where a drum kit used to be; the map text a host reads is built the same way
 * each mode resolves, which is what the difference is worth. The listing is not a snapshot of a locked table:
 * the caller hands in an accessor over whatever the kit manager holds at the
 * moment of the request, and a re-scan (kits copied to the card, then reboot)
 * moves it -- exactly as it already moves kit ids.
 *
 * Header-only, allocation-free, and free of ESP / LVGL / FreeRTOS includes on
 * purpose, so a host test compiles it as it is and a Program Change arriving at
 * 3 a.m. on a busy card allocates nothing. Threading: pure reads of caller-owned
 * data; nothing here takes a lock, posts to a task or touches a kit model.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace crosspad {
namespace kitpc {

enum class Mode : uint8_t {
    Off = 0,   ///< a Program Change means no more than it did before
    Flat = 1,  ///< program + bank MSB = the position in the kit listing
    Group = 2, ///< bank MSB = the kind, program = the position inside it
};

/// The kit browser's buckets, in the order it shows them.
enum class Group : uint8_t { Perf = 0, Drums = 1, Slice = 2, Pitch = 3, User = 4, Count = 5 };

/// One entry of the board's kit listing, flattened to what a decision needs.
struct Slot {
    int id = -1;             ///< the id a load request names (KIT_LIST's number)
    Group group = Group::Perf;
    bool loadable = false;   ///< there is a kit here that could be loaded now
    const char *name = nullptr; ///< may be null; only the map text reads it
};

/// Why a request ended as it did. Each of these is worth counting, because the
/// failure mode of a feature like this is a setlist that silently does nothing.
enum class Why : uint8_t {
    Ok = 0,
    Off,          ///< the feature is switched off
    Channel,      ///< the message is on a channel the filter does not accept
    OutOfRange,   ///< nothing at that position, or a bank no group names
    Empty,        ///< nothing has been scanned yet
    NotLoadable,  ///< the slot is there but is not a kit one can load now
};

struct Decision {
    int kitId = -1;  ///< the kit to load, or -1 when nothing is to be done
    int index = -1;  ///< the position the pair resolved to, for the log
    Why why = Why::Off;
};

inline const char *modeName(Mode m)
{
    return m == Mode::Flat ? "flat" : m == Mode::Group ? "group" : "off";
}

inline const char *whyName(Why w)
{
    switch (w) {
    case Why::Ok:         return "ok";
    case Why::Off:        return "off";
    case Why::Channel:    return "channel";
    case Why::OutOfRange: return "out_of_range";
    case Why::Empty:      return "no_kits";
    default:              return "not_loadable";
    }
}

inline const char *groupName(Group g)
{
    switch (g) {
    case Group::Perf:  return "perf";
    case Group::Drums: return "drums";
    case Group::Slice: return "slice";
    case Group::Pitch: return "pitch";
    case Group::User:  return "user";
    default:           return "?";
    }
}

/**
 * Resolve one wire pair to a kit id.
 *
 * @p listing is anything with `count()` and `at(size_t)` returning a Slot --
 * the board's kit manager on the device, a vector in the test. It is read once
 * per mode and never cached, so the caller decides how long the kit model stays
 * valid (on the board that is one pass over a listing it is not allowed to
 * walk during a card scan; the caller checks that before coming here).
 *
 * @param filterCh 0..15 to accept a single channel, -1 for any
 * @param msgCh    the channel the message arrived on, 0..15
 * @param msb      Bank Select MSB (CC 0) latched for that channel
 * @param lsb      Bank Select LSB (CC 32); in FLAT it is not part of the
 *                 address (128 blocks of 128 would outgrow any listing a board
 *                 can scan), in GROUP it is ignored. It is carried so the
 *                 readout can say what the host last sent.
 * @param program  the Program Change number, 0..127
 */
template <class Listing>
inline Decision resolve(const Listing &listing, Mode mode, int filterCh, int msgCh,
                        uint8_t msb, uint8_t lsb, uint8_t program)
{
    Decision d;
    if (mode == Mode::Off) {
        d.why = Why::Off;
        return d;
    }
    if (filterCh >= 0 && filterCh != msgCh) {
        d.why = Why::Channel;
        return d;
    }
    const size_t n = listing.count();
    if (!n) {
        d.why = Why::Empty;
        return d;
    }

    if (mode == Mode::Flat) {
        // The nth slot of the listing, counting deleted ones: nothing shifts.
        const size_t want = static_cast<size_t>(msb) * 128u + program;
        if (want >= n) {
            d.why = Why::OutOfRange;
            return d;
        }
        const Slot s = listing.at(want);
        d.index = static_cast<int>(want);
        d.kitId = s.id;
        d.why = s.loadable ? Why::Ok : Why::NotLoadable;
        return d;
    }

    // GROUP: the MSB names the kind, the program the position inside that kind.
    // The two modes count differently on purpose, and the map text shows each the
    // same way it resolves: in FLAT a position is the listing's own slot, so a
    // deleted kit stays in place and is refused by name; in GROUP a position is
    // "the nth kit of that kind", and a slot that is not a kit at all has no kind
    // to be counted in. (main/tests/test_kit_pc.cpp pins both halves.)
    if (msb >= static_cast<uint8_t>(Group::Count)) {
        d.why = Why::OutOfRange;
        return d;
    }
    (void)lsb;
    const Group want = static_cast<Group>(msb);
    size_t seen = 0;
    for (size_t i = 0; i < n; ++i) {
        const Slot s = listing.at(i);
        if (!s.loadable || s.group != want) continue;
        if (seen == program) {
            d.index = static_cast<int>(seen);
            d.kitId = s.id;
            d.why = s.loadable ? Why::Ok : Why::NotLoadable;
            return d;
        }
        ++seen;
    }
    d.why = Why::OutOfRange;
    return d;
}

/**
 * How many slots the active mode can address at all -- the size of the map a
 * host could use, so `KIT_PC STATUS` can say "112 of 121 reachable" instead of
 * leaving the host to discover it by sending programs into silence.
 */
template <class Listing>
inline size_t addressable(const Listing &listing, Mode mode)
{
    const size_t n = listing.count();
    if (mode == Mode::Off || !n) return 0;
    if (mode == Mode::Flat) return n;
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        if (listing.at(i).loadable) ++c;
    }
    return c;
}

/// The slot a position of the active mode's ordering names; an empty Slot
/// (id < 0) when the position is past the end. In GROUP the ordering is
/// kind by kind and only kits are counted.
template <class Listing>
inline Slot slotAt(const Listing &listing, Mode mode, size_t position)
{
    if (mode != Mode::Group) {
        return position < listing.count() ? listing.at(position) : Slot{};
    }
    size_t seen = 0;
    for (uint8_t g = 0; g < static_cast<uint8_t>(Group::Count); ++g) {
        for (size_t i = 0; i < listing.count(); ++i) {
            const Slot s = listing.at(i);
            if (!s.loadable || s.group != static_cast<Group>(g)) continue;
            if (seen == position) return s;
            ++seen;
        }
    }
    return Slot{};
}

/**
 * The (bank MSB, program) pair a host must send to reach `position` of the
 * active mode's ordering, or false when the position is past the end. This is
 * the same ordering `resolve()` walks, so a host that reads the map and sends
 * what it reads cannot land somewhere else -- which is the whole reason the map
 * exists.
 */
template <class Listing>
inline bool pairAt(const Listing &listing, Mode mode, size_t position, uint8_t *msb,
                   uint8_t *pc)
{
    if (!msb || !pc) return false;
    if (mode != Mode::Group) {
        if (position >= listing.count()) return false;
        *msb = static_cast<uint8_t>(position / 128u);
        *pc = static_cast<uint8_t>(position % 128u);
        return true;
    }
    size_t seen = 0;
    for (uint8_t g = 0; g < static_cast<uint8_t>(Group::Count); ++g) {
        size_t within = 0;
        for (size_t i = 0; i < listing.count(); ++i) {
            const Slot s = listing.at(i);
            if (!s.loadable || s.group != static_cast<Group>(g)) continue;
            if (seen == position) {
                *msb = g;
                *pc = static_cast<uint8_t>(within);
                return true;
            }
            ++seen;
            ++within;
        }
    }
    return false;
}

/**
 * Which position of the active mode's ordering names this kit id, or -1 when the
 * listing holds no such kit or the kit is not reachable in this mode. The inverse
 * of `pairAt`, and deliberately walked the same way `resolve` walks so that what
 * the board announces about a kit is the addressing a host would have sent to get
 * that kit -- a setlist and the board's own announcements cannot disagree.
 */
template <class Listing>
inline int positionOf(const Listing &listing, Mode mode, int kitId)
{
    if (mode == Mode::Off || kitId < 0) return -1;
    if (mode != Mode::Group) {
        for (size_t i = 0; i < listing.count(); ++i) {
            if (listing.at(i).id == kitId) return static_cast<int>(i);
        }
        return -1;
    }
    size_t seen = 0;
    for (uint8_t g = 0; g < static_cast<uint8_t>(Group::Count); ++g) {
        for (size_t i = 0; i < listing.count(); ++i) {
            const Slot s = listing.at(i);
            if (!s.loadable || s.group != static_cast<Group>(g)) continue;
            if (s.id == kitId) return static_cast<int>(seen);
            ++seen;
        }
    }
    return -1;
}

/**
 * One row of the map, as one line: `KITPCMAP: msb=<bank> pc=<program> id=<kit
 * id> <kind|kit|empty> "<name>"`, or an `out` left empty when the position is
 * past the end. One line per entry, the way `KITS:` answers KIT_LIST: a reply
 * the CDC FIFO has to split into 128-byte pieces must not lose the middle of a
 * row, and the host can read it line by line.
 *
 * A kit name is written with every byte below 0x21 and every quote replaced by
 * a space: names come off a card a person wrote, and a stray newline in one
 * would split the reply into lines nobody asked for. A name longer than 40
 * bytes is cut -- the line has a fixed size on purpose.
 */
template <class Listing>
inline bool formatRow(const Listing &listing, Mode mode, size_t position, char *out, size_t len)
{
    if (!out || !len) return false;
    out[0] = '\0';
    uint8_t msb = 0, pc = 0;
    if (!pairAt(listing, mode, position, &msb, &pc)) return false;
    const Slot s = slotAt(listing, mode, position);
    char safe[41];
    size_t j = 0;
    const char *src = s.name ? s.name : "";
    for (; *src && j + 1 < sizeof(safe); ++src, ++j) {
        const unsigned char c = static_cast<unsigned char>(*src);
        safe[j] = (c < 0x21u || c == '"') ? ' ' : static_cast<char>(c);
    }
    safe[j] = '\0';
    const char *tag = mode == Mode::Group ? groupName(s.group) : (s.loadable ? "kit" : "empty");
    const int put = snprintf(out, len, "KITPCMAP: msb=%u pc=%u id=%d %s \"%s\"%s", (unsigned)msb,
                             (unsigned)pc, s.id, tag, safe,
                             (mode != Mode::Group && !s.loadable) ? "!" : "");
    if (put <= 0 || static_cast<size_t>(put) >= len) {
        out[0] = '\0';        // nothing half-written goes to the wire
        return false;
    }
    return true;
}

}  // namespace kitpc
}  // namespace crosspad
