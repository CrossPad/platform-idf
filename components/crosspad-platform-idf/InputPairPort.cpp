// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input pairs: three stereo inputs, two ADCs. The mics are codec 0 LINE2,
// Line 1/2 codec 1 LINE2, Line 3/4 LINE1 of both codecs, so any two of the
// three can be heard at once and each pair puts each codec on one input. The
// mixer keeps a channel per input, not per codec: a pair moves which codec
// an input is read from, and its gain and sends go with it.

#include "InputPairPort.hpp"
#include "InputGainPort.hpp"

#include "crosspad/audio/IInputPairs.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include "crosspad_s3.h"
#include "esp_log.h"
#include "nvs.h"

#include <atomic>
#include <cstring>

/* main/audio_route_control.cpp: each codec's own input (1 = LINE1, 2 = LINE2,
 * its wire codes) -- the ADCs, and the DAC loop that shares LINE1. */
extern "C" bool input_pair_route(uint8_t in0, uint8_t in1);

namespace crosspad {
namespace {

constexpr const char* TAG = "InputPairs";
constexpr const char* kNvsNs = "audio_in";
constexpr uint8_t kLine1 = 1, kLine2 = 2;

struct Pair {
    const char* name;
    int8_t      codecOf[kInputSources];   // per input: its codec, -1 = left out
    uint8_t     adc[2];                   // per codec: the input it is on
};
const Pair kPairs[] = {
    {"Mic + 1/2", { 0,  1, -1}, {kLine2, kLine2}},
    {"Mic + 3/4", { 0, -1,  1}, {kLine2, kLine1}},
    {"1/2 + 3/4", {-1,  1,  0}, {kLine1, kLine2}},
};
constexpr int kPairCount = sizeof(kPairs) / sizeof(kPairs[0]);
const char* const kChannel[kInputSources] = {"IN1", "IN2", "IN3"};

std::atomic<int8_t> s_codecOf[kInputSources] = {{0}, {1}, {-1}};
std::atomic<int8_t> s_sourceOn[2] = {{kInputMic}, {kInputLine12}};

class InputPairs final : public IInputPairs {
public:
    int pairs() const override { return kPairCount; }
    const char* pairName(int i) const override { return i >= 0 && i < kPairCount ? kPairs[i].name : ""; }
    int pair() const override { return pair_.load(); }

    bool setPair(int i) override {
        if (i < 0 || i >= kPairCount) return false;
        const bool ok = connect(i);
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "pair", (uint8_t)i);
            nvs_commit(h);
            nvs_close(h);
        }
        return ok;
    }

    bool isConnected(const char* channelName) const override {
        for (int s = 0; s < kInputSources; ++s) {
            if (std::strcmp(kChannel[s], channelName) == 0) return s_codecOf[s].load() >= 0;
        }
        return true;     // not an input: always there
    }

    const char* inputOn(int codec) const override {
        static const char* const kName[kInputSources] = {"Mic", "Line 1/2", "Line 3/4"};
        const int s = input_pairs_source_on(codec);
        return s >= 0 && s < kInputSources ? kName[s] : nullptr;
    }

    void load() {
        uint8_t v = 0;
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "pair", &v);
            nvs_close(h);
        }
        connect(v < kPairCount ? v : 0);
    }

private:
    bool connect(int i) {
        const Pair& p = kPairs[i];
        /* An input that changes codec goes quiet first, so no render reads one
         * codec for two channels; the ADCs move, then the inputs read again. */
        for (int s = 0; s < kInputSources; ++s) {
            if (s_codecOf[s].load() != p.codecOf[s]) s_codecOf[s].store(-1);
        }
        const bool ok = input_pair_route(p.adc[0], p.adc[1]);
        for (int s = 0; s < kInputSources; ++s) {
            s_codecOf[s].store(p.codecOf[s]);
            if (p.codecOf[s] >= 0) s_sourceOn[p.codecOf[s]].store((int8_t)s);
        }
        pair_.store(i);
        input_gain_follow_pair();
        ESP_LOGI(TAG, "pair %d: %s%s", i, p.name, ok ? "" : " (codecs not up yet)");
        return ok;
    }

    std::atomic<int> pair_{0};
};

InputPairs s_pairs;

} // namespace

void input_pairs_init()
{
    if (!BSP_HAS_SHARED_LINE_IN) return;    // mics on codec 0, the jack on codec 1, always
    s_pairs.load();
    getPlatformServices().inputPairs = &s_pairs;
}

int input_pairs_codec_of(int src)
{
    return src >= 0 && src < kInputSources ? s_codecOf[src].load(std::memory_order_relaxed) : -1;
}

int input_pairs_source_on(int codec)
{
    return codec >= 0 && codec < 2 ? s_sourceOn[codec].load(std::memory_order_relaxed) : -1;
}

} // namespace crosspad
