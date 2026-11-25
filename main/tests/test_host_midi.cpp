// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_host_midi.cpp
 * @brief The host control surface's decisions, pinned where they can be: a
 *        Control Change's meaning, a 7-bit value's curve, and one frame of pad
 *        colours -- none of which needs a board to be wrong about.
 *
 * main/host_midi.cpp is the part that touches hardware and is checked on the
 * bench; this is the part that decides, and it is what a silent change to the
 * curve, the table or the parser would otherwise hide.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "host_midi_map.h"

using namespace crosspad::hostmidi;

TEST_CASE("a CC means what the table says it means, and only that")
{
    Rule rules[kMaxRules];
    size_t n = 0;
    defaultRules(rules, &n);
    REQUIRE(n >= 2);

    // The shipped defaults: channel 1's CC 10 rides the sampler's balance,
    // channel 16's CC 7 turns the speakers down in the hardware.
    const Rule *pan = findRule(rules, n, 0, 10);
    REQUIRE(pan);
    CHECK(pan->target == Target::Channel);
    CHECK(pan->isPan);
    CHECK(std::string(pan->name) == "Sampler");
    const Rule *spk = findRule(rules, n, 15, 7);
    REQUIRE(spk);
    CHECK(spk->target == Target::Hardware);
    CHECK(spk->index == 0);

    // Nothing else is taken for the board: an unmapped CC keeps its old life.
    CHECK(findRule(rules, n, 3, 7) == nullptr);
    CHECK(findRule(rules, n, 0, 7) == nullptr);
    CHECK(findRule(rules, n, 15, 7)->channel == 15);   // not shared with channel 1

    // A rule added by hand is found by the pair, not by its position.
    Rule extra{};
    extra.channel = 2;
    extra.cc = 7;
    extra.target = Target::Output;
    extra.index = 1;
    rules[n++] = extra;
    const Rule *found = findRule(rules, n, 2, 7);
    REQUIRE(found);
    CHECK(found->target == Target::Output);
    CHECK(found->index == 1);
    CHECK_FALSE(found->isPan);
}

TEST_CASE("a fader's 7 bits become a level the way a desk behaves")
{
    CHECK(levelFrom7(0) == 0.0f);
    CHECK(levelFrom7(127) == 1.0f);
    // Half way up a fader is not half as loud: a linear taper would put most of
    // the useful range in the last third, which is why hardware is not built so.
    const float half = levelFrom7(64);
    CHECK(half > 0.2f);
    CHECK(half < 0.3f);
    // Never past unity, and always growing -- a host watching a fader move must
    // not see a level go back down while the value climbs.
    float prev = -1.0f;
    for (int v = 0; v <= 127; ++v) {
        const float x = levelFrom7(static_cast<uint8_t>(v));
        CHECK(x >= prev);
        CHECK(x <= 1.0f);
        prev = x;
    }
    // The hardware step, for a level the analog stage sets after the DAC.
    CHECK(percentFrom7(0) == 0);
    CHECK(percentFrom7(127) == 100);
    CHECK(percentFrom7(64) == static_cast<int>(half * 100.0f + 0.5f));

    // Pan: the wire's centre is the board's centre, and unity on both sides.
    CHECK(panFrom7(64) == 0.0f);
    CHECK(panFrom7(0) == Catch::Approx(-1.0f));
    CHECK(panFrom7(127) == Catch::Approx(1.0f));
    CHECK(panFrom7(63) < 0.0f);
    CHECK(panFrom7(65) > 0.0f);
}

TEST_CASE("a level read back off the desk lands on the number a host sent")
{
    // The read-out in MIDI_CC MAP goes through sevenFromLevel(): what a person
    // sees there has to be close to the fader they moved, not to some third
    // unit nobody asked for. Round trip both ways for every wire value.
    int worst = 0;
    for (int v = 0; v <= 127; ++v) {
        const uint8_t back = sevenFromLevel(levelFrom7(static_cast<uint8_t>(v)));
        worst = std::max(worst, std::abs(static_cast<int>(back) - v));
    }
    CHECK(worst <= 1);
    CHECK(sevenFromLevel(0.0f) == 0);
    CHECK(sevenFromLevel(1.0f) == 127);
    CHECK(sevenFromLevel(2.0f) == 127);     // a reading past unity stays at the top
    CHECK(sevenFromLevel(-1.0f) == 0);
}

