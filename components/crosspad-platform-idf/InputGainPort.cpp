// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input gain, per input -- the mics, Line 1/2, Line 3/4 -- on whichever codec
// the input pair puts it. The ES8388's MicAmp gives 0..+24 dB in 3 dB steps,
// analog, ahead of the ADC -- the quiet way to gain. The mics need more than
// that in an ordinary room (a take at +24 dB peaked around -60 dBFS), so
// above +24 dB the rest is digital, applied to every ADC block where it is
// read. The line inputs need the other way: a line level at 0 dB already
// sits around -30 dBFS for -40 dBFS out of an interface (2026-10-29), so
// they go down to -24 dB, digital as well, and stop at the MicAmp's top --
// a digital boost on a line only raised its noise.

#include "InputGainPort.hpp"
#include "InputPairPort.hpp"

#include "crosspad/audio/IInputGain.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include "bsp_es8388.h"
#include "crosspad_s3.h"
#include "esp_log.h"
#include "nvs.h"

#include <atomic>
#include <cmath>

namespace crosspad {
namespace {

constexpr const char* TAG = "InputGain";
constexpr const char* kNvsNs = "audio_in";
constexpr int   kInputs     = kInputSources;
constexpr float kStepDb     = 3.0f;
constexpr float kAnalogDb   = 24.0f;     // the MicAmp's top
constexpr float kMaxDb      = 48.0f;
/* Per input: the mics 0..+48, the lines -24..+24. */
constexpr float kMinDbOf[kInputSources] = {0.0f, -24.0f, -24.0f};
constexpr float kMaxDbOf[kInputSources] = {kMaxDb, kAnalogDb, kAnalogDb};
/* The mics at the top of the MicAmp (what the codec init always used), the
 * jacks flat. */
constexpr float kDefaultDb[kInputs] = {24.0f, 0.0f, 0.0f};
const char* const kChannel[kInputs] = {"IN1", "IN2", "IN3"};

class InputGain final : public IInputGain {
public:
    /* Line 3/4 only where the board has it. */
    int inputs() const override { return BSP_HAS_SHARED_LINE_IN ? kInputs : kInputLine34; }
    const char* channelName(int i) const override { return i >= 0 && i < kInputs ? kChannel[i] : ""; }
    float gainDb(int i) const override { return i >= 0 && i < kInputs ? db_[i].load() : 0.0f; }
    float maxDb() const override { return kMaxDb; }
    float stepDb() const override { return kStepDb; }
    float minDbOf(int i) const override { return i >= 0 && i < kInputs ? kMinDbOf[i] : 0.0f; }
    float maxDbOf(int i) const override { return i >= 0 && i < kInputs ? kMaxDbOf[i] : kMaxDb; }

    bool setGainDb(int i, float db) override {
        if (i < 0 || i >= inputs()) return false;
        apply(i, db);
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            char key[8] = {'g', (char)('0' + i), 0};
            nvs_set_i16(h, key, (int16_t)lroundf(db_[i].load() * 10.0f));
            nvs_commit(h);
            nvs_close(h);
        }
        return true;
    }

    void load() {
        nvs_handle_t h;
        const bool open = nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK;
        for (int i = 0; i < inputs(); ++i) {
            int16_t v = (int16_t)lroundf(kDefaultDb[i] * 10.0f);
            if (open) {
                char key[8] = {'g', (char)('0' + i), 0};
                nvs_get_i16(h, key, &v);
            }
            apply(i, (float)v / 10.0f);
        }
        if (open) nvs_close(h);
    }

    float factor(int i) const { return i >= 0 && i < kInputs ? factor_[i].load(std::memory_order_relaxed) : 1.0f; }

    /// Input @p i's analog part onto the codec it is on now, if any.
    void setAnalog(int i) {
        const int codec = input_pairs_codec_of(i);
        if (codec < 0) return;
        bsp_es8388_set_mic_gain(codec == 0 ? BSP_I2S_INTERFACE_0 : BSP_I2S_INTERFACE_1,
                                (uint8_t)lroundf(analog(db_[i].load()) / kStepDb));
    }

private:
    /// The MicAmp's part of @p db: 0..+24 dB; the rest, above or below, is digital.
    static float analog(float db) { return db < 0.0f ? 0.0f : (db < kAnalogDb ? db : kAnalogDb); }

    void apply(int i, float db) {
        db = std::round(db / kStepDb) * kStepDb;
        db = db < kMinDbOf[i] ? kMinDbOf[i] : (db > kMaxDbOf[i] ? kMaxDbOf[i] : db);
        factor_[i].store(powf(10.0f, (db - analog(db)) / 20.0f), std::memory_order_relaxed);
        db_[i].store(db);
        setAnalog(i);
        ESP_LOGI(TAG, "input %d: %+.0f dB (%.0f analog)", i, (double)db, (double)analog(db));
    }

    std::atomic<float> db_[kInputs] = {};
    std::atomic<float> factor_[kInputs] = {};
};

InputGain s_gain;

} // namespace

void input_gain_init()
{
    s_gain.load();
    getPlatformServices().inputGain = &s_gain;
}

void input_gain_follow_pair()
{
    for (int i = 0; i < kInputs; ++i) s_gain.setAnalog(i);
}

void input_gain_apply(int codec, int16_t* block, uint32_t frames)
{
    if (codec < 0 || codec > 1 || !block) return;
    const int src = input_pairs_source_on(codec);
    const float g = s_gain.factor(src);
    if (fabsf(g - 1.0f) < 0.0001f) return;
    constexpr uint32_t kChunk = 32;
    float buf[kChunk * 2];
    for (uint32_t at = 0; at < frames; at += kChunk) {
        const uint32_t n = frames - at < kChunk ? frames - at : kChunk;
        int16_t* p = block + at * 2;
        for (uint32_t i = 0; i < n * 2; ++i) buf[i] = (float)p[i] * g;
        for (uint32_t i = 0; i < n * 2; ++i) {
            const float v = buf[i];
            p[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
    }
}

} // namespace crosspad
