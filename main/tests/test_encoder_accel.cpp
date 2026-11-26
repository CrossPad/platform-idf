// SPDX-License-Identifier: GPL-3.0-or-later
/* The detent-rate accelerator (ISSUES.md 9): a fast turn steps further, a slow
 * one stays fine. The rule lives in
 * components/bsp/crosspad/include/indev/encoder_accel.h, header-only and free of
 * LVGL and ESP includes for exactly this test -- the windows fed here are the
 * 33 ms ones the LVGL read callback really hands it (CONFIG_LV_DEF_REFR_PERIOD),
 * and the step sequences are what a hand does with a knob that has sixteen
 * detents to a turn. */
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "indev/encoder_accel.h"

using crosspad::EncoderAccel;

namespace {

/* One read window of `per` detents, `n` windows in a row, 33 ms apart. Returns
 * the multipliers the accelerator chose, and the last one for convenience. */
struct Fed {
    std::vector<int> mults;
    int totalReported = 0;
};

Fed run(EncoderAccel& a, uint32_t& now, int windows, int per, int step_ms = 33)
{
    Fed out;
    for (int i = 0; i < windows; ++i) {
        now += step_ms;
        const int m = a.apply(now, per);
        out.mults.push_back(m);
        out.totalReported += per ? per * m : 0;
    }
    return out;
}

}  // namespace

TEST_CASE("a slow deliberate turn is never touched")
{
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);                       // open the clock

    // 3 detents a second: one detent every tenth window, for four seconds.
    int got = 0;
    for (int i = 0; i < 120; ++i) {
        const int steps = (i % 10) ? 0 : 1;
        now += 33;
        if (steps) ++got;
        REQUIRE(a.apply(now, steps) >= 1);
        CHECK(a.multiplier() == 1);        // never anything but one unit
    }
    CHECK(got == 12);                      // 12 detents in ~4 s, none of them multiplied
}

TEST_CASE("a normal turn doubles the step, a fast turn four times, a very fast one eight")
{
    SECTION("about half a turn a second: 8 detents/s -> x2")
    {
        EncoderAccel a;
        uint32_t now = 0;
        a.apply(now, 0);
        const Fed f = run(a, now, 30, 1, 125);   // one detent every 125 ms = 8/s
        CHECK(a.multiplier() == 2);
        CHECK(f.totalReported > 30);             // 30 detents reported as more than 30 steps
    }
    SECTION("about one and a half turns a second: 24/s -> x4")
    {
        EncoderAccel a;
        uint32_t now = 0;
        a.apply(now, 0);
        run(a, now, 40, 1);                      // one detent a window ~= 30/s
        CHECK(a.multiplier() == 4);
    }
    SECTION("two and a half turns a second: 48/s -> x8, the cap")
    {
        EncoderAccel a;
        uint32_t now = 0;
        a.apply(now, 0);
        run(a, now, 40, 2);                      // two detents a window ~= 60/s
        CHECK(a.multiplier() == 8);
        run(a, now, 40, 20);                     // and even a flood stays at the cap
        CHECK(a.multiplier() == 8);
    }
}

TEST_CASE("the fader the user complains about is crossed in one turn, not twenty")
{
    /* ISSUES.md 9's own case: a fader 0..100, edited with the knob. A hand that
     * keeps the knob moving at a turn and a half a second (24 detents/s, x4) must
     * cover the range in about a turn and a half of detents, and the same hand
     * moving carefully must still land on individual units. */
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);

    int value = 0, detents = 0;
    while (value < 100 && detents < 100) {
        now += 33;
        const int m = a.apply(now, 1);           // one detent per window
        value += m;
        detents += 1;
    }
    CHECK(value >= 100);
    CHECK(detents <= 32);                        // under two turns of a 16-detent knob

    // ...and careful again after a pause: one unit a detent, exactly where the
    // slow hand wants to be. A detent every 400 ms is 2.5/s -- below the first
    // tier -- where the same hand at 8/s (125 ms) would already be a normal turn
    // and would be doubled, which is the point of the rule.
    for (int i = 0; i < 30; ++i) { now += 33; a.apply(now, 0); }   // 1 s of quiet
    value = 0;
    for (int i = 0; i < 5; ++i) {
        now += 400;                              // a deliberate 1 detent every 400 ms
        value += a.apply(now, 1);
    }
    CHECK(value == 5);
}

TEST_CASE("reversing direction goes back to one unit at once")
{
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);
    run(a, now, 40, 2);                          // fast one way: x8
    REQUIRE(a.multiplier() == 8);

    // The hand overshot and comes back: the very first detent the other way must
    // be a single unit, not eight.
    now += 33;
    CHECK(a.apply(now, -2) == 1);
    CHECK(a.stats().reversed == 1);
    // and the average starts over, so the correction stays fine unless the hand
    // really turns fast the other way too
    now += 33;
    CHECK(a.apply(now, -1) <= 2);
}

