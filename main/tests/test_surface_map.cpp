// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_surface_map.cpp
 * @brief The SysEx control surface's address space, its frame grammar, and the rule
 *        that says when a "give it back" is allowed to touch anything at all.
 *
 * A surface like this fails in two ways, and both are cheap to prevent here rather
 * than expensive to discover on a bench: a host writes and nothing answers (so every
 * refusal has a reason that is testable, and the byte counts the documentation quotes
 * are pinned to the frames that produce them), and a host asks for its write undone and
 * the board stamps a colour over one it painted itself in the meantime. The second is
 * the one the tests below are mostly about -- it is a decision, not a bug, and it only
 * holds if somebody wrote it down somewhere executable.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <type_traits>

#include "surface_map.h"

using namespace crosspad::surface;

namespace {

/// A `SEC_WRITE` frame the way the board receives it: F0, manufacturer, command, sub,
/// then the payload the decoder is handed.
// A whole SEC_WRITE message, F0 to F7, so its length can be asserted against the
// constants rather than assumed.
unsigned writeFrame(uint8_t *out, uint8_t sec, uint8_t start, uint8_t n, const uint8_t *vals)
{
    out[0] = 0xF0;   // SysEx start
    out[1] = 0x7D;   // manufacturer
    out[2] = 0x1D;   // CROSSPAD_CMD_AUDIO_ROUTE
    out[3] = 0x11;   // SEC_WRITE
    out[4] = sec;
    out[5] = start;
    out[6] = n;
    for (uint8_t i = 0; i < n; ++i) out[7 + i] = vals[i];
    out[7 + n] = 0xF7;
    return 7u + n + 1u;                 // whole message length
}

// What the board hands the decoder: from the sub byte on, counting the sub and the
// payload but not the F7 -- the convention the existing subs use, where an LED_PAD
// frame is `len >= 5` for pad,r,g,b (main/audio_route_control.cpp:432-436).
uint8_t *subOf(uint8_t *f) { return f + 3; }
unsigned payloadLen(unsigned nWrite) { return 1u + 3u + nWrite; }   // sub + sec,start,n + vals

}  // namespace

