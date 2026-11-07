// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output levels, per physical output -- the built-in speakers, the Out 1/2
// and Out 3/4 jacks -- each the analog volume of the codec output pairs it is
// wired to, after the DAC (pinout BSP_OUT_*). Where the speakers and a jack
// share one codec on different pairs they hear the same stream at their own
// levels. The level lives in CrosspadSettings::masterFX.outVolumeCodec --
// [0][0] the speakers, [0][1] Out 1/2, [1][0] Out 3/4 -- the fields Settings
// and the volume overlay already drive, so every control shows one number.
// Muted, an output sits at its codec's lowest step with its level kept; the
// speakers' amp is switched off besides (masterFX.speakersEnabled, the field
// Settings' "Speakers" switch and the STM amp gate read).

#include "OutputLevelsPort.hpp"

#include "NvsSettingsStore.hpp"

#include "crosspad/audio/IOutputLevels.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

#include "bsp_es8388.h"
#include "crosspad_s3.h"
#include "esp_log.h"

#include <cstring>

namespace crosspad {
namespace {

constexpr const char* TAG = "OutputLevels";
constexpr const char* kNvsNs = "audio_out";   // the jacks' mutes
constexpr uint8_t kMaxLevel = 100;

struct Output {
    BoardOutput role;
    const char* name;
    const char* mixerOutput;   // as CrosspadPlatform names the engine's buses
    int         codec;
    uint8_t     pairs;         // DACPOWER bits, bsp_es8388_dac_output_t
    int         row, col;      // masterFX.outVolumeCodec[row][col]
    const char* key;           // its NVS key in "cfg_audio"
};

/* Out 1/2 (the phones jack) is listed only where it has pairs of its own. */
constexpr Output kOutputs[] = {
    {BoardOutput::Speakers, "Speakers", "Out 1/2", BSP_OUT_SPEAKERS_CODEC, BSP_OUT_SPEAKERS_PAIRS, 0, 0, "ovc00"},
    {BoardOutput::Jack12,   "Phones",   "Out 1/2", BSP_OUT_JACK12_CODEC,   BSP_OUT_JACK12_PAIRS,   0, 1, "ovc01"},
    {BoardOutput::Jack34,   "Line out", "Out 3/4", BSP_OUT_JACK34_CODEC,   BSP_OUT_JACK34_PAIRS,   1, 0, "ovc10"},
};
constexpr int kAll = sizeof(kOutputs) / sizeof(kOutputs[0]);

uint8_t* field(const Output& o)
{
    auto* cfg = CrosspadSettings::getInstance();
    return cfg ? &cfg->masterFX.outVolumeCodec[o.row][o.col] : nullptr;
}

bool s_jackMuted[kAll];

int index_of(const Output& o) { return (int)(&o - kOutputs); }

bool is_muted(const Output& o)
{
    if (o.role == BoardOutput::Speakers) {
        auto* cfg = CrosspadSettings::getInstance();
        return cfg && !cfg->masterFX.speakersEnabled;
    }
    return s_jackMuted[index_of(o)];
}

/* The level the codec gets: the kept one, or its lowest step while muted.
 * Settings store 0..127 (kPercentMax clamps reads); the codec takes 0..100. */
void hw(const Output& o)
{
    const uint8_t* f = field(o);
    if (!o.pairs || !f) return;
    const uint8_t level = is_muted(o) ? 0 : (*f > kMaxLevel ? kMaxLevel : *f);
    bsp_es8388_set_output_volume(o.codec == 0 ? BSP_I2S_INTERFACE_0 : BSP_I2S_INTERFACE_1,
                                 (bsp_es8388_dac_output_t)o.pairs, level);
}

void jack_mute_key(const Output& o, char* key) { key[0] = 'm'; key[1] = (char)('0' + index_of(o)); key[2] = 0; }

const Output* find(BoardOutput role)
{
    for (const auto& o : kOutputs) if (o.role == role && o.pairs) return &o;
    return nullptr;
}

class OutputLevels final : public IOutputLevels {
public:
    OutputLevels()
    {
        for (const auto& o : kOutputs) if (o.pairs) list_[n_++] = &o;
    }

    int outputs() const override { return n_; }
    const char* name(int i) const override { return valid(i) ? list_[i]->name : ""; }
    const char* mixerOutput(int i) const override { return valid(i) ? list_[i]->mixerOutput : ""; }

    int level(int i) const override
    {
        const uint8_t* f = valid(i) ? field(*list_[i]) : nullptr;
        return f ? (*f > kMaxLevel ? kMaxLevel : *f) : 0;
    }

