// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/* Keeping a bench awake, and waking it: the rule behind `PWR` over CDC.
 *
 * The problem, measured on a v2 board on 2026-11-26: the STM dims the screen
 * after `REG_CTL_DIM_TO_S` (default 600 s) and takes the whole board into
 * soft-off after `REG_CTL_SLEEP_TO_S` (default 1800 s). In between — the state
 * the STM calls PWR_STATE_LIGHT — the ESP is still up, so a test console still
 * answers, and that is exactly what misleads an automated run: commands are
 * answered, the screen and the pads stay dimmed, and after half an hour the ESP
 * is told to restart into its soft-off gate and the console is simply gone.
 * Nothing a host sends brings it back: the STM wakes on its own power button
 * (BTN1 on its ADC ladder) or on a charger event, and there is no register a
 * host can use to ask for a wake. `REG_CMD_USER_ACTIVITY` only *resets* the
 * idle timer, and only reaches the STM while the ESP is running.
 *
 * So the useful half is to never fall into that state during a bench session,
 * and to climb back out of the dim one — which this policy does in three ways.
 *
 *  - **A host command is a person at the board.** The CDC and companion-SysEx
 *    loops feed `noteCommand()`. The first command after a quiet stretch asks
 *    for one nudge at once: that takes the board out of LIGHT (measured:
 *    writing `REG_CMD_USER_ACTIVITY` moved `REG_IN_PWR_STATE` from 0x03/0x04 to
 *    0x00, and the ESP then restores the backlight and the pad brightness in
 *    `app_pwr_state_cb`'s PWR_STATE_AWAKE case). A dim board that answers a
 *    command should be readable again, and today it is not.
 *  - **A session that keeps talking keeps the board awake** for as long as the
 *    commands are recent (within `kHostQuietMs`); the pump sends `USER_ACTIVITY`
 *    at most once every `kNudgePeriodMs`, so keeping the board awake costs one
 *    I2C byte a period, not one per command.
 *  - **A gap in the traffic is the dangerous case**: a build that takes twenty
 *    minutes sends nothing at all. `PWR KEEPAWAKE 3600` takes a lease — the
 *    board stays awake for that long whatever the host is doing — and it
 *    expires by itself, so a forgotten switch cannot leave a device lit on a
 *    shelf. `kMaxLeaseMs` is the outer bound.
 *
 * Deliberately *not* counted as activity: a host's MIDI note and CC traffic. A
 * DAW playing the board, or a clock that never stops, would then keep the
 * screen bright forever, and a performer who stops to look at the paper would
 * find the board awake. The pads and the encoder are scanned by the STM itself,
 * so playing the board already resets its timer. Control traffic — a console
 * verb, a companion request — is a person steering, and that is what counts.
 *
 * Not done here either: changing the STM's own timeouts. `STM_REGW` over the
 * same console can already write `REG_CTL_DIM_TO_S` / `REG_CTL_SLEEP_TO_S`, and
 * a keep-awake that quietly edited the user's stored settings would be a
 * surprise on their desk. The nudge is the reversible half; `PWR` reports both
 * what is stored and what the STM is really running, because those two can
 * disagree after a `STM_REGW` experiment, and that disagreement is how a bench
 * board ends up dimming after five seconds for reasons nobody remembers.
 *
 * The clock is the caller's (`now_ms` in, milliseconds from any monotonic
 * source), and differences are taken as signed so a 32-bit wrap — 49.7 days of
 * uptime, which a long-running instrument does reach — does not freeze the
 * policy. Keeping it a header with no ESP and no FreeRTOS in it is what lets
 * `main/tests/test_host_keepawake.cpp` pin all of this without a board.
 */

#include <cstdint>

namespace crosspad {

class HostKeepAwake
{
public:
    /* How often the pump may ask the STM for activity while a session is live.
     * Well under the shortest dim timeout the Settings page offers, and far
     * enough apart that a long test run does not lean on the I2C bus. */
    static constexpr uint32_t kNudgePeriodMs = 20u * 1000u;
    /* How long after the last host command the session still counts as live.
     * Longer than the gap between a scenario's steps, shorter than the 600 s
     * the board dims at, so an unattended bench goes back to sleep on its own. */
    static constexpr uint32_t kHostQuietMs = 120u * 1000u;
    /* A lease's bounds: short enough to mean something, long enough for a build. */
    static constexpr uint32_t kMinLeaseMs = 1u * 1000u;
    static constexpr uint32_t kMaxLeaseMs = 8u * 3600u * 1000u;
    /* The pump's own period, and how long it backs off to when nothing is live. */
    static constexpr uint32_t kPumpPeriodMs = 1000u;
    static constexpr uint32_t kIdlePumpPeriodMs = 60u * 1000u;

