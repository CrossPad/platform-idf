#include "imu_midi.h"

#include "crosspad/imu/ImuMidiMapper.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/midi/IMidiOutput.hpp"

#include "imu.h"
#include "bsp_worker.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ImuMidi";

namespace {

crosspad::ImuMidiMapper s_mapper;
bsp_worker_job_t *s_job = nullptr;

/* 50 Hz is the rate the mapper throttles to anyway, so feeding it faster would
 * only spend CPU to have samples discarded. The stack is small because this task
 * does arithmetic and one MIDI call; it holds no buffers. */
constexpr uint32_t kFeedPeriodMs = 20;

/* Worker job, every 20 ms while enabled. */
uint32_t feed_job(void *)
{
    bsp_imu_data_t d{};
    if (bsp_imu_get_data(&d) == ESP_OK) {
        s_mapper.update(d.fused_pitch, d.fused_roll,
                        d.gyro_x, d.gyro_y, d.gyro_z,
                        (uint32_t)(esp_timer_get_time() / 1000));
    }
    return 0;
}

} // namespace

extern "C" void imu_midi_apply_settings(void)
{
    auto *cfg = crosspad::CrosspadSettings::getInstance();
    if (!cfg) return;
    s_mapper.setOutput(&crosspad::getMidiOutput());
    cfg->imuMidi.applyTo(s_mapper);
}

extern "C" void imu_midi_set_enabled(bool on)
{
    imu_midi_apply_settings();
    s_mapper.setEnabled(on);
    /* A slow tilt can stay under the wake-up threshold, and a sleeping gyro
     * would freeze the mapped CCs mid-gesture. Switching it back on belongs to
     * the power state (LIGHT sleep), not here. */
    if (on) bsp_imu_set_inactivity(false);

    if (on && s_job == nullptr) {
        s_job = bsp_worker_add("imu_midi", kFeedPeriodMs, feed_job, nullptr);
        if (s_job == nullptr) {
            ESP_LOGW(TAG, "feeder job not registered — IMU MIDI stays off");
            s_mapper.setEnabled(false);
            return;
        }
        ESP_LOGI(TAG, "IMU MIDI on (%lu ms feed)", (unsigned long)kFeedPeriodMs);
    } else if (!on && s_job != nullptr) {
        bsp_worker_remove(s_job);
        s_job = nullptr;
        ESP_LOGI(TAG, "IMU MIDI off");
    }
}

extern "C" bool imu_midi_enabled(void) { return s_mapper.enabled(); }

extern "C" uint32_t imu_midi_sent(void) { return s_mapper.sent(); }

extern "C" void imu_midi_init(void)
{
    auto *cfg = crosspad::CrosspadSettings::getInstance();
    imu_midi_set_enabled(cfg && cfg->imuMidi.enabled);
}
