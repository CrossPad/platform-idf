#include "usb_audio_bridge.h"
#include "audio/audio_task.h"
#include "usb_audio_port.h"
#include "i2s_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "usb_audio_bridge";


/* Rate matching is the UAC feedback endpoint: it reports this board's sample
 * clock counted in CPU cycles (usb_device_uac.c,
 * tud_audio_feedback_interval_isr), so the host sends what the I2S clock
 * consumes and that clock stays at its PLL-derived rate, untuned. */

typedef struct {
    bool initialized;
    bool spk_logged;           /* a 'SPK #' line went out this session */
    volatile uint32_t spk_cb_count;         /* speaker callbacks this session: the 'SPK #' number */
    volatile uint16_t spk_len, spk_taken;   /* the latest callback's */
    TickType_t spk_log_tick;   /* when the last 'SPK #' line went out */
    uint32_t spk_log_events;   /* the event counters' sum as that line printed them */
    volatile size_t spk_ring_min;       /* lowest playback ring level before a write since then */
    uint32_t spk_last_us;      /* when the speaker task last handed audio over */
    volatile uint32_t spk_gap_max_us;   /* longest wait between two of those since then */
} usb_audio_bridge_t;

static usb_audio_bridge_t s_bridge = {0};

esp_err_t usb_audio_bridge_set_capture_codec(int codec)
{
    if (codec != 0 && codec != 1 && codec != USB_AUDIO_CAPTURE_MIX &&
        codec != USB_AUDIO_CAPTURE_SAMPLER) {
        return ESP_ERR_INVALID_ARG;
    }
    usb_audio_port_set_capture_source(codec);
    ESP_LOGI(TAG, "USB mic capture source -> codec %d", codec);
    return ESP_OK;
}

int usb_audio_bridge_get_capture_codec(void)
{
    return usb_audio_port_get_capture_source();
}


