/**
 * @file file_transfer.cpp
 * @brief Assets-partition flashing and arbitrary file transfer over USB CDC.
 *
 * See include/file_transfer.h for the protocol and the reasoning behind it.
 */

#include "include/file_transfer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_crc.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "tusb_cdc_acm.h"

#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "cdc_line.h"
#include "fw_slots.h"

static const char *TAG = "file_xfer";

/* ── Sizing ──────────────────────────────────────────────────────────── */

#define FX_PATH_MAX      160          /* longest accepted path, NUL included  */
/* The longest FS_* line is FS_GET with two 10-digit numbers and the longest
 * path; it has to arrive whole through the CDC line buffer. */
static_assert(sizeof("FS_GET 4294967295 4294967295 ") - 1 + FX_PATH_MAX <= CDC_LINE_MAX,
              "an FS_* line with the longest path must fit the CDC line buffer");
#define FX_CHUNK         4096         /* host chunk == flash page == ack unit */
#define FX_WORKER_STACK  5120         /* internal RAM: the worker writes flash */
#define FX_SLOTS         2            /* staging slots: fill one, write other */
#define FX_IO_BUF        2048         /* device->host read buffer             */
#define FX_STALL_US      (15 * 1000 * 1000)

/* ── Roots ───────────────────────────────────────────────────────────── */

#define FX_ROOT_ASSETS   "/assets"
#define FX_ROOT_SPIFLASH "/spiflash"
#define FX_ROOT_SDCARD   "/sdcard"

static wl_handle_t s_wl = WL_INVALID_HANDLE;

/* The FAT "storage" partition is not mounted at boot — nothing else uses it,
 * and a mount costs time and a FATFS work area. It is brought up on the first
 * transfer that names it, and formatted if it has never been used. */
static bool ensure_spiflash(void)
{
    if (s_wl != WL_INVALID_HANDLE) return true;
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true;
    cfg.max_files = 4;
    cfg.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FX_ROOT_SPIFLASH, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "mounted 'storage' at %s", FX_ROOT_SPIFLASH);
    return true;
}

/* Bounded copy that reports overflow rather than silently truncating a path
 * into one that names a different file. */
static bool copy_path(char *dst, size_t cap, const char *src)
{
    const size_t n = strlen(src);
    if (n == 0 || n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

/* Refuse anything outside the three known mounts, and any path that could
 * climb out of one. FS_DELETE is a real delete; it does not get a wildcard.
 * String work only — this runs on the drain loop, so it touches no volume. */
static bool path_ok(const char *p)
{
    if (!p || !*p) return false;
    if (strlen(p) >= FX_PATH_MAX) return false;
    if (strstr(p, "..")) return false;

    static const char *roots[] = { FX_ROOT_ASSETS, FX_ROOT_SPIFLASH, FX_ROOT_SDCARD };
    for (const char *r : roots) {
        const size_t n = strlen(r);
        if (strncmp(p, r, n) == 0 && (p[n] == '\0' || p[n] == '/')) return true;
    }
    return false;
}

/* Worker side: bring the volume up if this path needs it. */
static bool root_ready(const char *p)
{
    const size_t n = strlen(FX_ROOT_SPIFLASH);
    if (strncmp(p, FX_ROOT_SPIFLASH, n) == 0) return ensure_spiflash();
    return true;
}

/* ── CDC output ──────────────────────────────────────────────────────── */

/* The CDC TX FIFO is 256 bytes and the queue call silently drops what does not
 * fit, so everything goes out in acknowledged pieces. Both the drain loop and
 * the worker reply through here; they are never both mid-reply, because the
 * host protocol is strictly lockstep. */
static bool fx_write_raw(const uint8_t *data, size_t len)
{
    const int64_t deadline = esp_timer_get_time() + 5 * 1000 * 1000;
    while (len > 0) {
        const size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
        if (n == 0) {
            if (esp_timer_get_time() > deadline) return false;
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
            continue;
        }
        data += n;
        len  -= n;
        if (tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(500)) != ESP_OK &&
            esp_timer_get_time() > deadline) {
            return false;
        }
    }
    return true;
}

static void fx_reply(const char *text)
{
    fx_write_raw((const uint8_t *)text, strlen(text));
}

static void fx_replyf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fx_replyf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fx_reply(buf);
}

/* ── Worker ──────────────────────────────────────────────────────────── */

enum : uint8_t {
    OP_ROOTS, OP_LIST, OP_STAT, OP_DELETE, OP_MKDIR, OP_GET,
    OP_PUT_OPEN, OP_PUT_DATA, OP_PUT_DONE, OP_PUT_ABORT, OP_ASSETS_WRITE,
    OP_ASSETS_DONE, OP_ASSETS_READ
};

typedef struct {
    uint8_t   op;
    uint32_t  a;        /* offset / byte count            */
    uint32_t  b;        /* length / crc                   */
    uint8_t  *ptr;      /* staging slot for OP_PUT_DATA   */
    char      path[FX_PATH_MAX];
} fx_req_t;

static void stream_free(void);   /* frees the assets staging buffer */

static QueueHandle_t     s_req_q  = nullptr;
static SemaphoreHandle_t s_slots  = nullptr;
static TaskHandle_t      s_worker = nullptr;

/* ── Stream state (drain loop) ───────────────────────────────────────── */

