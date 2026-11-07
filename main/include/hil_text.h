// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * CDC text of the control verbs whose state crosspad-core gathers
 * (crosspad/control/): one function per reply line, writing into the caller's
 * buffer and returning the length of what is there. Every append is bounded:
 * a value too long for the buffer cuts the line, it never writes past it. No ESP-IDF call in here, so the
 * exact text crosspad-hil parses is pinned by host tests (main/tests/).
 *
 * The board-only fields a line carries beside the portable ones come in as
 * the small Hil*Board structs.
 */

#include <stddef.h>
#include <stdint.h>

#include "crosspad/control/AudioLevel.hpp"
#include "crosspad/control/KitList.hpp"
#include "crosspad/control/KitStatus.hpp"
#include "crosspad/control/LedState.hpp"
#include "crosspad/control/MixerLevels.hpp"
#include "crosspad/control/PitchedStatus.hpp"
#include "crosspad/control/WaveStatus.hpp"

/// KITSTATUS: current= loading= pending= name=
size_t hil_text_kit_status(char *out, size_t cap, const crosspad::control::KitStatus &st);

struct HilLedBoard {
    uint8_t  pwrState;        ///< last power-state byte the STM sent
    uint32_t pwrStateCount;   ///< power-state callbacks since boot
    uint32_t txFail;          ///< strip refreshes that failed
    uint32_t refreshes;       ///< strip refreshes attempted
};

/// LEDS: bri= anim= coalesce= cfgbri= pwr= pwrN= txfail= refreshes= colors=
size_t hil_text_led_state(char *out, size_t cap, const crosspad::control::LedState &st,
                       const HilLedBoard &board);

/// MIXLVL: ch= name= pk= vol= trim= mute= route=
size_t hil_text_mix_channel(char *out, size_t cap, const crosspad::control::MixerChannelLevel &row);

/// MIXLVL: out= pk= vol= mute= lim= ceil=
size_t hil_text_mix_output(char *out, size_t cap, const crosspad::control::MixerOutputLevel &row);

/// MIXLVL: tap lim= -- the limiter on the pads tap bounces and takes record
size_t hil_text_mix_tap(char *out, size_t cap, const crosspad::control::MixerTapLevel &row);

struct HilPitchedBoard {
    uint32_t    rtMaxUs;        ///< worst pitched render block since the last read
    const char *lastError;      ///< why the last load failed, "" after a good one; never null
    uint32_t    rtBusyMaxUs;    ///< worst whole render pass since the last read
    uint32_t    overruns;       ///< render passes longer than a block, since boot
    uint8_t     kitMaxVoices;   ///< what the kit asked for
};

/// Capacity of PITCHED's roots field: up to kMaxZones roots of "127.00," each.
constexpr size_t kHilPitchedRootsChars = 8 * crosspad::SampleBank::kMaxZones + 8;
/// A PITCHED line: the fixed fields, then the roots field.
constexpr size_t kHilPitchedLineChars = 192 + kHilPitchedRootsChars;

/// PITCHED: active= zones= roots= voices= bank_kb= rt_max_us= steals= [err=]
/// rt_busy_max_us= overruns= cap= kit_max= shed=
size_t hil_text_pitched_status(char *out, size_t cap, const crosspad::control::PitchedStatus &st,
                            const HilPitchedBoard &board);

/// WAVE: kit= total= cached= loader= queued= req= ok= retry= fail_info=
/// fail_read= alloc_fail= drop= sweep= start_fail= missing=
size_t hil_text_wave_status(char *out, size_t cap, const crosspad::control::WaveStatus &st);

/// KIT_LIST's lines: "KITS: id:name,..." in batches that fit the buffer, then
/// "KITS: end count= current=". Each finished line goes to @p emit.
struct HilKitListText {
    char  *out;
    size_t cap;
    size_t n;                       ///< used so far of the line being built
    int    onLine;                  ///< kits on it
    void (*emit)(const char *line);
};

void hil_text_kit_list_begin(HilKitListText &text);
/// A removed kit (no path) is no longer a kit and is left out.
void hil_text_kit_list_add(HilKitListText &text, const crosspad::control::KitListRow &row);
void hil_text_kit_list_end(HilKitListText &text, int count, int current);

/// AUDIOLVL: L= R= amp= allowed=
size_t hil_text_audio_level(char *out, size_t cap, const crosspad::control::AudioLevel &lv, bool ampOn,
                         bool ampAllowed);
