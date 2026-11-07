/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from espressif__usb_device_uac for CrossPad composite device.
 * Always operates in AS_PART mode — TinyUSB lifecycle is managed externally.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "tusb.h"
#include "uac_config.h"
#include "usb_device_uac.h"
#include "uac_descriptors.h"
#include "tusb_config_uac.h"

static const char *TAG = "usbd_uac";


#define N_SAMPLE_RATES  1

enum {
    VOLUME_CTRL_0_DB = 0,
    VOLUME_CTRL_10_DB = 2560,
    VOLUME_CTRL_20_DB = 5120,
    VOLUME_CTRL_30_DB = 7680,
    VOLUME_CTRL_40_DB = 10240,
    VOLUME_CTRL_50_DB = 12800,
    VOLUME_CTRL_60_DB = 15360,
    VOLUME_CTRL_70_DB = 17920,
    VOLUME_CTRL_80_DB = 20480,
    VOLUME_CTRL_90_DB = 23040,
    VOLUME_CTRL_100_DB = 25600,
    VOLUME_CTRL_SILENCE = 0x8000,
};

// Resolution per format
const uint8_t spk_resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX};
const uint8_t mic_resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX};

/* The speaker task takes the host's audio from TinyUSB's EP OUT FIFO in ~4 ms
 * chunks to keep its context switches down: tud_audio_rx_done_isr() wakes it
 * once a chunk has gathered, its own 4 ms timeout picks up the rest.
 * read_buf holds a chunk at the highest rate: 4ms @ 48kHz/2ch/16bit = 768 bytes */
#define SPK_TASK_READ_MS    4
#define SPK_TASK_READ_SZ    (DEFAULT_SAMPLE_RATE / 1000 * SPEAK_CHANNEL_NUM * 2 * SPK_TASK_READ_MS)

typedef struct {
    uac_device_config_t user_cfg;
    int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];         // +1 for master channel 0
    int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];      // +1 for master channel 0
    int16_t mic_buf1[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 2];
    int16_t mic_buf2[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 2];
    int16_t *mic_buf_write;
    int16_t *mic_buf_read;
    int mic_data_size;
    int spk_itf_num;
    int mic_itf_num;
    uint8_t spk_resolution;
    uint8_t mic_resolution;
    uint32_t current_sample_rate;
    TaskHandle_t mic_task_handle;
    TaskHandle_t spk_task_handle;
    size_t spk_bytes_per_ms;
    size_t mic_bytes_per_interval;   /* exact for the interval: 44.1 kHz is 441 frames per 10 ms, not 44 per ms */
    uint32_t mic_frac;               /* carried thousandths of a frame, see usb_mic_task() */
    bool spk_active;
    bool mic_active;
} uac_device_t;

static uac_device_t *s_uac_device = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define UAC_ENTER_CRITICAL()        portENTER_CRITICAL(&s_mux)
#define UAC_EXIT_CRITICAL()         portEXIT_CRITICAL(&s_mux)
#define UAC_ENTER_CRITICAL_ISR()    portENTER_CRITICAL_ISR(&s_mux)
#define UAC_EXIT_CRITICAL_ISR()     portEXIT_CRITICAL_ISR(&s_mux)
#define UAC_CHECK_INIT()            do { if (!s_uac_device) return false; } while(0)
#define UAC_CHECK_INIT_VOID()       do { if (!s_uac_device) return; } while(0)

// Helper for clock get requests
static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            TU_LOG1("Clock get current freq %lu\r\n", s_uac_device->current_sample_rate);
            audio_control_cur_4_t curf = { (int32_t) tu_htole32(s_uac_device->current_sample_rate) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
        } else if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) rangef = {
                .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
            };
            TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
            rangef.subrange[0].bMin = (int32_t) s_uac_device->current_sample_rate;
            rangef.subrange[0].bMax = (int32_t) s_uac_device->current_sample_rate;
            rangef.subrange[0].bRes = 0;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
        }
    } else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = { .bCur = 1 };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
    }

    TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

