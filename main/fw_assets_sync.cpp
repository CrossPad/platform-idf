#include "include/fw_assets_sync.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "include/file_transfer.h"
#include "include/fw_image_desc.h"
#include "include/fw_library.h"
#include "include/fw_slots.h"

static const char *TAG = "fw_assets_sync";

/* Internal RAM: fw_assets_sync_body() opens a package (fw_library_find() ->
 * fill_package(), the same esp_app_desc_t + path-buffer shape fw_install_start()
 * measured as too much for a small stack — see hil_control.cpp's own comment
 * on FW_INSTALL) plus fopen/fread of up to ~1 MB. app_main()'s own task is 4 kB
 * internal RAM (CONFIG_ESP_MAIN_TASK_STACK_SIZE) — nowhere near enough for that
 * chain run inline, so the real work happens on a dedicated task, the same
 * shape fw_switch_finish_task() in fw_slots.cpp uses for its own settings
 * import. */
static constexpr uint32_t kAssetsSyncStackBytes = 8192;

static constexpr const char *kAssetsMarkerPath = "/assets/.assets_id";

/* "<FW_PKG_ROOT>/<pkg.name>/assets.bin" — the one file this module joins onto
 * a package directory. */
static constexpr size_t kAssetsPathMax =
    sizeof(FW_PKG_ROOT) + 1 + FW_PKG_NAME_LEN + 1 + sizeof("assets.bin");

/* Fixed literal ("Icons are from another ... insert the card with ") is 54 B,
 * plus up to FW_PKG_NAME_LEN - 1 for the package name: 128 covers the worst
 * case with margin, the way main.cpp's own rollback toast buffer does for its
 * two package names. */
static bool s_toastPending = false;
static char s_toastMsg[128];

static void to_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* Trims a trailing newline collect_assets.cmake's file(WRITE ...) does not
 * add, but a hand-edited marker (or a future writer) might. */