esp_err_t usb_audio_bridge_init(void)
{
    if (s_bridge.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Nothing to reconfigure: the I2S channels and both codecs already run
     * 16-bit Philips stereo at MCLK x256 (i2s_driver.c, bsp_es8388.c
     * es8388_config_dac/adc), the format the UAC descriptors advertise, at
     * bsp_i2s_get_sample_rate(). So a profile switch leaves them alone --
     * the render keeps both DACs fed through it, and no channel is disabled
     * under a DMA that may not stop (TODO.md, the hang leaving audio). */
    for (int i = 0; i < BSP_I2S_INTERFACE_MAX; i++) {
        if (!bsp_i2s_get_tx_handle((bsp_i2s_interface_t)i) ||
            !bsp_i2s_get_rx_handle((bsp_i2s_interface_t)i)) {
            ESP_LOGE(TAG, "I2S%d not initialized", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Rings sized for the engine rate the UAC descriptors advertise; the
     * render loop reads the playback side and feeds the capture side. */
    const uint32_t block = audio_rt_block_frames();
    usb_audio_port_start(bsp_i2s_get_sample_rate(), block ? block : 64);
    s_bridge.spk_ring_min = SIZE_MAX;
    s_bridge.initialized = true;
    ESP_LOGI(TAG, "Initialized: 2ch 16-bit USB <-> mixer rings (feedback EP rate matching)");
    return ESP_OK;
}

esp_err_t usb_audio_bridge_deinit(void)
{
    if (s_bridge.initialized) {
        usb_audio_port_stop();
    }
    memset(&s_bridge, 0, sizeof(s_bridge));
    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

/* UAC speaker task: hand the host's audio to the render-side ring. The task
 * reads no more than usb_audio_output_space_cb() allows, so the ring takes
 * it all; refused= counts what it did not. No I2S here: the RT task is the
 * only writer of both TX channels. Nothing here may wait: the task runs above
 * the streamer (CONFIG_UAC_SPK_TASK_PRIORITY), so its line is printed by the
 * worker (usb_audio_bridge_report). */

esp_err_t usb_audio_output_cb(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;

    if (!s_bridge.initialized || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t frames = len / (2 * sizeof(int16_t));
    s_bridge.spk_taken = (uint16_t)usb_audio_port_playback_write((const int16_t *)buf, frames);
    s_bridge.spk_len = (uint16_t)len;
    /* How late the task got. An underrun with ring_min well above zero and a
     * gap here longer than ring_min's worth is the task held off. Half a
     * second and more is the host pausing the stream, not a stall. */
    const uint32_t us = (uint32_t)esp_timer_get_time();
    const uint32_t gap = us - s_bridge.spk_last_us;
    if (s_bridge.spk_cb_count++ && gap < 500000 && gap > s_bridge.spk_gap_max_us) {
        s_bridge.spk_gap_max_us = gap;
    }
    s_bridge.spk_last_us = us;
    return ESP_OK;
}

/* Worker, once a second (main.cpp pad_anim_job), never the UAC tasks: UART0
 * has no driver, so a line is a 4-18 ms busy wait in whoever prints it.
 * A line once the first callback of a session is in, then every 10 s, and
 * when an event counter has moved since the last one -- at most one a
 * second, the totals carrying whatever came in between: the bench reads
 * under=0 refused=0 cap_drop=0 cap_under=0 off these lines, so an event late
 * in a session must not wait out the 10 s. All four only grow within a
 * session, so their sum moves whenever one does; cap_drop and cap_under
 * count only while the host has the mic open (UsbAudioPort.cpp). ring_min is
 * the lowest the playback ring stood before a write since the last line,
 * spk_gap_max_us the speaker task's longest wait between two hand-overs;
 * rt_busy_max_us the longest render pass and idle1_pct core 1's idle share
 * since the last line -- this profile has no CDC to ask. */
void usb_audio_bridge_report(void)
{
    const uint32_t cb = s_bridge.spk_cb_count;
    if (!s_bridge.initialized || cb == 0) return;
    uint32_t under, refused, capdrop; size_t plevel, clevel;
    usb_audio_port_stats(&under, &refused, &capdrop, &plevel, &clevel);
    const uint32_t capunder = usb_audio_port_capture_underruns();
    const uint32_t events = under + refused + capdrop + capunder;
    const TickType_t now = xTaskGetTickCount();
    if (s_bridge.spk_logged && events == s_bridge.spk_log_events &&
        now - s_bridge.spk_log_tick < pdMS_TO_TICKS(10000)) {
        return;
    }
    s_bridge.spk_logged = true;
    s_bridge.spk_log_tick = now;
    s_bridge.spk_log_events = events;
    uint32_t idle1 = 0;
    const uint32_t busy = audio_rt_busy_max(AUDIO_RT_BUSY_SPK_LINE, &idle1);
    const size_t ring_min = s_bridge.spk_ring_min;
    s_bridge.spk_ring_min = SIZE_MAX;
    const uint32_t gap = s_bridge.spk_gap_max_us;
    s_bridge.spk_gap_max_us = 0;
    ESP_LOGI(TAG, "SPK #%lu: len=%u taken=%u ring=%u under=%lu refused=%lu cap_ring=%u cap_drop=%lu ring_min=%u"
             " rt_busy_max_us=%lu idle1_pct=%lu spk_gap_max_us=%lu cap_under=%lu",
             (unsigned long)cb, (unsigned)s_bridge.spk_len, (unsigned)s_bridge.spk_taken, (unsigned)plevel,
             (unsigned long)under, (unsigned long)refused, (unsigned)clevel,
             (unsigned long)capdrop, (unsigned)ring_min,
             (unsigned long)busy, (unsigned long)idle1, (unsigned long)gap, (unsigned long)capunder);
}

/* TinyUSB task, on the mic interface's SET_INTERFACE. */
void usb_audio_input_open_cb(bool open, void *ctx)
{
    (void)ctx;
    if (s_bridge.initialized) usb_audio_port_capture_open(open);
}

/* How much the UAC speaker task may take from TinyUSB's FIFO right now, in
 * bytes: what brings the playback ring up to half full. The remainder waits
 * in the FIFO for the next pass. Frames must never be thrown away here — a
 * dropped frame is a pitch error the host cannot see. The task asks just
 * before each read, so the level seen here is the ring's low point between
 * two writes: the ring_min of 'SPK #'. */
size_t usb_audio_output_space_cb(void *ctx)
{
    (void)ctx;
    if (!s_bridge.initialized) return 0;
    /* Half full, not full: a ring held full would be all of it as fixed
     * latency. */
    const size_t space = usb_audio_port_playback_space();
    const size_t level = usb_audio_port_playback_capacity() - space;
    if (level < s_bridge.spk_ring_min) s_bridge.spk_ring_min = level;
    const size_t target = usb_audio_port_playback_capacity() / 2;
    const size_t want = level < target ? target - level : 0;
    return (want < space ? want : space) * 2 * sizeof(int16_t);
}

/* UAC mic task: take one interval from the board→host ring. What the ring
 * cannot give yet (a stream just opened or re-primed) is zero-filled, never
 * waited for: the mic task's own pacing is the USB interval. */
esp_err_t usb_audio_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    (void)ctx;

    if (!s_bridge.initialized || len == 0) {
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }
    const size_t frames = len / (2 * sizeof(int16_t));
    const size_t got = usb_audio_port_capture_read((int16_t *)buf, frames);
    if (got < frames) {
        memset(buf + got * 2 * sizeof(int16_t), 0, (frames - got) * 2 * sizeof(int16_t));
    }
    *bytes_read = frames * 2 * sizeof(int16_t);
    return ESP_OK;
}