enum fx_state_t : uint8_t { FX_IDLE = 0, FX_RX_ASSETS, FX_RX_FILE };

static volatile fx_state_t s_state = FX_IDLE;
static size_t   s_total   = 0;
static size_t   s_recv    = 0;
static uint32_t s_want_crc = 0;
static bool     s_force   = false;
static int64_t  s_last_rx_us = 0;

/* The image is streamed into flash, not staged whole: this board runs with
 * ~600 kB of PSRAM free and the partition is 1 MiB, so a staged copy simply
 * does not fit once anything else is loaded. Page 0 is held back and written
 * last, the way stm32_fw_update.c does it — an interrupted transfer then
 * leaves no valid superblock, and app_main's format_if_mount_failed makes a
 * fresh empty partition rather than a half-old one. */
static uint8_t *s_a_page0 = nullptr;   /* first FX_CHUNK bytes, written last */
static uint32_t s_a_off = 0;           /* bytes handed to flash so far       */
static uint32_t s_a_crc = 0;           /* rolling CRC of what arrived        */
static bool     s_a_erased = false;
static uint8_t *s_slot_buf[FX_SLOTS] = { nullptr, nullptr };
static bool     s_slot_held = false;   /* the slot being filled is reserved */
static uint8_t  s_slot_next = 0;
static size_t   s_slot_fill = 0;

/* ── PUT state (worker) ──────────────────────────────────────────────── */

static FILE    *s_put_f = nullptr;
static char     s_put_tmp[FX_PATH_MAX + 8];
static char     s_put_final[FX_PATH_MAX];
static uint32_t s_put_crc = 0;
static size_t   s_put_written = 0;

/* ── Assets partition ────────────────────────────────────────────────── */

static const esp_partition_t *assets_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, "assets");
}

/* A LittleFS image carries the string "littlefs" in its superblock, a few
 * bytes into block 0. Refusing an image without it is what stops a firmware
 * binary or a truncated download from erasing every icon and the bundled STM32
 * image; ASSETS_BEGIN ... FORCE skips the check the way STM_FW_UPDATE FORCE
 * does. */
static bool looks_like_littlefs(const uint8_t *img, size_t len)
{
    const size_t scan = len < 64 ? len : 64;
    if (scan < 8) return false;
    for (size_t i = 0; i + 8 <= scan; i++) {
        if (memcmp(img + i, "littlefs", 8) == 0) return true;
    }
    return false;
}

/* Shared with fx_assets_write_image(): erase the whole partition. Does not
 * touch the LittleFS mount — the streaming path unregisters it once, right
 * before this is first called; fx_assets_write_image()'s caller does the same
 * around the whole call. */
static bool assets_erase(const esp_partition_t *part)
{
    return esp_partition_erase_range(part, 0, part->size) == ESP_OK;
}

/* Shared: write one incoming chunk at the partition offset tracked in *off,
 * holding the first FX_CHUNK bytes back into `page0` — the way
 * stm32_fw_update.c does it, so an interrupted write leaves no valid
 * superblock rather than a plausible-looking mix of two images. */
static bool assets_write_chunk(const esp_partition_t *part, uint8_t *page0,
                               uint32_t *off, const uint8_t *src, uint32_t n)
{
    uint32_t o = *off;
    uint32_t rest = n;
    if (o < FX_CHUNK) {
        const uint32_t take = (FX_CHUNK - o) < rest ? (FX_CHUNK - o) : rest;
        memcpy(page0 + o, src, take);
        o += take; src += take; rest -= take;
    }
    if (rest && esp_partition_write(part, o, src, rest) != ESP_OK) return false;
    *off = o + rest;
    return true;
}

/* Shared: commit the held-back page 0 once the rest of the image is down. */
static bool assets_commit_page0(const esp_partition_t *part, const uint8_t *page0)
{
    return esp_partition_write(part, 0, page0, FX_CHUNK) == ESP_OK;
}

enum class AssetsVerifyErr : uint8_t { None, NoMem, ReadFail };

/* Shared: read the whole partition back and CRC it — a write that reported OK
 * but did not land is exactly the failure this partition cannot absorb
 * silently. */
static AssetsVerifyErr assets_verify(const esp_partition_t *part, uint32_t total, uint32_t *out_crc)
{
    uint8_t *scratch = (uint8_t *)heap_caps_malloc(FX_CHUNK, MALLOC_CAP_SPIRAM);
    if (!scratch) scratch = (uint8_t *)malloc(FX_CHUNK);
    if (!scratch) return AssetsVerifyErr::NoMem;

    uint32_t crc = 0;
    for (uint32_t off = 0; off < total; off += FX_CHUNK) {
        const uint32_t n = (total - off) < FX_CHUNK ? (total - off) : FX_CHUNK;
        if (esp_partition_read(part, off, scratch, n) != ESP_OK) {
            free(scratch);
            return AssetsVerifyErr::ReadFail;
        }
        crc = esp_crc32_le(crc, scratch, n);
    }
    free(scratch);
    *out_crc = crc;
    return AssetsVerifyErr::None;
}

/* Runs on the worker. The image arrives in FX_CHUNK slots and goes straight to
 * flash; only page 0 is held back, so an interrupted transfer leaves an
 * unmountable partition that app_main reformats rather than a plausible-looking
 * mix of two images. */