// Helper for clock set requests
static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));
        uint32_t target_sample_rate = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;
        TU_LOG1("Clock set current freq: %ld\r\n", target_sample_rate);
        if (target_sample_rate != s_uac_device->current_sample_rate) {
            return false;
        }
        return true;
    }

    TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

/* Feedback measured against a clock locked to the sample rate, not from the FIFO
 * level. AUDIO_FEEDBACK_METHOD_FIFO_COUNT regulates the OUT FIFO towards half
 * full; TinyUSB's own note calls out that it needs FIFO headroom to absorb its
 * jitter, and that headroom is what a small host buffer does not have. On macOS
 * a 2048-frame buffer played clean while 1024 glitched and 512 was unusable,
 * with zero USB errors in the kernel log throughout -- the packets were valid,
 * they carried the wrong sample count. Windows resamples over it.
 *
 * The method is declared FREQUENCY_FIXED only so TinyUSB enables the SOF
 * interrupt and calls tud_audio_feedback_interval_isr(); the value itself is
 * computed there. */
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FREQUENCY_FIXED;
    feedback_param->sample_freq = s_uac_device ? s_uac_device->current_sample_rate : DEFAULT_SAMPLE_RATE;
    feedback_param->frequency.mclk_freq = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL;
}

/* Called from the SOF ISR every (1 << interval_shift) frames -- see
 * audiod_sof_isr(), which derives the shift from the feedback endpoint's
 * bInterval - 1. Full speed only, which is all this chip does.
 *
 * The value is computed here instead of through tud_audio_feedback_update():
 * that helper scales by 2^(16 - (frame_shift - 1)) while the callback covers
 * 2^frame_shift frames, which comes out one binary order high -- 96 samples per
 * frame instead of 48 at 48 kHz. Sending twice what the board consumes overruns
 * the OUT FIFO, measured as 30 % dropouts in a tone loopback.
 *
 * samples/frame = f_s * cycles / (f_cpu * n_frames), in 16.16. f_cpu is a valid
 * reference because it and the I2S sample clock both come from the same 40 MHz
 * crystal, so counting its cycles between host SOFs measures this board's rate
 * against the host's -- which is what the feedback endpoint reports. Requires
 * DFS to stay off (no CONFIG_PM_ENABLE); with frequency scaling the cycle count
 * would stop being proportional to time.
 *
 * Nothing else may go into this value -- no trim from the playback ring's
 * level. The host runs this device's OUT and IN streams on one clock, the one
 * the IN packets carry; a feedback that differs from it, even only while
 * refilling the ring after a stall, leaves the two streams that many frames
 * apart for good. macOS then reports "Streams wrapping on different frames"
 * on every buffer cycle and the playback stutters until the stream is
 * reopened (2026-10-18: a level PI loop here, any app start on the board). */
/* The last feedback value sent, samples per frame in 16.16, for UAC_STATS. */
static volatile uint32_t s_fb_q16;
/* Set when the speaker stream opens (macOS) or the host changes: the feedback counts from there. */
static volatile bool s_fb_restart;
/* The last SOF the feedback ISR saw (it runs every frame), for where in its
 * frame a speaker packet's interrupt came (uac_device_spk_timing). */
static volatile uint32_t s_sof_cycles;
static volatile uint32_t s_sof_frame;
/* The longest time between two SOF interrupts (us, reset on read) and the core they run on. */
static volatile uint32_t s_sof_gap_max_us;
static volatile uint32_t s_sof_core;
#define SOF_LATE_SLOTS 8
static struct { const char *name; uint32_t count; } s_sof_late[SOF_LATE_SLOTS];

int uac_device_sof_late(char *out, int len)
{
    int n = 0;
    for (int k = 0; k < SOF_LATE_SLOTS && s_sof_late[k].name && n < len; ++k)
        n += snprintf(out + n, len - n, "%s=%lu ", s_sof_late[k].name, (unsigned long)s_sof_late[k].count);
    return n;
}

