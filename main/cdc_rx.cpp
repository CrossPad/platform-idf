/* The CDC port's receive side: bytes from the TinyUSB task into the command
 * queue, and lines out of it to whoever handles them. */
#include "sd_sector_io.h"
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include <algorithm>
#include <iterator>
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "soc/rtc_cntl_reg.h"
#include "soc/system_reg.h"
#endif
#include "nvs_flash.h"
#include "nvs.h"
#include "imu_midi.h"
#include "NvsSettingsStore.hpp"
#include "OutputLevelsPort.hpp"
#include "audio/audio_task.h"
#include "freertos/idf_additions.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "bsp_worker.h"
#include "bsp_ulp.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "include/usb_descriptors.h"
#include "include/usb_config_manager.h"
#include "include/audio_route_control.h"
#include "include/hil_control.h"
#include "BleCompanion.hpp"
#include "companion_sysex.h"
#include "host_info.h"
#include "usb_host_profile.h"
#include "include/fw_updater.h"
#include "include/cp_account.h"
#include "include/cp_remote.h"
#include "instrument_host.h"
#include "wifi_creds.h"
#include "wifi_link.h"
#include "include/crosspad_deps.h"
#include "include/reset_log.h"
#include "include/power_button.h"
#include "include/lv_async_safe.h"
#include "include/file_transfer.h"
#include "include/usb_transfer.h"
#include "include/net_mode.h"
#include "include/fw_slots.h"
#include "include/fw_assets_sync.h"
#include "bsp/esp-bsp.h"
#include "bsp_sdmmc.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "espnow.h"
#include "espnow_utils.h"
#endif
#include <MIDI.h>
#include "EspTinyUsbMidiTransport.h"
#include "esp_vfs_fat.h"
#include "esp_littlefs.h"
#include "stm32_fw_update.h"
#include "stm_fw_overlay.h"
#include "CrossPad.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "CrosspadPlatform.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"
#include "stm32_pwr_sync.h"   // STM32 power-state IRQ callback (set_callback / ack)
#include "stm32_i2c.h"        // Early wake qualification reads
#include "stm32_reg_map.h"
#include <atomic>    // PWR_STATE_* constants
#include "stm32_hal_init.h"   // charger telemetry (SoC gauge for the status bar)
#include "crosspad/kit/KitFolders.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad-sampler/logic/kit_gate.hpp"
#ifdef USE_LVGL
#include "crosspad-gui/components/ui_toast.h"
#endif
#include "processMIDI.hpp"
#include "ota_cdc.h"
#include "cdc_line.h"
#include "charge_overlay.h"   // charge-while-off pad gauge + "Charging" screen
#include "app/settings/settings_app.h"
#include "sample_player_port.hpp"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "include/usb_audio_bridge.h"
#endif
#ifdef BSP_BOARD_CROSSPAD_S3
#include "audio_task.h"
#include "performance_monitor.h"
#include "midi_clock.h"
#include "motion_stream.h"
#endif
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#else
#include "engine_arena.hpp"
#include "SamplerEnginePort.hpp"
#endif
#include "include/cdc_rx.h"
#include "include/boot_power.h"

static const char *TAG = "crosspad_main";

static uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

QueueHandle_t app_queue;

/* CDC transport counters, reported as CDC_STATS. */
std::atomic<uint32_t> g_cdc_rx_chunks{0};
std::atomic<uint32_t> g_cdc_commands{0};
std::atomic<uint32_t> g_cdc_dropped{0};
/* Where a late reply was spent: inside one handler, or waiting for the loop to
 * get back to the queue. Both are read-and-cleared by CDC_STATS so a caller can
 * bracket a window. */
std::atomic<uint32_t> g_cdc_max_cmd_us{0};
std::atomic<uint32_t> g_cdc_max_gap_us{0};
char g_cdc_max_cmd_verb[20] = "-";
std::atomic<uint32_t> g_cdc_last_stack_free{0};

extern "C" void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret == ESP_OK) {

        app_message_t tx_msg;   /* buf filled by the memcpy below */
        tx_msg.buf_len = rx_size;
        tx_msg.itf = (uint8_t)itf;

        memcpy(tx_msg.buf, rx_buf, rx_size);
        const bool binaryStream = ota_is_active() || fx_stream_active();
        if (xQueueSend(app_queue, &tx_msg, binaryStream ? portMAX_DELAY : 0) == pdTRUE) {
            g_cdc_rx_chunks.fetch_add(1, std::memory_order_relaxed);
        } else {
            /* A drop here is silent and looks like a refused command, so it
             * is counted; the main loop reports it. */
            g_cdc_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        ESP_LOGE(TAG, "Read Error");
    }
}

