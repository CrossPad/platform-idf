#include "usb_config_manager.h"
#include "usb_descriptors.h"
#include "usb_descriptors_transfer.h"
#include "usb_transfer.h"
#include "bsp/esp-bsp.h"
#ifdef BSP_BOARD_CROSSPAD_S3
#include "usb_descriptors_audio.h"
#include "usb_device_uac.h"
#include "usb_audio_bridge.h"
#include "i2s_driver.h"
#endif

#include "audio/audio_task.h"
#include "IdfI2sInput.hpp"
#include "usb_audio_port.h"
#include "NvsSettingsStore.hpp"
#include <crosspad/settings/CrosspadSettings.hpp>
#include "EspTinyUsbMidiTransport.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <string.h>

static const char *TAG = "usb_cfg_mgr";

extern std::atomic<uint32_t> g_rt_gap_max_us;   /* audio_task.cpp */

static usb_mode_t s_current_mode = USB_MODE_DEFAULT;
static bool s_initialized = false;

// Forward declaration — CDC callbacks are defined in main.cpp
extern "C" void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event);
extern "C" void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event);

// Audio mode interface numbers (must match usb_descriptors_audio.c)
#define UAC_SPK_ITF_NUM  3  // ITF_NUM_AUDIO_STREAMING_SPK
#define UAC_MIC_ITF_NUM  4  // ITF_NUM_AUDIO_STREAMING_MIC

/* The profile is a persisted preference, and every way of changing it has to
 * write it: the Settings dropdown saves for itself, but the CDC command and
 * the SysEx one did not, so a host that switched a board to CDC lost it at the
 * next reboot — with USB Audio the default, that put the board back somewhere
 * the host tools cannot reach. Written only on a real change, so the boot-time
 * re-apply is not an NVS write per power cycle. Only a switch that succeeded
 * is written: the recovery fallback below is not a choice anyone made, and
 * persisting it turned one failed audio start into a board that never tried
 * audio again. */
static void persist_mode(usb_mode_t mode)
{
    auto* settings = crosspad::CrosspadSettings::getInstance();
    if (!settings) return;
    const bool audio = (mode == USB_MODE_AUDIO);
    if (settings->usbAudioEnabled == audio) return;
    settings->usbAudioEnabled = audio;
    /* The one key, as CrosspadSettings::saveTo() names it -- not saveTo()
     * itself, which rewrites every setting (~140 open/set/commit) for ~100 ms
     * in the task that parses the pads. */
    NvsSettingsStore::getInstance().saveBool("cfg_audio", "usb_audio", audio);
}

static esp_err_t install_cdc_acm(void)
{
    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };

    esp_err_t ret = tusb_cdc_acm_init(&acm_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tinyusb_cdcacm_register_callback(
        TINYUSB_CDC_ACM_0,
        CDC_EVENT_LINE_STATE_CHANGED,
        &tinyusb_cdc_line_state_changed_callback);

    return ret;
}

static esp_err_t install_tinyusb_default(void)
{
    s_str_desc[3] = usb_serial_string();
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device,
        .string_descriptor = s_str_desc,
        .string_descriptor_count = 5,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = desc_fs_configuration,
#endif
        .self_powered = false,
        .vbus_monitor_io = -1,
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    return install_cdc_acm();
}

/* MSC + CDC. The card has to be up before the driver install: the MSC
 * callbacks are answered from the very first enumeration, and every one of
 * them needs the card handle. */
static esp_err_t install_tinyusb_transfer(void)
{
    esp_err_t ret = usb_transfer_storage_attach();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No SD card to expose (%s); installing the CDC profile",
                 esp_err_to_name(ret));
        return ret;
    }

    s_str_desc_transfer[3] = usb_serial_string();
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device_transfer,
        .string_descriptor = s_str_desc_transfer,
        .string_descriptor_count = USB_TRANSFER_STR_DESC_COUNT,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = desc_cfg_transfer,
#endif
        .self_powered = false,
        .vbus_monitor_io = -1,
    };

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    return install_cdc_acm();
}