/* Which host the feedback is shaped for (uac_device_set_host). The two want
 * opposite things from the same cycle count:
 *
 * Linux takes the last value it polled as it is, so one interval alone -- it
 * carries the SOF interrupt's own entry latency, a single one read +5800 ppm
 * in a test while the I2S ran at 44100.00 Hz -- made it play at 44079 Hz
 * and drain the ring into an underrun every 4-7 s. Over the last kFbWindow
 * calls the two ends carry one latency each and the error shrinks by the
 * window's length (2026-10-23).
 *
 * macOS, fed that steady value, began filling its own stream with silence
 * after 75-270 s -- 2-3 ms gaps every 12 ms that grew to the whole signal
 * (Out 2 on an external audio interface, UAC_FLOW zh, 6 runs of 3-5 min, 2026-10-24). Per interval
 * it played clean: the intervals telescope, their sum is exact, and macOS
 * averages them itself. */
static volatile uint8_t s_host = UAC_HOST_LINUX;

void uac_device_set_host(uac_host_t host)
{
    s_host = (uint8_t)host;
    s_fb_restart = true;
}

uac_host_t uac_device_get_host(void) { return (uac_host_t)s_host; }

TU_ATTR_FAST_FUNC void tud_audio_feedback_interval_isr(uint8_t func_id, uint32_t frame_number, uint8_t interval_shift)
{
    enum { kFbWindow = 128 };
    static uint32_t s_stamp[kFbWindow];
    static uint32_t s_count;
    static uint32_t s_prev_cycles;
    static bool s_have_prev;

    const uint32_t now = esp_cpu_get_cycle_count();
    {
        const uint32_t gap = (now - s_sof_cycles) / 240u;
        if (gap < 100000u && gap > s_sof_gap_max_us) s_sof_gap_max_us = gap;
        if (gap > 1300u && gap < 100000u) {
            /* Who held the core while this SOF waited: the task interrupted. */
            TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(esp_cpu_get_core_id());
            const char *n = t ? pcTaskGetName(t) : "?";
            int k;
            for (k = 0; k < SOF_LATE_SLOTS && s_sof_late[k].name && s_sof_late[k].name != n; ++k) {}
            if (k < SOF_LATE_SLOTS) { s_sof_late[k].name = n; s_sof_late[k].count++; }
        }
        s_sof_core = (uint32_t)esp_cpu_get_core_id();
    }
    s_sof_cycles = now;
    s_sof_frame = frame_number & 0x7FFu;
    /* A stream (re)opened or the host changed: the stamps are from before the gap. */
    if (s_fb_restart) {
        s_fb_restart = false;
        s_have_prev = false;
        s_count = 0;
    }

    uint32_t cycles, n_frames;
    if (s_host == UAC_HOST_LINUX) {
        const uint32_t slot = s_count % kFbWindow;
        const uint32_t then = s_stamp[slot];
        s_stamp[slot] = now;
        const uint32_t have = s_count < kFbWindow ? s_count : kFbWindow;
        s_count++;
        if (have == 0) return;
        /* Until the window fills, the oldest stamp is the first one. */
        cycles = now - (s_count <= kFbWindow ? s_stamp[0] : then);   /* unsigned wrap is intended */
        n_frames = have << interval_shift;
    } else {
        cycles = now - s_prev_cycles;   /* unsigned wrap is intended */
        s_prev_cycles = now;
        if (!s_have_prev) { s_have_prev = true; return; }
        n_frames = 1UL << interval_shift;
    }

    const uint32_t fs = s_uac_device ? s_uac_device->current_sample_rate : DEFAULT_SAMPLE_RATE;
    const uint32_t fcpu = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL;

    uint64_t fb = (((uint64_t)fs * cycles) << 16) / ((uint64_t)fcpu * n_frames);

    /* FMT-2.0 section 2.3.1.1: the packet size must stay within one audio slot
     * of nominal, so a stale counter across a stream restart cannot run away. */
    const uint32_t nominal = (uint32_t)(((uint64_t)fs << 16) / 1000u);
    if (fb > (uint64_t)nominal + (1u << 16)) fb = (uint64_t)nominal + (1u << 16);
    if (fb < (uint64_t)nominal - (1u << 16)) fb = (uint64_t)nominal - (1u << 16);

    s_fb_q16 = (uint32_t)fb;
    tud_audio_n_fb_set(func_id, (uint32_t)fb);
}