extern "C" void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "CDC Line state changed %d: DTR:%d, RTS:%d", itf, dtr, rts);
}

void check_usb_bootloader_request(const char* buff, size_t len) {
    const char* bootloader_request = "BOOTLOADER_REQUEST";
    if (len < strlen(bootloader_request)) {
        return;
    }
    if (memcmp(buff, bootloader_request, strlen(bootloader_request)) == 0) {
        ESP_LOGW(TAG, "Entering bootloader from CDC");
        enter_bootloader();
    }
}

/* Run the CDC handler chain over one complete command. */
static void cdc_dispatch_one(const char *line, size_t len, uint8_t itf)
{
    if (len == 0) return;
    g_cdc_commands.fetch_add(1, std::memory_order_relaxed);
    hil_stack_mark();
    const int64_t t0Us = esp_timer_get_time();

    bool handled = ota_check_begin(line, len);
    if (!handled) handled = ota_check_delta_begin(line, len);
    if (!handled) handled = fx_handle_cdc(line, len);
    if (!handled) handled = audio_route_control_handle_cdc(line, len);
    if (!handled) handled = hil_control_handle_cdc(line, len);
    if (!handled) {
        check_usb_bootloader_request(line, len);
        check_usb_mode_command(line, len);
        /* The echo is the host's "unknown verb" signal, so it is a line of
         * its own: since the framing above drops the terminator, an echo
         * without one glued itself to the next reply ("KIT2" + "OK" read
         * "KIT2OK"), and both the unknown verb and the next one timed out. */
        tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf,
                                   (const uint8_t *)line, len);
        tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf,
                                   (const uint8_t *)"\r\n", 2);
        esp_err_t err = tinyusb_cdcacm_write_flush((tinyusb_cdcacm_itf_t)itf, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CDC ACM write flush error: %s", esp_err_to_name(err));
        }
    }

    g_cdc_last_stack_free.store(hil_stack_note(line, len), std::memory_order_relaxed);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0Us);
    if (us > g_cdc_max_cmd_us.load(std::memory_order_relaxed)) {
        g_cdc_max_cmd_us.store(us, std::memory_order_relaxed);
        size_t n = len < sizeof(g_cdc_max_cmd_verb) - 1 ? len : sizeof(g_cdc_max_cmd_verb) - 1;
        memcpy(g_cdc_max_cmd_verb, line, n);
        g_cdc_max_cmd_verb[n] = '\0';
    }
}

/* Split one CDC read into commands and dispatch each.
 *
 * OTA must not be framed: after OTA_BEGIN the stream is firmware bytes, where
 * 0x0A is data. An unterminated tail is held back only when the read filled
 * the buffer; a short read means the sender's write ended there. */
/* USB full-speed bulk max packet: the boundary the host's writes are cut on. */

void cdc_dispatch_lines(const uint8_t *buf, size_t len, uint8_t itf)
{
    static CdcLineAssembler s_line;

    /* A command ends at its terminator and nowhere else. A short USB packet
     * used to count as the end of a line too, and that guess was wrong twice:
     * a 65-byte command arrived as 64 + 1 and was dispatched truncated, and
     * Web Serial hands the kernel a 46-byte write in two pieces (36 + 10) —
     * the first piece ran as a command with a cut-off path, the second came
     * back as the echo of an unknown one. Every host terminates with LF, so
     * a partial line simply waits in s_line for the rest. */
    for (size_t i = 0; i < len; i++) {
        if (ota_is_active()) {
            /* OTA_BEGIN was accepted earlier in this very buffer. */
            ota_process_data((uint8_t *)&buf[i], len - i);
            s_line.reset();
            return;
        }
        if (fx_stream_active()) {
            /* Same exemption, same reason: after ASSETS_BEGIN or FS_PUT the
             * stream is payload, where 0x0A is data. It ends when the byte
             * count announced in the header has arrived. */
            fx_process_data(&buf[i], len - i);
            s_line.reset();
            return;
        }

        switch (s_line.feed((char)buf[i])) {
        case CdcLineAssembler::Result::Line:
            cdc_dispatch_one(s_line.text(), s_line.size(), itf);
            break;
        case CdcLineAssembler::Result::TooLong: {
            /* Longer than any command this firmware has: the whole line is
             * dropped, not dispatched truncated, and answered once. */
            ESP_LOGW(TAG, "CDC: command over %d bytes discarded", CDC_LINE_MAX - 1);
            static const char kTooLong[] = "ERR too_long\r\n";
            tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf,
                                       (const uint8_t *)kTooLong, sizeof(kTooLong) - 1);
            tinyusb_cdcacm_write_flush((tinyusb_cdcacm_itf_t)itf, 0);
            break;
        }
        case CdcLineAssembler::Result::None:
            break;
        }
    }
}