TEST_CASE("a pause makes the next move deliberate")
{
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);
    run(a, now, 40, 2);
    REQUIRE(a.multiplier() == 8);

    // Quiet, longer than kIdleMs. The first detent after it is one unit.
    now += EncoderAccel::kIdleMs + 50;
    a.apply(now, 0);
    now += 33;
    CHECK(a.apply(now, 1) == 1);
    CHECK(a.stats().idles >= 1);
}

TEST_CASE("a hand at a tier boundary does not flicker between two step sizes")
{
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);

    // Up to x4 first: one detent every 33 ms window is ~30/s.
    run(a, now, 30, 1);
    REQUIRE(a.multiplier() == 4);

    // Then a hand that wobbles around ~15/s: a window with a detent, a window
    // without. 15 is under the x4 threshold (18), so without the hold the step
    // would drop to 2 here and climb back a window later -- a fader that
    // sometimes jumps and sometimes does not is worse than one that is
    // consistently a little coarse. With it, x4 is kept until the rate really
    // falls (3/4 of 18 = 13.5).
    for (int i = 0; i < 60; ++i) {
        now += 33;
        a.apply(now, (i % 2) ? 1 : 0);
    }
    CHECK(a.ratePerSec() > 13);
    CHECK(a.ratePerSec() < EncoderAccel::kRate4);
    CHECK(a.multiplier() == 4);
    CHECK(a.stats().down == 0);

    // A deliberate slowdown does bring it down, one tier at a time.
    run(a, now, 60, 1, 125);                     // 8/s
    CHECK(a.multiplier() == 2);
    run(a, now, 60, 1, 400);                     // 2.5/s
    CHECK(a.multiplier() == 1);
    CHECK(a.stats().down == 2);
}

TEST_CASE("a stalled read does not read as a fast turn")
{
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);
    // A task stalls for a second; the knob moved two detents meanwhile. One
    // window carrying 2 detents must not look like 60/s.
    now += 1000;
    const int m = a.apply(now, 2);
    CHECK(m <= 2);                               // clamped dt keeps the rate sane
    CHECK(a.ratePerSec() < EncoderAccel::kRate8);
}

TEST_CASE("switched off, the rule is out and the counters still see the hand")
{
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);
    run(a, now, 40, 2);
    REQUIRE(a.multiplier() == 8);

    a.setEnabled(false);
    const Fed f = run(a, now, 40, 2);
    CHECK(f.totalReported == 80);                // 40 windows x 2 detents, multiplied by nothing
    for (int m : f.mults) CHECK(m == 1);
    // the read-out can still tell the user what their hand did
    CHECK(a.stats().detents >= 80);
    CHECK(a.stats().hist[2] >= 1);               // windows that carried 2-3 detents
}

TEST_CASE("one hard flick is not mistaken for a fast turn, and a sustained one is")
{
    /* The average needs a few windows to know a hand means it. One flick of the
     * knob puts three detents in a single window -- 90/s for that window alone --
     * and that must not become the cap, because the next detent of the same
     * gesture is usually the careful one. Cranking it *keeps* going and reaches
     * the cap within a fifth of a second (kTauMs). */
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);
    now += 400;
    CHECK(a.apply(now, 3) <= 2);              // the flick's own window
    now += 400;
    a.apply(now, 0);                          // let it settle again

    const Fed f = run(a, now, 12, 2);         // and now the hand really cranks it
    CHECK(f.mults.front() >= 1);
    CHECK(a.multiplier() == 8);
    size_t reached = 0;
    while (reached < f.mults.size() && f.mults[reached] < 8) ++reached;
    CHECK(reached <= 8);                      // the cap inside a quarter of a second
    for (size_t i = 1; i < f.mults.size(); ++i)
        CHECK(f.mults[i] >= f.mults[i - 1]);  // and it does not oscillate on the way up
}

TEST_CASE("the counters add up to the windows they describe")
{
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);
    run(a, now, 10, 0);
    run(a, now, 5, 1);
    run(a, now, 4, 3);
    run(a, now, 2, 9);
    const auto& st = a.stats();
    CHECK(st.hist[0] + st.hist[1] + st.hist[2] + st.hist[3] + st.hist[4] == st.windows);
    CHECK(st.detents == 5u * 1u + 4u * 3u + 2u * 9u);
    CHECK(st.moving == 11u);
    a.noteReported(-40);
    CHECK(a.stats().reported == 40u);            // sign does not matter for the sum
}

TEST_CASE("the reported window shape a hand actually makes is what the tiers were set for")
{
    /* Sixteen detents to a turn, a 33 ms read window: a hand moving the knob one
     * full turn in a second produces 16 detents over ~30 windows -- about one
     * every other window. That lands between the x2 and x4 tiers, which is what
     * the constants were chosen for: a deliberate turn is not turned into jumps
     * of eight. */
    EncoderAccel a;
    uint32_t now = 0;
    a.apply(now, 0);
    for (int w = 0; w < 30; ++w) {
        now += 33;
        a.apply(now, (w % 2) ? 1 : 0);
    }
    CHECK(a.multiplier() <= 4);
    CHECK(a.ratePerSec() >= 12);
    CHECK(a.ratePerSec() <= 30);
}
