#include "stm32_fw_update.h"
#include "stm32_hal_init.h"
#include "stm32_uart.h"
#include "stm32_i2c.h"
#include "stm32_reg_map.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "STM32_FW";

#define FW_DESC_OFFSET      0x100u
#define FW_DESC_MAGIC       0x57465043u        /* "CPFW" */
#define FW_PAGE_SIZE        2048u
#define FW_MAX_PAGES        63u                /* page 63 is the STM's NVM page */
#define FW_MIN_SIZE         (FW_DESC_OFFSET + 16u)

#define READY_TIMEOUT_MS    3000
#define REPLY_TIMEOUT_MS    1500               /* erase + program + verify of one page */
#define PAGE_RETRIES        3
#define REBOOT_PROBE_MS     6000

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  major;
    uint8_t  minor;
    uint16_t proto;
    uint8_t  pcb;        /* PCB the image targets; 0 = unknown */
    uint8_t  reserved0[3];
    uint32_t reserved1;
} fw_desc_t;

esp_err_t stm32_fw_image_info(const char *path, stm32_fw_image_info_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fw_desc_t d;
    bool ok = sz >= (long)FW_MIN_SIZE
           && fseek(f, FW_DESC_OFFSET, SEEK_SET) == 0
           && fread(&d, 1, sizeof(d), f) == sizeof(d)
           && d.magic == FW_DESC_MAGIC;
    fclose(f);
    if (!ok) return ESP_ERR_INVALID_RESPONSE;
    if ((size_t)sz > FW_MAX_PAGES * FW_PAGE_SIZE) return ESP_ERR_INVALID_SIZE;
    out->major = d.major;
    out->minor = d.minor;
    out->proto = d.proto;
    out->pcb   = d.pcb;
    out->size  = (size_t)sz;
    return ESP_OK;
}

/* Wait for one specific reply byte; anything else is a stray byte from the
 * old firmware still draining and is skipped. */
static int wait_reply(uint32_t timeout_ms)
{
    uint8_t b;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(end - xTaskGetTickCount()) > 0) {
        if (stm32_uart_raw_read(&b, 1, 50) == 1) return b;
    }
    return -1;
}

