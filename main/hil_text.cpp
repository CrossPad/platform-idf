// SPDX-License-Identifier: GPL-3.0-or-later
#include "hil_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using namespace crosspad::control;

namespace {

/// Room the colour loop keeps free for one "RRGGBB," and the line's end.
constexpr size_t kLedColorReserve = 10;
/// Room the roots loop keeps free for one more root.
constexpr size_t kPitchedRootReserve = 10;
/// Room a KIT_LIST line keeps beyond a name: its id, separators, the line end.
constexpr size_t kKitEntryReserve = 16;
/// Width of the route field, as the board always printed it.
constexpr size_t kMixRouteChars = 24;

/// Fixed-point parts of a meter value, digit for digit the text %f would
/// print. printf's %f on the CDC task is newlib's _dtoa_r -- a kilobyte of
/// frame and a malloc inside it -- and MIX_LVL is the verb a host polls in a
/// loop: it overflowed the main task's four kilobytes twice.
struct Fixed {
    int whole;
    int frac;
};

Fixed fixed(float v, int32_t scale)
{
    const int32_t x = (int32_t)(v * (float)scale + 0.5f);
    return Fixed{(int)(x / scale), (int)(x % scale)};
}

constexpr int32_t kPeakScale = 10000;   // pk=%d.%04d
constexpr int32_t kVolScale  = 100;     // vol=%d.%02d, trim=%d.%02d
constexpr int32_t kLimScale  = 1000;    // lim=%d.%03d, ceil=%d.%03d

/// Room a line keeps for its "\r\n".
constexpr size_t kLineEndChars = 2;

/// Appends at @p n, never past @p limit (one past the last byte it may use,
/// the terminator's included), and leaves @p n at the end of what is there:
/// a cut append stops at limit - 1, so the next append cannot start past the
/// buffer.
__attribute__((format(printf, 4, 5)))
void append(char *out, size_t limit, size_t &n, const char *fmt, ...)
{
    if (n + 1 >= limit) return;
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(out + n, limit - n, fmt, ap);
    va_end(ap);
    if (r < 0) return;
    n = n + (size_t)r < limit ? n + (size_t)r : limit - 1;
}

} // namespace

size_t hil_text_kit_status(char *out, size_t cap, const KitStatus &st)
{
    size_t n = 0;
    append(out, cap, n, "KITSTATUS: current=%d loading=%d pending=%d name=%s\r\n",
           st.current, st.loading ? 1 : 0, st.pending, st.name ? st.name : "-");
    return n;
}

size_t hil_text_led_state(char *out, size_t cap, const LedState &st, const HilLedBoard &board)
{
    size_t n = 0;
    append(out, cap, n,
           "LEDS: bri=%u anim=%d coalesce=%d cfgbri=%u "
           "pwr=0x%02X pwrN=%u txfail=%u refreshes=%u colors=",
           (unsigned)st.brightness, st.animating ? 1 : 0, st.coalesced ? 1 : 0,
           (unsigned)st.settingsBrightness, (unsigned)board.pwrState,
           (unsigned)board.pwrStateCount, (unsigned)board.txFail, (unsigned)board.refreshes);
    for (uint8_t i = 0; i < LedState::kPads && n + kLedColorReserve < cap; ++i) {
        const crosspad::RgbColor &c = st.colors[i];
        append(out, cap, n, "%s%02X%02X%02X", i ? "," : "", c.R, c.G, c.B);
    }
    append(out, cap, n, "\r\n");
    return n;
}

size_t hil_text_mix_channel(char *out, size_t cap, const MixerChannelLevel &row)
{
    char routes[kMixRouteChars];
    size_t ri = 0;
    for (uint8_t o = 0; o < row.outputs && o < kMixerRouteBits && ri < sizeof(routes) - 2; ++o) {
        routes[ri++] = (row.routes >> o) & 1u ? '1' : '0';
    }
    routes[ri] = 0;
    const Fixed pl = fixed(row.peakL, kPeakScale);
    const Fixed pr = fixed(row.peakR, kPeakScale);
    const Fixed v  = fixed(row.volume, kVolScale);
    const Fixed t  = fixed(row.trim, kVolScale);
    size_t n = 0;
    append(out, cap, n, "MIXLVL: ch=%u name=%s pk=%d.%04d/%d.%04d vol=%d.%02d trim=%d.%02d mute=%d route=%s\r\n",
           (unsigned)row.ch, row.name, pl.whole, pl.frac, pr.whole, pr.frac,
           v.whole, v.frac, t.whole, t.frac, row.muted ? 1 : 0, routes);
    return n;
}

size_t hil_text_mix_output(char *out, size_t cap, const MixerOutputLevel &row)
{
    const Fixed pl = fixed(row.peakL, kPeakScale);
    const Fixed pr = fixed(row.peakR, kPeakScale);
    const Fixed v  = fixed(row.volume, kVolScale);
    const Fixed lm = fixed(row.limit, kLimScale);
    const Fixed cl = fixed(row.ceiling, kLimScale);
    size_t n = 0;
    append(out, cap, n, "MIXLVL: out=%u pk=%d.%04d/%d.%04d vol=%d.%02d mute=%d lim=%d.%03d ceil=%d.%03d\r\n",
           (unsigned)row.out, pl.whole, pl.frac, pr.whole, pr.frac, v.whole, v.frac,
           row.muted ? 1 : 0, lm.whole, lm.frac, cl.whole, cl.frac);
    return n;
}

