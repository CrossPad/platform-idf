// SPDX-License-Identifier: GPL-3.0-or-later
//
// The effects bus as the board has it: the four sources are mixer channels
// (Line 1/2 = IN2, Line 3/4 = IN3, the mics = IN1, USB = the host's playback),
// the returns are Out 1/2, Out 3/4 and the USB rec bus. Attached, the
// sources' own routes are put aside so the processor is their only way out,
// and put back on detach. The sends pick the input pair: the two ADCs hear
// the analog sources sent, and the input monitor holds them there
// (InputMonitorPort.cpp counts an FX send as a send).

#include "FxBusPort.hpp"
#include "IdfAudioModule.hpp"
#include "usb_audio_port.h"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "crosspad/audio/IFxBus.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include "esp_log.h"

#include <atomic>

namespace crosspad {
namespace {

constexpr const char* TAG = "FxBus";
constexpr int kSources = (int)FxSource::Count;
constexpr int kOutputs = (int)FxOutput::Count;
constexpr uint8_t kOut[kOutputs] = {0, 1, IdfAudioModule::CAPTURE_BUS};

AudioMixerEngine* s_mixer = nullptr;
uint8_t s_ch[kSources] = {kMixerInvalidChannelId, kMixerInvalidChannelId, kMixerInvalidChannelId,
                          kMixerInvalidChannelId};

class FxBus final : public IFxBus {
public:
    bool hasSource(FxSource s) const override
    {
        return idx(s) >= 0 && s_ch[idx(s)] != kMixerInvalidChannelId;
    }

    bool sourceAvailable(FxSource s) const override
    {
        switch (s) {
        case FxSource::Usb:    return hasSource(s) && usb_audio_port_active();
        case FxSource::Mic:    return input_pairs_codec_of(kInputMic) >= 0;
        case FxSource::Line12: return input_pairs_codec_of(kInputLine12) >= 0;
        case FxSource::Line34: return hasSource(s) && input_pairs_codec_of(kInputLine34) >= 0;
        default:               return false;
        }
    }

    void setSend(FxSource s, float level) override
    {
        const int i = idx(s);
        if (i < 0) return;
        send_[i].store(level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level));
        if (attached_) apply();
    }

    float send(FxSource s) const override { return idx(s) >= 0 ? send_[idx(s)].load() : 0.0f; }

    void setPreFader(bool pre) override
    {
        pre_ = pre;
        if (attached_) s_mixer->setFxPreFader(pre);
    }

    bool hasOutput(FxOutput o) const override { return (int)o < kOutputs && kOut[(int)o] < s_mixer->numOutputs(); }

    void setReturn(FxOutput o, bool on) override
    {
        if ((int)o >= kOutputs) return;
        ret_[(int)o] = on;
        if (attached_) s_mixer->setFxReturn(kOut[(int)o], on ? 1.0f : 0.0f);
    }

    bool returnOn(FxOutput o) const override { return (int)o < kOutputs && ret_[(int)o]; }

    void attach(IAudioNode* processor) override
    {
        if (!s_mixer || !processor) return;
        if (!attached_) {
            /* The sources' own way out, put aside: the processor carries them. */
            for (int i = 0; i < kSources; ++i) {
                if (s_ch[i] == kMixerInvalidChannelId) continue;
                for (uint8_t o = 0; o < kMixerMaxOutputs; ++o) {
                    saved_[i][o] = s_mixer->isRouteEnabled(s_ch[i], o);
                    s_mixer->setRouteEnabled(s_ch[i], o, false);
                }
            }
            attached_ = true;
        }
        s_mixer->setFxProcessor(processor);
        apply();
        ESP_LOGI(TAG, "processor %s attached", processor->name());
    }

    void detach() override
    {
        if (!s_mixer || !attached_) return;
        s_mixer->setFxProcessor(nullptr);
        for (int i = 0; i < kSources; ++i) {
            if (s_ch[i] == kMixerInvalidChannelId) continue;
            s_mixer->setFxSend(s_ch[i], 0.0f);
            for (uint8_t o = 0; o < kMixerMaxOutputs; ++o) s_mixer->setRouteEnabled(s_ch[i], o, saved_[i][o]);
        }
        for (int o = 0; o < kOutputs; ++o) s_mixer->setFxReturn(kOut[o], 0.0f);
        attached_ = false;
        ESP_LOGI(TAG, "processor detached, routes back");
    }

    void level(float& left, float& right) const override
    {
        if (s_mixer) s_mixer->getFxLevel(left, right);
        else left = right = 0.0f;
    }

private:
    static int idx(FxSource s) { return (int)s < kSources ? (int)s : -1; }

    /* Sends, returns and the pair, as asked; only while attached. */
    void apply()
    {
        for (int i = 0; i < kSources; ++i) {
            if (s_ch[i] != kMixerInvalidChannelId) s_mixer->setFxSend(s_ch[i], send_[i].load());
        }
        for (int o = 0; o < kOutputs; ++o) s_mixer->setFxReturn(kOut[o], ret_[o] ? 1.0f : 0.0f);
        s_mixer->setFxPreFader(pre_);
        const bool mic = send_[(int)FxSource::Mic].load() > 0.0f;
        const bool l12 = send_[(int)FxSource::Line12].load() > 0.0f;
        const bool l34 = send_[(int)FxSource::Line34].load() > 0.0f;
        if (!input_pairs_cover(mic, l12, l34)) ESP_LOGW(TAG, "mic, line 1/2 and line 3/4 sent: two ADCs hear two");
    }

    std::atomic<float> send_[kSources] = {};
    bool ret_[kOutputs] = {true, false, false};
    bool pre_ = false;
    bool attached_ = false;
    bool saved_[kSources][kMixerMaxOutputs] = {};
};

FxBus s_bus;

} // namespace

void fx_bus_init(AudioMixerEngine& mixer, const uint8_t (&chIn)[kInputSources], uint8_t chUsb)
{
    s_mixer = &mixer;
    s_ch[(int)FxSource::Line12] = chIn[kInputLine12];
    s_ch[(int)FxSource::Line34] = chIn[kInputLine34];
    s_ch[(int)FxSource::Mic]    = chIn[kInputMic];
    s_ch[(int)FxSource::Usb]    = chUsb;
    getPlatformServices().fxBus = &s_bus;
}

} // namespace crosspad