static bool send_page(uint8_t page, const uint8_t *data, int *reply)
{
    uint8_t hdr[2] = { 'P', page };
    uint32_t crc = esp_rom_crc32_le(0, data, FW_PAGE_SIZE);
    uint8_t tail[4] = { (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
    stm32_uart_raw_write(hdr, sizeof(hdr));
    stm32_uart_raw_write(data, FW_PAGE_SIZE);
    stm32_uart_raw_write(tail, sizeof(tail));
    *reply = wait_reply(REPLY_TIMEOUT_MS);
    return *reply == 'A' || *reply == 'O';
}

/* Full session against a flasher that already answered 'R'. */
static esp_err_t run_session(FILE *f, size_t size, uint32_t crc,
                             stm32_fw_progress_cb cb, void *ctx, uint8_t *page)
{
    uint16_t npages = (uint16_t)((size + FW_PAGE_SIZE - 1) / FW_PAGE_SIZE);
    uint8_t h[10] = { 'H',
        (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24),
        (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24), 0 };
    for (int i = 1; i < 9; i++) h[9] ^= h[i];
    stm32_uart_raw_write(h, sizeof(h));
    int r = wait_reply(REPLY_TIMEOUT_MS);
    if (r != 'A') {
        ESP_LOGE(TAG, "header rejected (%d)", r);
        return r < 0 ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }

    /* pages 1..n-1 first, page 0 (vector table) last */
    for (uint16_t i = 1; i <= npages; i++) {
        uint16_t p = (i == npages) ? 0 : i;
        memset(page, 0xFF, FW_PAGE_SIZE);
        if (fseek(f, (long)p * FW_PAGE_SIZE, SEEK_SET) != 0) return ESP_FAIL;
        size_t n = fread(page, 1, FW_PAGE_SIZE, f);
        if (n == 0) return ESP_FAIL;
        int reply = -1;
        bool ok = false;
        for (int attempt = 0; attempt < PAGE_RETRIES && !ok; attempt++) {
            ok = send_page((uint8_t)p, page, &reply);
            if (!ok) ESP_LOGW(TAG, "page %u: reply %d, retry", p, reply);
        }
        if (!ok) {
            ESP_LOGE(TAG, "page %u failed (%d)", p, reply);
            return reply < 0 ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
        }
        if (p == 0 && reply != 'O') {
            ESP_LOGE(TAG, "image CRC rejected by STM (%d)", reply);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (cb) cb(i, npages, ctx);
    }
    return ESP_OK;
}

/* Both revisions known and different: an image for another board. */
static bool pcb_mismatch(const stm32_fw_image_info_t *img, const stm32_link_info_t *li)
{
    return img->pcb != 0 && li->pcb_rev != 0 && li->pcb_rev != 0xFF && img->pcb != li->pcb_rev;
}

esp_err_t stm32_fw_update(const char *path, stm32_fw_progress_cb cb, void *ctx)
{
    return stm32_fw_update_ex(path, false, cb, ctx);
}

esp_err_t stm32_fw_update_ex(const char *path, bool force, stm32_fw_progress_cb cb, void *ctx)
{
    stm32_fw_image_info_t info;
    esp_err_t err = stm32_fw_image_info(path, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: not a valid STM image (%s)", path, esp_err_to_name(err));
        return err;
    }
    if (!stm32_hal_is_initialized()) return ESP_ERR_INVALID_STATE;
    stm32_link_info_t board = {0};
    if (stm32_hal_get_link_info(&board) == ESP_OK && board.linked && pcb_mismatch(&info, &board)) {
        if (!force) {
            ESP_LOGE(TAG, "%s targets PCB r%u, this board is r%u — refused (FORCE overrides)",
                     path, info.pcb, board.pcb_rev);
            return ESP_ERR_NOT_SUPPORTED;
        }
        ESP_LOGW(TAG, "%s targets PCB r%u, this board is r%u — forced", path, info.pcb, board.pcb_rev);
    }

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint8_t *page = malloc(FW_PAGE_SIZE);
    if (!page) { fclose(f); return ESP_ERR_NO_MEM; }

    /* The STM programs doublewords: the image is padded with 0xFF to a
     * multiple of 8 (pages are sent 0xFF-padded anyway) and the CRC covers
     * the padded length, which is what the STM verifies in flash. */
    size_t padded = (info.size + 7u) & ~(size_t)7u;
    uint32_t crc = 0;
    size_t n, total = 0;
    while ((n = fread(page, 1, FW_PAGE_SIZE, f)) > 0) {
        total += n;
        if (total == info.size && padded > info.size) {
            memset(page + n, 0xFF, padded - info.size);
            n += padded - info.size;
        }
        crc = esp_rom_crc32_le(crc, page, n);
    }

    ESP_LOGW(TAG, "updating STM32 from %s: v%u.%u proto 0x%04X, %u bytes, crc %08" PRIX32,
             path, info.major, info.minor, info.proto, (unsigned)padded, crc);

    /* Own the link: no heartbeat, no MIDI parser, nothing else on the wire. */
    stm32_hal_heartbeat_suspend(true);
    stm32_uart_raw_begin();

    /* Ask over I2C, and over UART for a link whose I2C side is down. Both
     * only arm a flag; the STM enters the flasher from its main loop. */
    bool asked = stm32_i2c_is_initialized()
              && stm32_i2c_write_u8(REG_CMD_FW_UPDATE, FW_UPDATE_MAGIC) == ESP_OK;
    static const uint8_t sysex[] = { 0xF0, 0x7D, 0x31, 0xF7 };
    stm32_uart_raw_write(sysex, sizeof(sysex));
    ESP_LOGI(TAG, "entry requested via %s", asked ? "I2C+UART" : "UART");

    err = ESP_ERR_TIMEOUT;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(READY_TIMEOUT_MS);
    while ((int32_t)(end - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        stm32_uart_raw_flush();
        stm32_uart_raw_write((const uint8_t *)"Q", 1);
        if (wait_reply(150) == 'R') { err = ESP_OK; break; }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STM did not enter the flasher");
    } else {
        err = run_session(f, padded, crc, cb, ctx, page);
        if (err != ESP_OK) {
            /* Do not leave the STM parked in the flasher with its I2C off —
             * the rest of boot waits on that link. 'X' resets it: back to
             * the old app if page 0 was never erased, ROM DFU otherwise. */
            ESP_LOGW(TAG, "aborting: resetting the STM");
            stm32_uart_raw_write((const uint8_t *)"X", 1);
        }
    }
    free(page);
    fclose(f);

    if (err == ESP_OK) ESP_LOGI(TAG, "image accepted, STM rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    stm32_uart_raw_end();
    stm32_hal_heartbeat_suspend(false);
    if (err != ESP_OK) return err;

    /* The new firmware announces itself over I2C once it boots. */
    err = ESP_ERR_TIMEOUT;
    end = xTaskGetTickCount() + pdMS_TO_TICKS(REBOOT_PROBE_MS);
    while ((int32_t)(end - xTaskGetTickCount()) > 0) {
        if (stm32_hal_refresh_link_info() == ESP_OK) { err = ESP_OK; break; }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    stm32_link_info_t li;
    if (err != ESP_OK || stm32_hal_get_link_info(&li) != ESP_OK) {
        ESP_LOGE(TAG, "STM did not come back after the update");
        return ESP_ERR_TIMEOUT;
    }
    if (li.fw_major != info.major || li.fw_minor != info.minor || li.proto_version != info.proto) {
        ESP_LOGE(TAG, "STM reports v%u.%u proto 0x%04X after flashing v%u.%u proto 0x%04X",
                 li.fw_major, li.fw_minor, li.proto_version, info.major, info.minor, info.proto);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(TAG, "STM32 now v%u.%u proto 0x%04X", li.fw_major, li.fw_minor, li.proto_version);
    return ESP_OK;
}

static void log_progress(uint16_t done, uint16_t total, void *ctx)
{
    (void)ctx;
    if (done == total || (done % 8) == 0) ESP_LOGI(TAG, "  %u/%u pages", done, total);
}

bool stm32_fw_update_check(const char *path, stm32_fw_update_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (stm32_hal_get_link_info(&out->running) != ESP_OK || !out->running.linked) {
        ESP_LOGI(TAG, "check: STM link down");
        return false;
    }
    if (out->running.uart_only) {
        ESP_LOGI(TAG, "check: UART-only STM (PCB r%u), no in-field update", out->running.pcb_rev);
        return false;
    }
    esp_err_t err = stm32_fw_image_info(path, &out->bundled);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "check: no bundled image at %s (%s)", path, esp_err_to_name(err));
        return false;
    }
    out->have_image = true;
    const stm32_link_info_t *li = &out->running;
    const stm32_fw_image_info_t *img = &out->bundled;
    bool newer = img->major > li->fw_major
              || (img->major == li->fw_major && img->minor > li->fw_minor);
    bool older = img->major < li->fw_major
              || (img->major == li->fw_major && img->minor < li->fw_minor);
    out->pcb_mismatch = pcb_mismatch(img, li);
    /* A protocol difference alone no longer triggers an automatic update: a
     * firmware switch to an older package must not drag the STM32 backwards
     * with it. STM_FW_UPDATE ... FORCE bypasses `due` entirely, so an older
     * bundle is still installable on request. */
    out->due = !out->pcb_mismatch && !older && (newer || img->proto != li->proto_version);
    stm32_charger_telemetry_t chg;
    out->battery_low = stm32_hal_get_charger_telemetry(&chg) == ESP_OK
                    && !stm32_charger_present(&chg) && chg.soc_pct < 20;
    if (out->pcb_mismatch) {
        ESP_LOGE(TAG, "check: bundle targets PCB r%u but this board is r%u — no update offered",
                 img->pcb, li->pcb_rev);
    } else if (out->due) {
        ESP_LOGW(TAG, "check: STM v%u.%u proto 0x%04X running, bundle has v%u.%u proto 0x%04X%s",
                 li->fw_major, li->fw_minor, li->proto_version, img->major, img->minor, img->proto,
                 out->battery_low ? " (battery low, no charger)" : "");
    } else {
        ESP_LOGI(TAG, "check: STM v%u.%u proto 0x%04X is current", li->fw_major, li->fw_minor, li->proto_version);
    }
    return true;
}

struct auto_ctx { stm32_fw_progress_cb cb; void *ctx; };

static void auto_progress(uint16_t done, uint16_t total, void *ctx)
{
    struct auto_ctx *a = ctx;
    log_progress(done, total, NULL);
    if (a->cb) a->cb(done, total, a->ctx);
}

bool stm32_fw_update_auto(const char *path, stm32_fw_progress_cb cb, void *ctx)
{
    stm32_fw_update_status_t st;
    if (!stm32_fw_update_check(path, &st) || !st.due) return false;
    if (st.battery_low) {
        ESP_LOGW(TAG, "auto: deferred — battery low with no charger");
        return false;
    }
    struct auto_ctx actx = { cb, ctx };
    return stm32_fw_update(path, auto_progress, &actx) == ESP_OK;
}