size_t hil_text_mix_tap(char *out, size_t cap, const MixerTapLevel &row)
{
    const Fixed lm = fixed(row.limit, kLimScale);
    size_t n = 0;
    append(out, cap, n, "MIXLVL: tap lim=%d.%03d\r\n", lm.whole, lm.frac);
    return n;
}

size_t hil_text_pitched_status(char *out, size_t cap, const PitchedStatus &st, const HilPitchedBoard &board)
{
    size_t n = 0;
    append(out, cap, n, "PITCHED: active=%d zones=%u roots=", st.active ? 1 : 0, (unsigned)st.zones);
    /* The roots go straight into the line, bounded as the field always was:
     * a root is appended while the field holds under kHilPitchedRootsChars -
     * kPitchedRootReserve, and never past kHilPitchedRootsChars. */
    const size_t rootsAt = n;
    const size_t rootsEnd = rootsAt + kHilPitchedRootsChars < cap ? rootsAt + kHilPitchedRootsChars : cap;
    for (uint8_t i = 0; i < st.zones && i < crosspad::SampleBank::kMaxZones &&
                        n - rootsAt < kHilPitchedRootsChars - kPitchedRootReserve; ++i) {
        append(out, rootsEnd, n, "%s%.2f", i ? "," : "", (double)st.roots[i]);
    }
    /* err= is present only after a failed load, so a healthy reply keeps the
     * shape every existing parser knows; it stays right after steals= (the
     * harness reads it there) and the fields added later follow it. */
    append(out, cap, n,
           " voices=%u/%u bank_kb=%u rt_max_us=%u steals=%u%s%s rt_busy_max_us=%u "
           "overruns=%u cap=%u kit_max=%u shed=%u\r\n",
           (unsigned)st.activeVoices, (unsigned)st.maxVoices,
           (unsigned)(st.bankBytes / 1024), (unsigned)board.rtMaxUs,
           (unsigned)st.steals, board.lastError[0] ? " err=" : "", board.lastError,
           (unsigned)board.rtBusyMaxUs, (unsigned)board.overruns,
           (unsigned)st.voiceCap, (unsigned)board.kitMaxVoices, (unsigned)st.sheds);
    return n;
}

size_t hil_text_wave_status(char *out, size_t cap, const WaveStatus &st)
{
    size_t n = 0;
    append(out, cap, n,
           "WAVE: kit=%d total=%d cached=%d loader=%d queued=%u req=%u ok=%u "
           "retry=%u fail_info=%u fail_read=%u alloc_fail=%u drop=%u sweep=%u start_fail=%u missing=%s\r\n",
           st.kit, st.total, st.cached, st.loaderRunning ? 1 : 0,
           (unsigned)st.queued, (unsigned)st.requested, (unsigned)st.loaded,
           (unsigned)st.retried, (unsigned)st.failedInfo, (unsigned)st.failedRead,
           (unsigned)st.allocFail, (unsigned)st.dropped, (unsigned)st.sweeps,
           (unsigned)st.startFail, st.missing[0] ? st.missing : "-");
    return n;
}

void hil_text_kit_list_begin(HilKitListText &text)
{
    text.n = 0;
    append(text.out, text.cap, text.n, "KITS:");
    text.onLine = 0;
}

void hil_text_kit_list_add(HilKitListText &text, const KitListRow &row)
{
    if (row.removed) return;
    const char *name = row.name ? row.name : "?";
    /* Flush before a name that would not fit, never after: a truncated entry
     * is the thing the batching is here to prevent. A name too long for a line
     * of its own is cut, short of the room the line's end needs. */
    if (text.onLine > 0 && text.n + strlen(name) + kKitEntryReserve >= text.cap) {
        append(text.out, text.cap, text.n, "\r\n");
        text.emit(text.out);
        hil_text_kit_list_begin(text);
    }
    const size_t entryEnd = text.cap > kLineEndChars ? text.cap - kLineEndChars : 0;
    append(text.out, entryEnd, text.n, "%s%d:%s", text.onLine ? "," : " ", row.id, name);
    text.onLine++;
}

void hil_text_kit_list_end(HilKitListText &text, int count, int current)
{
    if (text.onLine > 0) {
        append(text.out, text.cap, text.n, "\r\n");
        text.emit(text.out);
    }
    text.n = 0;
    append(text.out, text.cap, text.n, "KITS: end count=%d current=%d\r\n", count, current);
    text.emit(text.out);
}

size_t hil_text_audio_level(char *out, size_t cap, const AudioLevel &lv, bool ampOn, bool ampAllowed)
{
    size_t n = 0;
    append(out, cap, n, "AUDIOLVL: L=%.5f R=%.5f amp=%d allowed=%d\r\n",
           (double)lv.left, (double)lv.right, ampOn ? 1 : 0, ampAllowed ? 1 : 0);
    return n;
}
