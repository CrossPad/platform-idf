// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SamplerPlatformPort.hpp
 * @brief Everything the crosspad-sampler app needs from the board.
 *
 * The callback table of crosspad_sampler::SamplerPlatformCallbacks, filled
 * from the board's two sample engines (the native streamer and, when built,
 * the prebuilt one), the pitched instrument and the SD card: pad playback and
 * the SamplePlayback bridge, pad reloads around the voices that are sounding,
 * the waveform reads (WAV headers through core's wavReadLayout), auditions and
 * the live scope. crosspad-pc fills the same table in its PcSamplerPort.cpp.
 */

#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

/// Install the sampler's platform callbacks, with @p kitSelector (the kit
/// gate's, kit_gate_install()) as its kit-selector hooks; subscribe the
/// SamplePlayback bridge; make the sampler the system instrument.
/// Once, from the LVGL task, before the first app starts.
void sampler_platform_port_init(const crosspad_sampler::KitSelectorAppCallbacks& kitSelector);

/* What holding hits during a pad's reload cost since SMPLLAST last read: the
 * latest hit's delay (ms), the hits a newer one on the same pad replaced, and
 * the longest a pad was marked. */
extern uint32_t g_late_max_ms, g_late_lost;
extern uint32_t g_reload_win_max_us;
