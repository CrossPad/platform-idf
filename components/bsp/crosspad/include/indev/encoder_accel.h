// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/* Detent-rate acceleration for the navigation encoder.
 *
 * The problem (ISSUES.md 9): the knob sends one step per detent, and one step is
 * one LVGL key event — one unit on a slider, one object of focus. A fader that
 * runs 0..100 therefore wants a hundred detents, and a hand that turns quickly
 * pays for it in wrist. The ask is the usual one: turn fast and the step grows,
 * turn slowly and it stays fine.
 *
 * What this file is: the rule, on its own, with no LVGL and no ESP in it, so it
 * can be tested against the timings a hand makes instead of by feel on a board.
 * `encoderTop_read()` (indev/encoder.h) feeds it one input-device window at a
 * time and reports back with the multiplier it returns.
 *
 * The best practices this follows, and why:
 *
 *  - **Rate, not acceleration of the value.** The detents-per-second of the last
 *    fraction of a second decides the step. That is what a hand actually changes
 *    (Windows' pointer speed, the jog wheel of a camera, a DAW's encoder mapping
 *    all do this); "spin-up of a step counter" would drift on its own while the
 *    knob keeps turning at one speed.
 *  - **A short average over every window, including the empty ones** (time
 *    constant `kTauMs`). Averaging only over windows that moved would read a slow
 *    turn as fast — one detent every 300 ms looks like 33 detents/s in the window
 *    it lands in — and the false fast is exactly the missed value the user then
 *    has to wind back.
 *  - **Tiers with hysteresis.** Going up needs the tier's rate; coming down needs
 *    `kHyst` of it, so a hand at the boundary does not flicker between steps of 2
 *    and 4 mid-move (a fader that sometimes jumps and sometimes doesn't is worse
 *    than one that is simply a bit coarse).
 *  - **A direction change resets to fine.** Overshoot is the risk of a big step;
 *    reversing is how you correct it, and correcting has to be precise. This is
 *    the single most important rule here — without it, backing off a fader you
 *    passed is as clumsy as reaching it was slow.
 *  - **Quiet resets to fine** (`kIdleMs`). The first detent after a pause is the
 *    start of a new, deliberate move. A device that keeps the fast step over a
 *    pause loses the setting the user was aiming at.
 *  - **A cap.** Eight is the practical ceiling here: the knob has sixteen
 *    detents to a turn (`Stm32MessageHandler`'s index is 0..15), so ×8 already
 *    puts a whole 0..100 fader inside one fast turn. Beyond that a step is bigger
 *    than the useful gap between two detents, and the value stops being
 *    reachable at all.
 *  - **Only whole multiples.** The multiplier is an integer ≥ 1, so a value
 *    moves on a grid the user can predict rather than landing on odd units from a
 *    half-step. (Where a widget's own step is not 1, the grid is that step
 *    times this — see the design note.)
 *
 * Not done here, on purpose: a per-widget sensitivity (LVGL has
 * `rotary_sensitivity` for scrollable objects and a style for it, but the widgets
 * on this board change through LV_KEY_LEFT/RIGHT, so a sensitivity there would not
 * reach them — and a fixed sensitivity is a scale, not an accelerator); a
 * user-facing setting (a switch over CDC/HIL, `ENC_ACCEL 0|1`, is enough while the
 * tiers are being tuned against a real hand, and the counters of the read-out
 * `ENC_ACCEL` are what the tuning is done from).
 */

#include <atomic>
#include <cstdint>

namespace crosspad {

class EncoderAccel
{
public:
    /* Detents per second at which the step grows. Measured against what a hand
     * does with a 16-detent knob: about 2/s is careful, 8/s is a normal turn,
     * 24/s and up is a fast one. */
    static constexpr int32_t kRate2 = 7;
    static constexpr int32_t kRate4 = 18;
    static constexpr int32_t kRate8 = 36;
    static constexpr int32_t kMaxMult = 8;

    /* How quickly the average follows the hand (ms), and how long a pause counts
     * as "the next move is deliberate" (ms). 200 ms is about six read windows:
     * long enough that one flick of the knob is not mistaken for a fast turn (it
     * lands around x2, not at the cap), short enough that a hand that really does
     * crank the knob is at its big step within a fifth of a second. */
    static constexpr uint32_t kTauMs = 200;
    static constexpr uint32_t kIdleMs = 500;

    /* Coming back down needs the tier's rate times this (3/4 of the way up). */
    static constexpr int32_t kHystNum = 3;
    static constexpr int32_t kHystDen = 4;

    /* A window's dt is clamped into this: a task that stalls (an app redraw, a
     * card read) must not read as a long quiet, and two reads in the same tick
     * must not divide by zero. */
    static constexpr uint32_t kMinWindowMs = 1;
    static constexpr uint32_t kMaxWindowMs = 500;

    /** Feed one input-device window: `now_ms` is the LVGL tick, `steps` the
     *  detents decoded in it, signed, 0 when the knob did not move. Returns the
     *  multiplier to report this window with — always ≥ 1, and 1 when the
     *  accelerator is off. */
    int apply(uint32_t now_ms, int steps)
    {
        if (!enabled_) {
            // Still count the window, so a read-out says what the board saw even
            // when the rule is switched off.
            if (st_.windows++ == 0) last_ms_ = now_ms;
            const int mag = steps < 0 ? -steps : steps;
            st_.moving += mag ? 1u : 0u;
            st_.detents += (uint32_t)mag;
            noteWindow(mag);
            return 1;
        }

        uint32_t dt = 0;
        if (st_.windows) {
            dt = now_ms - last_ms_;
            if (dt < kMinWindowMs) dt = kMinWindowMs;
            if (dt > kMaxWindowMs) dt = kMaxWindowMs;
        }
        last_ms_ = now_ms;
        st_.windows++;

        const int mag = steps < 0 ? -steps : steps;
        st_.moving += mag ? 1u : 0u;
        st_.detents += (uint32_t)mag;
        noteWindow(mag);

        if (!dt) return mult_;                     // first window: no rate yet

        // A pause longer than kIdleMs is a new move: fine first, whatever the
        // average still carries.
        if (mag == 0 && (now_ms - last_move_ms_) >= kIdleMs) {
            if (mult_ != 1 || rate_x256_ != 0) st_.idles++;
            rate_x256_ = 0;
            dir_ = 0;
            mult_ = 1;
            return 1;
        }

        bool fine_once = false;      // the window that reverses reports one unit
        if (mag) {
            const int sign = steps < 0 ? -1 : 1;
            if (dir_ != 0 && sign != dir_) {
                // Reversal: back to fine at once, and start the rate over from
                // this window. Correcting an overshoot has to be exact — the
                // window that reverses reports one unit whatever the hand's rate
                // is, and the ramp restarts from the next one if the hand really
                // is sweeping the other way.
                st_.reversed++;
                rate_x256_ = 0;
                mult_ = 1;
                fine_once = true;
            }
            dir_ = sign;
            last_move_ms_ = now_ms;
        }

        // Instant rate of this window, then one step of the exponential average.
        const int32_t inst_x256 = (int32_t)((mag * 1000ull * 256ull) / dt);
        const uint32_t alpha_num = dt;
        const uint32_t alpha_den = dt + kTauMs;
        rate_x256_ += (int64_t)(inst_x256 - rate_x256_) * alpha_num / (int64_t)alpha_den;
        if (rate_x256_ < 0) rate_x256_ = 0;
        if (rate_x256_ > st_.rate_max * 256) st_.rate_max = rate_x256_ / 256;

        if (!fine_once) mult_ = stepFor(rate_x256_ / 256);
        return mult_;
    }

    void setEnabled(bool on)
    {
        if (on == enabled_) return;
        enabled_ = on;
        reset();
    }
    bool enabled() const { return enabled_; }

    /** The multiplier the last window used. */
    int multiplier() const { return mult_; }
    /** The smoothed detent rate, in detents per second. */
    int32_t ratePerSec() const { return rate_x256_ / 256; }
    int32_t rateMaxPerSec() const { return st_.rate_max; }

    struct Stats {
        uint32_t windows = 0;      // windows fed in
        uint32_t moving = 0;       // of those, ones that carried detents
        uint32_t detents = 0;      // detents seen, absolute
        uint32_t reported = 0;     // what they were reported as, absolute
        uint32_t hist[5] = {};     // detents per window: 0, 1, 2-3, 4-7, 8+
        uint32_t up = 0;           // tier changes up
        uint32_t down = 0;         // tier changes down
        uint32_t reversed = 0;     // direction changes that reset to fine
        uint32_t idles = 0;        // quiet resets
        int32_t rate_max = 0;      // highest smoothed rate seen, per second
    };
    const Stats& stats() const { return st_; }

    /** Report what a window was multiplied into — the read-out and the tests use
     *  this to tell "the rule said ×4" from "the widget moved four units". */
    void noteReported(int reported_steps)
    {
        st_.reported += (uint32_t)(reported_steps < 0 ? -reported_steps : reported_steps);
    }

    void reset()
    {
        rate_x256_ = 0;
        mult_ = 1;
        dir_ = 0;
        last_ms_ = 0;
        last_move_ms_ = 0;
        st_.windows = 0;
    }
    void resetStats() { st_ = Stats{}; reset(); }

private:
    /* The tier ladder, with the hysteresis: every boundary the current step has
     * to cross going down is 3/4 of the one that got it up. */
    int stepFor(int32_t rate)
    {
        const int up   = mult_;
        int target = 1;
        if (rate >= kRate2) target = 2;
        if (rate >= kRate4) target = 4;
        if (rate >= kRate8) target = 8;
        if (target > up) {
            mult_ = target < kMaxMult ? target : kMaxMult;
            st_.up++;
        } else if (target < up) {
            // Only come down when the rate has really fallen below the tier this
            // step belongs to — otherwise a wobble at the boundary flickers.
            const int32_t hold = (tierRate(up) * kHystNum) / kHystDen;
            if (rate < hold) {
                mult_ = target;
                st_.down++;
            }
        }
        return mult_;
    }

    static int32_t tierRate(int mult)
    {
        return mult >= 8 ? kRate8 : mult >= 4 ? kRate4 : mult >= 2 ? kRate2 : 0;
    }

    void noteWindow(int mag)
    {
        const unsigned b = mag == 0 ? 0u : mag == 1 ? 1u : mag <= 3 ? 2u : mag <= 7 ? 3u : 4u;
        st_.hist[b]++;
    }

    Stats st_{};
    int64_t rate_x256_ = 0;      // smoothed detents/s, scaled by 256
    int mult_ = 1;
    int dir_ = 0;                // last moving direction (0 = none yet)
    uint32_t last_ms_ = 0;
    uint32_t last_move_ms_ = 0;
    bool enabled_ = true;
};

/* The one state the board runs with, and the two switches that reach it from
 * another task — the same shape as `encoder_hil.h`'s accessors: an inline
 * function with a static local, so the header can be included where it is needed
 * and there is still exactly one of each.
 *
 * `enc_accel_enabled()` is atomic because the knob's windows are fed by the LVGL
 * task while `ENC_ACCEL 0|1` arrives on the CDC task. The LVGL task copies the
 * flag into the instance at the top of each window, so everything else in the
 * instance belongs to one task; a read-out that wants the counters takes the
 * LVGL port lock the way the other UI verbs do. */
inline EncoderAccel& encoderAccel()
{
    static EncoderAccel accel;
    return accel;
}

inline std::atomic<bool>& enc_accel_enabled()
{
    static std::atomic<bool> on{true};
    return on;
}

/* Off by default: `ENC_ROTATE` injects a diff that means exactly what it says,
 * and the scenarios built on it count on that. With it on, an injected diff
 * enters the same window as the knob's own detents and is multiplied with them,
 * which is what lets a ladder show the tiers on hardware at a scripted rate. */
inline std::atomic<bool>& enc_accel_hil_inject()
{
    static std::atomic<bool> on{false};
    return on;
}

}  // namespace crosspad
