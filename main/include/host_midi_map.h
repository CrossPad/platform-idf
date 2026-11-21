// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file host_midi_map.h
 * @brief The host control surface's decisions, with no board in sight: what a
 *        incoming Control Change addresses, how a 7-bit value turns into a
 *        level, and what one frame of host-driven pad LEDs looks like.
 *
 * Kept header-only and free of ESP / LVGL / FreeRTOS includes so
 * main/tests/test_host_midi.cpp pins it on the host (the same reason kit_pc.h
 * exists). Everything in here is data and arithmetic; taking an action is the
 * platform's job, in main/host_midi.cpp.
 *
 * Why a table and not fixed assignments: on this board the thing a CC should
 * reach is decided by what is playing -- the mixer's channels are named by the
 * engine that registered them ("Sampler", "Sampler2", "Preview", "USB"), and
 * those come and go with the kit and the song. A person wiring a DAW to a
 * CrossPad therefore needs to *see* the map (`MIDI_CC MAP`) and point a CC at
 * what they have, the way they point a fader at a strip on a desk.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace crosspad {
namespace hostmidi {

/// What a Control Change can address on this board.
enum class Target : uint8_t {
    None = 0,     ///< the CC is left alone: it means nothing local, only forwarded
    Channel,      ///< a mixer channel's fader (AudioMixerEngine::setChannelVolume)
    Output,       ///< a mixer output bus (setOutputVolume)
    Hardware,     ///< an output's own level after the DAC (IOutputLevels::setLevel, 0..100)
    Pad,          ///< one pad's level in the kit's pad mix (IPadMix::setPadMix)
};

inline const char *targetName(Target t)
{
    switch (t) {
    case Target::Channel:  return "channel";
    case Target::Output:   return "output";
    case Target::Hardware: return "hardware";
    case Target::Pad:      return "pad";
    default:               return "none";
    }
}

/// One entry of the map: what a (channel, CC) pair is worth, and to what.
///
/// `name` is how a channel is addressed across a reboot: the mixer's channel
/// list is whatever the engines registered, in registration order, so an index
/// found today is not promised to tomorrow's boot. A named target is resolved
/// when the message arrives and, if it is not there, refused -- never guessed
/// at. Index-addressed targets (outputs, pads) are stable by what they are.
struct Rule {
    uint8_t channel = 0;      ///< 0..15, as the core API numbers channels
    uint8_t cc = 0;           ///< 0..127
    Target target = Target::None;
    int index = -1;           ///< for Output / Hardware / Pad
    char name[24] = {};       ///< for Channel: the mixer name to look for
    bool isPan = false;       ///< a bipolar target (pan) rather than a level
};

/// How many (channel, CC) pairs can be live at once. The desk the board really
/// has: sixteen channels and four outputs is 20; the pads are 16 more.
static constexpr size_t kMaxRules = 48;

/// The defaults a board ships with, in the order a person would expect to read
/// them: channel 1 (0-based 0) is the pad channel the notes come from, and the
/// speakers are the level a player wants to kill from a DAW without a menu.
///
/// A mixer *channel* default is deliberately absent here: which channel names
/// exist depends on the kit and the song at this very moment, so the first
/// message that asks for one resolves the name then and there (and refuses,
/// with a reason, if nothing of that name is registered).
inline void defaultRules(Rule *rules, size_t *count)
{
    if (!rules || !count) return;
    size_t n = 0;
    Rule r;
    // CC 10 on the pad channel: the sampler's own balance, if a desk has one.
    r = Rule{};
    r.channel = 0;
    r.cc = 10;
    r.target = Target::Channel;
    r.isPan = true;
    std::strncpy(r.name, "Sampler", sizeof(r.name) - 1);
    rules[n++] = r;
    // CC 7 on channel 16: the built-in speakers, in the hardware, so the hiss
    // goes down with it (IOutputLevels), not the mix.
    r = Rule{};
    r.channel = 15;
    r.cc = 7;
    r.target = Target::Hardware;
    r.index = 0;
    rules[n++] = r;
    *count = n;
}

/// The rule for a (channel, cc) pair, or null when nothing is mapped.
inline const Rule *findRule(const Rule *rules, size_t count, uint8_t channel, uint8_t cc)
{
    for (size_t i = 0; i < count; ++i) {
        if (rules[i].cc == cc && rules[i].channel == channel) return &rules[i];
    }
    return nullptr;
}

/**
 * A 7-bit volume as the board uses it: 0 is silence, 127 is unity, and nothing
 * in between is linear -- a linear taper puts most of the useful range in the
 * last third of the fader, which is why mixers are not built that way. The
 * curve here is the simple 1..127 quadratics a hardware desk uses; a host that
 * sends 100 still gets the top.
 */
inline float levelFrom7(uint8_t v)
{
    if (v >= 127) return 1.0f;
    if (v == 0) return 0.0f;
    const float x = static_cast<float>(v) / 127.0f;
    return x * x;
}

/// A 7-bit pan as the mixer wants it: -1 left, 0 centre (unity on both sides),
/// +1 right. 64 is the wire's centre; 63 and 65 are one step off it, and the
/// two sides are not quite the same size because 0 and 127 are both extremes.
inline float panFrom7(uint8_t v)
{
    if (v < 64) return -static_cast<float>(64 - v) / 64.0f;
    if (v > 64) return static_cast<float>(v - 64) / 63.0f;
    return 0.0f;
}

/// The same level as a 0..100 hardware step (IOutputLevels), so a CC can turn
/// a speaker down in the analog stage instead of in the mix.
inline int percentFrom7(uint8_t v)
{
    const float full = levelFrom7(v);
    int pct = static_cast<int>(full * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/// And back to 7 bits, for the read-out that tells a host what the board is at
/// now -- so `MIDI_CC MAP` can show a level a person can compare against their
/// own console without a second translation.
///
/// A 0..1 ratio (what `levelFrom7` produced) back to the 7-bit value that
/// reproduces it -- the same curve inverted, which is why 64 of 127 is 50 %.
inline uint8_t sevenFromLevel(float level)
{
    if (level <= 0.0f) return 0;
    if (level >= 1.0f) return 127;
    /* The inverse of levelFrom7(): x = sqrt(level), back onto 0..127. */
    const int v = static_cast<int>(0.5f + 127.0f * std::sqrt(level));
    return static_cast<uint8_t>(v > 127 ? 127 : (v < 0 ? 0 : v));
}

/// Bounded control: what a host took, it gives back.
///
/// A Control records the board's own value for one rule's target the first time a
/// host writes through that rule, and what `give` hands back is exactly that. The
/// rule is copied in rather than looked up again, because the map may be cleared
/// between taking control and giving it back - clearing the map *is* a handover.
/// A take that already holds is a no-op: the second CC of a ride must not overwrite
/// the value the board had before the host started.
struct Control {
    float before;   ///< what the board itself was at, when the host first touched it
    Rule rule;      ///< the addressing to write it back through
    bool held;
};

inline void controlClear(Control &c)
{
    c.before = 0.0f;
    c.held = false;
}

inline void controlTake(Control &c, const Rule &rule, float boardValue)
{
    if (c.held) return;
    c.before = boardValue;
    c.rule = rule;
    c.held = true;
}

/// True when this gave something back; false when nothing was held, so the caller
/// can count handovers rather than guess at them.
inline bool controlGive(Control &c)
{
    if (!c.held) return false;
    c.held = false;
    return true;
}

/// The pad-LED lease: 0 means the host holds the pads until it says otherwise,
/// anything else gives them back once it has been silent that long. `last` is
/// renewed by any traffic on the host surface - a mapped Control Change as much as
/// a painted pad - because the question is whether a host is there, not whether it
/// is painting right now. A lease that ran out is not renewed by a later frame
/// either: the next paint simply takes a new one.
inline bool leaseTimedOutUs(int64_t now, int64_t last, int leaseMs)
{
    return leaseMs > 0 && (now - last) > static_cast<int64_t>(leaseMs) * 1000;
}

/// A pan back to 7 bits, for a board that tells its host what the desk is doing:
/// the exact inverse of `panFrom7`, including the asymmetry that side of centre is
/// 64 steps wide (0..63) and the right side 63 (65..127), with 64 centre.
inline uint8_t sevenFromPan(float pan)
{
    const float wire = (pan <= 0.0f) ? (64.0f + 64.0f * pan) : (64.0f + 63.0f * pan);
    const int v = static_cast<int>(0.5f + wire);   // rounded: truncation would lose
    return static_cast<uint8_t>(v > 127 ? 127 : (v < 0 ? 0 : v));   // a step at 1/63
}

/// Whether the board has something to tell the host about this rule's value.
/// -1 means "not looked yet", which seeds a slot without sending anything -- a rule
/// that has just been mapped must not shout at the host about a level nobody moved
/// -- and equal values mean there is nothing to say. A handover (`MIDI_CC GIVE`)
/// deliberately leaves the baseline at what the host last asked for, so the board
/// does announce the level going back to where the board stood.
inline bool echoNeeded(int prev, int now)
{
    return prev >= 0 && prev != now;
}

/* ── Pad LED frames ────────────────────────────────────────────────────── */

/// One pad of a frame. 8 bits per component, the way a Launchpad-style host
/// sends it; the board's own brightness and the app's colours still apply
/// underneath (see the lease in main/host_midi.cpp).
struct LedPad {
    uint8_t r = 0, g = 0, b = 0;
    bool on = false;        ///< false: this pad is not addressed by the frame
};

/// One frame: all sixteen pads, in the board's pad order (0..15, the order the
/// kit browser and LED_STATE report).
struct LedFrame {
    LedPad pad[16];
    uint32_t seq = 0;       ///< bumped by the sender; the applier takes the newest only
};

/**
 * Parse a frame given as 96 hex characters (48 bytes: r,g,b per pad, pad 0
 * first) -- the compact form a host script sends for an animation, one verb
 * call per frame instead of sixteen. Returns false without touching @p out when
 * the text is not exactly 96 hex digits, so a half-typed frame never lights
 * anything.
 */
inline bool parseFrameHex(const char *text, LedFrame *out)
{
    if (!text || !out) return false;
    size_t n = 0;
    while (text[n] && n < 97) ++n;
    if (n != 96) return false;        // exactly 96 digits: neither short nor cut
    auto dig = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto byteAt = [&dig, &text](size_t charIndex, uint8_t *into) -> bool {
        const int hi = dig(text[charIndex]), lo = dig(text[charIndex + 1]);
        if (hi < 0 || lo < 0) return false;
        *into = static_cast<uint8_t>((hi << 4) | lo);
        return true;
    };
    /* Written field by field, not through the struct's bytes: a pad is four
     * members here (r, g, b and whether the frame addresses it at all), and
     * pretending otherwise would quietly paint the wrong pads. */
    LedFrame f;
    for (size_t p = 0; p < 16; ++p) {
        uint8_t r, g, b;
        if (!byteAt(p * 6, &r) || !byteAt(p * 6 + 2, &g) || !byteAt(p * 6 + 4, &b)) return false;
        f.pad[p] = LedPad{r, g, b, true};
    }
    f.seq = 0;
    *out = f;
    return true;
}

/// One chase frame: the lit pad moves, everything else off. Pure arithmetic so
/// the animation a board runs can be pinned without hardware.
inline void chaseFrame(uint32_t step, LedFrame *out, uint8_t r, uint8_t g, uint8_t b)
{
    if (!out) return;
    LedFrame f;
    const int lit = static_cast<int>(step % 16u);
    for (int p = 0; p < 16; ++p) {
        f.pad[p].on = true;
        f.pad[p].r = (p == lit) ? r : 0;
        f.pad[p].g = (p == lit) ? g : 0;
        f.pad[p].b = (p == lit) ? b : 0;
    }
    *out = f;
}

/// A hue around the ring, one hue per pad: the frame a person uses to check
/// that the wiring and the pad order are what they think they are.
inline void ringFrame(uint32_t step, LedFrame *out, uint8_t brightness)
{
    if (!out) return;
    static const uint8_t kHue[16][3] = {
        {255, 0, 0}, {255, 42, 0}, {255, 128, 0}, {255, 212, 0},
        {170, 255, 0}, {85, 255, 0}, {0, 255, 42}, {0, 255, 128},
        {0, 255, 212}, {0, 170, 255}, {0, 85, 255}, {42, 0, 255},
        {128, 0, 255}, {212, 0, 255}, {255, 0, 170}, {255, 0, 85}
    };
    LedFrame f;
    const uint32_t off = step % 16u;
    const float s = brightness / 255.0f;
    for (int p = 0; p < 16; ++p) {
        const uint8_t *c = kHue[(p + off) % 16u];
        f.pad[p].on = true;
        f.pad[p].r = static_cast<uint8_t>(c[0] * s + 0.5f);
        f.pad[p].g = static_cast<uint8_t>(c[1] * s + 0.5f);
        f.pad[p].b = static_cast<uint8_t>(c[2] * s + 0.5f);
    }
    *out = f;
}

}  // namespace hostmidi
}  // namespace crosspad
