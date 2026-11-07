// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input monitoring: the IN channels carry the mics and the jacks only while
// the ADC of the codec the input pair puts them on is on them
// (IdfI2sInput::setLive); a capture preset may leave it on the DAC loop, which
// sent to an output is a feedback loop. A send of the input on a codec
// anywhere claims that ADC for it, and the last send gone gives it back to
// the preset. Polled, not hooked: routes move
// from the Mixer, HIL and a desk loaded at boot, and a tenth of a second is
// nothing next to a finger -- the channel is silent until the ADC is there.
//
// It is also crosspad::IInputMonitor: an input sent to every heard output or
// to none, by the RecInput an app records it as -- the Mixer's MONITOR at
// "all" or "off", for Arrange's audio track.

#include "InputMonitorPort.hpp"
#include "InputPairPort.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "crosspad/audio/IInputMonitor.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include "bsp_worker.h"
#include "esp_log.h"

#include <atomic>
#include <cstring>

/* main/audio_route_control.cpp: hold or let go a codec's ADC on its input. */
extern "C" bool input_monitor_adc_claim(uint8_t codec, bool on);

namespace crosspad {
namespace {

constexpr const char* TAG = "InputMonitor";
constexpr uint32_t kPollMs = 100;

AudioMixerEngine* s_mixer = nullptr;
uint8_t s_ch[kInputSources] = {kMixerInvalidChannelId, kMixerInvalidChannelId, kMixerInvalidChannelId};
std::atomic<bool> s_wants[2] = {};

bool sent_anywhere(uint8_t ch)
{
    if (ch == kMixerInvalidChannelId || !s_mixer->isChannelActive(ch)) return false;
    for (uint8_t o = 0; o < s_mixer->numOutputs(); ++o) {
        if (s_mixer->isRouteEnabled(ch, o)) return true;
    }
    return false;
}

uint32_t poll(void*)
{
    for (uint8_t c = 0; c < 2; ++c) {
        const int src = input_pairs_source_on(c);
        const bool want = src >= 0 && sent_anywhere(s_ch[src]);
        if (want == s_wants[c].load()) continue;
        /* The ADC moves first and the render reads after; on the way out the
         * render stops first. */
        if (want) {
            input_monitor_adc_claim(c, true);
            s_wants[c].store(true);
        } else {
            s_wants[c].store(false);
            input_monitor_adc_claim(c, false);
        }
        ESP_LOGI(TAG, "IN%d (codec %u) %s", src + 1, (unsigned)c, want ? "sent: ADC held on its input" : "not sent any more");
    }
    return 0;
}

/// The channel the host's playback comes in on: what a USB take records.
uint8_t usb_channel()
{
    for (uint8_t c = 0; c < s_mixer->maxChannels(); ++c) {
        if (s_mixer->isChannelActive(c) && std::strcmp(s_mixer->getChannelName(c), "USB") == 0) return c;
    }
    return kMixerInvalidChannelId;
}

/// The channels @p in is recorded from, into @p out; how many.
int channels_of(RecInput in, uint8_t (&out)[2])
{
    int n = 0;
    auto add = [&](uint8_t ch) {
        if (ch != kMixerInvalidChannelId && s_mixer->isChannelActive(ch)) out[n++] = ch;
    };
    switch (in) {
    case RecInput::Mic:     add(s_ch[kInputMic]); break;
    case RecInput::Line:    add(s_ch[kInputLine12]); break;
    case RecInput::MicLine: add(s_ch[kInputMic]); add(s_ch[kInputLine12]); break;
    case RecInput::Usb:     add(usb_channel()); break;
    default:                break;   // the pads and Pitch are heard anyway
    }
    return n;
}

class InputMonitor final : public IInputMonitor {
public:
    bool canMonitor(RecInput in) const override
    {
        uint8_t ch[2];
        return s_mixer && channels_of(in, ch) > 0;
    }

    bool monitoring(RecInput in) const override
    {
        if (!s_mixer) return false;
        uint8_t ch[2];
        const int n = channels_of(in, ch);
        for (int i = 0; i < n; ++i) {
            for (uint8_t o = 0; o < s_mixer->numOutputs(); ++o) {
                if (s_mixer->isOutputHeard(o) && s_mixer->isRouteEnabled(ch[i], o)) return true;
            }
        }
        return false;
    }

    /* The Mixer's set_monitor() with every heard output or none: a send
     * switched on that was dragged to nothing comes back at unity. The desk
     * is saved as the Mixer's close saves it. */
    bool setMonitoring(RecInput in, bool on) override
    {
        if (!s_mixer) return false;
        uint8_t ch[2];
        const int n = channels_of(in, ch);
        if (!n) return false;
        for (int i = 0; i < n; ++i) {
            for (uint8_t o = 0; o < s_mixer->numOutputs(); ++o) {
                if (!s_mixer->isOutputHeard(o)) continue;
                if (on && s_mixer->getRouteVolume(ch[i], o) <= 0.0f) s_mixer->setRouteVolume(ch[i], o, 1.0f);
                s_mixer->setRouteEnabled(ch[i], o, on);
            }
        }
        s_mixer->notifyStateChanged();
        ESP_LOGI(TAG, "input %u monitored: %s", (unsigned)in, on ? "on every heard output" : "off");
        return true;
    }
};

InputMonitor s_monitor;

} // namespace

void input_monitor_start(AudioMixerEngine& mixer, const uint8_t (&chIn)[kInputSources])
{
    s_mixer = &mixer;
    for (int s = 0; s < kInputSources; ++s) s_ch[s] = chIn[s];
    getPlatformServices().inputMonitor = &s_monitor;
    if (!bsp_worker_add("in_monitor", kPollMs, poll, nullptr)) {
        ESP_LOGW(TAG, "no worker slot: IN channels follow the capture preset's ADC input");
    }
}

bool input_monitor_wants_codec(int codec)
{
    return codec >= 0 && codec < 2 && s_wants[codec].load(std::memory_order_relaxed);
}

} // namespace crosspad