static void assets_write_slot(uint8_t *slot, uint32_t n)
{
    const esp_partition_t *part = assets_partition();
    if (!part) { xSemaphoreGive(s_slots); return; }

    if (!s_a_erased) {
        /* A mounted LittleFS caches blocks and would write its own view back
         * over the new image, so it goes first. This is the point of no return
         * for the old contents. */
        esp_vfs_littlefs_unregister("assets");
        crosspad::getPadAnimator().startProgress();
        if (!assets_erase(part)) {
            xSemaphoreGive(s_slots);
            s_a_off = UINT32_MAX;       /* poison: report once, at DONE */
            return;
        }
        s_a_erased = true;
    }
    if (s_a_off == UINT32_MAX) { xSemaphoreGive(s_slots); return; }

    s_a_crc = esp_crc32_le(s_a_crc, slot, n);

    if (!assets_write_chunk(part, s_a_page0, &s_a_off, slot, n)) {
        s_a_off = UINT32_MAX;
        xSemaphoreGive(s_slots);
        return;
    }
    xSemaphoreGive(s_slots);

    crosspad::getPadAnimator().setProgress((float)s_a_off / (float)s_total);
    fx_replyf("OK %u/%u\r\n", (unsigned)s_a_off, (unsigned)s_total);
}

/* Everything received. Judge it, then commit page 0 and read the whole thing
 * back — a write that reported OK but did not land is exactly the failure this
 * partition cannot absorb silently. */