uint32_t uac_device_feedback_q16(void) { return s_fb_q16; }

// Helper for feature unit get requests
static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT ||
              request->bEntityID == UAC2_ENTITY_MIC_FEATURE_TERMINAL);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute1 = { .bCur = s_uac_device->mute[request->bChannelNumber] };
        TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
    } else if (UAC2_ENTITY_SPK_FEATURE_UNIT && request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range_vol = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256) }
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
        } else if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(s_uac_device->volume[request->bChannelNumber]) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
        }
    }
    TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT ||
              request->bEntityID == UAC2_ENTITY_MIC_FEATURE_TERMINAL);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
        s_uac_device->mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;
        TU_LOG1("Set speaker channel %d Mute: %d\r\n", request->bChannelNumber, s_uac_device->mute[request->bChannelNumber]);
        if (s_uac_device->user_cfg.set_mute_cb) {
            s_uac_device->user_cfg.set_mute_cb(s_uac_device->mute[request->bChannelNumber], s_uac_device->user_cfg.cb_ctx);
        }
        return true;
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
        s_uac_device->volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
        int volume_db = s_uac_device->volume[request->bChannelNumber] / 256;
        int volume = (volume_db + 50) * 2; // Map to range 0 to 100
        TU_LOG1("Set speaker channel %d volume: %d dB (%d)\r\n", request->bChannelNumber, volume_db, volume);
        if (s_uac_device->user_cfg.set_volume_cb) {
            s_uac_device->user_cfg.set_volume_cb(volume, s_uac_device->user_cfg.cb_ctx);
        }
        return true;
    }

    TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    UAC_CHECK_INIT();
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return tud_audio_clock_get_request(rhport, request);
    }
    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
        return tud_audio_feature_unit_get_request(rhport, request);
    }
    if (request->bEntityID == UAC2_ENTITY_MIC_FEATURE_TERMINAL) {
        return tud_audio_feature_unit_get_request(rhport, request);
    }
    // Input terminal connector status (Windows asks "is mic connected?")
    if (request->bEntityID == UAC2_ENTITY_MIC_INPUT_TERMINAL ||
        request->bEntityID == UAC2_ENTITY_SPK_INPUT_TERMINAL) {
        if (request->bControlSelector == AUDIO_TE_CTRL_CONNECTOR && request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_1_t cur_conn = { .bCur = 1 }; // connected
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_conn, sizeof(cur_conn));
        }
    }

    TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    UAC_CHECK_INIT();
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT ||
        request->bEntityID == UAC2_ENTITY_MIC_FEATURE_TERMINAL) {
        return tud_audio_feature_unit_set_request(rhport, request, buf);
    }
    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return tud_audio_clock_set_request(rhport, request, buf);
    }

    TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    UAC_CHECK_INIT();
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
    if (s_uac_device->spk_itf_num == itf && alt == 0) {
        TU_LOG2("Speaker interface closed");
        s_uac_device->spk_active = false;
    }
#endif

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX
    if (s_uac_device->mic_itf_num == itf && alt == 0) {
        TU_LOG2("Microphone interface closed");
        s_uac_device->mic_data_size = 0;
        s_uac_device->mic_active = false;
        if (s_uac_device->user_cfg.input_open_cb) {
            s_uac_device->user_cfg.input_open_cb(false, s_uac_device->user_cfg.cb_ctx);
        }
    }