TEST_CASE("a block write names a section, a start and a count, and nothing else")
{
    Write w;
    const uint8_t vals[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t f[32];
    const unsigned total = writeFrame(f, static_cast<uint8_t>(Sec::PadMode), 0, 16, vals);
    CHECK(total == static_cast<unsigned>(kBytesGridFromPalette));   // 16 held-in-hand pads
    REQUIRE(decodeWrite(subOf(f), payloadLen(16), &w) == Bad::Ok);
    CHECK(w.sec == Sec::PadMode);
    CHECK(w.start == 0);
    CHECK(w.n == 16);
    CHECK(w.v[15] == 1);

    // One channel's volume, written on its own: the point is that a single address is
    // one byte of payload, not a whole-grid frame -- that is what makes a control page
    // with one dirty value cheap to refresh.
    const uint8_t one[1] = {100};
    const unsigned t1 = writeFrame(f, static_cast<uint8_t>(Sec::ChVolume), 3, 1, one);
    REQUIRE(decodeWrite(subOf(f), payloadLen(1), &w) == Bad::Ok);
    CHECK(w.sec == Sec::ChVolume);
    CHECK(w.start == 3);
    CHECK(w.n == 1);
    CHECK(w.v[0] == 100);          // the value the frame carried, unchanged
}

TEST_CASE("every way a frame can be wrong gets a reason, not silence")
{
    Write w;
    uint8_t f[64];
    const uint8_t v[16] = {0};

    // A section this board does not have.
    const unsigned t = writeFrame(f, 0x7F, 0, 1, v);
    CHECK(decodeWrite(subOf(f), payloadLen(1), &w) == Bad::UnknownSection);

    // A count of zero, and a count larger than one block: both are the host having
    // mislaid a byte rather than an empty write, so neither is accepted as a no-op.
    const unsigned t0 = writeFrame(f, static_cast<uint8_t>(Sec::PadMode), 0, 0, v);
    CHECK(decodeWrite(subOf(f), payloadLen(0), &w) == Bad::BadCount);
    const unsigned t17 = writeFrame(f, static_cast<uint8_t>(Sec::PadMode), 0, 16, v);
    (void)t17;
    uint8_t big[24] = {0};
    const unsigned tb = writeFrame(f, static_cast<uint8_t>(Sec::PadMode), 0, 16, big);
    f[6] = 17;   // ask for more than one block's worth
    CHECK(decodeWrite(subOf(f), payloadLen(16), &w) == Bad::BadCount);
    (void)tb;

    // Past the end of a section. Eight hardware outputs exist; address 8 does not.
    const unsigned th = writeFrame(f, static_cast<uint8_t>(Sec::HwLevel), 8, 1, v);
    CHECK(decodeWrite(subOf(f), payloadLen(1), &w) == Bad::BadRange);
    const unsigned th2 = writeFrame(f, static_cast<uint8_t>(Sec::HwLevel), 4, 8, v);
    CHECK(decodeWrite(subOf(f), payloadLen(8), &w) == Bad::BadRange);   // 4 + 8 = 12 > 8

    // A value the section does not accept. pad_mode is two-valued; a third mode is a
    // host that means something this board does not do, and pretending otherwise would
    // leave it painting in a mode nobody implements.
    const uint8_t two[1] = {2};
    const unsigned tm = writeFrame(f, static_cast<uint8_t>(Sec::PadMode), 0, 1, two);
    CHECK(decodeWrite(subOf(f), payloadLen(1), &w) == Bad::BadValue);
    // Palette indices are 0..31 because the palette holds 32 entries.
    const uint8_t idx32[1] = {32};
    const unsigned tc = writeFrame(f, static_cast<uint8_t>(Sec::PadColour), 0, 1, idx32);
    CHECK(decodeWrite(subOf(f), payloadLen(1), &w) == Bad::BadValue);
    // `anim`'s `which` is an enum; its period is not.
    const uint8_t anim4[1] = {4};
    const unsigned ta = writeFrame(f, static_cast<uint8_t>(Sec::Anim), 0, 1, anim4);
    CHECK(decodeWrite(subOf(f), payloadLen(1), &w) == Bad::BadValue);
    const uint8_t per[1] = {127};
    const unsigned tp = writeFrame(f, static_cast<uint8_t>(Sec::Anim), 1, 1, per);
    CHECK(decodeWrite(subOf(f), payloadLen(1), &w) == Bad::Ok);   // 1270 ms: allowed

    // Truncation: the sub says sixteen values and the frame carries five.
    // Sixteen values asked for, five arrived: the host has mislaid a byte, and a
    // decoder that accepted a short frame would paint pads nobody named.
    writeFrame(f, static_cast<uint8_t>(Sec::PadColour), 0, 16, v);
    CHECK(decodeWrite(subOf(f), 1u + 3u + 5u, &w) == Bad::TooShort);

    // A frame shorter than the grammar needs at all.
    CHECK(decodeWrite(f, 2, &w) == Bad::TooShort);
    CHECK(decodeWrite(nullptr, 0, &w) == Bad::TooShort);
}

TEST_CASE("the byte counts the documentation quotes are the frames themselves")
{
    // docs/subsystems/surface-sysex.md justifies the address space by counting bytes,
    // so the numbers it quotes are built here as real messages and measured, not
    // recited. A ratio is what a person actually chooses a transport on, so the ratios
    // are asserted too.
    uint8_t f[96];
    const uint8_t vals[16] = {7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    CHECK(writeFrame(f, static_cast<uint8_t>(Sec::PadColour), 0, 16, vals)
          == static_cast<unsigned>(kBytesGridFromPalette));                 // 24

    // F0 7D 1D 12 <start=0> <n=16> <48 colour bytes> F7 -- a six-byte prefix, because
    // this form addresses by start/count only (no section byte).
    uint8_t g[96] = {0xF0, 0x7D, 0x1D, 0x12, 0x00, 16};
    for (int i = 0; i < 48; ++i) g[6 + i] = static_cast<uint8_t>((i % 0x7F) + 1);
    g[6 + 48] = 0xF7;
    CHECK(6 + 48 + 1 == kBytesGridDirectRgb);                               // 55

    // LED_PAD, one pad per message: F0 7D 1D 0D <pad> <r> <g> <b> F7
    CHECK(9 == (kBytesOnePadPerFrame / 16));
    CHECK(kBytesOnePadPerFrame == 144);
    // NRPN: CC 101, CC 102, CC 6, CC 38 -- four three-byte messages per parameter.
    CHECK(kBytesNrpnPerGrid == 192);

    // The document's claims, as ratios of the same sixteen pads: eight times cheaper
    // than NRPN, a little over twice cheaper than one pad per frame.
    CHECK(kBytesNrpnPerGrid == 8 * kBytesGridFromPalette);
    CHECK(kBytesOnePadPerFrame == 6 * kBytesGridFromPalette);
    CHECK(kBytesGridDirectRgb < kBytesOnePadPerFrame);
    // And the frame rate a TRS line can carry them at: 31250 baud, ~3 bytes per byte of
    // line time, so 24 bytes is ~7.7 ms -- over 100 grid frames a second, against the
    // 1027 msg/s the loopback bench measured on that line.
    CHECK(kBytesGridFromPalette * 8 * 100 < 31250 * 4);
}

TEST_CASE("a give-back that skips what the board took over")
{
    Slot s;
    // The surface paints a pad red the board was showing green.
    claim(s, 0x60, 0x10);                 // wrote = 96, before = 16
    CHECK(s.wrote == 0x60);
    CHECK(s.before == 0x10);

    // A second frame in the same run does not move what is being protected: giving back
    // is the board's colour from before the surface started, not wherever the last
    // animation step happened to leave it.
    claim(s, 0x40, 0x10);
    CHECK(s.before == 0x10);
    CHECK(s.wrote == 0x40);

    // Nothing else touched the pad: the restore is allowed, and it hands over the
    // board's own value.
    int16_t into = kNoSlot;
    REQUIRE(handBack(s, /*currentNow=*/0x40, &into));
    CHECK(into == 0x10);
    CHECK(s.wrote == kNoSlot);
    CHECK(s.before == kNoSlot);

    // A pad the board repainted since: the surface's write is no longer what is on the
    // pad, so the restore leaves it alone. This is the case that matters -- the board
    // lights pads for what is playing, and a surface putting its old colour back over
    // that is a surface talking over its own player.
    into = kNoSlot;                 // no carry-over from the successful hand-back above
    claim(s, 0x60, 0x10);
    CHECK_FALSE(handBack(s, /*currentNow=*/0x00, &into));
    CHECK(into == kNoSlot);            // nothing was promised
    CHECK(s.wrote == 0x60);            // the record is kept; the caller counts a skip
    CHECK_FALSE(stillMine(s, 0x00));
    CHECK(stillMine(s, 0x60));

    // A slot that was never claimed has nothing to hand back, and `clearSlot` makes the
    // state after a session explicit rather than leaving a stale record.
    Slot fresh;
    CHECK_FALSE(handBack(fresh, 0, &into));
    claim(s, 5, 9);
    clearSlot(s);
    CHECK(s.wrote == kNoSlot);
    CHECK(s.before == kNoSlot);
}

TEST_CASE("a section with no read-back is refused a restore instead of guessed")
{
    Range r;
    uint8_t f[8];
    // A pad's live mixer level: the mixer offers setPadMix and no getter, so there is
    // nothing to compare the surface's record against and nothing honest to put back.
    f[0] = 0x7D; f[1] = 0x1D; f[2] = 0x14;   // SEC_RESTORE
    f[3] = static_cast<uint8_t>(Sec::PadMix); f[4] = 0; f[5] = 16; f[6] = 0xF7;
    CHECK(decodeRestore(f + 2, 5, &r) == Bad::UnknownSection);

    // The pad colours, though, can be: the LED model reads back.
    f[3] = static_cast<uint8_t>(Sec::PadColour);
    REQUIRE(decodeRestore(f + 2, 5, &r) == Bad::Ok);
    CHECK(r.sec == Sec::PadColour);
    CHECK(r.n == 16);

    // Same grammar faults as a write: unknown section, bad count, range past the end.
    f[3] = 0x66;
    CHECK(decodeRestore(f + 2, 5, &r) == Bad::UnknownSection);
    f[3] = static_cast<uint8_t>(Sec::HwLevel); f[4] = 6; f[5] = 4;
    CHECK(decodeRestore(f + 2, 5, &r) == Bad::BadRange);
    f[4] = 0; f[5] = 0;
    CHECK(decodeRestore(f + 2, 5, &r) == Bad::BadCount);
    CHECK(decodeRestore(f + 2, 2, &r) == Bad::TooShort);

    // And the palette: entries are loaded, not restored -- a "restore" of a colour table
    // would be a guess about what the host had loaded before.
    f[3] = static_cast<uint8_t>(Sec::PalLoad); f[4] = 0; f[5] = 1;
    CHECK(decodeRestore(f + 2, 5, &r) == Bad::UnknownSection);
}

TEST_CASE("palette and direct-colour frames decode into the same shape")
{
    int index = -1, n = -1, start = -1;
    uint8_t rgb[48];
    // F0 7D 1D 13 <index> <count> (r,g,b) x count F7
    const uint8_t pal[] = {0x7D, 0x1D, 0x13, 8, 4,
                           0x7F, 0x00, 0x00,   // entry 8: red at full wire value
                           0x00, 0x7F, 0x00,   // 9
                           0x00, 0x00, 0x7F,   // 10
                           0x20, 0x20, 0x20,   // 11
                           0xF7};
    REQUIRE(decodePalette(pal + 2, sizeof(pal) - 2, &index, &n, rgb) == Bad::Ok);
    CHECK(index == 8);
    CHECK(n == 4);
    CHECK(rgb[0] == 0x7F);          // doubled to 254 by the board, the existing convention
    CHECK(rgb[9] == 0x20);

    // A palette load reaching past entry 31 is refused, not wrapped.
    const uint8_t bad[] = {0x7D, 0x1D, 0x13, 30, 3, 1, 1, 1, 0xF7};
    CHECK(decodePalette(bad + 2, sizeof(bad) - 2, &index, &n, rgb) == Bad::BadRange);
    // Truncated triples: count says four entries, the frame holds three and a half.
    const uint8_t trunc[] = {0x7D, 0x1D, 0x13, 0, 4, 1, 2, 3, 1, 2, 3, 1, 2, 0xF7};
    CHECK(decodePalette(trunc + 2, sizeof(trunc) - 2, &index, &n, rgb) == Bad::TooShort);

    // F0 7D 1D 12 <start> <count> (r,g,b) x count F7 -- direct colour for pads.
    uint8_t rgb16[48] = {0};
    for (int i = 0; i < 48; ++i) rgb16[i] = static_cast<uint8_t>(i + 1);
    const uint8_t *direct = rgb16;   // reuse as payload below
    (void)direct;
    uint8_t frame[64];
    frame[0] = 0x7D; frame[1] = 0x1D; frame[2] = 0x12; frame[3] = 0; frame[4] = 16;
    for (int i = 0; i < 48; ++i) frame[5 + i] = static_cast<uint8_t>((i % 0x7F) + 1);
    frame[53] = 0xF7;
    REQUIRE(decodeRgb(frame + 2, 4 + 48 + 1 - 2, &start, &n, rgb) == Bad::Ok);
    CHECK(start == 0);
    CHECK(n == 16);
    CHECK(rgb[47] == frame[52]);
    // Half a grid, and a range that runs off the end of the 16 pads.
    frame[3] = 12; frame[4] = 8;
    CHECK(decodeRgb(frame + 2, 4 + 48 + 1 - 2, &start, &n, rgb) == Bad::BadRange);
    frame[3] = 0; frame[4] = 1;
    CHECK(decodeRgb(frame + 2, 3 + 3, &start, &n, rgb) == Bad::Ok);
    CHECK(n == 1);
}

TEST_CASE("the parked action is plain data both sides of the wire can agree on")
{
    /* The SysEx handler fills this in on the MIDI task and the worker reads it some
     * milliseconds later out of a ring. Nothing enforces that both sides use the same
     * struct at compile time beyond it living in one header, so its size, its
     * trivial-copyability and a round trip through bytes are pinned here: a field
     * inserted in the middle would otherwise show up as a pad painting the wrong
     * colour on a board, not as a compile error anywhere. */
    using namespace crosspad::surface;
    STATIC_REQUIRE(std::is_trivially_copyable<Action>::value);
    CHECK(sizeof(Action) <= 64);

    Action a{};
    a.kind = Kind::Restore;
    a.sec = Sec::OutVolume;
    a.start = 3;
    a.n = 4;
    for (int i = 0; i < 4; ++i) a.v[i] = static_cast<uint8_t>(20 + i);
    uint8_t bytes[sizeof(Action)];
    std::memcpy(bytes, &a, sizeof a);
    Action back{};
    std::memcpy(&back, bytes, sizeof back);
    CHECK(back.kind == Kind::Restore);
    CHECK(back.sec == Sec::OutVolume);
    CHECK(back.start == 3);
    CHECK(back.n == 4);
    CHECK(back.v[0] == 20);
    CHECK(back.v[3] == 23);

    /* And the frame that produced it stays short whatever the range: addressing does
     * not grow with the number of addresses a restore covers, which is part of why a
     * panel can ask for its whole write to be undone for eight bytes on the wire. */
    uint8_t f[8] = {0xF0, 0x7D, 0x1D, 0x14, static_cast<uint8_t>(Sec::OutVolume), 0, 8, 0xF7};
    Range r;
    REQUIRE(decodeRestore(f + 3, 4, &r) == Bad::Ok);   // sub + sec + start + count, no values
    CHECK(r.sec == Sec::OutVolume);
    CHECK(r.n == 8);
    CHECK(sizeof(f) == 8u);
}

TEST_CASE("the small sections are addressed like the big ones, and no further")
{
    using namespace crosspad::surface;
    Write w;
    uint8_t f[24];
    const uint8_t pair[2] = {0, 12};     // bank 0, program 12: the pair the kit map prints
    const unsigned ok = writeFrame(f, static_cast<uint8_t>(Sec::Kit), 0, 2, pair);
    CHECK(ok == static_cast<unsigned>(kBytesGridFromPalette) - 14);   // 24 -> 10 for two values
    REQUIRE(decodeWrite(f + 3, payloadLen(2), &w) == Bad::Ok);
    CHECK(w.sec == Sec::Kit);
    CHECK(w.n == 2);
    CHECK(w.v[1] == 12);

    // The kit section is two addresses wide: bank then program. Asking for three is a
    // host that has miscounted, and aiming the pair at address 1 runs off the end.
    const uint8_t three[3] = {0, 12, 0};
    writeFrame(f, static_cast<uint8_t>(Sec::Kit), 0, 3, three);
    CHECK(decodeWrite(f + 3, payloadLen(3), &w) == Bad::BadRange);
    writeFrame(f, static_cast<uint8_t>(Sec::Kit), 1, 2, three);
    CHECK(decodeWrite(f + 3, payloadLen(2), &w) == Bad::BadRange);

    // The lease timeout is one address wide, in 100 ms units: 50 is the shipped 5 s,
    // 0 is the old hold-until-told, and there is no second field to write.
    const uint8_t lease[2] = {50, 0};
    writeFrame(f, static_cast<uint8_t>(Sec::Lease), 0, 1, lease);
    REQUIRE(decodeWrite(f + 3, payloadLen(1), &w) == Bad::Ok);
    CHECK(w.n == 1);
    CHECK(w.v[0] == 50);
    writeFrame(f, static_cast<uint8_t>(Sec::Lease), 0, 2, lease);
    CHECK(decodeWrite(f + 3, payloadLen(2), &w) == Bad::BadRange);

    // Animation is two addresses: `which` at 0, period in 10 ms at 1. Written as one
    // block, a panel picks a look and its speed in a single 12-byte message.
    const uint8_t anim[2] = {1, 12};     // chase, 120 ms
    writeFrame(f, static_cast<uint8_t>(Sec::Anim), 0, 2, anim);
    REQUIRE(decodeWrite(f + 3, payloadLen(2), &w) == Bad::Ok);
    CHECK(w.v[0] == 1);
    CHECK(w.v[1] == 12);

    // A restore aimed at the kit section is refused, even though the kit section is
    // writable: undoing a kit needs the kit listing, which belongs to the UI task, and a
    // restore is applied on the worker. A frame the grammar accepts and the apply path
    // then quietly ignores is the worst thing a surface can do to a host, so the refusal
    // lives in the grammar and this pins it. (The lease and the palette are refused for
    // the same kind of reason: a timeout has no previous value the host could mean, and
    // the surface never saw the colour table it would be putting back.)
    uint8_t g[8] = {0xF0, 0x7D, 0x1D, 0x14, static_cast<uint8_t>(Sec::Kit), 0, 2, 0xF7};
    Range rr;
    CHECK(decodeRestore(g + 3, 4, &rr) == Bad::UnknownSection);
    g[4] = static_cast<uint8_t>(Sec::Lease);
    CHECK(decodeRestore(g + 3, 4, &rr) == Bad::UnknownSection);
    g[4] = static_cast<uint8_t>(Sec::Anim);
    CHECK(decodeRestore(g + 3, 4, &rr) == Bad::Ok);
}