static void assets_finish(uint32_t want_crc)
{
    auto &anim = crosspad::getPadAnimator();
    const esp_partition_t *part = assets_partition();
    if (!part)                { fx_reply("ASSETS_ERROR no_partition\r\n"); anim.playError(); return; }
    if (s_a_off == UINT32_MAX){ fx_reply("ASSETS_ERROR write\r\n");        anim.playError(); return; }

    if (s_a_crc != want_crc) {
        ESP_LOGE(TAG, "assets crc mismatch: got %08x want %08x",
                 (unsigned)s_a_crc, (unsigned)want_crc);
        fx_replyf("ASSETS_ERROR crc_%08x\r\n", (unsigned)s_a_crc);
        anim.playError();
        return;   /* page 0 stays unwritten: the partition reformats on boot */
    }
    if (!s_force && !looks_like_littlefs(s_a_page0, FX_CHUNK)) {
        fx_reply("ASSETS_ERROR not_littlefs\r\n");
        anim.playError();
        return;
    }
    if (!assets_commit_page0(part, s_a_page0)) {
        fx_reply("ASSETS_ERROR write_page0\r\n");
        anim.playError();
        return;
    }

    uint32_t crc = 0;
    switch (assets_verify(part, s_total, &crc)) {
        case AssetsVerifyErr::NoMem:    fx_reply("ASSETS_ERROR verify_nomem\r\n"); anim.playError(); return;
        case AssetsVerifyErr::ReadFail: fx_reply("ASSETS_ERROR verify_read\r\n");  anim.playError(); return;
        case AssetsVerifyErr::None:     break;
    }
    if (crc != want_crc) {
        ESP_LOGE(TAG, "assets verify mismatch: got %08x want %08x",
                 (unsigned)crc, (unsigned)want_crc);
        fx_replyf("ASSETS_ERROR verify_crc_%08x\r\n", (unsigned)crc);
        anim.playError();
        return;
    }

    ESP_LOGW(TAG, "assets image written (%u bytes, crc %08x) — restarting",
             (unsigned)s_total, (unsigned)crc);
    anim.playSuccess();
    fx_reply("ASSETS_OK\r\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* Public: the same erase/write/verify core as the streaming CDC path above,
 * for a caller that already holds the whole image in one buffer (fw_assets_sync()).
 * No LittleFS unregister/register, no CDC reply, no restart — those are the
 * caller's job, since this is also called well outside a CDC transfer. */
bool fx_assets_write_image(const uint8_t *image, uint32_t size, uint32_t crc32)
{
    const esp_partition_t *part = assets_partition();
    if (!part || size == 0 || size > part->size) return false;

    uint8_t *page0 = (uint8_t *)heap_caps_malloc(FX_CHUNK, MALLOC_CAP_SPIRAM);
    if (!page0) page0 = (uint8_t *)malloc(FX_CHUNK);
    if (!page0) return false;

    bool ok = assets_erase(part);
    uint32_t off = 0;
    for (uint32_t pos = 0; ok && pos < size; pos += FX_CHUNK) {
        const uint32_t n = (size - pos) < FX_CHUNK ? (size - pos) : FX_CHUNK;
        ok = assets_write_chunk(part, page0, &off, image + pos, n);
    }
    if (ok) ok = assets_commit_page0(part, page0);

    uint32_t crc = 0;
    if (ok) ok = assets_verify(part, size, &crc) == AssetsVerifyErr::None && crc == crc32;

    free(page0);
    if (!ok) ESP_LOGE(TAG, "fx_assets_write_image failed (size=%u)", (unsigned)size);
    return ok;
}

static void do_assets_read(uint32_t off, uint32_t want)
{
    const esp_partition_t *part = assets_partition();
    if (!part) { fx_reply("FS_ERR no_partition\r\n"); return; }
    if (off >= part->size) { fx_reply("FS_ERR bad_offset\r\n"); return; }
    uint32_t len = (want == 0) ? part->size - off : want;
    if (off + len > part->size) len = part->size - off;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(FX_IO_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t *)malloc(FX_IO_BUF);
    if (!buf) { fx_reply("FS_ERR nomem\r\n"); return; }

    fx_replyf("FS_DATA %u\r\n", (unsigned)len);

    uint32_t crc = 0, sent = 0;
    bool ok = true;
    while (sent < len) {
        const size_t n = (len - sent) < FX_IO_BUF ? (len - sent) : FX_IO_BUF;
        if (esp_partition_read(part, off + sent, buf, n) != ESP_OK) { ok = false; break; }
        if (!fx_write_raw(buf, n)) { ok = false; break; }
        crc = esp_crc32_le(crc, buf, n);
        sent += n;
    }
    /* The header promised <len> bytes; produce them even after a failed read,
     * or the host waits forever and the next command lands mid-payload. */
    if (!ok) {
        memset(buf, 0, FX_IO_BUF);
        for (uint32_t pad = sent; pad < len; ) {
            const size_t n = (len - pad) < FX_IO_BUF ? (len - pad) : FX_IO_BUF;
            if (!fx_write_raw(buf, n)) break;
            pad += n;
        }
    }
    free(buf);

    if (!ok) fx_replyf("\r\nFS_TRUNC %u/%u\r\n", (unsigned)sent, (unsigned)len);
    else     fx_replyf("\r\nFS_OK %08x\r\n", (unsigned)crc);
}

/* ── Worker operations ───────────────────────────────────────────────── */

static void do_roots(void)
{
    ensure_spiflash();
    size_t total = 0, used = 0;
    if (esp_littlefs_info("assets", &total, &used) == ESP_OK) {
        fx_replyf("FS_ROOTS: %s %u %u\r\n", FX_ROOT_ASSETS, (unsigned)total, (unsigned)used);
    } else {
        fx_replyf("FS_ROOTS: %s - -\r\n", FX_ROOT_ASSETS);
    }

    uint64_t t64 = 0, f64 = 0;
    if (s_wl != WL_INVALID_HANDLE &&
        esp_vfs_fat_info(FX_ROOT_SPIFLASH, &t64, &f64) == ESP_OK) {
        fx_replyf("FS_ROOTS: %s %llu %llu\r\n", FX_ROOT_SPIFLASH,
                  (unsigned long long)t64, (unsigned long long)(t64 - f64));
    } else {
        fx_replyf("FS_ROOTS: %s - -\r\n", FX_ROOT_SPIFLASH);
    }

    if (esp_vfs_fat_info(FX_ROOT_SDCARD, &t64, &f64) == ESP_OK) {
        fx_replyf("FS_ROOTS: %s %llu %llu\r\n", FX_ROOT_SDCARD,
                  (unsigned long long)t64, (unsigned long long)(t64 - f64));
    } else {
        fx_replyf("FS_ROOTS: %s - -\r\n", FX_ROOT_SDCARD);
    }
    fx_reply("FS_END\r\n");
}

static void do_list(const char *path)
{
    if (!root_ready(path)) { fx_reply("FS_ERR mount\r\n"); return; }
    DIR *d = opendir(path);
    if (!d) { fx_replyf("FS_ERR opendir_%d\r\n", errno); return; }

    /* No entry count in the header: producing one means walking the directory
     * twice, and off the SD card that is two of the slowest calls here. The
     * host reads until FS_END. */
    char full[FX_PATH_MAX + 258];
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        struct stat st = {};
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        const bool isDir = (e->d_type == DT_DIR);
        long size = 0;
        if (!isDir && stat(full, &st) == 0) size = (long)st.st_size;
        fx_replyf("FS_LIST: %c %ld %s\r\n", isDir ? 'D' : 'F', size, e->d_name);
    }
    closedir(d);
    fx_reply("FS_END\r\n");
}

static void do_stat(const char *path)
{
    if (!root_ready(path)) { fx_reply("FS_ERR mount\r\n"); return; }
    struct stat st = {};
    if (stat(path, &st) != 0) { fx_replyf("FS_ERR stat_%d\r\n", errno); return; }
    fx_replyf("FS_STAT %ld %c\r\n", (long)st.st_size, S_ISDIR(st.st_mode) ? 'D' : 'F');
}

static void do_delete(const char *path)
{
    if (!root_ready(path)) { fx_reply("FS_ERR mount\r\n"); return; }
    struct stat st = {};
    if (stat(path, &st) != 0) { fx_replyf("FS_ERR stat_%d\r\n", errno); return; }
    const int rc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (rc != 0) { fx_replyf("FS_ERR delete_%d\r\n", errno); return; }
    fx_reply("OK\r\n");
}

static void do_mkdir(const char *path)
{
    if (!root_ready(path)) { fx_reply("FS_ERR mount\r\n"); return; }
    if (mkdir(path, 0777) != 0) { fx_replyf("FS_ERR mkdir_%d\r\n", errno); return; }
    fx_reply("OK\r\n");
}

/* Device -> host. The CRC goes in the trailer, not the header: putting it in
 * the header would mean reading the whole file twice, and a 4 MB WAV off the
 * SD card is not something to read twice for a checksum the host can compute
 * as it receives. */
static void do_get(const char *path, uint32_t off, uint32_t want)
{
    if (!root_ready(path)) { fx_reply("FS_ERR mount\r\n"); return; }
    struct stat st = {};
    if (stat(path, &st) != 0) { fx_replyf("FS_ERR stat_%d\r\n", errno); return; }
    if (S_ISDIR(st.st_mode))  { fx_reply("FS_ERR is_dir\r\n"); return; }

    const uint32_t fileSize = (uint32_t)st.st_size;
    if (off > fileSize) { fx_reply("FS_ERR bad_offset\r\n"); return; }
    uint32_t len = fileSize - off;
    if (want && want < len) len = want;

    FILE *f = fopen(path, "rb");
    if (!f) { fx_replyf("FS_ERR open_%d\r\n", errno); return; }
    if (off && fseek(f, (long)off, SEEK_SET) != 0) {
        fclose(f);
        fx_reply("FS_ERR seek\r\n");
        return;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(FX_IO_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t *)malloc(FX_IO_BUF);
    if (!buf) { fclose(f); fx_reply("FS_ERR nomem\r\n"); return; }

    fx_replyf("FS_DATA %u\r\n", (unsigned)len);

    uint32_t crc = 0, sent = 0;
    bool ok = true;
    while (sent < len) {
        const size_t want_n = (len - sent) < FX_IO_BUF ? (len - sent) : FX_IO_BUF;
        const size_t got = fread(buf, 1, want_n, f);
        if (got == 0) { ok = false; break; }
        if (!fx_write_raw(buf, got)) { ok = false; break; }
        crc = esp_crc32_le(crc, buf, got);
        sent += got;
    }
    fclose(f);

    /* The header promised <len> bytes and the host is counting them, so a read
     * that came up short still has to produce them — otherwise the host waits
     * for bytes that never arrive and the next command lands in the middle of
     * a payload. Pad, then say what actually came off the disk. */
    if (!ok) {
        memset(buf, 0, FX_IO_BUF);
        for (uint32_t pad = sent; pad < len; ) {
            const size_t n = (len - pad) < FX_IO_BUF ? (len - pad) : FX_IO_BUF;
            if (!fx_write_raw(buf, n)) break;
            pad += n;
        }
    }
    free(buf);

    if (!ok) fx_replyf("\r\nFS_TRUNC %u/%u\r\n", (unsigned)sent, (unsigned)len);
    else     fx_replyf("\r\nFS_OK %08x\r\n", (unsigned)crc);
}

static void put_cleanup(bool removeTmp)
{
    if (s_put_f) { fclose(s_put_f); s_put_f = nullptr; }
    if (removeTmp && s_put_tmp[0]) unlink(s_put_tmp);
    s_put_tmp[0] = '\0';
    s_put_final[0] = '\0';
    s_put_crc = 0;
    s_put_written = 0;
}

/* Opening the destination is the one filesystem call the transfer needs before
 * the pipe goes binary, and on the SD card it takes the same transaction lock
 * a kit load holds — seconds, on the loop that answers every other query. So
 * the drain loop only flips the framing (deterministically, before the host
 * can send a byte) and the worker does the open, answering FS_READY or the
 * error and putting the framing back if it failed. */
static void do_put_open(const char *path, uint32_t size, uint32_t crc);

static void do_put_data(uint8_t *slot, uint32_t n)
{
    if (!s_put_f) {
        /* The transfer already failed and said so; the host is still draining
         * the chunk that was in flight. Return the slot and stay quiet. */
        xSemaphoreGive(s_slots);
        return;
    }
    bool ok = fwrite(slot, 1, n, s_put_f) == n;
    if (ok) {
        s_put_crc = esp_crc32_le(s_put_crc, slot, n);
        s_put_written += n;
    }
    xSemaphoreGive(s_slots);
    if (!ok) {
        ESP_LOGE(TAG, "FS_PUT write failed at %u", (unsigned)s_put_written);
        put_cleanup(true);
        fx_reply("FS_ERR write\r\n");
        return;
    }
    fx_replyf("OK %u/%u\r\n", (unsigned)s_put_written, (unsigned)s_total);
}

/* Host -> device completion. The payload was streamed straight to a .part
 * file: a partial file on a filesystem costs one file, unlike a partial
 * partition image, so it does not need staging in RAM — but it is only given
 * its real name once the CRC matches. */
static void do_put_done(uint32_t wantCrc)
{
    if (!s_put_f) { fx_reply("FS_ERR no_transfer\r\n"); return; }
    fclose(s_put_f);
    s_put_f = nullptr;

    if (s_put_crc != wantCrc) {
        ESP_LOGE(TAG, "FS_PUT crc mismatch: got %08x want %08x",
                 (unsigned)s_put_crc, (unsigned)wantCrc);
        char got[16];
        snprintf(got, sizeof(got), "%08x", (unsigned)s_put_crc);
        put_cleanup(true);
        fx_replyf("FS_ERR crc_%s\r\n", got);
        return;
    }

    unlink(s_put_final);   /* FATFS f_rename refuses an existing destination */
    if (rename(s_put_tmp, s_put_final) != 0) {
        const int e = errno;
        put_cleanup(true);
        fx_replyf("FS_ERR rename_%d\r\n", e);
        return;
    }
    const uint32_t crc = s_put_crc;
    put_cleanup(false);
    fx_replyf("FS_OK %08x\r\n", (unsigned)crc);
}

static void fx_worker_task(void *)
{
    fx_req_t r;
    for (;;) {
        if (xQueueReceive(s_req_q, &r, portMAX_DELAY) != pdTRUE) continue;
        switch (r.op) {
            case OP_ROOTS:   do_roots();                      break;
            case OP_PUT_OPEN: do_put_open(r.path, r.a, r.b);  break;
            case OP_LIST:    do_list(r.path);                 break;
            case OP_STAT:    do_stat(r.path);                 break;
            case OP_DELETE:  do_delete(r.path);               break;
            case OP_MKDIR:   do_mkdir(r.path);                break;
            case OP_GET:     do_get(r.path, r.a, r.b);        break;
            case OP_ASSETS_READ: do_assets_read(r.a, r.b);    break;
            case OP_PUT_DATA:  do_put_data(r.ptr, r.a);       break;
            case OP_PUT_DONE:  do_put_done(r.b);              break;
            case OP_PUT_ABORT: put_cleanup(true);             break;
            case OP_ASSETS_WRITE: assets_write_slot(r.ptr, r.a); break;
            case OP_ASSETS_DONE:  assets_finish(r.b);             break;
            default: break;
        }
    }
}

/* Created on first use, then parked on its queue forever. A board that never
 * transfers a file pays nothing for this; one that does pays for it once. */
static bool worker_ready(void)
{
    if (s_worker) return true;
    if (!s_req_q) s_req_q = xQueueCreate(4, sizeof(fx_req_t));
    if (!s_slots) s_slots = xSemaphoreCreateCounting(FX_SLOTS, FX_SLOTS);
    if (!s_req_q || !s_slots) return false;
    /* Priority 4: above the priority-1 drain loop that feeds it, below the
     * audio tasks it must never stall. */
    if (xTaskCreatePinnedToCore(fx_worker_task, "fx_worker", FX_WORKER_STACK, nullptr, 4,
                                &s_worker, 0) != pdPASS) {
        s_worker = nullptr;
        ESP_LOGE(TAG, "worker task create failed");
        return false;
    }
    return true;
}

/* The stack has to be internal RAM (the worker writes SPI flash, and a task on
 * a PSRAM stack cannot survive the cache being disabled for that), so this is
 * the one verb family that fails when the board is out of it — with BLE up
 * the launcher sits at ~8 kB free, largest block under 4 kB. Saying so in the
 * reply turns "no_worker" from a mystery into "stop BLE first". */
static void reply_no_worker(const char *prefix)
{
    fx_replyf("%s no_worker int_largest=%u need=%u\r\n", prefix,
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
              (unsigned)FX_WORKER_STACK);
}

static bool post(const fx_req_t *r)
{
    return s_req_q && xQueueSend(s_req_q, r, pdMS_TO_TICKS(1000)) == pdTRUE;
}

/* ── Stream teardown ─────────────────────────────────────────────────── */

static void stream_free(void)
{
    if (s_a_page0) { free(s_a_page0); s_a_page0 = nullptr; }
}

/* The staging slots are allocated once and kept. Freeing them at the end of a
 * transfer would mean the drain loop releasing a buffer the worker may still
 * be writing from — two threads and a double buffer make that a lifetime
 * question with no cheap answer. 8 KB of PSRAM, on a board that has 8 MB of
 * it, buys the answer "never". */
static bool slots_alloc(void)
{
    s_slot_next = 0;
    s_slot_fill = 0;
    for (int i = 0; i < FX_SLOTS; i++) {
        if (s_slot_buf[i]) continue;
        s_slot_buf[i] = (uint8_t *)heap_caps_malloc(FX_CHUNK, MALLOC_CAP_SPIRAM);
        if (!s_slot_buf[i]) s_slot_buf[i] = (uint8_t *)malloc(FX_CHUNK);
        if (!s_slot_buf[i]) return false;
    }
    return true;
}

/* Hand the filled slot to the worker. Blocks only until the worker has
 * finished the previous slot, which under the lockstep ack it already has —
 * the host does not send chunk N+1 before the "OK" that this write produced. */
/* The slot is reserved by slot_reserve() before a byte is copied into it, so
 * this only hands it over. Taking the semaphore here instead would let the
 * producer write into a slot the worker is still reading from — two slots in
 * flight and a slow destination is all it takes, and the corruption only shows
 * up as a CRC mismatch at the end of a transfer. */
static bool slot_flush(uint8_t op)
{
    if (s_slot_fill == 0) return true;

    fx_req_t r = {};
    r.op  = op;
    r.a   = (uint32_t)s_slot_fill;
    r.ptr = s_slot_buf[s_slot_next];
    if (!post(&r)) { xSemaphoreGive(s_slots); return false; }

    s_slot_next = (uint8_t)((s_slot_next + 1) % FX_SLOTS);
    s_slot_fill = 0;
    s_slot_held = false;
    return true;
}

/* Claim the slot about to be filled. Blocks while both are still with the
 * worker, which is what stops the host from outrunning the card. */
static bool slot_reserve(void)
{
    if (s_slot_held) return true;
    if (xSemaphoreTake(s_slots, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
    s_slot_held = true;
    return true;
}

/* ── Command handling (drain loop) ───────────────────────────────────── */

static bool handle_assets_begin(const char *cmd)
{
    unsigned size = 0, crc = 0;
    char flag[8] = {0};
    const int got = sscanf(cmd + 13, "%u %x %7s", &size, &crc, flag);
    if (got < 2 || size == 0) {
        fx_reply("ASSETS_ERROR usage\r\n");
        return true;
    }
    const esp_partition_t *part = assets_partition();
    if (!part)             { fx_reply("ASSETS_ERROR no_partition\r\n"); return true; }
    if (size > part->size) { fx_replyf("ASSETS_ERROR too_big_%u\r\n", (unsigned)part->size); return true; }
    if (!worker_ready())   { reply_no_worker("ASSETS_ERROR"); return true; }
    /* ASSETS_OK restarts the board, and a restart before the running image
     * confirmed itself rolls it back to the other slot. */
    if (fw_running_on_trial()) { fx_reply("ASSETS_ERROR trial\r\n"); return true; }

    if (!slots_alloc()) {
        fx_replyf("ASSETS_ERROR nomem_%u\r\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return true;
    }
    s_a_page0 = (uint8_t *)heap_caps_malloc(FX_CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_a_page0) s_a_page0 = (uint8_t *)malloc(FX_CHUNK);
    if (!s_a_page0) { fx_reply("ASSETS_ERROR nomem_page0\r\n"); return true; }
    s_slot_fill = 0;
    s_slot_held = false;
    s_a_off = 0;
    s_a_crc = 0;
    s_a_erased = false;

    s_total  = size;
    s_recv   = 0;
    s_want_crc = crc;
    s_force  = (strncmp(flag, "FORCE", 5) == 0);
    s_last_rx_us = esp_timer_get_time();
    s_state  = FX_RX_ASSETS;
    ESP_LOGW(TAG, "assets stream started, size=%u crc=%08x force=%d",
             size, crc, (int)s_force);
    fx_reply("ASSETS_READY\r\n");
    return true;
}

/* Worker: create the .part file and hand the framing decision back to the
 * host by answering FS_READY (or FS_ERR, which also puts the pipe back into
 * text mode — the host has sent nothing yet either way). */
static void do_put_open(const char *path, uint32_t size, uint32_t crc)
{
    if (!root_ready(path)) {
        s_state = FX_IDLE;
        fx_reply("FS_ERR mount\r\n");
        return;
    }
    snprintf(s_put_final, sizeof(s_put_final), "%s", path);
    snprintf(s_put_tmp, sizeof(s_put_tmp), "%s.part", path);
    unlink(s_put_tmp);
    s_put_f = fopen(s_put_tmp, "wb");
    if (!s_put_f) {
        const int e = errno;
        s_put_tmp[0] = s_put_final[0] = '\0';
        s_state = FX_IDLE;
        fx_replyf("FS_ERR open_%d\r\n", e);
        return;
    }
    s_put_crc = 0;
    s_put_written = 0;
    ESP_LOGI(TAG, "FS_PUT %s size=%u crc=%08x", s_put_final, (unsigned)size, (unsigned)crc);
    fx_reply("FS_READY\r\n");
}

static bool handle_fs_put(const char *cmd)
{
    unsigned size = 0, crc = 0;
    int consumed = 0;
    /* The path is the rest of the line, not a %s token: kit names carry
     * spaces ("Dilla Pyth", "Keczup_kit (1)") and a token-wise parse silently
     * addressed a different file. Everything that needs splitting is numeric
     * and comes first. */
    if (sscanf(cmd + 7, "%u %x %n", &size, &crc, &consumed) != 2 || size == 0 || consumed == 0) {
        fx_reply("FS_ERR usage\r\n");
        return true;
    }
    const char *path = cmd + 7 + consumed;
    if (*path == '\0') { fx_reply("FS_ERR usage\r\n"); return true; }
    if (!path_ok(path))  { fx_reply("FS_ERR bad_path\r\n"); return true; }
    if (!worker_ready()) { reply_no_worker("FS_ERR"); return true; }
    if (s_put_f)         { fx_reply("FS_ERR busy\r\n"); return true; }
    if (!slots_alloc())  { fx_reply("FS_ERR nomem\r\n"); return true; }

    fx_req_t r = {};
    r.op = OP_PUT_OPEN;
    r.a  = size;
    r.b  = crc;
    if (!copy_path(r.path, sizeof(r.path), path)) {
        fx_reply("FS_ERR bad_path\r\n");
        return true;
    }

    s_total = size;
    s_recv  = 0;
    s_slot_fill = 0;
    s_slot_held = false;
    s_want_crc = crc;
    s_last_rx_us = esp_timer_get_time();
    /* Set before the open is even attempted: the framing must not depend on
     * when the worker gets scheduled. The host cannot send a payload byte
     * before FS_READY, and a failed open puts this back to FX_IDLE. */
    s_state = FX_RX_FILE;
    if (!post(&r)) {
        s_state = FX_IDLE;
        fx_reply("FS_ERR busy\r\n");
    }
    return true;
}

bool fx_handle_cdc(const char *buff, size_t len)
{
    char cmd[CDC_LINE_MAX];
    const size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
    memcpy(cmd, buff, n);
    cmd[n] = '\0';

    if (strncmp(cmd, "ASSETS_BEGIN ", 13) == 0) return handle_assets_begin(cmd);
    if (strncmp(cmd, "FS_PUT ", 7) == 0)        return handle_fs_put(cmd);

    fx_req_t r = {};

    const char *arg = nullptr;

    if (strncmp(cmd, "ASSETS_DUMP", 11) == 0) {
        unsigned off = 0, want = 0;
        sscanf(cmd + 11, "%u %u", &off, &want);
        r.op = OP_ASSETS_READ;
        r.a = off;
        r.b = want;
    } else if (strncmp(cmd, "FS_ROOTS", 8) == 0) {
        r.op = OP_ROOTS;   /* the worker mounts, this loop does no I/O */
    } else if (strncmp(cmd, "FS_LIST ", 8) == 0) {
        r.op = OP_LIST;  arg = cmd + 8;
    } else if (strncmp(cmd, "FS_STAT ", 8) == 0) {
        r.op = OP_STAT;  arg = cmd + 8;
    } else if (strncmp(cmd, "FS_DELETE ", 10) == 0) {
        r.op = OP_DELETE; arg = cmd + 10;
    } else if (strncmp(cmd, "FS_MKDIR ", 9) == 0) {
        r.op = OP_MKDIR; arg = cmd + 9;
    } else if (strncmp(cmd, "FS_GET ", 7) == 0) {
        /* FS_GET <off> <len> <path> — the two numbers lead so the path can be
         * the rest of the line, spaces and all. 0 0 reads the whole file. */
        unsigned off = 0, want = 0;
        int consumed = 0;
        if (sscanf(cmd + 7, "%u %u %n", &off, &want, &consumed) != 2 || consumed == 0) {
            fx_reply("FS_ERR usage\r\n");
            return true;
        }
        r.op = OP_GET;
        r.a = off;
        r.b = want;
        if (!copy_path(r.path, sizeof(r.path), cmd + 7 + consumed)) {
            fx_reply("FS_ERR bad_path\r\n");
            return true;
        }
    } else {
        return false;
    }

    if (arg && !copy_path(r.path, sizeof(r.path), arg)) {
        fx_reply("FS_ERR bad_path\r\n");
        return true;
    }

    if (r.op != OP_ROOTS && r.op != OP_ASSETS_READ && !path_ok(r.path)) {
        fx_reply("FS_ERR bad_path\r\n"); return true;
    }
    if (!worker_ready())                      { reply_no_worker("FS_ERR"); return true; }
    if (!post(&r))                            { fx_reply("FS_ERR busy\r\n"); return true; }
    return true;
}

/* ── Binary mode ─────────────────────────────────────────────────────── */

bool fx_stream_active(void)
{
    return s_state != FX_IDLE;
}

static void stream_fail(const char *reason)
{
    const bool assets = (s_state == FX_RX_ASSETS);
    s_state = FX_IDLE;
    if (s_slot_held) { xSemaphoreGive(s_slots); s_slot_held = false; }
    stream_free();
    if (!assets) {
        fx_req_t r = {};
        r.op = OP_PUT_ABORT;
        post(&r);
    }
    fx_replyf("%s %s\r\n", assets ? "ASSETS_ERROR" : "FS_ERR", reason);
    crosspad::getPadAnimator().playError();
}

void fx_process_data(const uint8_t *data, size_t len)
{
    if (s_state == FX_IDLE) return;
    s_last_rx_us = esp_timer_get_time();

    size_t off = 0;
    while (off < len && s_state != FX_IDLE) {
        const size_t remaining = s_total - s_recv;
        if (remaining == 0) break;

        if (!slot_reserve()) { stream_fail("stall"); return; }
        size_t take = FX_CHUNK - s_slot_fill;
        if (take > len - off)   take = len - off;
        if (take > remaining)   take = remaining;
        memcpy(s_slot_buf[s_slot_next] + s_slot_fill, data + off, take);
        s_slot_fill += take;
        s_recv      += take;
        off         += take;
        if (s_slot_fill == FX_CHUNK || s_recv >= s_total) {
            const uint8_t op = (s_state == FX_RX_ASSETS) ? OP_ASSETS_WRITE : OP_PUT_DATA;
            if (!slot_flush(op)) { stream_fail("stall"); return; }
        }
    }

    if (s_recv < s_total) return;

    /* ── Completion ── */
    fx_req_t r = {};
    r.op = (s_state == FX_RX_ASSETS) ? OP_ASSETS_DONE : OP_PUT_DONE;
    r.b  = s_want_crc;
    const bool assets = (s_state == FX_RX_ASSETS);
    s_state = FX_IDLE;
    if (!post(&r)) fx_reply(assets ? "ASSETS_ERROR busy\r\n" : "FS_ERR busy\r\n");
    /* The slots are still in flight to the worker; it gives the semaphore back
     * as it consumes them. Freeing them here would pull the buffer out from
     * under a write in progress, so they are released on the next allocation
     * instead — slots_alloc() starts from a clean stream_free(). */
}

void fx_abort_timeout(void)
{
    if (s_state == FX_IDLE) return;
    if (esp_timer_get_time() - s_last_rx_us < FX_STALL_US) return;
    ESP_LOGE(TAG, "transfer timed out at %u/%u", (unsigned)s_recv, (unsigned)s_total);
    stream_fail("timeout");
}
