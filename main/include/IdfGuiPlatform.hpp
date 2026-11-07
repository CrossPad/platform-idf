#pragma once

#include "crosspad-gui/platform/IGuiPlatform.h"
#include "SamplerEnginePort.hpp"
#include "PitchedSampleNode.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "CrosspadPlatform.hpp"
#ifdef BSP_BOARD_CROSSPAD_S3
#include "stm32_uart.h"
#include "bsp_es8388.h"
#include "OutputLevelsPort.hpp"
#include "usb_config_manager.h"
#include "usb_transfer.h"
#include "audio_route_control.h"
#include "crosspad_s3.h"
#endif
#include "esp_err.h"
#include "bsp_sdmmc.h"
#include "wifi_link.h"
#include "fw_updater.h"
#include "cp_account.h"
#include "lv_async_safe.h"
#include "../audio/audio_task.h"
extern "C" esp_err_t bsp_display_brightness_set(int brightness_percent);
#include "display/lv_display_private.h"

#include <algorithm>
#include <cstring>
#include <cmath>

extern lv_indev_t * indev_encoderTop;
#ifdef BSP_BOARD_CROSSPAD_S3
void app_arm_softoff_boot_gate();
#endif

class IdfGuiPlatform : public crosspad_gui::IGuiPlatform {
public:
    lv_indev_t * getNavigationEncoder() override { return indev_encoderTop; }

    crosspad_gui::HeapStats getHeapStats() override {
        crosspad_gui::HeapStats stats;
        stats.sram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        stats.sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        stats.psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        stats.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        return stats;
    }

    crosspad_gui::VoiceMeter getVoiceMeter() override {
        crosspad_gui::VoiceMeter m = {};
        /* Only the native engines can answer: the prebuilt one has slots, not
         * voices, and no idea which of them is reading the card. With none
         * holding a kit, no pitched bank and no song playing, `limit` stays 0
         * and the bar draws nothing. */
        native_sampler_voice_meter(&m.active, &m.streaming, &m.limit, &m.full);
        const crosspad::PitchedInstrument& pi = crosspad::getPitchedSampleNode().instrument();
        if (pi.bank()) {
            const unsigned on = pi.soundingVoices(), cap = pi.maxVoices();
            m.active = (uint8_t)std::min(255u, m.active + on);
            m.limit = (uint8_t)std::min(255u, m.limit + cap);
            m.full = m.full || (cap && on >= cap);
        }
        /* The song's tracks played from files (audio, frozen seq tracks) are
         * voices too: one each, counted while heard; never at a ceiling. */
        if (crosspad::ISongEngine* se = crosspad::getPlatformServices().songEngine) {
            uint8_t sounding = 0, streams = 0;
            se->ringVoices(sounding, streams);
            m.active = (uint8_t)std::min(255u, (unsigned)m.active + sounding);
            m.limit = (uint8_t)std::min(255u, (unsigned)m.limit + streams);
        }
        return m;
    }

    crosspad_gui::DspLoad getDspLoad() override {
        crosspad_gui::DspLoad d;
        const uint32_t blockUs = audio_rt_block_us();
        if (!blockUs) return d;
        auto pct = [blockUs](uint32_t us) {
            const uint32_t v = (us * 100 + blockUs / 2) / blockUs;
            return (uint8_t)(v > 255 ? 255 : v);
        };
        d.avgPct = pct(audio_rt_busy_avg());
        d.peakPct = pct(audio_rt_busy_max(AUDIO_RT_BUSY_BAR, nullptr));
        d.overruns = audio_rt_overruns();
        d.measured = true;
        return d;
    }