#ifdef BSP_BOARD_CROSSPAD_S3
static esp_err_t install_tinyusb_audio(void)
{
    s_str_desc_audio[3] = usb_serial_string();
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device_audio,
        .string_descriptor = s_str_desc_audio,
        .string_descriptor_count = 8,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = desc_cfg_audio,
#endif
        .self_powered = false,
        .vbus_monitor_io = -1,
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // No CDC in audio mode — MIDI SysEx replaces it for data transfer

    // The render reads RX for the capture tap from its first pass with the
    // port active; a line-in node reading the same channel would take every
    // other block. Suppressed first, and the port started only once the pass
    // in flight is over: it read line-in before the flag, and finding the
    // port active at its end it would read the codec again and wait a block
    // for it (a 2-block rt_gap, and with no RX block queued the render then
    // paced by RX, one TX block short). A pass is one block (1.45 ms at 64
    // frames, 5.8 ms at 256); sleeping two blocks and a tick covers it.
    crosspad::IdfI2sInput::setSuppressed(true);
    {
        const uint32_t block = audio_rt_block_frames();
        const uint32_t rate = bsp_i2s_get_sample_rate();
        const uint32_t passMs = (block && rate) ? (block * 1000 + rate - 1) / rate : 2;
        vTaskDelay(pdMS_TO_TICKS(2 * passMs + 1));
    }
    ret = usb_audio_bridge_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB audio bridge init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize UAC2 device in AS_PART mode (TinyUSB already running)
    uac_device_config_t uac_cfg = {
        .skip_tinyusb_init = true,
        .output_cb = usb_audio_output_cb,
        .input_cb = usb_audio_input_cb,
        .output_space_cb = usb_audio_output_space_cb,
        .set_mute_cb = NULL,
        .set_volume_cb = NULL,
        .input_open_cb = usb_audio_input_open_cb,
        .cb_ctx = NULL,
        .spk_itf_num = UAC_SPK_ITF_NUM,
        .mic_itf_num = UAC_MIC_ITF_NUM,
        .sample_rate = bsp_i2s_get_sample_rate(),
    };

    ret = uac_device_init(&uac_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UAC device init failed: %s", esp_err_to_name(ret));
        usb_audio_bridge_deinit();
        return ret;
    }

    return ESP_OK;
}
#endif // BSP_BOARD_CROSSPAD_S3

esp_err_t usb_config_manager_init(usb_mode_t mode)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "USB config manager already initialized");
        return ESP_OK;
    }

    /* esp_tinyusb prints a ~2 kB descriptor table at every driver install:
     * on UART0, which has no driver, ~150 ms of busy wait in the task doing
     * a profile switch -- the one that also parses the pads and USB MIDI --
     * with the console lock held. Its warnings still come through. */
    esp_log_level_set("tusb_desc", ESP_LOG_WARN);

    esp_err_t ret = ESP_OK;
    if (mode == USB_MODE_TRANSFER) {
        ESP_LOGI(TAG, "Initializing USB in transfer mode (MSC + CDC)");
        ret = install_tinyusb_transfer();
        /* A card that did not come up leaves nothing to expose; the transfer
         * screen says so and the plain CDC profile keeps the board reachable. */
        if (ret != ESP_OK) mode = USB_MODE_DEFAULT;
    }
    if (mode != USB_MODE_TRANSFER) {
        ESP_LOGI(TAG, "Initializing USB in default mode (CDC + MIDI)");
        ret = install_tinyusb_default();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    s_current_mode = mode;
    s_initialized = true;
    ESP_LOGI(TAG, "USB config manager initialized, mode: %s", usb_config_manager_get_mode_name(s_current_mode));
    return ESP_OK;
}

esp_err_t usb_config_manager_set_mode(usb_mode_t mode)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "USB config manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (mode >= USB_MODE_COUNT) {
        ESP_LOGE(TAG, "Invalid USB mode: %d", mode);
        return ESP_ERR_INVALID_ARG;
    }

    /* Transfer mode is entered and left by rebooting (usb_transfer.h): the
     * card is exposed raw, and nothing may re-mount it under a host. */
    if (mode == USB_MODE_TRANSFER || s_current_mode == USB_MODE_TRANSFER) {
        ESP_LOGE(TAG, "Transfer mode is a boot mode; use usb_transfer_request/leave");
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == s_current_mode) {
        ESP_LOGI(TAG, "Already in mode: %s", usb_config_manager_get_mode_name(mode));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Switching USB mode: %s -> %s (USB will disconnect!)",
             usb_config_manager_get_mode_name(s_current_mode),
             usb_config_manager_get_mode_name(mode));

    // Nothing may write MIDI from here until the new stack is up. tud_deinit()
    // destroys the MIDI class driver's endpoint mutex before it clears
    // cfg_num, so tud_midi_mounted() still says yes while a write would assert
    // inside osal_mutex_lock — and a pad hit landing in that window (over the
    // STM32 UART, which keeps running throughout) took the board down.
    midi::EspTinyUsbMidiTransport::setTxBlocked(true);
    // Nor any write already past that flag. With a host that does not read
    // MIDI IN, sendMidiPacket() retries a full FIFO after vTaskDelay(1), up to
    // three times, without looking at the flag again, and a retry landing
    // after tud_deinit() is that same assert. The render used to be parked
    // here for 10 ms, which covered it; it runs on now, so the wait stays.
    vTaskDelay(pdMS_TO_TICKS(10));