TEST_CASE("a grid frame arrives whole or not at all")
{
    LedFrame f;
    // A pad painted red, one blue, the rest off -- written as 96 hex digits.
    char text[97];
    std::memset(text, '0', 96);
    text[96] = '\0';
    text[0] = 'f'; text[1] = 'f';     // pad 0 red = ff 00 00
    CHECK(parseFrameHex(text, &f));
    CHECK(f.pad[0].r == 0xff);
    CHECK(f.pad[0].g == 0);
    CHECK(f.pad[1].on);               // every pad of the grid is addressed
    CHECK(f.pad[15].r == 0);

    // Uppercase, and the last pad only lit.
    for (int i = 0; i < 96; ++i) text[i] = (i >= 90) ? 'A' : '0';
    text[95] = '5';
    CHECK(parseFrameHex(text, &f));
    CHECK(f.pad[15].r == 0xAA);
    CHECK(f.pad[15].b == 0xA5);     // 'A' then '5': the last digit is the low nibble

    // Anything else is refused without touching the frame: a half-typed frame
    // must never light pads with whatever was there before.
    char bad[97];
    std::memset(bad, '0', 96);
    bad[96] = '\0';
    bad[95] = 'g';
    f.pad[0].r = 0x5A;
    CHECK_FALSE(parseFrameHex(bad, &f));
    CHECK(f.pad[0].r == 0x5A);                       // the caller's frame untouched
    CHECK_FALSE(parseFrameHex("abc", &f));           // too short
    CHECK_FALSE(parseFrameHex(nullptr, &f));
    char long_[100];
    std::memset(long_, '0', 99);
    long_[99] = '\0';
    long_[98] = 'f';
    CHECK_FALSE(parseFrameHex(long_, &f));           // too long, and not silently cut
}

TEST_CASE("the board's own animations step over the pads, all sixteen each frame")
{
    LedFrame f;
    chaseFrame(0, &f, 255, 128, 0);
    CHECK(f.pad[0].r == 255);
    CHECK(f.pad[1].r == 0);
    chaseFrame(17, &f, 255, 128, 0);                 // wraps, sixteen pads
    CHECK(f.pad[1].g == 128);
    CHECK(f.pad[1].b == 0);
    for (int p = 0; p < 16; ++p) CHECK(f.pad[p].on);

    ringFrame(0, &f, 255);
    CHECK(f.pad[0].r == 255);                        // pad 0 starts at red
    ringFrame(1, &f, 255);
    CHECK(f.pad[15].r == 255);                       // the whole ring moved one
    for (int p = 0; p < 16; ++p) {
        CHECK(f.pad[p].on);
        CHECK(f.pad[p].r + f.pad[p].g + f.pad[p].b > 0);   // no pad goes dark mid-ring
    }
    ringFrame(0, &f, 0);
    for (int p = 0; p < 16; ++p) CHECK(f.pad[p].r + f.pad[p].g + f.pad[p].b == 0);
}

TEST_CASE("target names are what a person reads back in MIDI_CC MAP")
{
    // The reply line is the interface: a reason word nobody recognises is how a
    // refused message becomes a support question.
    CHECK(std::string(targetName(Target::Channel)) == "channel");
    CHECK(std::string(targetName(Target::Hardware)) == "hardware");
    CHECK(std::string(targetName(Target::Output)) == "output");
    CHECK(std::string(targetName(Target::Pad)) == "pad");
    CHECK(std::string(targetName(Target::None)) == "none");
}