    /** A host command arrived. The first one after a quiet stretch — and the
     *  first one of a session — asks for an immediate nudge, which is what
     *  turns the screen back on for the person who just typed. */
    void noteCommand(uint32_t now_ms)
    {
        ++st_.commands;
        if (st_.commands == 1u ||
            (int32_t)(now_ms - last_cmd_ms_) > (int32_t)kHostQuietMs) {
            st_.reopens++;
            nudge_now_ = true;
        }
        last_cmd_ms_ = now_ms;
    }

    /** Take (or, with 0, drop) a keep-awake lease. Clamped into the bounds. */
    void keepAwakeFor(uint32_t ms, uint32_t now_ms)
    {
        if (ms == 0u) {
            lease_end_ms_ = 0u;
            return;
        }
        uint32_t len = ms < kMinLeaseMs ? kMinLeaseMs : (ms > kMaxLeaseMs ? kMaxLeaseMs : ms);
        lease_end_ms_ = now_ms + len;
        st_.leases++;
        nudge_now_ = true;      // taking a lease while the board is dim must relight it
    }

    /** Ask for a nudge now — `PWR WAKE`, and the test for the wake path. */
    void requestNudge() { nudge_now_ = true; }

    /** True while a lease runs or host commands are recent. */
    bool active(uint32_t now_ms) const
    {
        if (lease_end_ms_ && (int32_t)(lease_end_ms_ - now_ms) > 0) return true;
        if (st_.commands == 0u) return false;
        return (int32_t)(now_ms - last_cmd_ms_) < (int32_t)kHostQuietMs;
    }

    /** Consume one nudge: true when the caller must tell the STM there was
     *  activity, false when it would only spend a bus transaction. */
    bool shouldNudge(uint32_t now_ms)
    {
        if (nudge_now_) {
            nudge_now_ = false;
            last_nudge_ms_ = now_ms;
            ++st_.nudges;
            return true;
        }
        if (!active(now_ms)) return false;
        if ((int32_t)(now_ms - last_nudge_ms_) < (int32_t)kNudgePeriodMs) return false;
        last_nudge_ms_ = now_ms;
        ++st_.nudges;
        return true;
    }

    /** What period the pump should run at: fast while a session is live, slow
     *  (but not stopped — a lease has to end) the rest of the time. */
    uint32_t pumpPeriodMs(uint32_t now_ms) const
    {
        return active(now_ms) ? kPumpPeriodMs : kIdlePumpPeriodMs;
    }

    uint32_t leaseLeftMs(uint32_t now_ms) const
    {
        if (!lease_end_ms_) return 0u;
        const int32_t left = (int32_t)(lease_end_ms_ - now_ms);
        return left > 0 ? (uint32_t)left : 0u;
    }
    /** How long the host has been silent. Reportable: it is what tells a person
     *  whether the board they are watching is being kept awake by their session
     *  or is about to go back to sleep on its own. */
    uint32_t quietMs(uint32_t now_ms) const
    {
        if (st_.commands == 0u) return 0u;
        return (uint32_t)(now_ms - last_cmd_ms_);
    }

    struct Stats {
        uint32_t commands = 0;   // host commands seen
        uint32_t nudges = 0;     // USER_ACTIVITY requests handed out
        uint32_t leases = 0;     // keep-awake leases taken
        uint32_t reopens = 0;    // commands that reopened a quiet session
    };
    const Stats& stats() const { return st_; }
    uint32_t leaseEndMs() const { return lease_end_ms_; }

private:
    Stats st_;
    uint32_t last_cmd_ms_ = 0;
    uint32_t last_nudge_ms_ = 0;
    uint32_t lease_end_ms_ = 0;
    bool nudge_now_ = false;
};

/** The one state the board runs with — the same shape as the encoder
 *  accelerator's singleton: an inline function with a static local, so the
 *  header is includable from both the command parser and the pump without
 *  anyone else having to own a translation unit. Only the pump task calls
 *  `shouldNudge()`; the command loops only call `noteCommand()`/`keepAwakeFor()`,
 *  and both of those touch a handful of plain words from a single task or with
 *  last-writer-wins semantics, which is what a keep-awake hint needs anyway. */
inline HostKeepAwake& hostKeepAwake()
{
    static HostKeepAwake ka;
    return ka;
}

}  // namespace crosspad
