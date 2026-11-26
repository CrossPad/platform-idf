// SPDX-License-Identifier: GPL-3.0-or-later
/* Keeping a bench awake (main/include/host_keepawake.hpp): when a host's
 * commands should keep the board awake, when a nudge is owed, and when the
 * whole thing has to go back to sleep on its own. The numbers in the comments
 * are the ones a v2 board was measured with on 2026-11-26: the STM dims after
 * 600 s of no activity (REG_CTL_DIM_TO_S) and takes the board into soft-off
 * after 1800 s (REG_CTL_SLEEP_TO_S), and a board in PWR_STATE_LIGHT answers a
 * console command while its screen and pads stay dimmed. */
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

#include "host_keepawake.hpp"

using crosspad::HostKeepAwake;

namespace {
constexpr uint32_t kMinute = 60u * 1000u;
constexpr uint32_t kHour = 60u * kMinute;
constexpr uint32_t kNudgeS = HostKeepAwake::kNudgePeriodMs / 1000u;
}

TEST_CASE("the first command of a session asks for one nudge at once")
{
    HostKeepAwake ka;
    const uint32_t t = 10u * 1000u;
    ka.noteCommand(t);
    CHECK(ka.shouldNudge(t));                 // relight the board for whoever typed
    CHECK_FALSE(ka.shouldNudge(t + 100));     // and not again a moment later
    CHECK(ka.stats().reopens == 1u);
    CHECK(ka.stats().nudges == 1u);
}

TEST_CASE("a live session is nudged once per period, not once per command")
{
    HostKeepAwake ka;
    uint32_t t = 0;
    ka.noteCommand(t);                        // opens the session
    REQUIRE(ka.shouldNudge(t));               // its relight

    // Ten minutes of a command a second: the STM is told 20 s at a time.
    unsigned pump_runs = 0, nudges = 0;
    for (; t < 10u * kMinute; t += 1000u) {
        ka.noteCommand(t);
        ++pump_runs;
        if (ka.shouldNudge(t)) ++nudges;
    }
    CHECK(pump_runs == 600u);
    // One every 20 s from t=20000 to t=580000; the next would be at 600000,
    // which is past the loop, and the session's own relight is not counted here.
    CHECK(nudges == (10u * 60u) / kNudgeS - 1u);
    CHECK(ka.stats().nudges == (10u * 60u) / kNudgeS);   // 30 including the relight
    CHECK(ka.stats().commands == 601u);
}

TEST_CASE("a host that stops talking gives the board back to its own timeouts")
{
    HostKeepAwake ka;
    uint32_t t = 0;
    ka.noteCommand(t);                        // one command, then silence
    REQUIRE(ka.shouldNudge(t));

    // Still live inside the quiet window: the pump keeps the idle timer reset.
    CHECK(ka.active(t + HostKeepAwake::kHostQuietMs - 1000u));
    // Past it the policy stops touching the bus: a person who sat down and
    // stopped typing gets their board dimming the way Settings say it should.
    CHECK_FALSE(ka.active(t + HostKeepAwake::kHostQuietMs + 1000u));
    CHECK_FALSE(ka.shouldNudge(t + HostKeepAwake::kHostQuietMs + 1000u));
    CHECK(ka.pumpPeriodMs(t + kHour) == HostKeepAwake::kIdlePumpPeriodMs);
}