TEST_CASE("what a host drove on the mixer, it gives back")
{
    Rule r{};
    r.channel = 15;
    r.cc = 7;
    r.target = Target::Hardware;
    r.index = 0;
    r.isPan = false;
    std::strcpy(r.name, "Speakers");

    Control c;
    controlClear(c);
    CHECK_FALSE(c.held);
    CHECK_FALSE(controlGive(c));                    // nothing taken: nothing to give

    controlTake(c, r, 50.0f);                       // the board stood at 50 % before the host
    CHECK(c.held);
    CHECK(c.before == Catch::Approx(50.0f));
    CHECK(c.rule.target == Target::Hardware);
    CHECK(c.rule.index == 0);

    /* A ride is many messages. Only the first records the board's value: if a later
     * one moved it, "give back" would restore wherever the fader happened to be
     * standing instead of where the board was. */
    controlTake(c, r, 12.0f);
    CHECK(c.before == Catch::Approx(50.0f));

    CHECK(controlGive(c));
    CHECK_FALSE(c.held);
    CHECK_FALSE(controlGive(c));                    // giving twice gives nothing twice

    /* A new ride after a handover protects the level reached *now*, not the one the
     * host left behind. */
    controlTake(c, r, 30.0f);
    CHECK(c.before == Catch::Approx(30.0f));

    /* The rule travels inside the control, so a handover still finds its target when
     * the map was cleared first - and clearing the map is one of the ways a host
     * gives up control. */
    CHECK(c.rule.channel == 15);
    CHECK(c.rule.cc == 7);
}

TEST_CASE("a pad-LED lease is held only while the host is talking")
{
    CHECK_FALSE(leaseTimedOutUs(1'000'000, 1'000'000, 5000));    // nothing elapsed
    CHECK_FALSE(leaseTimedOutUs(6'000'000, 1'000'000, 5000));    // exactly the limit: not yet
    CHECK(leaseTimedOutUs(6'000'001, 1'000'000, 5000));          // a microsecond over: pads back
    CHECK_FALSE(leaseTimedOutUs(1LL << 40, 0, 0));               // 0 = hold until told

    /* Traffic renews the clock: a host riding faders keeps the colours it painted,
     * because the timeout runs from the last thing the host said and not from the
     * last thing it painted. */
    const int64_t painted = 5'000'000;
    /* The clock the timeout is measured from is the *renewal*, not the paint: four
     * seconds after a Control Change at +1 s, the lease still stands (and it stands
     * at exactly 5 s, since the limit is "more than"), and it is only gone once the
     * host has been quiet for longer than the timeout. */
    CHECK_FALSE(leaseTimedOutUs(painted + 4'000'000, painted + 1'000'000, 5000));
    CHECK_FALSE(leaseTimedOutUs(painted + 6'000'000, painted + 1'000'000, 5000));   // exactly 5 s
    CHECK(leaseTimedOutUs(painted + 6'500'000, painted + 1'000'000, 5000));         // 5.5 s quiet
}

TEST_CASE("a balance reads back as the very Control Change that would have set it")
{
    /* The echo path announces `sevenFromPan(mixer pan)`, and `MIDI_CC MAP` prints
     * the same number as `cur=`: if the two curves were not exact inverses, a host
     * would see its own value drift one step every round trip and would be right to
     * call it a bug. Sweeping all 128 wire values is what catches the float
     * rounding that a couple of spot checks would miss (1/63 is not exact in
     * binary, so truncation would answer 64 where the wire said 65). */
    for (int v = 0; v <= 127; ++v) {
        const float pan = panFrom7(static_cast<uint8_t>(v));
        CHECK(sevenFromPan(pan) == v);
    }
    CHECK(sevenFromPan(0.0f) == 64);
    CHECK(sevenFromPan(-1.0f) == 0);
    CHECK(sevenFromPan(1.0f) == 127);
    CHECK(panFrom7(64) == 0.0f);
    CHECK(panFrom7(63) == Catch::Approx(-1.0f / 64.0f));
    CHECK(panFrom7(65) == Catch::Approx(1.0f / 63.0f));
}

TEST_CASE("the board speaks up when a value moves, not when a rule appears")
{
    CHECK_FALSE(echoNeeded(-1, 0));        // first read: seed quietly
    CHECK_FALSE(echoNeeded(-1, 90));       // idem, whatever the desk is standing at
    CHECK_FALSE(echoNeeded(90, 90));       // nothing moved
    CHECK(echoNeeded(90, 127));            // the board (or a handover) moved it
    CHECK(echoNeeded(90, 0));              // down to silence is still news
}
