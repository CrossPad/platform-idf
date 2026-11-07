/**
 * @file audio_control.cpp
 * @brief Implementation of the audio control
 *
 * @date 27.03.2026
 * @author Marcel Licence
 */


#include "audio_control.h"
#include "audio_settings.h"

#include "crosspad_s3.h"
#include "bsp_es8388.h"
#include "audio_route_control.h"
#include "OutputLevelsPort.hpp"

#include "crosspad/settings/CrosspadSettings.hpp"


//#define AUDIO_MUTE_TIMEOUT_S    (SAMPLE_RATE * 5)


static i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();

static const char* TAG = "AudioControl";


bool AudioControl_Setup(void)
{
    // The sample engine plays files 1:1, so the I2S clock has to run at the
    // rate the content is in (kits ship at 44.1 kHz). 96 kHz is in the settings
    // list but the UAC endpoints are sized for 48 kHz at most.
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    uint32_t rate = cfg ? cfg->audioEngine.sampleRate : BSP_I2S_DEFAULT_SAMPLE_RATE;
    if (rate != 44100 && rate != 48000) {
        ESP_LOGW(TAG, "engine rate %lu Hz unsupported on this board, using %u",
                 (unsigned long)rate, BSP_I2S_DEFAULT_SAMPLE_RATE);
        rate = BSP_I2S_DEFAULT_SAMPLE_RATE;
    }
    bsp_i2s_set_sample_rate(rate);
    ESP_LOGI(TAG, "audio engine rate %lu Hz", (unsigned long)rate);
    esp_err_t ret = bsp_i2s_es8388_init(i2c_handle, NULL, true);
    if (ret != ESP_OK)
    {
        return false;
    }
    audio_route_codecs_started();
    crosspad::output_levels_apply();

    // Warm boot leaves DACCONTROL3 muted and start() short-circuits without
    // unmuting, so playback stays silent until a cold boot. Force it here.
    bsp_es8388_set_mute((bsp_i2s_interface_t)0, false);
    bsp_es8388_set_mute((bsp_i2s_interface_t)1, false);

    // The USB host records the Mixer's USB bus: what ROUTE sends there, the
    // inputs included, is what a DAW gets (2026-10-31: a board left on MICS
    // recorded the mics while ROUTE sent the Sampler to USB). The ADC presets
    // stay for the bench, over CDC and SysEx, until the next boot. This also
    // makes the routing shadow state honest -- bsp_i2s_es8388_init() leaves
    // both ADCs on LINE1.
    audio_route_apply_preset(AUDIO_ROUTE_PRESET_SAMPLER);
    /* The capture bus keeps its own level across boots (the preset does not). */
    audio_route_set_daw_gain(cfg ? cfg->usbAudioGainDb : 0);

    return true;
}

void AudioControl_Process(void)
{
#ifdef AUDIO_MUTE_TIMEOUT_S
    static uint32_t count;

    if (count < AUDIO_MUTE_TIMEOUT_S)
    {
        count += FRAME_COUNT;
    }
    else
    {
        static bool do_mute = true;
        if (do_mute)
        {
            do_mute = false;
            bsp_es8388_set_volume((bsp_i2s_interface_t)0, 0);
        }
    }
#endif
}