TEST_CASE("a lease covers a gap in the traffic — the build that takes an hour")
{
    HostKeepAwake ka;
    uint32_t t = 0;
    ka.noteCommand(t);                        // `PWR KEEPAWAKE 3600` arrives
    REQUIRE(ka.shouldNudge(t));
    ka.keepAwakeFor(kHour, t);                // then the host builds, silently, for an hour

    CHECK(ka.active(t + kHour - kMinute));
    CHECK(ka.leaseLeftMs(t + kHour - kMinute) == kMinute);
    // The pump still runs a second at a time while the lease lives, and pays
    // for the STM once per period -- not once per pump tick.
    unsigned nudges_in_gap = 0;
    for (uint32_t s = 0; s < kHour; s += 1000u) {
        if (ka.shouldNudge(t + s)) ++nudges_in_gap;
    }
    CHECK(nudges_in_gap == kHour / HostKeepAwake::kNudgePeriodMs);   // relight + one per 20 s
    CHECK(ka.pumpPeriodMs(t + kMinute) == HostKeepAwake::kPumpPeriodMs);
    // The lease ends by itself: a forgotten switch cannot leave a device lit.
    CHECK_FALSE(ka.active(t + kHour + 2u * kMinute));
    CHECK_FALSE(ka.shouldNudge(t + kHour + 2u * kMinute));
}

TEST_CASE("a lease is bounded, and 0 drops it")
{
    HostKeepAwake ka;
    const uint32_t t = 5u * 1000u;
    ka.keepAwakeFor(std::numeric_limits<uint32_t>::max(), t);
    CHECK(ka.leaseLeftMs(t) == HostKeepAwake::kMaxLeaseMs);
    ka.keepAwakeFor(1u, t);                   // shorter than a second means nothing
    CHECK(ka.leaseLeftMs(t) == HostKeepAwake::kMinLeaseMs);
    ka.keepAwakeFor(0u, t);
    CHECK(ka.leaseLeftMs(t) == 0u);
    CHECK_FALSE(ka.active(t));
    // Taking a lease while the board is dim relights it.
    ka.keepAwakeFor(kMinute, t);
    CHECK(ka.shouldNudge(t));
    CHECK(ka.stats().leases == 3u);
}

TEST_CASE("a command after a long quiet stretch reopens the session")
{
    HostKeepAwake ka;
    uint32_t t = 0;
    ka.noteCommand(t);
    REQUIRE(ka.shouldNudge(t));
    t += 40u * kMinute;                       // silence long enough for the board to have dimmed
    ka.noteCommand(t);
    CHECK(ka.stats().reopens == 2u);
    CHECK(ka.shouldNudge(t));                 // and the screen comes back for this person
    CHECK(ka.quietMs(t + 3u * 1000u) == 3u * 1000u);
}

TEST_CASE("the 32-bit clock wrap does not freeze the policy")
{
    HostKeepAwake ka;
    constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
    // A board with 49.7 days of uptime: a command, and its relight, just short
    // of the wrap.
    const uint32_t t0 = kMax - 25u * 1000u;
    ka.noteCommand(t0);
    REQUIRE(ka.shouldNudge(t0));
    const uint32_t after = t0 + 30u * 1000u;  // wraps to 5000
    CHECK(ka.active(after));                  // 30 s of quiet, not 2^32 of them
    CHECK(ka.shouldNudge(after));             // owed: 30 s since the last nudge
    CHECK(ka.quietMs(after) == 30u * 1000u);
}

TEST_CASE("nudges stop once the host has been quiet past the window")
{
    HostKeepAwake ka;
    uint32_t t = 100u;
    for (int i = 0; i < 7; ++i) ka.noteCommand(t += 100u);   // one session, seven commands
    CHECK(ka.stats().commands == 7u);
    CHECK(ka.stats().reopens == 1u);          // the first of them reopened
    ka.keepAwakeFor(10u * 1000u, t);          // a short lease, then the host goes away
    unsigned asked = 0;
    for (uint32_t s = 0; s < 5u * kMinute; s += 1000u) {
        t += 1000u;
        if (ka.shouldNudge(t)) ++asked;
    }
    // Six, and they stop: the relight the lease asked for, then one per period
    // while the last command is still inside the quiet window (t=800 + 120000 =
    // 120800, and the last nudge that fits is 101800). Past that the session is
    // over and the board is left to its own timeouts -- which is the point of
    // the window being minutes rather than hours.
    CHECK(asked == 6u);
    CHECK(ka.stats().nudges == asked);
    CHECK(ka.pumpPeriodMs(t) == HostKeepAwake::kIdlePumpPeriodMs);
}