#endif

    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    UAC_CHECK_INIT();
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    TU_LOG2("Set interface %d alt %d\r\n", itf, alt);

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
    if (s_uac_device->spk_itf_num == itf && alt != 0) {
        if (!s_uac_device->spk_task_handle) {
            ESP_LOGE(TAG, "Speaker task not running, cannot open interface %d-%d", itf, alt);
            return false;
        }
        s_uac_device->spk_resolution = spk_resolutions_per_format[alt - 1];
        s_uac_device->spk_active = true;
        s_uac_device->spk_bytes_per_ms = s_uac_device->current_sample_rate / 1000 * SPEAK_CHANNEL_NUM * s_uac_device->spk_resolution / 8;
        /* macOS: the nominal rate until tud_audio_feedback_interval_isr() has an
         * interval to measure. Linux keeps the window it was counting. */
        if (s_host == UAC_HOST_MACOS) {
            s_fb_restart = true;
            tud_audio_n_fb_set(0, (uint32_t)(((uint64_t)s_uac_device->current_sample_rate << 16) / 1000u));
        }
        xTaskNotifyGive(s_uac_device->spk_task_handle);
        ESP_LOGI(TAG, "Speaker interface %d-%d opened", itf, alt);
    }
#endif

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX
    if (s_uac_device->mic_itf_num == itf && alt != 0) {
        if (!s_uac_device->mic_task_handle) {
            ESP_LOGE(TAG, "Mic task not running, cannot open interface %d-%d", itf, alt);
            return false;
        }
        s_uac_device->mic_data_size = 0;
        s_uac_device->mic_resolution = mic_resolutions_per_format[alt - 1];
        s_uac_device->mic_active = true;
        s_uac_device->mic_bytes_per_interval = s_uac_device->current_sample_rate * MIC_INTERVAL_MS / 1000 * MIC_CHANNEL_NUM * s_uac_device->mic_resolution / 8;
        s_uac_device->mic_frac = 0;
        if (s_uac_device->user_cfg.input_open_cb) {
            s_uac_device->user_cfg.input_open_cb(true, s_uac_device->user_cfg.cb_ctx);
        }
        xTaskNotifyGive(s_uac_device->mic_task_handle);
        ESP_LOGI(TAG, "Microphone interface %d-%d opened", itf, alt);
    }
#endif

    return true;
}

/* The speaker stream as it arrives: packets, and packets short of the
 * rate's whole frames a millisecond (43 at 44.1 kHz). A host that sends
 * fewer frames does so in one of two ways -- fewer packets or shorter ones --
 * and a packet the board never armed for shows only as the first. */
static volatile uint32_t s_spk_packets;
static volatile uint32_t s_spk_short;
/* Frames with no speaker packet between two that came, two packets handled
 * in one frame, and the latest a packet's interrupt ran into its frame. */
static volatile uint32_t s_spk_skipped;
static volatile uint32_t s_spk_doubled;
static volatile uint32_t s_spk_late_us;
static uint32_t s_spk_last_frame = UINT32_MAX;

void uac_device_spk_packets(uint32_t *packets, uint32_t *short_packets)
{
    if (packets) *packets = s_spk_packets;
    if (short_packets) *short_packets = s_spk_short;
}

void uac_device_spk_timing(uint32_t *skipped, uint32_t *doubled, uint32_t *late_us)
{
    if (skipped) *skipped = s_spk_skipped;
    if (doubled) *doubled = s_spk_doubled;
    if (late_us) { *late_us = s_spk_late_us; s_spk_late_us = 0; }
}

void uac_device_sof_timing(uint32_t *gap_max_us, uint32_t *core)
{
    if (gap_max_us) { *gap_max_us = s_sof_gap_max_us; s_sof_gap_max_us = 0; }
    if (core) *core = s_sof_core;
}

