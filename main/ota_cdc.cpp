/**
 * @file ota_cdc.cpp
 * @brief OTA firmware update over USB CDC — protocol logic only
 *
 * Contains:
 *   - OTA state machine (begin, data write, complete/error)
 *   - NVS auto-accept preference
 *   - CDC response helper
 *   - UI callback dispatch (no LVGL dependency)
 */

#include "esp_attr.h"
#include "ota_cdc.h"
#include "motion_stream.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_delta_ota.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tusb_cdc_acm.h"

#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "bsp/esp-bsp.h"

static const char *TAG = "ota_cdc";

/* ── OTA state ───────────────────────────────────────────────────────── */

static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *ota_partition = NULL;
static size_t ota_total_size = 0;
static size_t ota_received = 0;
#define OTA_WRITE_BUF_SIZE 4096
/* PSRAM: esp_flash_write() copies a buffer from outside internal RAM through
 * its own, and the 4 kB here are part of what pays for the 32 kB I-cache. */
EXT_RAM_BSS_ATTR static uint8_t ota_write_buf[OTA_WRITE_BUF_SIZE];
static size_t ota_write_pos = 0;
static volatile bool ota_active = false;
/* Last time stream data arrived (or the stream began); ota_poll_timeout()
 * abandons the session after OTA_STALL_US without data. */
static int64_t ota_last_rx_us = 0;
#define OTA_STALL_US (10 * 1000 * 1000)

/* ── Delta OTA state ─────────────────────────────────────────────────── */

static bool ota_delta_mode = false;
static esp_delta_ota_handle_t delta_handle = NULL;
static const esp_partition_t *ota_base_partition = NULL;

/* ── UI callbacks ────────────────────────────────────────────────────── */

static ota_ui_callbacks_t s_ui_cbs = {};
static SemaphoreHandle_t ota_confirm_sem = NULL;
static volatile bool ota_user_accepted = false;

void ota_set_ui_callbacks(const ota_ui_callbacks_t *cbs) {
    if (cbs) {
        s_ui_cbs = *cbs;
    } else {
        memset(&s_ui_cbs, 0, sizeof(s_ui_cbs));
    }
}

void ota_user_respond(bool accepted) {
    ota_user_accepted = accepted;
    if (ota_confirm_sem) {
        xSemaphoreGive(ota_confirm_sem);
    }
}

/* ── OTA NVS preferences ────────────────────────────────────────────── */

bool ota_auto_accept_enabled(void) {
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open("ota_prefs", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "auto_accept", &val);
        nvs_close(h);
    }
    return val != 0;
}

/* ── CDC response helper ─────────────────────────────────────────────── */

/* For a host page watching (motion_stream.h): how far the transfer is. */
static unsigned ota_pct(void) {
    return ota_total_size ? (unsigned)((uint64_t)ota_received * 100 / ota_total_size) : 0;
}

static void cdc_respond(const char *msg) {
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)msg, strlen(msg));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

/* ── Delta OTA callbacks ──────────────────────────────────────────────── */

static esp_err_t delta_read_cb(uint8_t *buf_p, size_t size, int src_offset, void *user_data) {
    const esp_partition_t *base = (const esp_partition_t *)user_data;
    return esp_partition_read(base, (size_t)src_offset, buf_p, size);
}

static esp_err_t delta_write_cb(const uint8_t *buf_p, size_t size, void *user_data) {
    return esp_ota_write(ota_handle, buf_p, size);
}

static void delta_cleanup(void) {
    if (delta_handle) {
        esp_delta_ota_deinit(delta_handle);
        delta_handle = NULL;
    }
    ota_delta_mode = false;
    ota_base_partition = NULL;
}

/* ── OTA protocol logic ──────────────────────────────────────────────── */

