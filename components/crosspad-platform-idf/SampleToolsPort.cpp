// SPDX-License-Identifier: GPL-3.0-or-later
//
// This board's side of the sample tools (crosspad/sampletools/
// SampleToolsEngine): its worker is tools_io, a FreeRTOS task on core 0; its
// rings live in PSRAM above the UI's reserve, a preview's lent from the
// engine arena's slack; its card is the SD mount; a take from the mics or the
// line-in holds that codec's ADC on its input; and the "Preview" channel
// carries the native sampler's audition too. The render's taps are forwarded
// from where IdfAudioModule, IdfI2sInput and the USB bridge read the inputs.

#include "SampleToolsPort.hpp"
#include "SamplerEnginePort.hpp"
#include "engine_arena.hpp"
#include "IdfAudioModule.hpp"
#include "usb_audio_port.h"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "SampleToolsCaptureTap.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/SampleToolsEngine.hpp"

#include "bsp_sdmmc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* main/audio_route_control.cpp: hold a codec's ADC on its own input (the
 * mics, the jack) while the tools listen to it; the capture preset's choice
 * comes back when the last claim on it goes. */
extern "C" bool sample_tools_adc_claim(uint8_t codec, bool on);
/* Defined in main (sample_player_port.cpp): makes a bulk card reader or
 * writer give way to the streamer while pads are sounding. */
extern void sample_player_port_yield_to_streamer(uint32_t weight);

namespace crosspad {
namespace {

constexpr const char* TAG = "SampleTools";
constexpr const char* kCardRoot = "/sdcard";
/// Internal RAM an open needs: newlib's lock and the SDMMC DMA buffer come
/// from there, and a failed allocation in it is an abort, not NULL.
constexpr size_t kMinInternalForOpen = 2048;
/// PSRAM the UI keeps whatever a ring would like: LVGL aborts on a failed
/// allocation, a ring only gets shorter. The launcher runs in well under
/// 150 kB with a kit loaded, and an open app's next kit list or image decode
/// needs its share of that.
constexpr size_t kUiReserveBytes = 128 * 1024;
constexpr uint32_t kWorkerStack = 8192;
/// Above LVGL (4), as the song engine's worker: a busy screen must not starve
/// a take's writes. It waits on the card nearly all the time.
constexpr UBaseType_t kWorkerPriority = 5;
constexpr BaseType_t kWorkerCore = 0;

class BoardToolsPlatform final : public ISampleToolsPlatform {
public:
    BoardToolsPlatform() { wake_ = xSemaphoreCreateBinary(); }

    bool startWorker(void (*entry)(void*), void* arg) override {
        /* PSRAM stack: card and memory work only, never a flash write. */
        return xTaskCreatePinnedToCoreWithCaps(entry, "tools_io", kWorkerStack, arg, kWorkerPriority, nullptr,
                                               kWorkerCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    void joinWorker() override {}   // the engine lives as long as the board runs
    void wake() override { xSemaphoreGive(wake_); }
    void waitWake(uint32_t ms) override {
        xSemaphoreTake(wake_, ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(ms));
    }
    void sleepMs(uint32_t ms) override { vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 1); }
    int64_t nowUs() override { return esp_timer_get_time(); }

    /* A preview's or a take's ring may take the engine arena's slack
     * (engine_arena_lend_ring(): while a kit holds the arena, never the
     * owner's reserve, and past what the next pitched instrument needs),
     * given back by the arena's reclaim hook before a reset -- a take then
     * ends written out. Then the PSRAM heap, above the UI's reserve. */
    void* allocRingSegment(size_t bytes, uint32_t missingSegs, bool mayBorrow, bool& lent) override {
        void* p = mayBorrow ? engine_arena_lend_ring(bytes) : nullptr;
        lent = p != nullptr;
        if (p) return p;
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < missingSegs * bytes + kUiReserveBytes) return nullptr;
        return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    void freeRingSegment(void* p) override { engine_arena_free(p); }
    void* allocScratch(size_t bytes) override { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM); }

    /* An fopen that cannot abort: a failed allocation in internal RAM is an
     * abort, not NULL. */
    bool storageReady() override {
        return bsp_sdcard_get_handle() != nullptr &&
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= kMinInternalForOpen;
    }
    void yieldIo() override { sample_player_port_yield_to_streamer(1); }
    std::string cardRoot() override { return kCardRoot; }

    bool claimCodec(uint8_t codec, bool on) override { return sample_tools_adc_claim(codec, on); }
    bool inputAvailable(RecInput in) override { return in != RecInput::Usb || usb_audio_port_active(); }
    /* The native engine's audition (a file browsed, the start-up sound) is
     * heard on this channel too: one "Preview" on the desk. */
    void mixAudition(float* out, uint32_t frames) override { native_sampler_mix_preview(out, frames); }

    void log(bool warning, const char* msg) override {
        if (warning) ESP_LOGW(TAG, "%s", msg);
        else         ESP_LOGI(TAG, "%s", msg);
    }

private:
    SemaphoreHandle_t wake_ = nullptr;
};

SampleToolsEngine* s_tools = nullptr;

/// Built on first use: the capture tap may be set before sample_tools_init().
SampleToolsEngine& engine()
{
    static BoardToolsPlatform platform;
    static SampleToolsEngine tools(platform);
    return tools;
}

} // namespace

void sample_tools_init(AudioMixerEngine& mixer)
{
    SampleToolsEngine& tools = engine();
    s_tools = &tools;
    /* The preview ring may be lent from the engine arena: every reset asks
     * for it back first. */
    engine_arena_set_reclaim([](void* t) { static_cast<SampleToolsEngine*>(t)->reclaimLentRing(); }, &tools);
    const auto ch = mixer.addChannel(&tools, "Preview");
    if (ch != kMixerInvalidChannelId) {
        /* Speakers and phones, not the USB rec bus. A resample records the
         * pads tap, which the audition is never in. */
        for (uint8_t o = 0; o < IdfAudioModule::NUM_STREAM_OUTPUTS; ++o) mixer.setRouteEnabled(ch, o, true);
    }
    getPlatformServices().sampleTools = &tools;
}

void sample_tools_rt_input(int codec, const int16_t* block, uint32_t frames)
{
    if (s_tools && block) s_tools->rtInput(codec, block, frames);
}

void sample_tools_rt_usb(const int16_t* block, uint32_t frames)
{
    if (s_tools && block) s_tools->rtUsb(block, frames);
}

void sample_tools_rt_block_begin()
{
    if (s_tools) s_tools->rtBlockBegin();
}

void sample_tools_rt_bus(const float* padsBus, uint32_t frames)
{
    if (s_tools) s_tools->rtBus(padsBus, frames);
}

bool sample_tools_wants_bus()
{
    return s_tools && s_tools->wantsBus();
}

bool sample_tools_wants_codec(int codec)
{
    return s_tools && s_tools->wantsCodec(codec);
}

void sample_tools_set_capture_tap(CaptureTap fn, void* ctx)
{
    engine().setCaptureTap(fn, ctx);
}

} // namespace crosspad