/* DSTS of the S3's DWC2 (0x60080000 + 0x808): SOFFN, the current frame, in bits 8-18. */
static inline uint32_t spk_frame_now(void)
{
    return ((*(volatile uint32_t *)0x60080808u) >> 8) & 0x7FFu;
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    /* The device is read once, before the FIFO count: s_uac_device is not
     * volatile, so after that call the compiler would load it and the task
     * handle again, unchecked -- and uac_device_deinit() may run on the
     * other core meanwhile. */
    uac_device_t *const dev = s_uac_device;
    if (!dev) return false;
    TaskHandle_t const task = dev->spk_task_handle;
    const size_t chunk = dev->spk_bytes_per_ms * SPK_TASK_READ_MS;
    s_spk_packets++;
    if (n_bytes_received < dev->spk_bytes_per_ms) s_spk_short++;
    {
        const uint32_t fn = spk_frame_now();
        if (s_spk_last_frame != UINT32_MAX) {
            const uint32_t d = (fn - s_spk_last_frame) & 0x7FFu;
            if (d == 0) s_spk_doubled++;
            else if (d > 1 && d < 64) s_spk_skipped += d - 1;
        }
        s_spk_last_frame = fn;
        if (fn == s_sof_frame) {
            const uint32_t us = (esp_cpu_get_cycle_count() - s_sof_cycles) / 240u;
            if (us > s_spk_late_us) s_spk_late_us = us;
        }
    }

    /* Wake the speaker task once a read chunk (SPK_TASK_READ_MS of audio:
     * 704 B at 44.1 kHz stereo 16 bit) is in the FIFO, not for every 1 ms
     * packet -- each wake-up costs ~60-80 us for a 176 B copy. The task's
     * own SPK_TASK_READ_MS timeout collects what is short of a chunk. The
     * data waiting here changes nothing the host is told: the feedback is
     * measured in CPU cycles (tud_audio_feedback_interval_isr), not from
     * this FIFO's level. tud_audio_available() is a lock-free FIFO count. */
    if (n_bytes_received > 0 && task && tud_audio_available() >= chunk) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    return true;
}

/* The capture stream as the host gets it: frames handed to TinyUSB, and
 * intervals the mic task replaced before the ISR had written them (lost). The
 * host clocks the speaker stream by this one, so a lost interval reads to it as
 * a slower board. */
static volatile uint32_t s_mic_sent_frames;
static volatile uint32_t s_mic_lost;

void uac_device_mic_flow(uint32_t *sent_frames, uint32_t *lost)
{
    if (sent_frames) *sent_frames = s_mic_sent_frames;
    if (lost) *lost = s_mic_lost;
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting)
{
    UAC_CHECK_INIT();
    (void)rhport;
    (void)n_bytes_sent;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    size_t bytes_require = s_uac_device->mic_bytes_per_interval;

    tu_fifo_t *sw_in_fifo = tud_audio_get_ep_in_ff();
    uint16_t fifo_remained = tu_fifo_remaining(sw_in_fifo);
    if (fifo_remained < bytes_require) {
        return true;
    }

    UAC_ENTER_CRITICAL_ISR();
    if (s_uac_device->mic_data_size > 0) {
        tud_audio_write((void *)s_uac_device->mic_buf_read, s_uac_device->mic_data_size);
        s_mic_sent_frames += s_uac_device->mic_data_size / (MIC_CHANNEL_NUM * s_uac_device->mic_resolution / 8);
        s_uac_device->mic_data_size = 0;
    }
    UAC_EXIT_CRITICAL_ISR();

    return true;
}

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
static volatile uint32_t s_spk_gap_max_us;
static volatile uint32_t s_spk_fifo_max;

void uac_device_spk_health(uint32_t *gap_max_us, uint32_t *fifo_max, uint32_t *fifo_size)
{
    if (gap_max_us) *gap_max_us = s_spk_gap_max_us;
    if (fifo_max) *fifo_max = s_spk_fifo_max;
    if (fifo_size) *fifo_size = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ;
    s_spk_gap_max_us = 0;
    s_spk_fifo_max = 0;
}

