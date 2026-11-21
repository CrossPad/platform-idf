/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
 * @file host_midi.cpp
 * @brief What a host's Control Changes and pad-LED frames do to this board,
 *        and the verbs that make the arrangement visible and changeable.
 *
 * The decisions (which (channel, CC) means what, how a 7-bit value becomes a
 * level, what one frame of colours is) are in main/include/host_midi_map.h and
 * pinned on the host by main/tests/test_host_midi.cpp. This file is the part
 * that touches a board, and it touches it from one place only: the parking
 * side (the MIDI task, and the main task for a verb) leaves nothing but a
 * handful of bytes and a flag, and the `bsp_worker` job applies them -- the
 * same single worker task that already runs `pad_anim` (main.cpp:739, :1252),
 * which is what the coalesced LED refresh assumes (CLAUDE.md, "Changing Kits
 * While Playing": "Pad LED frames go out from the `pad_anim` worker only").
 *
 * Two rules this file is built around:
 *  - the MIDI task never blocks and never reaches into the mixer or the strip:
 *    a stall there is a dropped MIDI byte and an audio glitch
 *    (main/midiCallbacks.cpp:684-747, docs/subsystems/kit-swap.md);
 *  - a host may take the pads' colours, but the board must be able to give them
 *    back: the first frame takes a lease, and a lease remembers what the board
 *    itself was showing, so `LED_HOST OFF` hands over exactly the colours it
 *    found -- not the global defaults, which would quietly erase an app's own
 *    pad colours.
 *
 * The Control Change map lives in RAM: which mixer channels exist depends on
 * the kit and the song running now (docs/subsystems/sampler-engine.md: the
 * native engine's channel is "Sampler", the prebuilt one's "Sampler2"), so a
 * saved number would point at nothing after a reboot with a different kit. A
 * host sets up its mapping at the start of a session -- which is what a DAW
 * template does anyway -- and `MIDI_CC MAP` shows what a board answers to.
 */

#include "host_midi.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "bsp_worker.h"

#include "crosspad/audio/IOutputLevels.hpp"
#include "crosspad/midi/MidiInputHandler.hpp"
#include "crosspad/led/RgbColor.hpp"
#include "crosspad/pad/IPadMix.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include <crosspad-mixer/AudioMixerEngine.hpp>
#define HOSTMIDI_HAS_MIXER 1
#else
#define HOSTMIDI_HAS_MIXER 0
#endif

#include "host_midi_map.h"
#include "surface_map.h"
#include "midi_kit_pc.h"      /* the kit the board announces on the same wire */

static const char *TAG = "host_midi";

using crosspad::hostmidi::LedFrame;
using crosspad::hostmidi::Rule;
using crosspad::hostmidi::Target;

namespace {

constexpr int kPadCount = 16;         /* the board's sixteen pads */
constexpr int kCcRing = 16;           /* CCs parked between two applies */

/* ── the map ───────────────────────────────────────────────────────────── */
Rule s_rules[crosspad::hostmidi::kMaxRules];
size_t s_rule_count = 0;

/* ── parking: MIDI task in, worker out ─────────────────────────────────── */
/* One producer (uartLoop, the only task that decodes a host's CC) and one
 * consumer (the worker): a plain ring of packed words, no locks. A full ring
 * drops the oldest, which is the right thing for a fader being moved -- the
 * newest position is the one that matters. */
std::atomic<uint32_t> s_cc_head{0};
std::atomic<uint32_t> s_cc_tail{0};
uint32_t s_cc_ring[kCcRing];

/* Pad-LED requests, newest wins -- but per pad, not globally. One word per pad
 * plus one for an all-pads frame: a host painting a chase sends several one-pad
 * frames inside one 25 ms pass, and with a single shared word the earlier pads
 * of that burst would vanish before they were ever painted (measured: three
 * `LED_HOST SET` calls in a row landed fine because each round trip took longer
 * than the pass, so this only bites the SysEx path, where frames arrive as fast
 * as a host can send them). Exchange per slot, apply in one pass: all-pads
 * first, then the per-pad colours that came with it. */
constexpr uint32_t kLedValid = 1u << 31;
inline uint32_t pack_led(uint8_t r, uint8_t g, uint8_t b)
{
    return kLedValid | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}
std::atomic<uint32_t> s_pad_word[kPadCount];
std::atomic<uint32_t> s_all_word{0};

/* A request that the host is done with the pads, from any task. */
std::atomic<bool> s_release{false};

/* A whole-grid frame: staged by the main task only (the verb over CDC), then
 * published with a release store the worker reads with acquire. SysEx carries
 * the one-pad and all-pads forms instead -- a 96-character grid has no place in
 * a frame whose payload is already taken by routing and arguments, and a host
 * driving an animation over SysEx sends one pad per frame at 16 the rate. */
uint8_t s_grid_stage[3 * kPadCount];
std::atomic<bool> s_grid_ready{false};

/* ── state the worker owns ─────────────────────────────────────────────── */
/* The lease a host takes over the pad LEDs gives itself back after this long of
 * host silence - a pad panel is the host's to draw on, but a board that stays
 * painted by a program that died is a board the player in front of it cannot
 * bring back. `LED_HOST LEASE 0` asks for the old sticky behaviour, `LEASE AUTO`
 * returns to this. */
constexpr int kDefaultLeaseMs = 5000;

bool s_leased = false;
uint8_t s_lease_rgb[kPadCount][3];       /* what the board was showing */
int s_anim = -1;                         /* -1 none, 0 chase, 1 ring */
uint32_t s_anim_step = 0;
int64_t s_anim_next_us = 0;
int s_anim_period_ms = 120;
int s_lease_ms = kDefaultLeaseMs;
/* The last thing any host put on this surface, in microseconds: a paint, a grid,
 * an animation step, or a Control Change that matched a rule. Only the worker and
 * the MIDI callback task write it, both from one store, so an atomic keeps the
 * 64-bit read on the worker honest against a store from another core. */
std::atomic<int64_t> s_last_host_us{0};

/* counters, for the one question a person asks: did my message reach the board
 * at all, and if not, why */
std::atomic<uint32_t> s_cc_seen{0};
std::atomic<uint32_t> s_cc_applied{0};
std::atomic<uint32_t> s_cc_unmapped{0};
std::atomic<uint32_t> s_cc_refused{0};
std::atomic<uint32_t> s_frames{0};
/* What the host drove on the mixer, per rule slot: handed back on `MIDI_CC GIVE`,
 * and when the map itself is cleared. Pad levels are not in this: the engine's pad
 * mix has no getter to read the board's own value from, so there is nothing to
 * give back. */
crosspad::hostmidi::Control s_ctrl[crosspad::hostmidi::kMaxRules];
std::atomic<bool> s_give_levels{false};
std::atomic<uint32_t> s_heldNow{0};
std::atomic<uint32_t> s_gives{0};

/* The other direction: the board telling a host what moved on the desk, so a DAW's
 * faders follow what a person did at the board instead of standing where they were
 * last told to be. Three rules make this quiet rather than a loop:
 *   - a slot is *seeded* the first time it is read, and a seeded slot sends nothing
 *     (a rule that has just been mapped must not shout about a level nobody moved);
 *   - a Control Change the board applied from a host re-baselines the slot, so what
 *     the host asked for never comes back at it;
 *   - a handover deliberately does not re-baseline: the level going back to where
 *     the board stood is exactly the change a host wants to hear about.
 * It runs as its own worker job, not inside the apply pass: the reads are mixer
 * getters, and a moving fader should cost one pass a tenth of a second, not a
 * message per CC. The send goes out through the same fan-out the encoder's CC 1 and
 * CC 64 use. `MIDI_CC ECHO off` silences it for a host that does not want it. */
bool s_echo_on = true;
int16_t s_echo_prev[crosspad::hostmidi::kMaxRules];
std::atomic<uint32_t> s_echoed{0};
bsp_worker_job_t *s_echo_job = nullptr;

std::atomic<uint32_t> s_leases{0};
std::atomic<uint32_t> s_releases{0};

/* ── the SysEx control surface (main/include/surface_map.h for what it means) ──
 *
 * A host addresses a parameter directly and writes blocks of them, which is how a
 * control page or an animation paints sixteen pads in one 24-byte message instead of
 * sixteen messages. The grammar and the give-back rule live in the pure header; this is
 * the parking and the applying, and it follows the contract of everything else in this
 * file: the SysEx task parks, the worker applies, one task writes the LED model and the
 * mixer.
 *
 * Claims are kept here per address, in the *board's* units rather than the wire's:
 * comparing a written 7-bit value against a read-back 7-bit value would be wrong for a
 * hardware output, whose wire value goes through a 0..100 percent step and comes back a
 * step off (finding F-18: a 90 asked for as 7 bits reads back as 18). The board's own
 * number is what the write actually set, so that is what a restore compares against.
 */
constexpr int kSurfRing = 8;
/* The layout is the shared one in surface_map.h, not a local twin: the SysEx handler
 * fills it in on the MIDI task and this file reads it on the worker. */
crosspad::surface::Action s_surf_ring[kSurfRing];
std::atomic<uint32_t> s_surf_head{0};
std::atomic<uint32_t> s_surf_tail{0};

/* Palette entries, staged then published the way a grid frame is (the SysEx task only
 * copies bytes). Doubled to the strip's 8-bit on the way in, exactly like the existing
 * LED_PAD/LED_GRID frames. */
uint8_t s_pal_stage_rgb[3 * crosspad::surface::kMaxBlock];
uint8_t s_pal_stage_index = 0;
uint8_t s_pal_stage_n = 0;
std::atomic<bool> s_pal_ready{false};
uint8_t s_palette[crosspad::surface::kPalette][3];
uint32_t s_palette_rev = 0;          // bumped on every load: a host can see its own writes

/* Per-pad claims: what the surface last wrote there, and what the board stood at
 * before the surface first touched it. `pad_hold` is the surface's per-pad answer to
 * the 5 s lease timeout: a pad a host explicitly asked to hold is not taken back when
 * it goes quiet, and a pad it did not is (docs/subsystems/surface-sysex.md). */
uint8_t s_surf_written[16][3];
uint8_t s_surf_before[16][3];
std::atomic<uint32_t> s_surf_claim{0};
std::atomic<uint32_t> s_pad_hold{0};

/* Per-address claims for the level sections, in the board's units. Small on purpose:
 * 48 slots of four bytes, because internal RAM is what a board like this runs out of
 * first (CLAUDE.md: BLE alone costs 30-60 kB). */
struct SurfLevel {
    uint8_t held;
    int32_t wrote;      ///< scaled: level x1000, pan (pan+1)x1000, hardware percent
    int32_t before;
};
SurfLevel s_surf_lvl[4][16];         ///< ChVolume, ChPan, OutVolume, HwLevel

std::atomic<uint32_t> s_surf_writes{0};      ///< blocks applied
std::atomic<uint32_t> s_surf_values{0};      ///< individual values that landed
std::atomic<uint32_t> s_surf_missed{0};      ///< and the ones whose target was not there
std::atomic<uint32_t> s_surf_restores{0};    ///< addresses handed back
std::atomic<uint32_t> s_surf_skips{0};       ///< refused because the board moved them
std::atomic<uint32_t> s_surf_refused{0};     ///< frames that did not decode
std::atomic<uint32_t> s_surf_drops{0};       ///< parked frames lost to a full ring

void apply_surface_pass(void);       // defined below the worker job that calls it

int64_t now_us(void) { return esp_timer_get_time(); }

/* A mixer channel by name, the way the desk names them; -1 when nothing of
 * that name is registered right now. */
int find_channel(const char *name)
{
#if HOSTMIDI_HAS_MIXER
    auto &mx = getMixerEngine();
    const uint8_t n = mx.numActiveChannels();
    for (uint8_t c = 0; c < n; ++c) {
        const char *have = mx.getChannelName(c);
        if (have && strcasecmp(have, name) == 0) return c;
    }
#else
    (void)name;
#endif
    return -1;
}

/* The target's current value, from the board's own readback. False when the kind
 * of target has no getter (an engine pad mix) or the named channel is not on the
 * desk right now - then there is nothing to hand back later, and the write stands
 * on its own. */
bool current_level_of(const Rule &r, float *out)
{
    switch (r.target) {
    case Target::Channel:
#if HOSTMIDI_HAS_MIXER
    {
        const int c = r.name[0] ? find_channel(r.name) : r.index;
        if (c < 0) return false;
        auto &mx = getMixerEngine();
        *out = r.isPan ? mx.getChannelPan(static_cast<uint8_t>(c))
                       : mx.getChannelVolume(static_cast<uint8_t>(c));
        return true;
    }
#else
        return false;
#endif
    case Target::Output:
#if HOSTMIDI_HAS_MIXER
    {
        auto &mx = getMixerEngine();
        if (r.index < 0 || r.index >= mx.numOutputs()) return false;
        *out = mx.getOutputVolume(static_cast<uint8_t>(r.index));
        return true;
    }
#else
        return false;
#endif
    case Target::Hardware: {
        crosspad::IOutputLevels *lv = crosspad::getPlatformServices().outputLevels;
        if (!lv || r.index < 0 || r.index >= lv->outputs()) return false;
        *out = static_cast<float>(lv->level(r.index));
        return true;
    }
    case Target::Pad:
    case Target::None:
        return false;
    }
    return false;
}

bool write_level_of(const Rule &r, float value)
{
    switch (r.target) {
    case Target::Channel:
#if HOSTMIDI_HAS_MIXER
    {
        const int c = r.name[0] ? find_channel(r.name) : r.index;
        if (c < 0) return false;
        auto &mx = getMixerEngine();
        if (r.isPan) mx.setChannelPan(static_cast<uint8_t>(c), value);
        else mx.setChannelVolume(static_cast<uint8_t>(c), value);
        return true;
    }
#else
        return false;
#endif
    case Target::Output:
#if HOSTMIDI_HAS_MIXER
    {
        auto &mx = getMixerEngine();
        if (r.index < 0 || r.index >= mx.numOutputs()) return false;
        mx.setOutputVolume(static_cast<uint8_t>(r.index), value);
        return true;
    }
#else
        return false;
#endif
    case Target::Hardware: {
        crosspad::IOutputLevels *lv = crosspad::getPlatformServices().outputLevels;
        if (!lv || r.index < 0 || r.index >= lv->outputs()) return false;
        lv->setLevel(r.index, static_cast<int>(value));
        return true;
    }
    case Target::Pad:
    case Target::None:
        return false;
    }
    return false;
}

/* Snapshot the board's own value for this rule's target, once: the first CC of a
 * ride records what to hand back later, and the ones after it must not move that
 * value. Counted here and at the give, so `MIDICC:` can show how many levels the
 * host currently holds without walking the table from another task. */
/* A target's value in the 7-bit form the host uses: a level through the inverse of
 * the squaring curve (hardware levels are 0..100, mixer levels are 0..1), a balance
 * through the inverse of the pan curve. Same arithmetic `MIDI_CC MAP` already
 * prints as `cur=`, which is the point: what the board announces is what its own
 * read-out says, so host and read-out cannot disagree. */
int seven_of(const Rule &r, float value)
{
    if (r.isPan) return crosspad::hostmidi::sevenFromPan(value);
    if (r.target == Target::Hardware) return crosspad::hostmidi::sevenFromLevel(value / 100.0f);
    return crosspad::hostmidi::sevenFromLevel(value);
}

/* Every slot "not read yet": used at init and whenever the map changes, so mapping
 * a rule never sends a message about a level that was already standing there. */
void seed_echo(void)
{
    for (size_t i = 0; i < crosspad::hostmidi::kMaxRules; ++i) s_echo_prev[i] = -1;
}

void take_control(size_t i, const Rule &r, float was)
{
    if (s_ctrl[i].held) return;
    crosspad::hostmidi::controlTake(s_ctrl[i], r, was);
    s_heldNow.fetch_add(1, std::memory_order_relaxed);
}

void apply_cc(uint32_t word)
{
    const uint8_t ch = static_cast<uint8_t>((word >> 16) & 0x0F);
    const uint8_t cc = static_cast<uint8_t>((word >> 8) & 0x7F);
    const uint8_t val = static_cast<uint8_t>(word & 0x7F);
    const Rule *rule = crosspad::hostmidi::findRule(s_rules, s_rule_count, ch, cc);
    if (!rule) {
        s_cc_unmapped.fetch_add(1);
        return;
    }
    /* Which slot of the control table this rule owns: the same index the snapshot
     * is kept under, and the reason findRule returns a pointer into s_rules. */
    const size_t idx = static_cast<size_t>(rule - s_rules);
    switch (rule->target) {
    case Target::None:
        s_cc_unmapped.fetch_add(1);
        break;
    case Target::Channel: {
#if HOSTMIDI_HAS_MIXER
        const int c = rule->name[0] ? find_channel(rule->name) : rule->index;
        if (c < 0) {
            /* Named after a channel the kit or the song does not have right
             * now: say which, once per refusal, and leave the level alone. */
            s_cc_refused.fetch_add(1);
            ESP_LOGW(TAG, "CC ch%u cc%u named \"%s\", which is not on the desk now", ch + 1, cc,
                     rule->name);
            break;
        }
        auto &mx = getMixerEngine();
        float was = 0.0f;
        if (current_level_of(*rule, &was))
            take_control(idx, *rule, was);
        /* Baseline first, then the write: `host_echo` reads the desk on another task
         * than this one applies it on, and a value that lands before its baseline
         * would be read as a change the board made and sent back at the host that
         * just asked for it. A write that then fails leaves the baseline a step
         * ahead of reality, which the next pass corrects by announcing what the desk
         * really is at - the honest direction for the mistake to go in. */
        s_echo_prev[idx] = static_cast<int16_t>(val);
        if (rule->isPan) mx.setChannelPan(static_cast<uint8_t>(c),
                                          crosspad::hostmidi::panFrom7(val));
        else mx.setChannelVolume(static_cast<uint8_t>(c),
                                 crosspad::hostmidi::levelFrom7(val));
        s_cc_applied.fetch_add(1);
#endif
        break;
    }
    case Target::Output: {
#if HOSTMIDI_HAS_MIXER
        auto &mx = getMixerEngine();
        if (rule->index < 0 || rule->index >= mx.numOutputs()) {
            s_cc_refused.fetch_add(1);
            break;
        }
        float was = 0.0f;
        if (current_level_of(*rule, &was))
            take_control(idx, *rule, was);
        s_echo_prev[idx] = static_cast<int16_t>(val);   // baseline before the write
        mx.setOutputVolume(static_cast<uint8_t>(rule->index),
                           crosspad::hostmidi::levelFrom7(val));
        s_cc_applied.fetch_add(1);
#endif
        break;
    }
    case Target::Hardware: {
        crosspad::IOutputLevels *lv = crosspad::getPlatformServices().outputLevels;
        if (!lv || rule->index < 0 || rule->index >= lv->outputs()) {
            s_cc_refused.fetch_add(1);
            break;
        }
        float was = 0.0f;
        if (current_level_of(*rule, &was))
            take_control(idx, *rule, was);
        s_echo_prev[idx] = static_cast<int16_t>(val);   // baseline before the write
        lv->setLevel(rule->index, crosspad::hostmidi::percentFrom7(val));
        s_cc_applied.fetch_add(1);
        break;
    }
    case Target::Pad: {
        crosspad::IPadMix *mix = crosspad::getPlatformServices().padMix;
        if (!mix || rule->index < 0 || rule->index >= kPadCount) {
            s_cc_refused.fetch_add(1);
            break;
        }
        /* Live only: this is the level the engine plays at now, not the kit's
         * stored value -- the PAD_MIX verb writes the kit model, from the LVGL
         * thread, and marks it modified (main/hil_control.cpp:393-415). A CC
         * fader riding a pad's level from a DAW is a playing decision, not an
         * edit of the kit, and it must not silently make kit.json dirty. */
        mix->setPadMix(static_cast<uint8_t>(rule->index),
                       crosspad::hostmidi::percentFrom7(val), rule->isPan ?
                           static_cast<int>(crosspad::hostmidi::panFrom7(val) * 100.0f) : 0);
        s_cc_applied.fetch_add(1);
        break;
    }
    }
}

/* Hand the levels back: every rule the host drove through this surface goes back to
 * what the board had before it started. Runs on the worker, like the writes it
 * undoes, and reads only s_ctrl - the rules may have been cleared in the meantime,
 * which is one of the ways a host gives up control, hence the copy per control. */
void give_levels(void)
{
    size_t n = 0;
    for (size_t i = 0; i < crosspad::hostmidi::kMaxRules; ++i) {
        if (!s_ctrl[i].held) continue;
        const Rule r = s_ctrl[i].rule;
        const float was = s_ctrl[i].before;
        if (crosspad::hostmidi::controlGive(s_ctrl[i])) {
            ++n;
            write_level_of(r, was);
        }
    }
    if (n) {
        s_gives.fetch_add(static_cast<uint32_t>(n));
        s_heldNow.fetch_sub(static_cast<uint32_t>(n));
    }
}

/* The colours the board itself had when the host took the pads. Read through
 * the LED model, so an app's own pad colours are part of what comes back.
 *
 * Neither lease function logs: a line out of the console UART is a couple of
 * milliseconds, and these run from the worker job, whose whole contract is a
 * short body (bsp_worker.h) on the task that also paints the pads. What happens
 * to the lease is instead two counters, read out by `LEDHOST:`, and the verbs
 * that asked for it log on the main task. */
void take_lease(void)
{
    if (s_leased) return;
    auto &leds = crosspad::getPadLedController();
    for (int p = 0; p < kPadCount; ++p) {
        const crosspad::RgbColor c = leds.getPadColor(static_cast<uint8_t>(p));
        s_lease_rgb[p][0] = c.R;
        s_lease_rgb[p][1] = c.G;
        s_lease_rgb[p][2] = c.B;
    }
    s_leased = true;
    s_leases.fetch_add(1);
}

bool surface_give_pad(int pad);   // defined with the surface's apply code below

void give_lease_back(void)
{
    if (!s_leased) return;
    s_anim = -1;
    auto &leds = crosspad::getPadLedController();
    const uint32_t held = s_pad_hold.load(std::memory_order_relaxed);
    /* Start from the claims as they are. Masking the held pads out here -- which an
     * earlier version did -- drops their records at the same time the lease gives the
     * other pads back, and then a later `PadMode 0` or SEC_RESTORE aimed at a held pad
     * finds nothing to undo: the pad keeps the colour its host gave it, which is right,
     * but the surface has forgotten that it painted it. The held pad's bit is cleared
     * only by the hand-backs that actually happen below. */
    uint32_t left = s_surf_claim.load(std::memory_order_relaxed);
    for (int p = 0; p < kPadCount; ++p) {
        /* A pad the surface asked to hold (PadMode 1) stays as the surface left it,
         * even when the lease expires: that is the per-pad version of `LED_HOST LEASE
         * 0`, and it is what a Launchpad-style panel needs -- a panel that paints a
         * mode view and means it to stay until it says otherwise, without the whole
         * board becoming sticky for every other host. Its claim stays recorded, so a
         * later SEC_RESTORE can still hand that pad back honestly. */
        if (held & (1u << p)) continue;
        /* A pad the SysEx surface painted goes back to the colour *it* found there, not
         * to whatever the verb-path lease had snapshotted: the two doors each know what
         * they changed, and guessing across them would put a colour back that neither
         * door saw. `surface_give_pad` applies the same "is it still what I left?" test
         * as an explicit SEC_RESTORE does, so a pad an app has since taken over is left
         * alone here too, and counted as a skip either way. */
        if (s_surf_claim.load(std::memory_order_relaxed) & (1u << p)) {
            /* Only forget the record when the hand-back actually happened. A pad the
             * board took over keeps its claim: the surface still cannot touch it, and a
             * later explicit SEC_RESTORE has to be able to notice that again rather than
             * stamp a stale colour onto a pad it no longer owns. */
            if (surface_give_pad(p)) left &= ~(1u << p);
            continue;
        }
        leds.setPadColor(static_cast<uint8_t>(p), s_lease_rgb[p][0], s_lease_rgb[p][1],
                         s_lease_rgb[p][2]);
        left &= ~(1u << p);
    }
    s_surf_claim.store(left, std::memory_order_relaxed);
    s_leased = false;
    s_releases.fetch_add(1);
}

void paint(int pad, uint32_t word)
{
    auto &leds = crosspad::getPadLedController();
    leds.setPadColor(static_cast<uint8_t>(pad), static_cast<uint8_t>((word >> 16) & 0xFF),
                     static_cast<uint8_t>((word >> 8) & 0xFF), static_cast<uint8_t>(word & 0xFF));
}

/* The whole grid from one all-pads word, then the per-pad words that came with
 * it: so a burst of one-pad frames inside a single pass paints every pad it
 * named, in the order the mode means (a frame covering all sixteen first, the
 * later individual colours on top). */
void apply_led_pass(void)
{
    const uint32_t all = s_all_word.exchange(0, std::memory_order_relaxed);
    bool any = false;
    if (all & kLedValid) {
        take_lease();
        for (int p = 0; p < kPadCount; ++p) paint(p, all);
        any = true;
    }
    for (int p = 0; p < kPadCount; ++p) {
        const uint32_t w = s_pad_word[p].exchange(0, std::memory_order_relaxed);
        if (!(w & kLedValid)) continue;
        take_lease();
        paint(p, w);
        any = true;
    }
    if (!any) return;
    s_frames.fetch_add(1);
    s_last_host_us.store(now_us(), std::memory_order_relaxed);
}

uint32_t host_midi_job(void *)
{
    if (s_release.exchange(false, std::memory_order_acquire)) {
        s_anim = -1;
        give_lease_back();
    }
    if (s_give_levels.exchange(false, std::memory_order_acquire)) give_levels();
    /* Control Changes: drain the ring, oldest first, so a fast move ends at the
     * position the host last sent. */
    while (s_cc_tail.load(std::memory_order_relaxed) != s_cc_head.load(std::memory_order_relaxed)) {
        const uint32_t slot = s_cc_tail.load(std::memory_order_relaxed) % kCcRing;
        const uint32_t word = s_cc_ring[slot];
        s_cc_tail.fetch_add(1, std::memory_order_relaxed);
        apply_cc(word);
    }

    /* Pad colours: newest wins per slot; more than one frame a period is
     * pointless, the strip repaints once a pad_anim tick anyway. */
    apply_led_pass();

    /* The SysEx surface, on the same task and in the same pass: a block write and a
     * pad frame cannot disagree about who owns a pad, because they are applied one
     * after the other rather than from two places. */
    apply_surface_pass();

    if (s_grid_ready.exchange(false, std::memory_order_acquire)) {
        take_lease();
        auto &leds = crosspad::getPadLedController();
        for (int p = 0; p < kPadCount; ++p) {
            leds.setPadColor(static_cast<uint8_t>(p), s_grid_stage[p * 3], s_grid_stage[p * 3 + 1],
                             s_grid_stage[p * 3 + 2]);
        }
        s_frames.fetch_add(1);
        s_last_host_us.store(now_us(), std::memory_order_relaxed);
    }

    const int64_t t = now_us();
    if (s_anim >= 0) {
        if (t >= s_anim_next_us) {
            take_lease();
            LedFrame f;
            if (s_anim == 0) crosspad::hostmidi::chaseFrame(s_anim_step, &f, 255, 255, 255);
            else crosspad::hostmidi::ringFrame(s_anim_step, &f, 200);
            auto &leds = crosspad::getPadLedController();
            for (int p = 0; p < kPadCount; ++p) {
                if (!f.pad[p].on) continue;
                leds.setPadColor(static_cast<uint8_t>(p), f.pad[p].r, f.pad[p].g, f.pad[p].b);
                /* The animation has taken this pad, whatever a surface write left there
                 * before. Dropping the claim is honest bookkeeping rather than a loophole:
                 * the record would only ever cause a skip, and keeping it means a host
                 * that switches its own animation off can never get the board's colour
                 * back at all -- the lease give-back below goes through the claim check
                 * for claimed pads, and would refuse every one of them. A host that wants
                 * the pad again after the animation should write it, or hand it to the
                 * board and let it animate. */
                s_surf_claim.fetch_and(~(1u << p), std::memory_order_relaxed);
            }
            ++s_anim_step;
            s_frames.fetch_add(1);
            s_last_host_us.store(t, std::memory_order_relaxed);
            s_anim_next_us = t + static_cast<int64_t>(s_anim_period_ms) * 1000;
        }
    } else if (s_leased && crosspad::hostmidi::leaseTimedOutUs(t, s_last_host_us.load(
                                 std::memory_order_relaxed), s_lease_ms)) {
        /* A host that stopped talking gets its colours back: an animation host
         * that dies in the middle should not leave a board painted. The counter
         * is the record; `LEDHOST:` shows it, and the console stays quiet. */
        give_lease_back();
    }
    return 0;
}

bsp_worker_job_t *s_job = nullptr;

/* ── applying the SysEx surface, on the worker ───────────────────────────── */

/* The board's value for a level section, in the integer units the claim is kept in.
 * One function for both the compare and the restore, so a claim cannot be judged
 * against a different scale than the one it was taken in. */
int board_units_of(crosspad::surface::Sec sec, const Rule &r)
{
    float v = 0.0f;
    if (!current_level_of(r, &v)) return -1;
    if (sec == crosspad::surface::Sec::HwLevel) return static_cast<int>(v + 0.5f);
    if (sec == crosspad::surface::Sec::ChPan) return static_cast<int>((v + 1.0f) * 1000.0f + 0.5f);
    return static_cast<int>(v * 1000.0f + 0.5f);
}

int surf_slot_of(crosspad::surface::Sec sec, int *secIdx)
{
    switch (sec) {
    case crosspad::surface::Sec::ChVolume:  *secIdx = 0; return 16;
    case crosspad::surface::Sec::ChPan:     *secIdx = 1; return 16;
    case crosspad::surface::Sec::OutVolume: *secIdx = 2; return 8;
    case crosspad::surface::Sec::HwLevel:   *secIdx = 3; return 8;
    default:                                *secIdx = 99; return 0;
    }
}

Rule rule_for(crosspad::surface::Sec sec, int addr)
{
    Rule r{};
    r.channel = 0;
    r.cc = 0;
    r.index = addr;
    r.name[0] = '\0';
    switch (sec) {
    case crosspad::surface::Sec::ChVolume:  r.target = Target::Channel;  r.isPan = false; r.cc = 7;  break;
    case crosspad::surface::Sec::ChPan:     r.target = Target::Channel;  r.isPan = true;  r.cc = 10; break;
    case crosspad::surface::Sec::OutVolume: r.target = Target::Output;   r.isPan = false; r.cc = 7;  break;
    default:                                r.target = Target::Hardware; r.isPan = false; r.cc = 7;  break;
    }
    return r;
}

float value_from_units(crosspad::surface::Sec sec, int units)
{
    if (sec == crosspad::surface::Sec::HwLevel) return static_cast<float>(units);
    if (sec == crosspad::surface::Sec::ChPan) return static_cast<float>(units) / 1000.0f - 1.0f;
    return static_cast<float>(units) / 1000.0f;
}

/* Paint one pad from the surface and take the claim the restore needs. No logging:
 * this runs on the worker, whose body has to stay short (bsp_worker.h), so what
 * happened is counters read by `SURFACE:` instead. */
void surface_paint(int pad, uint8_t r, uint8_t g, uint8_t b)
{
    auto &leds = crosspad::getPadLedController();
    /* The lease, first: a pad the surface painted is host-owned, and the bounded lease
     * is the only thing that takes it back when the host dies. Without this call the
     * timeout added in e6aca4f protected the verbs' pads and left the SysEx surface's
     * pads lit forever -- the same failure mode, arriving again through the second door.
     * `PadMode 1` is the deliberate exemption from it, per pad. */
    take_lease();
    const uint32_t claims = s_surf_claim.load(std::memory_order_relaxed);
    if (!(claims & (1u << pad))) {
        const crosspad::RgbColor now = leds.getPadColor(static_cast<uint8_t>(pad));
        s_surf_before[pad][0] = now.R;
        s_surf_before[pad][1] = now.G;
        s_surf_before[pad][2] = now.B;
        s_surf_claim.fetch_or(1u << pad, std::memory_order_relaxed);
    }
    s_surf_written[pad][0] = r;
    s_surf_written[pad][1] = g;
    s_surf_written[pad][2] = b;
    leds.setPadColor(static_cast<uint8_t>(pad), r, g, b);
}

/* Hand one pad back, and only if the pad still shows what the surface left there.
 * The board lights pads for what is playing (docs/subsystems/sample-tools.md, and the
 * apps set pad colours as their own feedback), so a restore that ignores that stamps
 * a colour over information the player is being given. A pad that has been taken over
 * is counted as a skip, not restored and not silently passed over. */
bool surface_give_pad(int pad)
{
    if (!(s_surf_claim.load(std::memory_order_relaxed) & (1u << pad))) return false;
    auto &leds = crosspad::getPadLedController();
    const crosspad::RgbColor now = leds.getPadColor(static_cast<uint8_t>(pad));
    if (now.R != s_surf_written[pad][0] || now.G != s_surf_written[pad][1]
        || now.B != s_surf_written[pad][2]) {
        s_surf_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    leds.setPadColor(static_cast<uint8_t>(pad), s_surf_before[pad][0], s_surf_before[pad][1],
                     s_surf_before[pad][2]);
    s_surf_claim.fetch_and(~(1u << pad), std::memory_order_relaxed);
    s_surf_restores.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void apply_surface_write(const crosspad::surface::Action &a)
{
    using namespace crosspad::surface;
    const Sec sec = static_cast<Sec>(a.sec);
    switch (sec) {
    case Sec::PadMode:
        for (int i = 0; i < a.n; ++i) {
            const int pad = a.start + i;
            if (a.v[i]) s_pad_hold.fetch_or(1u << pad, std::memory_order_relaxed);
            else {
                s_pad_hold.fetch_and(~(1u << pad), std::memory_order_relaxed);
                surface_give_pad(pad);      // the host's way of handing one pad back
            }
        }
        s_last_host_us.store(now_us(), std::memory_order_relaxed);
        break;
    case Sec::PadColour:
        for (int i = 0; i < a.n; ++i) {
            const int idx = a.v[i];
            if (idx >= kPalette) continue;   // decoded range-checked; belt and braces
            const int pad = a.start + i;
            surface_paint(pad, s_palette[idx][0], s_palette[idx][1], s_palette[idx][2]);
        }
        s_last_host_us.store(now_us(), std::memory_order_relaxed);
        break;
    case Sec::PadMix: {
        crosspad::IPadMix *mix = crosspad::getPlatformServices().padMix;
        if (!mix) break;
        for (int i = 0; i < a.n; ++i) {
            /* The same convention as a Control Change mapped to a pad: the level is
             * live and the kit model is untouched. The pan half is 0 for a level write
             * because there is no getter to read the pad's current balance from -
             * stated in the docs rather than hidden, and the reason a pad level is the
             * one thing this surface cannot give back. */
            mix->setPadMix(static_cast<uint8_t>(a.start + i),
                           crosspad::hostmidi::percentFrom7(a.v[i]), 0);
        }
        break;
    }
    case Sec::ChVolume: case Sec::ChPan: case Sec::OutVolume: case Sec::HwLevel: {
        int si = 99;
        const int addrs = surf_slot_of(sec, &si);
        if (si > 3) break;
        for (int i = 0; i < a.n; ++i) {
            const int addr = a.start + i;
            if (addr >= addrs) continue;
            const Rule r = rule_for(sec, addr);
            const int nowU = board_units_of(sec, r);
            if (nowU < 0) {
                /* No such output, or a channel that is not on the desk right now. Named
                 * with a counter rather than left silent, because "my write did nothing"
                 * is the question this read-out exists to answer. */
                s_surf_missed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!s_surf_lvl[si][addr].held) {
                s_surf_lvl[si][addr].held = 1;
                s_surf_lvl[si][addr].before = nowU;
            }
            const int seven = a.v[i];
            const float want = (sec == Sec::ChPan)
                                   ? crosspad::hostmidi::panFrom7(static_cast<uint8_t>(seven))
                                   : (sec == Sec::HwLevel)
                                         ? static_cast<float>(
                                               crosspad::hostmidi::percentFrom7(
                                                   static_cast<uint8_t>(seven)))
                                         : crosspad::hostmidi::levelFrom7(
                                               static_cast<uint8_t>(seven));
            s_surf_lvl[si][addr].wrote =
                (sec == Sec::ChPan) ? static_cast<int>((want + 1.0f) * 1000.0f + 0.5f)
                                    : (sec == Sec::HwLevel) ? static_cast<int>(want + 0.5f)
                                                            : static_cast<int>(want * 1000.0f + 0.5f);
            if (write_level_of(r, want)) s_surf_values.fetch_add(1, std::memory_order_relaxed);
            else s_surf_missed.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    case Sec::Anim:
        for (int i = 0; i < a.n; ++i) {
            const int addr = a.start + i;
            if (addr == 0 && a.v[i] == 0) {
                /* "Stop" from a host that asked for the board's own animation also means
                 * "give the pads back": the animation painted pads it has no record of, so
                 * without this the board keeps the last frame of a chase forever, and the
                 * host -- having done the right thing, asking twice -- is left looking at a
                 * painted board. `s_release` is the same request `LED_HOST OFF` and the
                 * `LED_GIVE` frame raise, applied on this task. */
                s_anim = -1;
                s_release.store(true, std::memory_order_release);
            }
            else if (addr == 0) s_anim = (a.v[i] == 1) ? 0 : (a.v[i] == 2) ? 1 : -1;
            else if (addr == 1) s_anim_period_ms = a.v[i] * 10;
        }
        s_last_host_us.store(now_us(), std::memory_order_relaxed);
        break;
    case Sec::Lease:
        if (a.n >= 1 && a.start == 0) s_lease_ms = a.v[0] * 100;
        break;
    case Sec::Kit:
        /* Bank MSB + program in one frame, addressed the way `KIT_PC MAP` prints it.
         * The kit loader is LVGL-thread-owned, so this goes through the same parked
         * path a Program Change uses (main/midi_kit_pc.cpp), which also means the
         * board's own kit announcement stays seeded -- a surface write is a host
         * being told, not news to it. */
        if (a.n >= 2) kit_change_pair(1, a.v[0], a.v[1]);
        break;
    default:
        break;
    }
    s_surf_writes.fetch_add(1, std::memory_order_relaxed);
}

void apply_surface_rgb(const crosspad::surface::Action &a)
{
    for (int i = 0; i < a.n; ++i) {
        const int pad = a.start + i;
        surface_paint(pad, static_cast<uint8_t>(a.v[3 * i] << 1),
                      static_cast<uint8_t>(a.v[3 * i + 1] << 1),
                      static_cast<uint8_t>(a.v[3 * i + 2] << 1));
    }
    s_last_host_us.store(now_us(), std::memory_order_relaxed);
    s_surf_writes.fetch_add(1, std::memory_order_relaxed);
}

void apply_surface_restore(const crosspad::surface::Action &a)
{
    using namespace crosspad::surface;
    const Sec sec = static_cast<Sec>(a.sec);
    if (sec == Sec::PadColour || sec == Sec::PadMode) {
        for (int i = 0; i < a.n; ++i) surface_give_pad(a.start + i);
        return;
    }
    int si = 99;
    const int addrs = surf_slot_of(sec, &si);
    if (si > 3) return;
    for (int i = 0; i < a.n; ++i) {
        const int addr = a.start + i;
        if (addr >= addrs || !s_surf_lvl[si][addr].held) continue;
        const Rule r = rule_for(sec, addr);
        const int nowU = board_units_of(sec, r);
        if (nowU != s_surf_lvl[si][addr].wrote) {
            /* Someone else moved it after us - the person at the desk, another host,
             * or a preset. Leave it, count it, and let `SURFACE:` say so: the host can
             * then decide whether to ask again or accept what the board is doing. */
            s_surf_skips.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        write_level_of(r, value_from_units(sec, s_surf_lvl[si][addr].before));
        s_surf_lvl[si][addr].held = 0;
        s_surf_restores.fetch_add(1, std::memory_order_relaxed);
    }
}

void apply_surface_pass(void)
{
    if (s_pal_ready.exchange(false, std::memory_order_acquire)) {
        for (int i = 0; i < s_pal_stage_n; ++i) {
            const int idx = s_pal_stage_index + i;
            if (idx >= crosspad::surface::kPalette) break;
            s_palette[idx][0] = static_cast<uint8_t>(s_pal_stage_rgb[3 * i] << 1);
            s_palette[idx][1] = static_cast<uint8_t>(s_pal_stage_rgb[3 * i + 1] << 1);
            s_palette[idx][2] = static_cast<uint8_t>(s_pal_stage_rgb[3 * i + 2] << 1);
        }
        ++s_palette_rev;
        s_surf_writes.fetch_add(1, std::memory_order_relaxed);
    }
    while (s_surf_tail.load(std::memory_order_relaxed) != s_surf_head.load(std::memory_order_relaxed)) {
        const uint32_t slot = s_surf_tail.load(std::memory_order_relaxed) % kSurfRing;
        const crosspad::surface::Action a = s_surf_ring[slot];
        s_surf_tail.fetch_add(1, std::memory_order_relaxed);
        if (a.kind == crosspad::surface::Kind::Write) apply_surface_write(a);
        else if (a.kind == crosspad::surface::Kind::Restore) apply_surface_restore(a);
        else apply_surface_rgb(a);
    }
}

}  // namespace

/* A tenth of a second, for what a person or a desk preset moves at. Reading the
 * board's own values costs what a getter costs; the only output is a CC when
 * something actually changed against the baseline this module keeps. */
uint32_t host_echo_job(void *)
{
    if (!s_echo_on) return 0;
    auto &sink = crosspad::getMidiInputHandler();
    for (size_t i = 0; i < s_rule_count; ++i) {
        const Rule &r = s_rules[i];
        /* A pad's live level has no getter (IPadMix), so there is nothing to
         * compare and nothing to announce: the rule works one way, and the docs
         * say so rather than the board staying mysteriously silent. */
        if (r.target == Target::None || r.target == Target::Pad) continue;
        float value = 0.0f;
        if (!current_level_of(r, &value)) continue;
        const int seven = seven_of(r, value);
        if (s_echo_prev[i] >= 0 && s_echo_prev[i] != seven
            && crosspad::hostmidi::echoNeeded(s_echo_prev[i], seven)) {
            sink.sendControlChange(r.cc, static_cast<uint8_t>(seven), r.channel);
            s_echoed.fetch_add(1, std::memory_order_relaxed);
        }
        s_echo_prev[i] = static_cast<int16_t>(seven);
    }
    /* The kit gets the same treatment on the same beat: one comparison here, and
     * kit_pc posts the sending itself to the LVGL thread, which owns the kit
     * listing. `KIT_PC ECHO off` silences that one on its own, so a host can ask
     * for faders without kit traffic or the other way round. */
    kit_pc_check_announce();
    return 0;
}

void host_midi_init(void)
{
    crosspad::hostmidi::defaultRules(s_rules, &s_rule_count);
    seed_echo();
    if (!s_job) s_job = bsp_worker_add("host_midi", 25, host_midi_job, nullptr);
    if (!s_echo_job) s_echo_job = bsp_worker_add("host_echo", 100, host_echo_job, nullptr);
    if (!s_job) ESP_LOGE(TAG, "no worker job: host Control Changes and pad frames will not apply");
    ESP_LOGI(TAG, "host MIDI surface ready: %u mapped CCs, LEDs under the board's control",
             (unsigned)s_rule_count);
}

extern "C" int host_midi_control_change(uint8_t ch, uint8_t cc, uint8_t val)
{
    if (ch < 1 || ch > 16) return 0;
    const uint8_t channel = static_cast<uint8_t>(ch - 1);
    const Rule *rule = crosspad::hostmidi::findRule(s_rules, s_rule_count, channel, cc);
    if (!rule) return 0;
    s_cc_seen.fetch_add(1);
    /* A host riding faders is a host that exists: the pad lease it may also be
     * holding is renewed by this as much as by a painted pad. */
    s_last_host_us.store(now_us(), std::memory_order_relaxed);
    const uint32_t head = s_cc_head.load(std::memory_order_relaxed);
    if (head - s_cc_tail.load(std::memory_order_relaxed) >= kCcRing) {
        s_cc_tail.fetch_add(1, std::memory_order_relaxed);   /* oldest goes: newest matters */
    }
    s_cc_ring[head % kCcRing] =
        (static_cast<uint32_t>(channel) << 16) | (static_cast<uint32_t>(cc) << 8) |
        static_cast<uint32_t>(val);
    s_cc_head.fetch_add(1, std::memory_order_release);
    return 1;
}

extern "C" void host_midi_led_pad(uint8_t pad, uint8_t r, uint8_t g, uint8_t b)
{
    if (pad >= kPadCount) return;
    s_pad_word[pad].store(pack_led(r, g, b), std::memory_order_relaxed);
}

extern "C" void host_midi_led_all(uint8_t r, uint8_t g, uint8_t b)
{
    s_all_word.store(pack_led(r, g, b), std::memory_order_relaxed);
}

extern "C" void host_midi_led_bytes(const uint8_t *rgb48)
{
    if (!rgb48) return;
    /* Staged, then published: the worker takes the buffer only after the flag
     * (release/acquire), and the writers are the two verbs' tasks, never the
     * audio path. */
    for (int p = 0; p < 3 * kPadCount; ++p) s_grid_stage[p] = rgb48[p];
    s_grid_ready.store(true, std::memory_order_release);
}

extern "C" int host_midi_led_hex(const char *hex96)
{
    LedFrame f;
    if (!crosspad::hostmidi::parseFrameHex(hex96, &f)) return 0;
    uint8_t rgb[3 * kPadCount];
    for (int p = 0; p < kPadCount; ++p) {
        rgb[p * 3] = f.pad[p].r;
        rgb[p * 3 + 1] = f.pad[p].g;
        rgb[p * 3 + 2] = f.pad[p].b;
    }
    host_midi_led_bytes(rgb);
    return 1;
}

extern "C" void host_midi_led_reset(void)
{
    /* From any task: the worker does the giving-back, so nothing here blocks,
     * and the LED model is written from one task whatever asked for it. */
    for (int p = 0; p < kPadCount; ++p) s_pad_word[p].store(0, std::memory_order_relaxed);
    s_all_word.store(0, std::memory_order_relaxed);
    s_grid_ready.store(false, std::memory_order_relaxed);
    s_release.store(true, std::memory_order_release);
    /* A whole-grid give back drops the surface's pad claims too: they would otherwise
     * remember colours that no longer mean anything, and a later restore could stamp
     * them onto pads the board has since repainted. */
    s_surf_claim.store(0, std::memory_order_relaxed);
    s_pad_hold.store(0, std::memory_order_relaxed);
}

/* ── the SysEx surface: park, do not act ─────────────────────────────────── */

extern "C" void host_midi_park(const uint8_t *action)
{
    /* `action` is a SurfAction laid out by the caller; the ring is small because the
     * worker drains it every 25 ms and a host that outruns that is an animation host
     * that will not notice a dropped frame anyway -- the drop is counted, not hidden. */
    const uint32_t head = s_surf_head.load(std::memory_order_relaxed);
    if (head - s_surf_tail.load(std::memory_order_relaxed) >= static_cast<uint32_t>(kSurfRing)) {
        s_surf_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_surf_ring[head % kSurfRing] =
        *reinterpret_cast<const crosspad::surface::Action *>(action);
    s_surf_head.fetch_add(1, std::memory_order_release);
    s_last_host_us.store(now_us(), std::memory_order_relaxed);
}

extern "C" void host_midi_park_palette(uint8_t index, uint8_t n, const uint8_t *rgb7)
{
    for (int i = 0; i < 3 * n; ++i) s_pal_stage_rgb[i] = rgb7[i];
    s_pal_stage_index = index;
    s_pal_stage_n = n;
    s_pal_ready.store(true, std::memory_order_release);
    s_last_host_us.store(now_us(), std::memory_order_relaxed);
}

extern "C" void host_midi_surface_refused(void)
{
    s_surf_refused.fetch_add(1, std::memory_order_relaxed);
}

/* `SURFACE [STATUS | PAD <0..15> | CLR]` -- the counters, and one pad's whole story:
 * what the board shows now, what the surface left there, what it stood at before, and
 * whether the surface may still take it back. That is the readout a bench needs to
 * tell "the restore was refused because the board moved" from "the restore did not
 * run", which otherwise look identical from the outside. */
extern "C" void host_midi_surface_counters(unsigned *c)
{
    /* One source for the numbers, read by both the verb line and the SysEx reply: two
     * places that each pull the same counters out of different strings is how a
     * readout ends up lying about its own subsystem. c[6] is the pad-claim mask, c[7]
     * the explicitly-held mask. */
    c[0] = s_surf_writes.load();
    c[1] = s_surf_values.load();
    c[2] = s_surf_restores.load();
    c[3] = s_surf_skips.load();
    c[4] = s_surf_refused.load();
    c[5] = s_surf_drops.load();
    c[6] = s_surf_claim.load();
    c[7] = s_pad_hold.load();
}

extern "C" int host_midi_surface_status_line(char *out, size_t len)
{
    if (!out || !len) return 0;
    return snprintf(out, len,
                    "SURFACE: writes=%u values=%u missed=%u restores=%u skipped=%u refused=%u "
                    "drops=%u claims=0x%04X held=0x%04X palette_rev=%u lease_ms=%d anim=%s ring=%s",
                    (unsigned)s_surf_writes.load(), (unsigned)s_surf_values.load(),
                    (unsigned)s_surf_missed.load(),
                    (unsigned)s_surf_restores.load(), (unsigned)s_surf_skips.load(),
                    (unsigned)s_surf_refused.load(), (unsigned)s_surf_drops.load(),
                    (unsigned)s_surf_claim.load(), (unsigned)s_pad_hold.load(),
                    (unsigned)s_palette_rev, s_lease_ms,
                    s_anim < 0 ? "off" : (s_anim == 0 ? "chase" : "ring"),
                    s_job ? "host_midi" : "NONE");
}

/* `SURFACE LVL <sec> <addr>`: one level address's whole record -- the board's live
 * value, in its own units and in the 7-bit form a host writes, plus what the surface
 * left there and what the board stood at before it. A level write can otherwise only be
 * judged through another subsystem's number, and those numbers are not the same thing:
 * `OUTVOL` reports the codec layer's tracked state while the surface writes
 * IOutputLevels, and a check that compares one against the other can pass while nothing
 * at all has moved (the first run of this ladder did exactly that).
 *
 * The pad mix is the address with no answer: IPadMix has no getter, so there is no
 * `now` to report and no honest `before`. That is said out loud here rather than left as
 * a blank field, because a blank invites a reader to think the board forgot. */
extern "C" int host_midi_surface_level_line(char *out, size_t len, const char *sec_arg, int addr)
{
    using namespace crosspad::surface;
    if (!out || !len || !sec_arg || addr < 0 || addr > 15) return 0;
    Sec sec = Sec::Count;
    if (strncasecmp(sec_arg, "out", 3) == 0) sec = Sec::OutVolume;
    else if (strncasecmp(sec_arg, "hw", 2) == 0) sec = Sec::HwLevel;
    else if (strncasecmp(sec_arg, "pan", 3) == 0) sec = Sec::ChPan;
    else if (strncasecmp(sec_arg, "chv", 3) == 0 || strncasecmp(sec_arg, "chan", 4) == 0) sec = Sec::ChVolume;
    else if (strncasecmp(sec_arg, "padmix", 6) == 0) sec = Sec::PadMix;
    if (sec == Sec::Count) return 0;
    if (sec == Sec::PadMix) {
        snprintf(out, len, "SURFACELVL: sec=pad_mix addr=%d now=NO_READBACK wrote=NA before=NA "
                           "held=0 note=IPadMix has no getter: the value is not comparable "
                           "and cannot be given back", addr);
        return static_cast<int>(strlen(out));
    }
    int si = 99;
    const int addrs = surf_slot_of(sec, &si);
    if (si > 3 || addr >= addrs) {
        snprintf(out, len, "SURFACELVL: sec=%s addr=%d note=no such address (section has %d)",
                 secName(sec), addr, addrs);
        return static_cast<int>(strlen(out));
    }
    const Rule r = rule_for(sec, addr);
    const int nowU = board_units_of(sec, r);
    snprintf(out, len,
             "SURFACELVL: sec=%s addr=%d now=%ld seven=%d wrote=%ld before=%ld held=%d",
             secName(sec), addr, static_cast<long>(nowU),
             nowU < 0 ? -1 : seven_of(r, value_from_units(sec, nowU)),
             static_cast<long>(s_surf_lvl[si][addr].wrote),
             static_cast<long>(s_surf_lvl[si][addr].before),
             s_surf_lvl[si][addr].held);
    return static_cast<int>(strlen(out));
}

extern "C" int host_midi_surface_pad_line(char *out, size_t len, int pad)
{
    if (!out || !len || pad < 0 || pad >= kPadCount) return 0;
    const bool claimed = (s_surf_claim.load(std::memory_order_relaxed) >> pad) & 1u;
    uint8_t cr = 0, cg = 0, cb = 0;
    auto &leds = crosspad::getPadLedController();
    const crosspad::RgbColor now = leds.getPadColor(static_cast<uint8_t>(pad));
    cr = now.R; cg = now.G; cb = now.B;
    const bool mine = claimed && cr == s_surf_written[pad][0] && cg == s_surf_written[pad][1]
                      && cb == s_surf_written[pad][2];
    snprintf(out, len,
             "SURFACEPAD: pad=%d now=%02X%02X%02X wrote=%02X%02X%02X before=%02X%02X%02X "
             "claim=%d mine=%d hold=%d",
             pad, cr, cg, cb, s_surf_written[pad][0], s_surf_written[pad][1],
             s_surf_written[pad][2], s_surf_before[pad][0], s_surf_before[pad][1],
             s_surf_before[pad][2], claimed ? 1 : 0, mine ? 1 : 0,
             ((s_pad_hold.load(std::memory_order_relaxed) >> pad) & 1u) ? 1 : 0);
    return static_cast<int>(strlen(out));
}

/* `SURFACE CLR` forgets the surface's claims and held pads. A panel that re-syncs from
 * scratch wants that -- otherwise it would be judging its restores against records of
 * frames it has already forgotten sending. */
extern "C" void host_midi_surface_clear(void)
{
    s_surf_claim.store(0, std::memory_order_relaxed);
    s_pad_hold.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i)
        for (int a = 0; a < 16; ++a) s_surf_lvl[i][a].held = 0;
    ESP_LOGI(TAG, "surface records cleared: claims, holds and level snapshots");
}

extern "C" void host_midi_led_anim(int which, int period_ms)
{
    s_anim = which;
    s_anim_step = 0;
    if (period_ms >= 20 && period_ms <= 4000) s_anim_period_ms = period_ms;
    s_anim_next_us = now_us();
}

/* ── the verbs ─────────────────────────────────────────────────────────── */

extern "C" int host_midi_cc_total(void)
{
    return static_cast<int>(s_rule_count);
}

extern "C" int host_midi_cc_row(int position, char *out, size_t len)
{
    if (!out || !len || position < 0 || static_cast<size_t>(position) >= s_rule_count) {
        if (out && len) out[0] = '\0';
        return 0;
    }
    const Rule &r = s_rules[position];
    int cur = -1;
    switch (r.target) {
#if HOSTMIDI_HAS_MIXER
    case Target::Channel: {
        const int c = r.name[0] ? find_channel(r.name) : r.index;
        if (c >= 0) {
            auto &mx = getMixerEngine();
            cur = r.isPan ? static_cast<int>(64.0f + mx.getChannelPan(
                                      static_cast<uint8_t>(c)) * 63.0f)
                          : static_cast<int>(crosspad::hostmidi::sevenFromLevel(mx.getChannelVolume(
                                      static_cast<uint8_t>(c))));
        }
        break;
    }
    case Target::Output: {
        auto &mx = getMixerEngine();
        if (r.index >= 0 && r.index < mx.numOutputs()) {
            cur = static_cast<int>(crosspad::hostmidi::sevenFromLevel(mx.getOutputVolume(
                static_cast<uint8_t>(r.index))));
        }
        break;
    }
#endif
    case Target::Hardware: {
        crosspad::IOutputLevels *lv = crosspad::getPlatformServices().outputLevels;
        if (lv && r.index >= 0 && r.index < lv->outputs()) {
            /* Back through the same curve the CC came in on: `cur` is then the
             * number a host sends to get where the board is now, which is what
             * a person restoring a level after a test actually wants. */
            cur = crosspad::hostmidi::sevenFromLevel(static_cast<float>(lv->level(r.index)) / 100.0f);
        }
        break;
    }
    case Target::Pad:
    case Target::None:
        break;
    }
    const int put = snprintf(out, len,
                             "MIDICC: pos=%d ch=%u cc=%u target=%s idx=%d name=\"%s\" %s cur=%d",
                             position, (unsigned)(r.channel + 1), (unsigned)r.cc,
                             crosspad::hostmidi::targetName(r.target), r.index, r.name,
                             r.isPan ? "pan" : "level", cur);
    if (put <= 0 || static_cast<size_t>(put) >= len) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

extern "C" int host_midi_cc_status_line(char *out, size_t len)
{
    if (!out || !len) return 0;
    const int put = snprintf(out, len,
                             "MIDICC: rules=%u seen=%lu applied=%lu unmapped=%lu refused=%lu "
                             "held=%u gives=%lu echoed=%lu echo=%d job=%s mixer=%d",
                             (unsigned)s_rule_count, (unsigned long)s_cc_seen.load(),
                             (unsigned long)s_cc_applied.load(), (unsigned long)s_cc_unmapped.load(),
                             (unsigned long)s_cc_refused.load(),
                             (unsigned)s_heldNow.load(), (unsigned long)s_gives.load(),
                             (unsigned long)s_echoed.load(), s_echo_on ? 1 : 0,
                             s_job ? "host_midi" : "NONE",
                             HOSTMIDI_HAS_MIXER ? find_channel("Sampler") : -1);
    return put < 0 ? 0 : static_cast<int>(strlen(out));
}

/* Read one argument, honouring "quoted words" for a channel name. File-local:
 * nothing outside this translation unit needs it. */
static const char *next_arg(char **p, char *quote_buf)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) { *p = s; return nullptr; }
    if (*s == '"') {
        ++s;
        char *end = strchr(s, '"');
        if (!end) { *p = s + strlen(s); return nullptr; }
        *end = '\0';
        if (quote_buf) strcpy(quote_buf, s);
        *p = end + 1;
        return quote_buf ? quote_buf : s;
    }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') ++s;
    if (*s) {
        *s = '\0';
        ++s;                      // the next argument starts after this separator
    }
    *p = s;
    return start;
}

extern "C" const char *host_midi_cc_set(const char *arg)
{
    if (!arg) return "usage";
    char work[96];
    strncpy(work, arg, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    char *p = work;
    const char *what = next_arg(&p, nullptr);
    if (!what) return "usage";
    if (strcasecmp(what, "ECHO") == 0) {
        const char *onoff = next_arg(&p, nullptr);
        if (!onoff) return "usage";
        if (strcasecmp(onoff, "ON") == 0) s_echo_on = true;
        else if (strcasecmp(onoff, "OFF") == 0) s_echo_on = false;
        else return "unknown";
        /* Turning it back on starts from what the desk is at now, so a switch does
         * not announce every level that moved while it was off. */
        seed_echo();
        ESP_LOGI(TAG, "host echo %s", s_echo_on ? "on" : "off");
        return nullptr;
    }
    if (strcasecmp(what, "GIVE") == 0) {
        /* The host says "I am done with these faders": every level it drove
         * through a rule goes back to what the board had before. Parked, applied
         * on the worker, which is the task that writes the mixer anyway. */
        s_give_levels.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "giving back the levels the host drove");
        return nullptr;
    }
    if (strcasecmp(what, "RESET") == 0) {
        /* A map going away is a handover too: the levels the host drove under it
         * had no one left to look after them. */
        s_give_levels.store(true, std::memory_order_release);
        seed_echo();
        s_rule_count = 0;
        crosspad::hostmidi::defaultRules(s_rules, &s_rule_count);
        ESP_LOGI(TAG, "Control Change map back to the %u defaults", (unsigned)s_rule_count);
        return nullptr;
    }
    if (strcasecmp(what, "NONE") == 0 || strcasecmp(what, "CLEAR") == 0) {
        s_give_levels.store(true, std::memory_order_release);
        seed_echo();
        s_rule_count = 0;
        return nullptr;
    }
    if (strcasecmp(what, "SET") != 0) return "unknown";
    char name[32] = {};
    const char *a_ch = next_arg(&p, nullptr);
    const char *a_cc = next_arg(&p, nullptr);
    const char *a_target = next_arg(&p, nullptr);
    if (!a_ch || !a_cc || !a_target) return "usage";
    const int ch1 = atoi(a_ch), cc = atoi(a_cc);
    if (ch1 < 1 || ch1 > 16 || cc < 0 || cc > 127) return "range";
    Rule r;
    r.channel = static_cast<uint8_t>(ch1 - 1);
    r.cc = static_cast<uint8_t>(cc);
    if (strcasecmp(a_target, "NONE") == 0) {
        /* an existing rule switched off, rather than a hole in the table */
        for (size_t i = 0; i < s_rule_count; ++i) {
            if (s_rules[i].channel == r.channel && s_rules[i].cc == r.cc) {
                s_rules[i].target = Target::None;
                return nullptr;
            }
        }
        return nullptr;
    }
    if (strcasecmp(a_target, "CHANNEL") == 0) {
        const char *nm = next_arg(&p, name);
        if (!nm || !nm[0]) return "need_name";
        if (strlen(nm) >= sizeof(r.name)) return "name_long";
        r.target = Target::Channel;
        r.index = -1;
        strncpy(r.name, nm, sizeof(r.name) - 1);
    } else if (strcasecmp(a_target, "OUTPUT") == 0 || strcasecmp(a_target, "HARDWARE") == 0 ||
               strcasecmp(a_target, "PAD") == 0) {
        const char *a_idx = next_arg(&p, nullptr);
        if (!a_idx) return "need_index";
        const int idx = atoi(a_idx);
        if (idx < 0) return "range";
        if (strcasecmp(a_target, "PAD") == 0) {
            if (idx >= kPadCount) return "range";
            r.target = Target::Pad;
        } else if (strcasecmp(a_target, "OUTPUT") == 0) {
            r.target = Target::Output;
        } else {
            crosspad::IOutputLevels *lv = crosspad::getPlatformServices().outputLevels;
            if (!lv || idx >= lv->outputs()) return "range";
            r.target = Target::Hardware;
        }
        r.index = idx;
    } else {
        return "unknown_target";
    }
    /* A pan CC is asked for as `... PAN` after the target, or by CC 10 out of
     * habit: the CC number alone decides nothing, so say it. */
    const char *a_pan = next_arg(&p, nullptr);
    if (a_pan && strcasecmp(a_pan, "PAN") == 0) r.isPan = true;
    for (size_t i = 0; i < s_rule_count; ++i) {
        if (s_rules[i].channel == r.channel && s_rules[i].cc == r.cc) {
            s_rules[i] = r;
            s_echo_prev[i] = -1;          // re-pointed: seed the new target, send nothing
            return nullptr;
        }
    }
    if (s_rule_count >= crosspad::hostmidi::kMaxRules) return "full";
    s_rules[s_rule_count++] = r;
    s_echo_prev[s_rule_count - 1] = -1;
    return nullptr;
}

extern "C" int host_midi_led_status_line(char *out, size_t len)
{
    if (!out || !len) return 0;
    const int put = snprintf(out, len,
                             "LEDHOST: lease=%d leases=%lu releases=%lu frames=%lu anim=%s "
                             "anim_ms=%d lease_ms=%d idle_ms=%lld job=%s",
                             s_leased ? 1 : 0, (unsigned long)s_leases.load(),
                             (unsigned long)s_releases.load(), (unsigned long)s_frames.load(),
                             s_anim < 0 ? "off" : (s_anim == 0 ? "chase" : "ring"), s_anim_period_ms,
                             s_lease_ms,
                             s_leased
                                 ? (long long)((now_us() - s_last_host_us.load(
                                       std::memory_order_relaxed)) / 1000)
                                 : 0LL,
                             s_job ? "host_midi" : "NONE");
    return put < 0 ? 0 : static_cast<int>(strlen(out));
}

extern "C" const char *host_midi_led_cmd(const char *arg)
{
    if (!arg) return "usage";
    char work[128];
    strncpy(work, arg, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    char *p = work;
    const char *what = next_arg(&p, nullptr);
    if (!what) return "usage";
    if (strcasecmp(what, "STATUS") == 0) return nullptr;
    if (strcasecmp(what, "OFF") == 0 || strcasecmp(what, "RESET") == 0) {
        host_midi_led_reset();
        return nullptr;
    }
    if (strcasecmp(what, "SET") == 0) {
        const char *a_pad = next_arg(&p, nullptr), *a_r = next_arg(&p, nullptr);
        const char *a_g = next_arg(&p, nullptr), *a_b = next_arg(&p, nullptr);
        if (!a_pad || !a_r || !a_g || !a_b) return "usage";
        const int pad = atoi(a_pad), r = atoi(a_r), g = atoi(a_g), b = atoi(a_b);
        if (pad < 0 || pad >= kPadCount) return "range";
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return "range";
        host_midi_led_pad(static_cast<uint8_t>(pad), static_cast<uint8_t>(r),
                          static_cast<uint8_t>(g), static_cast<uint8_t>(b));
        return nullptr;
    }
    if (strcasecmp(what, "ALL") == 0) {
        const char *a_r = next_arg(&p, nullptr), *a_g = next_arg(&p, nullptr);
        const char *a_b = next_arg(&p, nullptr);
        if (!a_r || !a_g || !a_b) return "usage";
        const int r = atoi(a_r), g = atoi(a_g), b = atoi(a_b);
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return "range";
        host_midi_led_all(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                          static_cast<uint8_t>(b));
        return nullptr;
    }
    if (strcasecmp(what, "GRID") == 0) {
        const char *hex = next_arg(&p, nullptr);
        if (!hex) return "usage";
        if (!host_midi_led_hex(hex)) return "hex96";
        return nullptr;
    }
    if (strcasecmp(what, "ANIM") == 0) {
        const char *which = next_arg(&p, nullptr), *ms = next_arg(&p, nullptr);
        if (!which) return "usage";
        int mode = -1;
        if (strcasecmp(which, "CHASE") == 0) mode = 0;
        else if (strcasecmp(which, "RING") == 0) mode = 1;
        else if (strcasecmp(which, "OFF") != 0) return "unknown";
        const int period = ms ? atoi(ms) : 0;
        if (mode < 0) {
            s_anim = -1;
            give_lease_back();
            return nullptr;
        }
        if (period && (period < 20 || period > 4000)) return "range";
        host_midi_led_anim(mode, period);
        return nullptr;
    }
    if (strcasecmp(what, "LEASE") == 0) {
        const char *ms = next_arg(&p, nullptr);
        if (!ms) return "usage";
        int v;
        if (strcasecmp(ms, "AUTO") == 0) {
            v = kDefaultLeaseMs;
        } else {
            v = atoi(ms);
            if (v < 0 || v > 600000) return "range";
        }
        s_lease_ms = v;
        /* A lease that is already being held starts counting from now, so setting
         * a short timeout does not hand the pads back this instant. */
        s_last_host_us.store(now_us(), std::memory_order_relaxed);
        ESP_LOGI(TAG, "pad LED lease timeout now %d ms%s", v,
                 v ? "" : " (hold until told)");
        return nullptr;
    }
    return "unknown";
}