    crosspad_gui::PerfStats getPerfStats() override {
        crosspad_gui::PerfStats ps = {};

        // FPS from LVGL perf monitor
#if LV_USE_PERF_MONITOR
        lv_display_t * disp = lv_display_get_default();
        if (disp) {
            ps.fps = disp->perf_sysmon_info.calculated.fps_avg_total;
        }
#endif

        // Per-core CPU usage via FreeRTOS idle task deltas
        getPerCoreCpu(&ps.cpu0_pct, &ps.cpu1_pct);

        // SRAM usage
        static size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ps.sram_pct = sram_total ? (uint32_t)((sram_total - sram_free) * 100 / sram_total) : 0;

        // PSRAM usage
        static size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ps.psram_pct = psram_total ? (uint32_t)((psram_total - psram_free) * 100 / psram_total) : 0;

        // STM32 messages per second (EMA smoothed)
        ps.stm32_mps = getStm32Mps();

        ps.sram_free_kb = (uint32_t)(sram_free / 1024);
        ps.sram_min_kb = (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
        ps.psram_free_kb = (uint32_t)(psram_free / 1024);
        ps.psram_block_kb = (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);

        /* The longest render pass since the bar last asked, against the block
         * it has to fit in -- the same measure SMPL_PEAK's rt_busy_max_us is. */
        const uint32_t blockUs = audio_rt_block_us();
        const uint32_t busyUs = audio_rt_busy_max(AUDIO_RT_BUSY_GUI, nullptr);
        ps.dsp_pct = blockUs ? (busyUs * 100 + blockUs / 2) / blockUs : 0;
        ps.dsp_overruns = audio_rt_overruns();

        return ps;
    }

    uint32_t millis() override {
        return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }

    const char * assetPathPrefix() override {
        return "/assets/";
    }

    // The explorer's paths are the card's own ("/crosspad/kits"); one that
    // already names the mount is left as it is.
    std::string storagePath(const std::string & path) override {
        const std::string mount = BSP_SD_MOUNT_POINT;
        return path.compare(0, mount.size(), mount) == 0 ? path : mount + path;
    }

    void delayMs(uint32_t ms) override {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    void sendPowerOff() override {
#ifdef BSP_BOARD_CROSSPAD_S3
        // Arm before the first command, independently of the STM UART IRQ. The
        // marker survives the ESP software reset when its own USB keeps power.
        app_arm_softoff_boot_gate();
        // Dialog calls sendPowerOff() 3× with delays; gracefully stop codecs
        // once before STM32 cuts 3.3V (~500ms after first command).
        static bool s_codecsStopped = false;
        if (!s_codecsStopped) {
            s_codecsStopped = true;
            bsp_es8388_stop(BSP_I2S_INTERFACE_0, BSP_ES8388_MODE_BOTH);
            bsp_es8388_stop(BSP_I2S_INTERFACE_1, BSP_ES8388_MODE_BOTH);
        }
        crosspad::getStm32CommandSender().sendPowerOff();
#endif
    }

    void setLcdBrightness(uint8_t pct) override {
        if (pct > 100) pct = 100;
#ifdef BSP_BOARD_CROSSPAD_S3
        // Crosspad S3 backlight is owned by the STM32 co-processor.
        crosspad::getStm32CommandSender().sendBrightness(pct);
#else
        bsp_display_brightness_set((int)pct);
#endif
    }

    void setTouchSensitivity(crosspad::TouchSensitivity level) override {
#ifdef BSP_BOARD_CROSSPAD_S3
        static_assert((int)crosspad::TouchSensitivity::Low == BSP_TOUCH_SENSITIVITY_LOW &&
                      (int)crosspad::TouchSensitivity::Medium == BSP_TOUCH_SENSITIVITY_MEDIUM &&
                      (int)crosspad::TouchSensitivity::High == BSP_TOUCH_SENSITIVITY_HIGH,
                      "TouchSensitivity and bsp_touch_sensitivity_t must match");
        bsp_touch_set_sensitivity((bsp_touch_sensitivity_t)level);
#else
        (void)level;
#endif
    }

    /* ── Which codec output each volume reaches ────────────────────────
     *
     * One level per physical output, each the analog volume of the codec
     * output pairs it is wired to (pinout BSP_OUT_*, OutputLevelsPort). On
     * PCB 2.0, measured 2026-10-12: codec 0 LOUT1/ROUT1 is the built-in
     * speakers, LOUT2/ROUT2 the Out 1/2 jack -- one stream at two levels --
     * and codec 1 the Out 3/4 jack. The speakers' amp has a gate on the STM
     * besides (REG_CTL_AUDIO_EN, Stm32Manager's SpeakerAmpGate), a switch.
     *
     * The "headphone" volume is Out 3/4. `OUTVOL [<1|2> <pct>]` over CDC
     * still sets a whole codec. */
    void setSpeakerVolume(uint8_t v) override {
#ifdef BSP_BOARD_CROSSPAD_S3
        crosspad::output_levels_set(crosspad::BoardOutput::Speakers, v);
#else
        (void)v;
#endif
    }

    void setHeadphoneVolume(uint8_t v) override {
#ifdef BSP_BOARD_CROSSPAD_S3
        crosspad::output_levels_set(crosspad::BoardOutput::Jack34, v);
#else
        (void)v;
#endif
    }

    void onUserInteraction(crosspad_gui::InteractionEvent /*event*/) override {
        // Touch is ESP-owned — the STM pad scan never sees it, so its idle timer
        // needs an explicit nudge or the screen would dim mid-use.
        crosspad::getStm32Manager().notifyUserActivity();
    }

    bool usbAudioSupported() override { return true; }

    void setUsbAudioEnabled(bool enabled) override {
        // Re-enumerates the USB device; the persisted preference is re-applied at
        // boot in app_main so the choice survives a power cycle.
        usb_config_manager_set_mode(enabled ? USB_MODE_AUDIO : USB_MODE_DEFAULT);
    }

    bool usbTransferSupported() override { return true; }
    void requestUsbTransfer() override { usb_transfer_request(); }

    // Saved WiFi networks live outside CrosspadSettings (shared by both A/B
    // slots), so the reset forgets them here; the reset has turned WiFi off,
    // which stops a running radio. The updater's result and choices live in
    // NVS "fwupd" the same way, and its RAM copies would write them back, so
    // it forgets under its own lock. The crosspad.app link (NVS "cpacct") goes
    // too: a board that changes hands must not stay on the owner's account.
    void onFactoryReset() override {
        wifi_factory_reset();
        fw_updater_forget();
        cp_account_forget();
    }

    /* LVGL is built without an OS layer here, so a callback posted from the
     * pad task goes through the display-lock forwarder, which never makes the
     * caller wait (lv_async_safe.h). */
    void postToGui(lv_async_cb_t cb, void * user_data) override {
        lv_async_call_locked(cb, user_data);
    }

    bool audioRouteSupported() override { return true; }

    bool getAudioRoute(crosspad_gui::AudioRouteState & out) override {
        audio_route_state_t st;
        audio_route_get_state(&st);
        out.captureCodec = st.captureCodec;
        out.adcInput[0]  = st.adcInput[0];
        out.adcInput[1]  = st.adcInput[1];
        out.dacOutput[0] = st.dacOutput[0];
        out.dacOutput[1] = st.dacOutput[1];
        out.mute[0]      = st.mute[0];
        out.mute[1]      = st.mute[1];
        out.preset       = (uint8_t)audio_route_get_preset();
        return true;
    }

    void setAudioRoutePreset(uint8_t preset) override {
        audio_route_apply_preset((audio_route_preset_t)preset);
    }
    void setAudioCaptureCodec(uint8_t codec) override {
        audio_route_set_capture_codec(codec);
    }
    void setAudioAdcInput(uint8_t codec, uint8_t input) override {
        audio_route_set_adc_input(codec, input);
    }
    void setAudioDacOutput(uint8_t codec, uint8_t output) override {
        audio_route_set_dac_output(codec, output);
    }
    void setAudioCodecMute(uint8_t codec, bool mute) override {
        audio_route_set_mute(codec, mute ? 1 : 0);
    }
    void setDawOutputGain(uint8_t db) override {
        audio_route_set_daw_gain(db);
    }

    bool hasStmConfigWrite() override {
        return crosspad::getStm32Manager().configSync() != nullptr;
    }
    void writeStmConfig8(uint16_t reg, uint8_t val) override {
        if (auto* cs = crosspad::getStm32Manager().configSync()) cs->writeReg8(reg, val);
    }
    void writeStmConfig16(uint16_t hiReg, uint16_t val) override {
        if (auto* cs = crosspad::getStm32Manager().configSync()) cs->writeReg16(hiReg, val);
    }
    void writeStmConfigPad(uint8_t padIdx) override {
        auto* cs = crosspad::getStm32Manager().configSync();
        auto* s  = crosspad::CrosspadSettings::getInstance();
        if (cs && s) cs->writePadConfig(padIdx, s->stm);
    }
    bool readStmPadVelRaw(uint8_t padIdx, uint8_t& out) override {
        auto* cs = crosspad::getStm32Manager().configSync();
        return cs && cs->readPadVelRaw(padIdx, out);
    }
    bool readStmPadLive(uint8_t padIdx, crosspad::Stm32PadLive& out) override {
        auto* cs = crosspad::getStm32Manager().configSync();
        return cs && cs->readPadLive(padIdx, out);
    }

private:
    // Per-core CPU usage from idle task runtime counters
    uint32_t prev_idle0_ = 0;
    uint32_t prev_idle1_ = 0;
    uint32_t prev_total_ = 0;

    void getPerCoreCpu(uint32_t * cpu0_out, uint32_t * cpu1_out) {
        *cpu0_out = 0;
        *cpu1_out = 0;
#if configGENERATE_RUN_TIME_STATS
        /* Each core's idle task against the time gone by. Not against half
         * of uxTaskGetSystemState()'s total: on this SMP port that total is
         * the clock (esp_timer, CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER),
         * not a sum over the cores, so halving it read every core as more than
         * 100 % idle -- 0 % busy -- whatever ran. And no walk of every task. */
        const uint32_t idle0 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
        const uint32_t idle1 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
        const uint32_t now = (uint32_t)esp_timer_get_time();
        const uint32_t dt = now - prev_total_;
        if (prev_total_ && dt) {
            auto busy = [dt](uint32_t idle) -> uint32_t {
                const uint32_t idlePct = (uint32_t)((uint64_t)idle * 100 / dt);
                return idlePct >= 100 ? 0 : 100 - idlePct;
            };
            *cpu0_out = busy(idle0 - prev_idle0_);
            *cpu1_out = busy(idle1 - prev_idle1_);
        }
        prev_idle0_ = idle0;
        prev_idle1_ = idle1;
        prev_total_ = now;
#endif
    }

    // STM32 message rate with exponential moving average
    float mps_avg_ = 0.0f;
    TickType_t mps_last_time_ = 0;

    uint32_t getStm32Mps() {
#ifdef BSP_BOARD_CROSSPAD_S3
        uint32_t stm_count = stm32_get_rx_msg_count(true);
        TickType_t now = xTaskGetTickCount();
        TickType_t dt = now - mps_last_time_;
        if (mps_last_time_ > 0 && dt > 0) {
            float instant_mps = (float)stm_count * configTICK_RATE_HZ / (float)dt;
            const float alpha = 0.6f;
            mps_avg_ = alpha * instant_mps + (1.0f - alpha) * mps_avg_;
        }
        mps_last_time_ = now;
        return (uint32_t)(mps_avg_ + 0.5f);
#else
        return 0;
#endif
    }
};