static void usb_spk_task(void *pvParam)
{
    static uint8_t read_buf[SPK_TASK_READ_SZ];
    int64_t last_us = 0;

    while (1) {
        if (!s_uac_device->spk_active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            last_us = 0;
            continue;
        }

        /* Woken by tud_audio_rx_done_isr() once a chunk is in the FIFO, or
         * after SPK_TASK_READ_MS to collect what is short of one. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SPK_TASK_READ_MS));

        /* A gap well past the SPK_TASK_READ_MS timeout is this task kept off
         * the CPU: the host keeps sending, the FIFO fills and the ring drains. */
        const int64_t now_us = esp_timer_get_time();
        if (last_us && now_us - last_us > (int64_t)s_spk_gap_max_us) s_spk_gap_max_us = (uint32_t)(now_us - last_us);
        last_us = now_us;

        /* Up to a chunk at a time straight from TinyUSB's ep_out_ff, and
         * again while more is waiting: a pass that came late finds two chunks
         * there, and leaving the second for the next wake-up let the
         * consumer's ring run dry with the audio sitting in the FIFO. */
        for (int chunk = 0; chunk < 4; ++chunk) {
            int avail = tud_audio_available();
            if (chunk == 0 && avail > (int)s_spk_fifo_max) s_spk_fifo_max = (uint32_t)avail;
            if (avail <= 0) break;
            if (avail > (int)sizeof(read_buf)) avail = (int)sizeof(read_buf);
            /* Take only what the consumer can hold; the rest waits in the
             * FIFO for the next pass instead of being dropped. The host's rate
             * is not steered from here: the feedback endpoint reports the
             * board's clock counted in CPU cycles (tud_audio_feedback_interval_isr),
             * whatever the FIFO holds. */
            if (s_uac_device->user_cfg.output_space_cb) {
                size_t space = s_uac_device->user_cfg.output_space_cb(s_uac_device->user_cfg.cb_ctx);
                space -= space % (SPEAK_CHANNEL_NUM * 2);
                if ((size_t)avail > space) avail = (int)space;
                if (avail <= 0) break;
            }

            int nread = tud_audio_read(read_buf, avail);
            if (nread <= 0) break;
            if (s_uac_device->user_cfg.output_cb) {
                s_uac_device->user_cfg.output_cb(read_buf, nread, s_uac_device->user_cfg.cb_ctx);
            }
        }
    }
}
#endif

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX
static void usb_mic_task(void *pvParam)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (1) {
        if (s_uac_device->mic_active == false) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            xLastWakeTime = xTaskGetTickCount();
            continue;
        }
        /* Frames for this interval, carrying the remainder. This interval sets
         * both the task granularity and, through EP_IN_SW_BUF_SZ, the endpoint
         * buffer, so it is the knob for capture latency: the two together were
         * 21 ms of the offset a DAW records. Below 10 ms the frame count turns
         * fractional at 44.1 kHz (5 ms is 220.5) and truncating would run the
         * stream slow; carrying the thousandths keeps it exact. */
        const size_t frame_bytes = MIC_CHANNEL_NUM * s_uac_device->mic_resolution / 8;
        const uint32_t thousandths = s_uac_device->current_sample_rate * MIC_INTERVAL_MS;
        uint32_t frames = thousandths / 1000;
        s_uac_device->mic_frac += thousandths % 1000;
        if (s_uac_device->mic_frac >= 1000) {
            frames++;
            s_uac_device->mic_frac -= 1000;
        }
        size_t bytes_require = frames * frame_bytes;
        if (s_uac_device->user_cfg.input_cb) {
            size_t bytes_read = 0;
            esp_err_t ret = s_uac_device->user_cfg.input_cb((uint8_t *)s_uac_device->mic_buf_write, bytes_require, &bytes_read, s_uac_device->user_cfg.cb_ctx);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read data from mic");
                continue;
            }
            int16_t *tmp_buf = s_uac_device->mic_buf_write;
            UAC_ENTER_CRITICAL();
            if (s_uac_device->mic_data_size > 0) s_mic_lost++;
            s_uac_device->mic_buf_write = s_uac_device->mic_buf_read;
            s_uac_device->mic_buf_read = tmp_buf;
            s_uac_device->mic_data_size = bytes_read;
            UAC_EXIT_CRITICAL();
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(MIC_INTERVAL_MS));
    }
}
#endif