    bool setLevel(int i, int level) override
    {
        if (!valid(i)) return false;
        const uint8_t v = (uint8_t)(level < 0 ? 0 : level > kMaxLevel ? kMaxLevel : level);
        const Output& o = *list_[i];
        if (uint8_t* f = field(o)) *f = v;
        hw(o);
        NvsSettingsStore::getInstance().saveU8("cfg_audio", o.key, v);
        return true;
    }

    bool muted(int i) const override { return valid(i) && is_muted(*list_[i]); }

    bool setMuted(int i, bool m) override
    {
        if (!valid(i)) return false;
        const Output& o = *list_[i];
        if (o.role == BoardOutput::Speakers) {
            auto* cfg = CrosspadSettings::getInstance();
            if (!cfg) return false;
            cfg->masterFX.speakersEnabled = !m;
            cfg->stm.audioEn = m ? 0 : 1;
            NvsSettingsStore::getInstance().saveBool("cfg_audio", "spk_en", !m);
            NvsSettingsStore::getInstance().saveU8("cfg_stm", "audio_en", m ? 0 : 1);
        } else {
            s_jackMuted[index_of(o)] = m;
            char key[4];
            jack_mute_key(o, key);
            NvsSettingsStore::getInstance().saveBool(kNvsNs, key, m);
        }
        hw(o);
        return true;
    }

private:
    bool valid(int i) const { return i >= 0 && i < n_; }

    const Output* list_[kAll] = {};
    int n_ = 0;
};

OutputLevels s_levels;

} // namespace

void output_levels_init()
{
    for (const auto& o : kOutputs) {
        if (o.role == BoardOutput::Speakers || !o.pairs) continue;
        char key[4];
        jack_mute_key(o, key);
        s_jackMuted[index_of(o)] = NvsSettingsStore::getInstance().readBool(kNvsNs, key, false);
    }
    getPlatformServices().outputLevels = &s_levels;
}

namespace {

/* The Founders firmware (tag first-release-2026-10-31, d90ee01) gave the
 * codec Line out's kept level x 56 / 100; from 52177eb the codec gets it 1:1.
 * An image's version is its `git describe`: the tag itself, or d90ee01 by
 * hash where the tag came after the build. */
bool is_capped_image(const char* version)
{
    if (!version) return false;
    return std::strcmp(version, "first-release-2026-10-31") == 0 ||
           std::strcmp(version, "first-release-2026-10-31-dirty") == 0 ||
           std::strstr(version, "-gd90ee01") != nullptr;
}

} // namespace

/* Once, on the first boot of this firmware, and only when the image it came
 * from (the other slot: a Store update writes the slot not running and boots
 * it) is the Founders firmware: the kept level becomes the codec level that
 * firmware played, so Out 3/4 stays as loud and the number on screen changes
 * instead (a kept 70 played codec 39; 1:1 it is ~15 dB hotter). A board that
 * never kept a level played the default the same way, so it is rescaled too.
 * A board that ran v1.1.1 already plays 1:1 and keeps its number. The marker
 * sits beside the jacks' mutes, which a settings reset leaves alone, so a
 * reset board starts from the plain default. */
void output_levels_migrate(const char* previousImage)
{
    constexpr const char* kMarker = "lo_1to1";
    constexpr unsigned kOldTop = 56;
    auto& store = NvsSettingsStore::getInstance();
    if (store.readU8(kNvsNs, kMarker, 0)) return;
    store.saveU8(kNvsNs, kMarker, 1);

    const Output* o = find(BoardOutput::Jack34);
    uint8_t* f = o ? field(*o) : nullptr;
    if (!f || !is_capped_image(previousImage)) return;
    const unsigned was = *f > kMaxLevel ? kMaxLevel : *f;
    *f = (uint8_t)((was * kOldTop + kMaxLevel / 2) / kMaxLevel);
    store.saveU8("cfg_audio", o->key, *f);
    ESP_LOGW(TAG, "%s: %u -> %u, as loud as under %s", o->name, was, (unsigned)*f, previousImage);
}

void output_levels_apply()
{
    for (const auto& o : kOutputs) {
        const uint8_t* f = field(o);
        if (!o.pairs || !f) continue;
        hw(o);
        ESP_LOGI(TAG, "%s: %u%s (codec %d, pairs 0x%02X)", o.name, (unsigned)*f,
                 is_muted(o) ? " muted" : "", o.codec, (unsigned)o.pairs);
    }
}

void output_levels_set(BoardOutput out, uint8_t level)
{
    const Output* o = find(out);
    if (!o) return;
    if (uint8_t* f = field(*o)) *f = level;
    hw(*o);
}

} // namespace crosspad