static bool read_marker(char *out, size_t outLen)
{
    FILE *f = fopen(kAssetsMarkerPath, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, outLen - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return n > 0;
}

static void remount_assets(void)
{
    /* Two attempts rather than one with format_if_mount_failed on, so a
     * format is something this can report: it wipes every icon, font and the
     * bundled STM32 image, and the only way back is a host. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/assets",
        .partition_label = "assets",
        .partition = NULL,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    if (esp_vfs_littlefs_register(&conf) == ESP_OK) return;

    conf.format_if_mount_failed = true;
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "assets: remount failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGE(TAG, "assets: the partition would not mount and was formatted - icons, fonts and "
                  "stm32_fw.bin are gone until tools/fs_transfer.py assets");
}

static const FwSlotInfo *running_slot_of(const FwSlotInfo slots[2])
{
    if (slots[0].running) return &slots[0];
    if (slots[1].running) return &slots[1];
    return nullptr;
}

static FwAssetsResult fw_assets_sync_body(void)
{
    const fw_image_desc_t *running = fw_image_desc_running();
    char wantedHex[FW_ASSETS_ID_LEN * 2 + 1];
    to_hex(running->assets_id, FW_ASSETS_ID_LEN, wantedHex);

    char haveHex[FW_ASSETS_ID_LEN * 2 + 8];
    if (read_marker(haveHex, sizeof(haveHex)) && strcmp(haveHex, wantedHex) == 0) {
        ESP_LOGI(TAG, "assets: match");
        return FwAssetsResult::Match;
    }

    FwSlotInfo slots[2];
    fw_slots_get(slots);
    const FwSlotInfo *runningSlot = running_slot_of(slots);

    FwPackage pkg;
    const bool haveSource = runningSlot && runningSlot->package[0] &&
                            fw_library_find(runningSlot->package, &pkg) &&
                            pkg.hasAssets &&
                            memcmp(pkg.assetsId, running->assets_id, FW_ASSETS_ID_LEN) == 0;
    if (!haveSource) {
        /* A slot with no package name was written by an OTA over USB (the
         * browser updater, ota_flash.py): there is no package to insert, the
         * host that flashed it is where the assets come from. */
        if (runningSlot && runningSlot->package[0]) {
            snprintf(s_toastMsg, sizeof(s_toastMsg),
                     "Icons are from another firmware - insert the card with %s", runningSlot->package);
        } else {
            snprintf(s_toastMsg, sizeof(s_toastMsg),
                     "Icons are from an older firmware - update them at play.crosspad.app/device");
        }
        s_toastPending = true;
        ESP_LOGW(TAG, "assets: mismatch, no source package on the card");
        return FwAssetsResult::MismatchNoSource;
    }

    char path[kAssetsPathMax];
    fw_library_path(pkg, "assets.bin", path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "assets: cannot stat %s", path);
        return FwAssetsResult::Failed;
    }
    const uint32_t size = (uint32_t)st.st_size;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "assets: no PSRAM for a %u byte staging buffer (largest free %u)",
                 (unsigned)size, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return FwAssetsResult::Failed;
    }

    FILE *f = fopen(path, "rb");
    const size_t got = f ? fread(buf, 1, size, f) : 0;
    if (f) fclose(f);
    if (got != size) {
        ESP_LOGE(TAG, "assets: short read from %s (%u/%u)", path, (unsigned)got, (unsigned)size);
        heap_caps_free(buf);
        return FwAssetsResult::Failed;
    }

    const uint32_t crc = esp_crc32_le(0, buf, size);

    esp_vfs_littlefs_unregister("assets");
    const bool wrote = fx_assets_write_image(buf, size, crc);
    heap_caps_free(buf);
    remount_assets();

    if (!wrote) {
        ESP_LOGE(TAG, "assets: rewrite from %s failed", path);
        return FwAssetsResult::Failed;
    }

    char confirmHex[FW_ASSETS_ID_LEN * 2 + 8];
    if (read_marker(confirmHex, sizeof(confirmHex)) && strcmp(confirmHex, wantedHex) == 0) {
        ESP_LOGW(TAG, "assets: rewritten from %s (%u bytes)", pkg.name, (unsigned)size);
        return FwAssetsResult::Rewritten;
    }
    ESP_LOGE(TAG, "assets: rewritten but the marker still does not match");
    return FwAssetsResult::Failed;
}

namespace {
struct FwAssetsSyncArgs {
    FwAssetsResult result;
    SemaphoreHandle_t done;
};
}  // namespace

static void fw_assets_sync_task(void *arg)
{
    auto *args = static_cast<FwAssetsSyncArgs *>(arg);
    args->result = fw_assets_sync_body();
    ESP_LOGI(TAG, "fw_assets_sync_task stack high-water mark: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    xSemaphoreGive(args->done);
    vTaskDeleteWithCaps(nullptr);
}

FwAssetsResult fw_assets_sync(void)
{
    FwAssetsSyncArgs args;
    args.result = FwAssetsResult::Failed;
    args.done = xSemaphoreCreateBinary();
    if (!args.done) {
        ESP_LOGE(TAG, "assets: no semaphore for the sync task");
        return FwAssetsResult::Failed;
    }

    if (xTaskCreatePinnedToCoreWithCaps(fw_assets_sync_task, "fw_assets_sync", kAssetsSyncStackBytes,
                                        &args, 1, nullptr, 0,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "assets: no task for the sync");
        vSemaphoreDelete(args.done);
        return FwAssetsResult::Failed;
    }

    xSemaphoreTake(args.done, portMAX_DELAY);
    vSemaphoreDelete(args.done);
    return args.result;
}

bool fw_assets_sync_toast_pending(char *buf, size_t len)
{
    if (!s_toastPending) return false;
    s_toastPending = false;
    strlcpy(buf, s_toastMsg, len);
    return true;
}