esp_err_t uac_device_init(uac_device_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    if (s_uac_device != NULL) {
        ESP_LOGW(TAG, "uac device already initialized");
        return ESP_OK;
    }

    s_uac_device = calloc(1, sizeof(uac_device_t));
    ESP_RETURN_ON_FALSE(s_uac_device != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for uac device");

    s_uac_device->user_cfg.output_cb = config->output_cb;
    s_uac_device->user_cfg.input_cb = config->input_cb;
    s_uac_device->user_cfg.output_space_cb = config->output_space_cb;
    s_uac_device->user_cfg.cb_ctx = config->cb_ctx;
    s_uac_device->user_cfg.set_mute_cb = config->set_mute_cb;
    s_uac_device->user_cfg.set_volume_cb = config->set_volume_cb;
    s_uac_device->user_cfg.input_open_cb = config->input_open_cb;
    s_uac_device->current_sample_rate = (config->sample_rate && config->sample_rate <= DEFAULT_SAMPLE_RATE)
                                        ? config->sample_rate : DEFAULT_SAMPLE_RATE;
    s_uac_device->mic_buf_write = s_uac_device->mic_buf1;
    s_uac_device->mic_buf_read = s_uac_device->mic_buf2;

    // In AS_PART mode, interface numbers come from config
    s_uac_device->spk_itf_num = config->spk_itf_num;
    s_uac_device->mic_itf_num = config->mic_itf_num;

    // TinyUSB is managed externally — skip PHY and task init

    /* No intermediate StreamBuffer — spk_task reads directly from TinyUSB's
     * ep_out_ff FIFO via tud_audio_read(). */

    BaseType_t ret_val;

    /* Both tasks are copy loops between TinyUSB's FIFOs and the I2S bridge —
     * no flash access, no ISR context — so their stacks live in PSRAM. On a
     * board where BLE and the audio engine have already taken the internal
     * heap, two 4 kB internal stacks are exactly what makes entering USB audio
     * mode fail, and nothing here needs them to be internal. */
#define UAC_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX
    ret_val = xTaskCreatePinnedToCoreWithCaps(usb_mic_task, "usb_mic_task", 4096, NULL, CONFIG_UAC_MIC_TASK_PRIORITY,
                                      &s_uac_device->mic_task_handle, CONFIG_UAC_MIC_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_MIC_TASK_CORE,
                                      UAC_TASK_STACK_CAPS);
    if (ret_val != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_mic task");
        free(s_uac_device);
        s_uac_device = NULL;
        return ESP_FAIL;
    }
#endif

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
    ret_val = xTaskCreatePinnedToCoreWithCaps(usb_spk_task, "usb_spk_task", 4096, NULL, CONFIG_UAC_SPK_TASK_PRIORITY,
                                      &s_uac_device->spk_task_handle, CONFIG_UAC_SPK_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_SPK_TASK_CORE,
                                      UAC_TASK_STACK_CAPS);
    if (ret_val != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_spk task");
#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX
        vTaskDeleteWithCaps(s_uac_device->mic_task_handle);
#endif
        free(s_uac_device);
        s_uac_device = NULL;
        return ESP_FAIL;
    }
#endif

    ESP_LOGI(TAG, "UAC Device initialized (AS_PART mode), sample rate: %"PRIu32" Hz, spk_itf: %d, mic_itf: %d",
             s_uac_device->current_sample_rate, s_uac_device->spk_itf_num, s_uac_device->mic_itf_num);
    return ESP_OK;
}

void uac_device_deinit(void)
{
    if (s_uac_device == NULL) {
        return;
    }

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX
    if (s_uac_device->mic_task_handle) {
        vTaskDeleteWithCaps(s_uac_device->mic_task_handle);
        s_uac_device->mic_task_handle = NULL;
    }
#endif

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
    if (s_uac_device->spk_task_handle) {
        vTaskDeleteWithCaps(s_uac_device->spk_task_handle);
        s_uac_device->spk_task_handle = NULL;
    }
#endif

    free(s_uac_device);
    s_uac_device = NULL;
    ESP_LOGI(TAG, "UAC Device deinitialized");
}