void ota_process_data(const uint8_t *data, size_t len) {
    ota_last_rx_us = esp_timer_get_time();
    if (ota_delta_mode) {
        /* ── Delta mode: feed patch data to delta engine ── */
        ota_received += len;

        esp_err_t err = esp_delta_ota_feed_patch(delta_handle, data, (int)len);
        if (err != ESP_OK) {
            char buf[64];
            snprintf(buf, sizeof(buf), "OTA_ERROR delta_%s\n", esp_err_to_name(err));
            cdc_respond(buf);
            esp_ota_abort(ota_handle);
            delta_cleanup();
            if (s_ui_cbs.on_error) s_ui_cbs.on_error();
            motion_stream_ota(MOTION_OTA_FAILED, ota_pct());
            crosspad::getPadAnimator().playError();
            ota_active = false;
            return;
        }

        char resp[48];
        snprintf(resp, sizeof(resp), "OK %zu/%zu\n", ota_received, ota_total_size);
        cdc_respond(resp);

        if (s_ui_cbs.on_progress) {
            s_ui_cbs.on_progress(ota_received, ota_total_size);
        }
        motion_stream_ota(MOTION_OTA_RECEIVING, ota_pct());
        crosspad::getPadAnimator().setProgress((float)ota_received / (float)ota_total_size);
    } else {
        /* ── Full OTA mode: buffer and write raw firmware ── */
        size_t offset = 0;
        while (offset < len) {
            size_t space = OTA_WRITE_BUF_SIZE - ota_write_pos;
            size_t to_copy = (len - offset) < space ? (len - offset) : space;
            memcpy(ota_write_buf + ota_write_pos, data + offset, to_copy);
            ota_write_pos += to_copy;
            ota_received += to_copy;
            offset += to_copy;

            if (ota_write_pos >= OTA_WRITE_BUF_SIZE || ota_received >= ota_total_size) {
                esp_err_t err = esp_ota_write(ota_handle, ota_write_buf, ota_write_pos);
                if (err != ESP_OK) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "OTA_ERROR write_%s\n", esp_err_to_name(err));
                    cdc_respond(buf);
                    esp_ota_abort(ota_handle);
                    if (s_ui_cbs.on_error) s_ui_cbs.on_error();
                    motion_stream_ota(MOTION_OTA_FAILED, ota_pct());
                    crosspad::getPadAnimator().playError();
                    ota_active = false;
                    return;
                }
                char resp[48];
                snprintf(resp, sizeof(resp), "OK %zu/%zu\n", ota_received, ota_total_size);
                cdc_respond(resp);
                ota_write_pos = 0;

                if (s_ui_cbs.on_progress) {
                    s_ui_cbs.on_progress(ota_received, ota_total_size);
                }
                motion_stream_ota(MOTION_OTA_RECEIVING, ota_pct());
                crosspad::getPadAnimator().setProgress((float)ota_received / (float)ota_total_size);
            }
        }
    }

    /* ── Completion (both modes) ── */
    if (ota_received >= ota_total_size) {
        esp_err_t err;

        if (ota_delta_mode) {
            err = esp_delta_ota_finalize(delta_handle);
            if (err != ESP_OK) {
                char buf[64];
                snprintf(buf, sizeof(buf), "OTA_ERROR delta_finalize_%s\n", esp_err_to_name(err));
                cdc_respond(buf);
                esp_ota_abort(ota_handle);
                delta_cleanup();
                if (s_ui_cbs.on_error) s_ui_cbs.on_error();
                motion_stream_ota(MOTION_OTA_FAILED, ota_pct());
                ota_active = false;
                return;
            }
            delta_cleanup();
        }

        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            char buf[64];
            snprintf(buf, sizeof(buf), "OTA_ERROR end_%s\n", esp_err_to_name(err));
            cdc_respond(buf);
            if (s_ui_cbs.on_error) s_ui_cbs.on_error();
            motion_stream_ota(MOTION_OTA_FAILED, ota_pct());
            ota_active = false;
            return;
        }
        err = esp_ota_set_boot_partition(ota_partition);
        if (err != ESP_OK) {
            char buf[64];
            snprintf(buf, sizeof(buf), "OTA_ERROR boot_%s\n", esp_err_to_name(err));
            cdc_respond(buf);
            if (s_ui_cbs.on_error) s_ui_cbs.on_error();
            motion_stream_ota(MOTION_OTA_FAILED, ota_pct());
            ota_active = false;
            return;
        }
        ESP_LOGW(TAG, "OTA complete! Restarting...");
        cdc_respond("OTA_OK\n");
        if (s_ui_cbs.on_complete) s_ui_cbs.on_complete();
        motion_stream_ota(MOTION_OTA_RESTARTING, 100);
        crosspad::getPadAnimator().setCompletionCallback([]() {
            bsp_display_brightness_set(0);
        });
        crosspad::getPadAnimator().playSuccess();
        ota_active = false;
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
}

bool ota_check_begin(const char *buf, size_t len) {
    if (len < 10 || memcmp(buf, "OTA_BEGIN ", 10) != 0) return false;

    size_t size = 0;
    char version[33] = {0};
    size_t i = 10;
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
        size = size * 10 + (buf[i] - '0');
    }
    if (i < len && buf[i] == ' ') {
        i++;
        size_t v_len = 0;
        while (i < len && buf[i] != '\n' && buf[i] != '\r' && v_len < 32) {
            version[v_len++] = buf[i++];
        }
        version[v_len] = '\0';
    }
    if (size == 0) {
        cdc_respond("OTA_ERROR invalid_size\n");
        return true;
    }

    bool auto_accept = ota_auto_accept_enabled();

    if (!auto_accept && ota_confirm_sem) {
        cdc_respond("OTA_WAIT\n");

        xSemaphoreTake(ota_confirm_sem, 0); // clear any stale signal
        ota_user_accepted = false;

        if (s_ui_cbs.on_confirm_request) {
            s_ui_cbs.on_confirm_request(size, version);
        }

        if (xSemaphoreTake(ota_confirm_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
            cdc_respond("OTA_ERROR timeout_confirm\n");
            ESP_LOGW(TAG, "OTA confirmation timed out");
            return true;
        }

        if (!ota_user_accepted) {
            cdc_respond("OTA_ERROR rejected\n");
            ESP_LOGI(TAG, "OTA rejected by user");
            return true;
        }
    }

    ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!ota_partition) {
        cdc_respond("OTA_ERROR no_partition\n");
        return true;
    }

    esp_err_t err = esp_ota_begin(ota_partition, size, &ota_handle);
    if (err != ESP_OK) {
        char resp[64];
        snprintf(resp, sizeof(resp), "OTA_ERROR begin_%s\n", esp_err_to_name(err));
        cdc_respond(resp);
        return true;
    }

    ota_total_size = size;
    ota_received = 0;
    ota_write_pos = 0;
    ota_active = true;
    ota_last_rx_us = esp_timer_get_time();
    ESP_LOGW(TAG, "OTA started, size=%zu, partition=%s", size, ota_partition->label);
    cdc_respond("OTA_READY\n");

    crosspad::getPadAnimator().startProgress();

    if (s_ui_cbs.on_begin) {
        s_ui_cbs.on_begin(size, version);
    }
    motion_stream_ota(MOTION_OTA_RECEIVING, 0);

    return true;
}