#ifdef BSP_BOARD_CROSSPAD_S3
    // The mixer keeps running through the switch: host playback enters it as
    // the "USB" channel and the capture tap feeds the host from the render
    // thread, so there is one I2S writer and one I2S reader on the board, in
    // either profile, and the bridge does not touch the I2S channels. What is
    // sounding, the sequencer and the line-in monitor play on while USB
    // re-enumerates (a pad hit from the STM waits for this task, which parses
    // them). The line-in nodes are suppressed exactly while the port
    // is active (the capture tap is then the sole RX reader): from just before
    // the bridge starts it (install_tinyusb_audio) to just after it stops it.
    if (s_current_mode == USB_MODE_AUDIO) {
        uac_device_deinit();
        usb_audio_bridge_deinit();
        crosspad::IdfI2sInput::setSuppressed(false);
    }
#endif

    // Deinit CDC ACM if currently in default mode (audio mode has no CDC)
    if (s_current_mode == USB_MODE_DEFAULT) {
        tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0);
    }

    // Tear down current USB stack
    esp_err_t ret = tinyusb_driver_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to uninstall TinyUSB driver: %s", esp_err_to_name(ret));
        // The old stack is still standing, so MIDI can flow again.
        midi::EspTinyUsbMidiTransport::setTxBlocked(false);
        return ret;
    }

    // Let host detect the disconnect
    vTaskDelay(pdMS_TO_TICKS(200));

    // Reinstall with new descriptors
    switch (mode) {
        case USB_MODE_DEFAULT:
            ret = install_tinyusb_default();
            break;
#ifdef BSP_BOARD_CROSSPAD_S3
        case USB_MODE_AUDIO:
            ret = install_tinyusb_audio();
            break;
#endif
        default:
            ret = ESP_ERR_INVALID_ARG;
            break;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install new USB mode: %s", esp_err_to_name(ret));
        // Try to recover to default mode
        if (mode != USB_MODE_DEFAULT) {
            ESP_LOGW(TAG, "Attempting recovery to default mode...");
            // TinyUSB may still be running from the failed mode — tear it down first
            tinyusb_driver_uninstall();
            vTaskDelay(pdMS_TO_TICKS(200));
            ret = install_tinyusb_default();
            if (ret == ESP_OK) {
                s_current_mode = USB_MODE_DEFAULT;
                ESP_LOGW(TAG, "Recovered to default USB mode");
            }
        }
#ifdef BSP_BOARD_CROSSPAD_S3
        // No path here has a port running: a failed audio install stopped
        // its bridge or never started it, and leaving audio stopped it above.
        AudioTasks_SetSuspended(false);
        crosspad::IdfI2sInput::setSuppressed(false);
#endif
        midi::EspTinyUsbMidiTransport::setTxBlocked(false);
        return ret;
    }

    s_current_mode = mode;
#ifdef BSP_BOARD_CROSSPAD_S3
    // A switch has always ended with the mixer running; a bench override that
    // parked it (AUDIO_TASKS 0, SysEx 1D 06 00) still ends here.
    AudioTasks_SetSuspended(false);
#endif
    midi::EspTinyUsbMidiTransport::setTxBlocked(false);
    persist_mode(s_current_mode);
    /* The longest wait between two render passes since the kit-load swap log
     * last took it (read, not cleared): a switch that parked the render would
     * show its whole length here, one that did not stays under a block. */
    ESP_LOGI(TAG, "USB mode switched to: %s (rt_gap_max_us=%lu)",
             usb_config_manager_get_mode_name(s_current_mode),
             (unsigned long)g_rt_gap_max_us.load(std::memory_order_relaxed));
    return ESP_OK;
}

usb_mode_t usb_config_manager_get_mode(void)
{
    return s_current_mode;
}

const char* usb_config_manager_get_mode_name(usb_mode_t mode)
{
    switch (mode) {
        case USB_MODE_DEFAULT:  return "CDC + MIDI";
        case USB_MODE_AUDIO:    return "MIDI + Audio";
        case USB_MODE_TRANSFER: return "MSC + CDC";
        default:                return "Unknown";
    }
}

void check_usb_mode_command(const char* buff, size_t len)
{
    if (len >= 9 && memcmp(buff, "USB_AUDIO", 9) == 0) {
        ESP_LOGI(TAG, "Received USB_AUDIO command");
        usb_config_manager_set_mode(USB_MODE_AUDIO);
    } else if (len >= 11 && memcmp(buff, "USB_DEFAULT", 11) == 0) {
        ESP_LOGI(TAG, "Received USB_DEFAULT command");
        usb_config_manager_set_mode(USB_MODE_DEFAULT);
    }
}