bool ota_check_delta_begin(const char *buf, size_t len) {
    if (len < 10 || memcmp(buf, "OTA_DELTA ", 10) != 0) return false;

    size_t size = 0;
    char version[33] = {0};
    size_t i = 10;
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
        size = size * 10 + (buf[i] - '0');
    }
    if (i < len && buf[i] == ' ') {
        i++;
        size_t v_len = 0;
        while (i < len && buf[i] != '\n' && buf[i] != '\r' && v_len < 32) {
            version[v_len++] = buf[i++];
        }
        version[v_len] = '\0';
    }
    if (size == 0) {
        cdc_respond("OTA_ERROR invalid_size\n");
        return true;
    }

    bool auto_accept = ota_auto_accept_enabled();

    if (!auto_accept && ota_confirm_sem) {
        cdc_respond("OTA_WAIT\n");

        xSemaphoreTake(ota_confirm_sem, 0);
        ota_user_accepted = false;

        if (s_ui_cbs.on_confirm_request) {
            s_ui_cbs.on_confirm_request(size, version);
        }

        if (xSemaphoreTake(ota_confirm_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
            cdc_respond("OTA_ERROR timeout_confirm\n");
            ESP_LOGW(TAG, "OTA confirmation timed out");
            return true;
        }

        if (!ota_user_accepted) {
            cdc_respond("OTA_ERROR rejected\n");
            ESP_LOGI(TAG, "OTA rejected by user");
            return true;
        }
    }

    /* Get base (running) partition for delta reads */
    ota_base_partition = esp_ota_get_running_partition();
    if (!ota_base_partition) {
        cdc_respond("OTA_ERROR no_base_partition\n");
        return true;
    }

    ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!ota_partition) {
        cdc_respond("OTA_ERROR no_partition\n");
        return true;
    }

    /* Begin OTA write with unknown output size (delta reconstructs full firmware) */
    esp_err_t err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        char resp[64];
        snprintf(resp, sizeof(resp), "OTA_ERROR begin_%s\n", esp_err_to_name(err));
        cdc_respond(resp);
        return true;
    }

    /* Initialize delta patching engine */
    esp_delta_ota_cfg_t delta_cfg = {};
    delta_cfg.user_data = (void *)ota_base_partition;
    delta_cfg.read_cb_with_user_data = delta_read_cb;
    delta_cfg.write_cb_with_user_data = delta_write_cb;

    delta_handle = esp_delta_ota_init(&delta_cfg);
    if (!delta_handle) {
        ESP_LOGE(TAG, "Failed to init delta OTA engine");
        esp_ota_abort(ota_handle);
        cdc_respond("OTA_ERROR delta_init\n");
        return true;
    }

    ota_total_size = size;
    ota_received = 0;
    ota_write_pos = 0;
    ota_delta_mode = true;
    ota_active = true;
    ota_last_rx_us = esp_timer_get_time();
    ESP_LOGW(TAG, "Delta OTA started, patch_size=%zu, base=%s, target=%s",
             size, ota_base_partition->label, ota_partition->label);
    cdc_respond("OTA_READY\n");

    crosspad::getPadAnimator().startProgress();

    if (s_ui_cbs.on_begin) {
        s_ui_cbs.on_begin(size, version);
    }
    motion_stream_ota(MOTION_OTA_RECEIVING, 0);

    return true;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ota_init(void) {
    ota_confirm_sem = xSemaphoreCreateBinary();
}

bool ota_is_active(void) {
    return ota_active;
}

bool ota_is_delta(void) {
    return ota_delta_mode;
}

void ota_poll_timeout(void) {
    if (!ota_active) return;
    if (esp_timer_get_time() - ota_last_rx_us < OTA_STALL_US) return;
    ota_abort_timeout();
}

void ota_abort_timeout(void) {
    ESP_LOGE(TAG, "OTA timeout - aborting");
    if (ota_delta_mode) {
        delta_cleanup();
    }
    esp_ota_abort(ota_handle);
    ota_active = false;
    cdc_respond("OTA_ERROR timeout\n");
    if (s_ui_cbs.on_error) s_ui_cbs.on_error();
    motion_stream_ota(MOTION_OTA_FAILED, ota_pct());
    crosspad::getPadAnimator().playError();
}
