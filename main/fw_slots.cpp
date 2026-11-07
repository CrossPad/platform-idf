#include "include/fw_slots.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <atomic>

#include <ArduinoJson.h>

#include "esp_app_desc.h"
#include "esp_bootloader_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

#include "esp_lvgl_port.h"

#include "include/lv_async_safe.h"
#include "stm32_hal_init.h"
#include "engine_handoff.h"
#include "NvsSettingsStore.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/settings/SettingsIo.hpp"
#include "crosspad/settings/SettingsScope.hpp"
#include "crosspad-sampler/logic/kit_load_task.hpp"

static const char *TAG = "fw_slots";

/* Not declared in a header (fw_library.cpp and sample_player_port.cpp both
 * forward-declare it locally); every bulk reader of the card yields to the
 * streamer between pieces of work, and hashing a whole package image before
 * the engine is stopped is exactly that. */
extern void sample_player_port_yield_to_streamer(uint32_t weight);

static constexpr const char *kFwSlotsNs = "fw_slots";

/* Read/write in this many bytes at a time — both while hashing an image
 * against its manifest and while streaming it into the OTA slot. */
static constexpr size_t  kInstallChunk      = 4096;
static constexpr uint8_t kMinBatteryPercent = 20;

/* Internal RAM: this task's own frame stays small (paths, a JsonDocument
 * handle, loop counters) — the 4 kB read buffer lives in PSRAM instead (see
 * fw_install_task()), so this does not have to hold it. Measured on the
 * bench per the dispatch: see task-5-report.md. */
static constexpr uint32_t kInstallStackBytes = 6144;

/* "<FW_PKG_ROOT>/<pkg.name>/<file>" — sized for the longest filename this
 * module joins onto a package directory ("manifest.json"). */
static constexpr size_t kInstallPathMax =
    sizeof(FW_PKG_ROOT) + 1 + FW_PKG_NAME_LEN + 1 + sizeof("manifest.json");

/* ── NVS keys ────────────────────────────────────────────────────────── */

static const char *key_pkg(char label)   { return label == 'A' ? "a_pkg"   : "b_pkg"; }
static const char *key_sha(char label)   { return label == 'A' ? "a_sha"   : "b_sha"; }
static const char *key_state(char label) { return label == 'A' ? "a_state" : "b_state"; }

/* Set by fw_switch_request() right before it moves the boot partition, read
 * back by fw_switch_finish_boot() on the next boot — the one slot the two
 * calls agree on across the restart in between. */
static const char *kKeyPending = "pending";

static char partition_label(const esp_partition_t *p)
{
    return (p && p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? 'B' : 'A';
}

/* ── Install status: written by fw_install_task(), polled by FW_INSTALL ── */

static FwInstallStatus s_status = { FwInstallPhase::Idle, 0, "" };

static SemaphoreHandle_t status_lock()
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

static void set_status(FwInstallPhase phase, uint8_t percent, const char *error)
{
    SemaphoreHandle_t lock = status_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    s_status.phase = phase;
    s_status.percent = percent;
    if (error) strlcpy(s_status.error, error, sizeof(s_status.error));
    else       s_status.error[0] = '\0';
    xSemaphoreGive(lock);
}

FwInstallStatus fw_install_status(void)
{
    SemaphoreHandle_t lock = status_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    FwInstallStatus copy = s_status;
    xSemaphoreGive(lock);
    return copy;
}

/* CDC and the GUI each keep their own busy flag, so neither can see the
 * other's request: this is what makes "is anything running?" and "arm it"
 * one decision for both fw_install_start() and fw_switch_request(). Distinct
 * from status_lock(), which only guards the status struct's own bytes —
 * these bodies read that status while holding this. */
static SemaphoreHandle_t op_lock()
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

namespace {
struct OpLockGuard {
    OpLockGuard()  { xSemaphoreTake(op_lock(), portMAX_DELAY); }
    ~OpLockGuard() { xSemaphoreGive(op_lock()); }
};
}  // namespace

/* State only, pkg/sha left alone — the Trial/Confirmed/RolledBack
 * transitions below never touch what's installed, only how it's trusted. */
static void slot_set_state(char label, FwSlotState state)
{
    nvs_handle_t h;
    if (nvs_open(kFwSlotsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key_state(label), static_cast<uint8_t>(state));
    nvs_commit(h);
    nvs_close(h);
}

static FwSlotState slot_get_state(char label)
{
    nvs_handle_t h;
    uint8_t st = static_cast<uint8_t>(FwSlotState::Empty);
    if (nvs_open(kFwSlotsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key_state(label), &st);
        nvs_close(h);
    }
    return static_cast<FwSlotState>(st);
}

static void slot_get_pkg(char label, char *out, size_t outLen)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(kFwSlotsNs, NVS_READONLY, &h) == ESP_OK) {
        size_t len = outLen;
        nvs_get_str(h, key_pkg(label), out, &len);
        nvs_close(h);
    }
}

/* Set by fw_slots_init() when the other slot is found freshly rolled back;
 * cleared (read-once) by fw_rollback_toast_pending(). GUI-agnostic on
 * purpose — this module has no LVGL dependency of its own. */
static bool s_rollbackToastPending = false;
static char s_rollbackToastFailedPkg[FW_PKG_NAME_LEN] = "";
static char s_rollbackToastRunningPkg[FW_PKG_NAME_LEN] = "";

/* ── Registry ────────────────────────────────────────────────────────── */

/* The non-running slot's kit and version while it holds an image that can
 * boot (Installed, Trial or Confirmed), for fw_slots_other_holds(). A copy in
 * RAM because the updater asks from its PSRAM-stack job, which must not read
 * flash; refreshed wherever the registry or that slot's image changes on an
 * internal stack: boot, the end of an install, a cleared rollback. */
static portMUX_TYPE s_otherMux = portMUX_INITIALIZER_UNLOCKED;
static char s_otherKit[FW_KIT_ID_LEN];
static char s_otherVersion[sizeof(FwSlotInfo::version)];

static void other_slot_refresh(void);

void fw_slots_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return;
    const char label = partition_label(running);

    uint8_t actualSha[32];
    if (esp_partition_get_sha256(running, actualSha) != ESP_OK) {
        ESP_LOGW(TAG, "could not hash the running partition");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(kFwSlotsNs, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t storedSha[32];
    size_t shaLen = sizeof(storedSha);
    const bool matches = nvs_get_blob(h, key_sha(label), storedSha, &shaLen) == ESP_OK &&
                          shaLen == sizeof(storedSha) &&
                          memcmp(storedSha, actualSha, sizeof(actualSha)) == 0;
    if (!matches) {
        /* An OTA outside fw_install_start() (plain OTA_BEGIN/ota_flash.py)
         * put this image here — its package name, if any, is unknown. */
        nvs_erase_key(h, key_pkg(label));
        nvs_set_blob(h, key_sha(label), actualSha, sizeof(actualSha));
        ESP_LOGI(TAG, "slot %c: running image changed, package cleared", label);
    }
    nvs_commit(h);
    nvs_close(h);

    /* Did the switch that got us here (if any) actually get staged as a
     * trial? Only a bootloader built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
     * ever sets ESP_OTA_IMG_PENDING_VERIFY — an older one hands the image
     * straight to VALID/UNDEFINED (or leaves it at the NEW this firmware's
     * own esp_ota_write() always writes), so a slot stuck at Trial with no
     * PENDING_VERIFY this boot has nothing left to trial: nothing to do here,
     * fw_confirm_running() promotes the genuine PENDING_VERIFY case. Also
     * corrects a stale RolledBack left over from a previous boot's registry:
     * whatever this slot's own history, it is running right now, which is
     * proof by itself that it isn't the one that "did not start last time". */
    esp_ota_img_states_t runSt;
    if (esp_ota_get_state_partition(running, &runSt) == ESP_OK) {
        if (runSt == ESP_OTA_IMG_PENDING_VERIFY) {
            /* A switch is not the only way to land here: a plain OTA over CDC
             * (ota_flash.py, OTA_BEGIN) also sets the boot partition, so its
             * image boots PENDING_VERIFY too and this boot really is a trial —
             * the registry said `installed` through the whole window, which is
             * what let a reset inside it roll the image back unannounced. */
            if (slot_get_state(label) != FwSlotState::Trial) {
                slot_set_state(label, FwSlotState::Trial);
            }
        } else {
            const FwSlotState st = slot_get_state(label);
            if (st == FwSlotState::Trial || st == FwSlotState::RolledBack) {
                slot_set_state(label, FwSlotState::Confirmed);
            }
        }
    }

    /* The other slot rolled back on its own since the last time this ran —
     * queue one toast for the GUI and record it in the registry so
     * fw_switch_request() refuses to switch back into it. Never the running
     * partition itself: a manual UART reflash (tools/bench.py recover) writes
     * a blank ota_data_initial.bin straight over whatever otadata a stuck
     * trial left behind, and the bootloader's own "no history" first-boot
     * path (ota_has_initial_contents) can still leave esp_ota_get_last_invalid_partition()
     * naming the slot that just booted successfully — a running slot can
     * never be the one that "did not start last time". */
    const esp_partition_t *lastInvalid = esp_ota_get_last_invalid_partition();
    if (lastInvalid && lastInvalid != running) {
        esp_ota_img_states_t invSt;
        if (esp_ota_get_state_partition(lastInvalid, &invSt) == ESP_OK &&
            (invSt == ESP_OTA_IMG_INVALID || invSt == ESP_OTA_IMG_ABORTED)) {
            const char otherLabel = partition_label(lastInvalid);
            if (slot_get_state(otherLabel) != FwSlotState::RolledBack) {
                slot_get_pkg(otherLabel, s_rollbackToastFailedPkg, sizeof(s_rollbackToastFailedPkg));
                slot_get_pkg(label, s_rollbackToastRunningPkg, sizeof(s_rollbackToastRunningPkg));
                slot_set_state(otherLabel, FwSlotState::RolledBack);
                s_rollbackToastPending = true;
            }
        }
    }
    other_slot_refresh();
}

static void fill_slot(const esp_partition_t *part, char label, FwSlotInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->label = label;
    info->state = FwSlotState::Empty;
    if (!part) return;
    info->running = part == esp_ota_get_running_partition();

    nvs_handle_t h;
    if (nvs_open(kFwSlotsNs, NVS_READONLY, &h) == ESP_OK) {
        uint8_t st = static_cast<uint8_t>(FwSlotState::Empty);
        if (nvs_get_u8(h, key_state(label), &st) == ESP_OK) info->state = static_cast<FwSlotState>(st);
        size_t len = sizeof(info->package);
        nvs_get_str(h, key_pkg(label), info->package, &len);
        nvs_close(h);
    }

    esp_app_desc_t app;
    if (esp_ota_get_partition_description(part, &app) == ESP_OK) {
        strlcpy(info->version, app.version, sizeof(info->version));
    }

    fw_image_desc_t desc;
    if (esp_partition_read(part, FW_DESC_FILE_OFFSET, &desc, sizeof(desc)) == ESP_OK &&
        desc.magic == FW_DESC_MAGIC && desc.desc_version == FW_DESC_VERSION) {
        strlcpy(info->kit, desc.kit_id, sizeof(info->kit));
        info->hasDesc = true;
        info->boardRev = desc.board_rev;
    }
}

void fw_slots_get(FwSlotInfo out[2])
{
    const esp_partition_t *a =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const esp_partition_t *b =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    fill_slot(a, 'A', &out[0]);
    fill_slot(b, 'B', &out[1]);
}

static void other_slot_refresh(void)
{
    FwSlotInfo slots[2];
    fw_slots_get(slots);
    const FwSlotInfo &other = slots[slots[0].running ? 1 : 0];
    const bool bootable = other.state == FwSlotState::Installed || other.state == FwSlotState::Trial ||
                          other.state == FwSlotState::Confirmed;
    portENTER_CRITICAL(&s_otherMux);
    strlcpy(s_otherKit, bootable ? other.kit : "", sizeof(s_otherKit));
    strlcpy(s_otherVersion, bootable ? other.version : "", sizeof(s_otherVersion));
    portEXIT_CRITICAL(&s_otherMux);
}

bool fw_slots_other_holds(const char *kit, const char *version)
{
    char k[sizeof(s_otherKit)];
    char v[sizeof(s_otherVersion)];
    portENTER_CRITICAL(&s_otherMux);
    memcpy(k, s_otherKit, sizeof(k));
    memcpy(v, s_otherVersion, sizeof(v));
    portEXIT_CRITICAL(&s_otherMux);
    const char *bare = v[0] == 'v' ? v + 1 : v;   // image versions carry the tag's 'v', catalog ones do not
    if (version[0] == 'v') ++version;
    return k[0] && bare[0] && strcmp(k, kit) == 0 && strcmp(bare, version) == 0;
}

void fw_slot_clear_rollback(char label)
{
    if (slot_get_state(label) != FwSlotState::RolledBack) return;
    slot_set_state(label, FwSlotState::Installed);
    other_slot_refresh();
    ESP_LOGW(TAG, "slot %c: rolled-back state cleared, it may be tried again", label);
}

static void slot_store(char label, const char *pkgName, const uint8_t sha[32], FwSlotState state)
{
    nvs_handle_t h;
    if (nvs_open(kFwSlotsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key_pkg(label), pkgName);
    nvs_set_blob(h, key_sha(label), sha, 32);
    nvs_set_u8(h, key_state(label), static_cast<uint8_t>(state));
    nvs_commit(h);
    nvs_close(h);
}

/* ── Install ─────────────────────────────────────────────────────────── */

/* Same source stm32_fw_update.c uses for its own "battery >= 20% or
 * charger" gate: no second reading of the charge state. */
static bool battery_too_low(void)
{
    stm32_charger_telemetry_t chg;
    return stm32_hal_get_charger_telemetry(&chg) == ESP_OK &&
           !stm32_charger_present(&chg) && chg.soc_pct < kMinBatteryPercent;
}

static bool sha256_matches(const char *path, const char *hexWant, uint8_t *chunk)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    size_t n;
    while ((n = fread(chunk, 1, kInstallChunk, f)) > 0) {
        mbedtls_sha256_update(&ctx, chunk, n);
        if (n == kInstallChunk) sample_player_port_yield_to_streamer(1);
    }
    fclose(f);

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return strcasecmp(hex, hexWant) == 0;
}

/* Set by fw_install_start() right before the task is created; read once at
 * the top of fw_install_task(). Safe unguarded: the phase check in
 * fw_install_start() refuses a second install before a new value could ever
 * be written here while a task is still reading the old one. */
static FwPackage s_installPkg;

/* Set alongside s_installPkg: whether the package just handed to
 * fw_install_start() was built against an older STM32 protocol than the one
 * currently running. Informational only — it never refuses the install, it
 * just gives FW_INSTALL's CDC reply and the GUI confirm dialog something to
 * warn about before the switch that would actually apply it. */
static bool s_installStmProtoOlder = false;

static void reload_kit_async_cb(void *arg)
{
    crosspad_sampler::kit_load_task_start(false, static_cast<int>(reinterpret_cast<intptr_t>(arg)));
}

/* How hard reload_current_kit() tries to reach the LVGL thread. It runs on the
 * install task with nothing waiting on it, and the alternative to waiting is
 * silent pads until the user changes kit by hand — so a held display lock (a
 * long render, or a kit load of its own finishing) is worth sitting out. */
static constexpr int      kReloadAttempts   = 10;
static constexpr uint32_t kReloadRetryMs    = 100;
static constexpr uint32_t kReloadLockWaitMs = 3000;

/* marcel_engine_stop() is a DeInit plus an arena reset: the engine comes back
 * from marcel_engine_start() empty, with no samples on any pad. Every other
 * stop/start pair (SamplerPlatformPort.cpp) sits inside the kit reload loop, which puts them
 * back; here the reload has to be asked for. kit_load_task_start() must run on
 * the LVGL thread, hence the async hop — taken here through lvgl_port_lock()
 * + lv_async_call() rather than lv_async_call_locked(), whose void return
 * gives this no way to know the call was ever placed. */
static void reload_current_kit(void)
{
    auto *mgr = crosspad::getKitManager();
    if (!mgr) {
        ESP_LOGW(TAG, "no kit manager: the pads stay silent until the next kit change");
        return;
    }
    const int kitId = mgr->getCurrentKitId();
    if (kitId < 0) {
        ESP_LOGI(TAG, "no kit was current, nothing to reload after the engine restart");
        return;
    }

    ESP_LOGI(TAG, "reloading kit %d after the engine restart", kitId);
    void *arg = reinterpret_cast<void *>(static_cast<intptr_t>(kitId));
    for (int attempt = 0; attempt < kReloadAttempts; attempt++) {
        if (lvgl_port_lock(kReloadLockWaitMs)) {
            lv_async_call(reload_kit_async_cb, arg);
            lvgl_port_unlock();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(kReloadRetryMs));
    }
    ESP_LOGE(TAG, "could not reach the LVGL thread in %d attempts: kit %d not reloaded, "
                  "the pads are silent until the next kit change", kReloadAttempts, kitId);
}

static void fw_install_task(void *)
{
    const FwPackage pkg = s_installPkg;
    /* Whichever engine owns the sample-playback arena right now is the one
     * to hand back afterwards. A pitched kit leaves marcel_engine_running()
     * false because its bank, not the prebuilt streamer, holds the arena —
     * starting marcel back up in that case would tear the pitched bank out
     * from under it, so this task must never call marcel_engine_start()
     * unless it is the one that called marcel_engine_stop(). */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    const bool wasRunning = marcel_engine_running();
#else
    /* The native engine plays on through an install, as it did beside the
     * prebuilt streamer (which was all this ever stopped): it holds no
     * library a DeInit could tear down, and it reads the card through the
     * same pacing every other reader does. */
    const bool wasRunning = false;
    (void)wasRunning;
#endif
    bool stopped = false;

    uint8_t *chunk = static_cast<uint8_t *>(heap_caps_malloc(kInstallChunk, MALLOC_CAP_SPIRAM));
    if (!chunk) {
        set_status(FwInstallPhase::Failed, 0, "no PSRAM for the install buffer");
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    auto abort_with = [&](const char *why) {
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        if (wasRunning) marcel_engine_start();
#endif
        if (stopped) reload_current_kit();
        heap_caps_free(chunk);
        other_slot_refresh();   // the write may have begun: the slot's old image is gone
        set_status(FwInstallPhase::Failed, 0, why);
        vTaskDeleteWithCaps(nullptr);
    };

    /* Step 1: Verifying — the image is the authority; a manifest.json that
     * disagrees with what fw_image_desc_read_file() already read out of the
     * image itself means the package directory holds a mismatched pair.
     *
     * Read through the install task's own chunk buffer (plain FILE*, not
     * std::ifstream): a manifest is a few hundred bytes, and kit_load_task.cpp
     * measured std::ifstream + JsonDocument together at close to 7 kB of
     * stack for the same small-file-into-JsonDocument shape — more than this
     * task's whole internal-RAM stack. deserializeJson() copies every string
     * it keeps into the document's own pool, so reusing `chunk` for the raw
     * image bytes afterwards is safe. */
    char manifestPath[kInstallPathMax];
    fw_library_path(pkg, "manifest.json", manifestPath, sizeof(manifestPath));
    if (access(manifestPath, F_OK) == 0) {
        FILE *mf = fopen(manifestPath, "rb");
        const size_t mlen = mf ? fread(chunk, 1, kInstallChunk - 1, mf) : 0;
        if (mf) fclose(mf);
        chunk[mlen] = '\0';

        JsonDocument doc;
        /* mlen == kInstallChunk - 1: the file may have been larger than the
         * buffer and got truncated — a real manifest is tiny, so treat that
         * the same as a parse failure rather than judge a cut-off document. */
        if (!mf || mlen == 0 || mlen == kInstallChunk - 1 ||
            deserializeJson(doc, reinterpret_cast<const char *>(chunk), mlen)) {
            abort_with("manifest disagrees with the image");
            return;
        }
        const int manifestBoardRev = doc["board_rev"] | -1;
        const char *manifestKit = doc["kit"] | "";
        if (manifestBoardRev != static_cast<int>(pkg.boardRev) || strcmp(manifestKit, pkg.kit) != 0) {
            abort_with("manifest disagrees with the image");
            return;
        }
        const char *wantHex = doc["files"]["CrossPad.bin"]["sha256"] | "";
        if (wantHex[0]) {
            char imagePath[kInstallPathMax];
            fw_library_path(pkg, "CrossPad.bin", imagePath, sizeof(imagePath));
            if (!sha256_matches(imagePath, wantHex, chunk)) {
                abort_with("image does not match its manifest");
                return;
            }
        }
    }

    /* Step 2 */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (wasRunning) {
        marcel_engine_stop();
        stopped = true;
    }
#endif

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        abort_with("no OTA slot to install into");
        return;
    }

    char imagePath[kInstallPathMax];
    fw_library_path(pkg, "CrossPad.bin", imagePath, sizeof(imagePath));
    FILE *f = fopen(imagePath, "rb");
    if (!f) {
        abort_with("cannot open the image");
        return;
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(target, pkg.imageSize, &handle);
    if (err != ESP_OK) {
        fclose(f);
        char why[64];
        snprintf(why, sizeof(why), "esp_ota_begin: %s", esp_err_to_name(err));
        abort_with(why);
        return;
    }

    /* Step 3: Writing */
    set_status(FwInstallPhase::Writing, 0, nullptr);
    size_t done = 0;
    size_t n;
    int chunksSinceYield = 0;
    while ((n = fread(chunk, 1, kInstallChunk, f)) > 0) {
        err = esp_ota_write(handle, chunk, n);
        if (err != ESP_OK) break;
        done += n;
        const uint8_t percent = pkg.imageSize ? static_cast<uint8_t>(done * 100 / pkg.imageSize) : 100;
        set_status(FwInstallPhase::Writing, percent, nullptr);
        if (++chunksSinceYield >= 16) {
            vTaskDelay(1);
            chunksSinceYield = 0;
        }
    }
    fclose(f);

    if (err != ESP_OK || done != pkg.imageSize) {
        esp_ota_abort(handle);
        char why[64];
        if (err != ESP_OK) snprintf(why, sizeof(why), "esp_ota_write: %s", esp_err_to_name(err));
        else                strlcpy(why, "short read from the card", sizeof(why));
        abort_with(why);
        return;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        char why[64];
        snprintf(why, sizeof(why), "esp_ota_end: %s", esp_err_to_name(err));
        abort_with(why);
        return;
    }

    /* Step 4 — the target slot's registry entry. The boot partition is not
     * touched: only fw_switch_request() ever moves it. */
    uint8_t sha[32];
    esp_partition_get_sha256(target, sha);
    slot_store(partition_label(target), pkg.name, sha, FwSlotState::Installed);
    other_slot_refresh();

    /* Step 5 */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (wasRunning) marcel_engine_start();
#endif
    if (stopped) reload_current_kit();
    heap_caps_free(chunk);
    set_status(FwInstallPhase::Done, 100, nullptr);
    vTaskDeleteWithCaps(nullptr);
}

bool fw_package_stm_proto_older(const FwPackage &pkg)
{
    stm32_link_info_t stmLink;
    return stm32_hal_get_link_info(&stmLink) == ESP_OK && pkg.stmProtoMin < stmLink.proto_version;
}

bool fw_install_start(const char *packageName, bool allowUnknownBuild, const char **error)
{
    /* Held across the whole body: the phase read below and the
     * set_status(Verifying) that arms this install are one decision, and the
     * two callers (CDC, GUI) have no shared flag of their own to make it
     * with. Also excludes a concurrent fw_switch_request(). */
    OpLockGuard guard;

    /* A refusal that names a condition of *this* request writes the reason
     * into the status, because that is where the GUI's poll timer reads it.
     * "Already running" is the exception: the status then belongs to the
     * install that is actually running, and overwriting its error field
     * would make that one look broken. */
    auto refuse = [&](const char *why, bool publish) {
        if (error) *error = why;
        if (publish) set_status(FwInstallPhase::Idle, 0, why);
        return false;
    };

    const FwInstallStatus cur = fw_install_status();
    if (cur.phase == FwInstallPhase::Verifying || cur.phase == FwInstallPhase::Writing) {
        return refuse("install already running", false);
    }

    FwPackage pkg;
    if (!fw_library_find(packageName, &pkg)) return refuse("no such package", true);

    if (pkg.verdict != FwPkgVerdict::Ok &&
        !(pkg.verdict == FwPkgVerdict::UnknownBuild && allowUnknownBuild)) {
        return refuse(crosspad::fwPkgVerdictText(pkg.verdict), true);
    }

    if (battery_too_low()) return refuse("battery low, no charger", true);

    /* A kit load is a long run of guarded per-pad calls into the sample
     * library; stopping the engine between two of them tears the library
     * down underneath the loader. */
    if (crosspad_sampler::kit_load_task_is_loading()) return refuse("kit loading", true);

    s_installStmProtoOlder = fw_package_stm_proto_older(pkg);

    s_installPkg = pkg;
    set_status(FwInstallPhase::Verifying, 0, nullptr);

    /* Stack is internal RAM on purpose (see kInstallStackBytes) — the 4 kB
     * read buffer this task needs is allocated from PSRAM once it is
     * running, not carried on the stack. */
    if (xTaskCreatePinnedToCoreWithCaps(fw_install_task, "fw_install", kInstallStackBytes,
                                        nullptr, 1, nullptr, 0,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        return refuse("no memory for the install task", true);
    }
    return true;
}

bool fw_install_stm_proto_older(void)
{
    return s_installStmProtoOlder;
}

/* ── Switch ──────────────────────────────────────────────────────────── */

/* The kits a recovery goes back to: the sampler firmwares. */
static constexpr const char *kFwMainKits[] = {"full", "lite"};

static bool is_main_kit(const char *kit)
{
    for (const char *k : kFwMainKits)
        if (strcmp(kit, k) == 0) return true;
    return false;
}

/* fw_switch_request() and fw_recovery_switch(): a recovery leaves a slot that
 * may not have confirmed itself (it is the one that does not work) for one
 * holding a main kit, whatever that slot's last start did. */
static bool switch_to_other(bool recovery, const char **error)
{
    /* The install-phase read below and everything that follows it are one
     * decision, against a fw_install_start() that may be running on another
     * task — the same mutex it takes. */
    OpLockGuard guard;

    auto refuse = [&](const char *why) { if (error) *error = why; return false; };

    /* fw_install_task() writes the very partition "the other slot" names —
     * a stale FwSlotState read mid-write (state is only stored after
     * esp_ota_end()) must not read as anything but "not now". */
    const FwInstallStatus inst = fw_install_status();
    if (inst.phase == FwInstallPhase::Verifying || inst.phase == FwInstallPhase::Writing) {
        return refuse("install running");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target) return refuse("no other slot");

    /* Leaving a slot that has not confirmed itself yet hands the bootloader a
     * still-PENDING_VERIFY image on its next look at it: it marks the one that
     * was working ABORTED and this device ends up with a RolledBack slot it
     * never had a reason to distrust. */
    const esp_partition_t *runningPart = esp_ota_get_running_partition();
    esp_ota_img_states_t runSt;
    if (!recovery && runningPart && esp_ota_get_state_partition(runningPart, &runSt) == ESP_OK &&
        runSt == ESP_OTA_IMG_PENDING_VERIFY) {
        return refuse("running slot not confirmed yet");
    }

    FwSlotInfo slots[2];
    fw_slots_get(slots);
    const FwSlotInfo &running = slots[0].running ? slots[0] : slots[1];
    const FwSlotInfo &other   = slots[0].running ? slots[1] : slots[0];

    if (recovery && is_main_kit(running.kit))   return refuse("the main firmware is running");
    if (recovery && !is_main_kit(other.kit))    return refuse("the other slot holds no main firmware");
    if (other.state == FwSlotState::Empty)      return refuse("other slot is empty");
    if (!recovery && other.state == FwSlotState::RolledBack) return refuse("other slot did not start last time");
    /* An image that came over USB was never judged like a card package: one
     * built for another revision would boot on the wrong pinout. */
    if (other.hasDesc && other.boardRev != fw_image_desc_running()->board_rev)
        return refuse("other slot is for another board");

    /* Best-effort: the leaving firmware's own settings ride along in its own
     * package directory. No directory (an unnamed, OTA-flashed slot) means
     * nowhere on the card to put them — nothing to export. */
    if (running.package[0]) {
        char dir[kInstallPathMax];
        snprintf(dir, sizeof(dir), "%s/%s", FW_PKG_ROOT, running.package);
        if (access(dir, F_OK) == 0) {
            /* Built straight from FW_PKG_ROOT + running.package, not from
             * `dir`: GCC bounds a %s through a fixed-size array member fine
             * (running.package is FW_PKG_NAME_LEN), but not through a buffer
             * it can only see is *up to* kInstallPathMax long — the same
             * format-truncation trap fw_library_path() already avoids. */
            char path[kInstallPathMax];
            snprintf(path, sizeof(path), "%s/%s/settings.json", FW_PKG_ROOT, running.package);
            crosspad::SettingsIoResult res =
                crosspad::settingsExportAll(*crosspad::CrosspadSettings::getInstance(), path);
            if (!res.ok) {
                ESP_LOGW(TAG, "settings export before switch failed: %s",
                         res.error ? res.error : "?");
            }
        }
    }

    /* Last, and only once the boot partition has actually moved: a failure
     * here used to leave `pending` pointing at a firmware that never boots
     * and the other slot claiming a Trial it never got. */
    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return refuse("cannot set boot partition");
    }

    nvs_handle_t h;
    if (nvs_open(kFwSlotsNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, kKeyPending, other.package);
        nvs_commit(h);
        nvs_close(h);
    }

    /* A freshly installed slot is about to get its first boot — that's the
     * trial. A slot that was already Confirmed (switching back to firmware
     * this device has run and confirmed before) gets no trial: its otadata
     * is already VALID, so the bootloader stages no PENDING_VERIFY for it
     * either. */
    if (other.state == FwSlotState::Installed) {
        slot_set_state(other.label, FwSlotState::Trial);
    }

    /* Committed: the caller restarts once it has told the host, never here —
     * this module has no CDC of its own to flush first. */
    return true;
}

bool fw_switch_request(const char **error) { return switch_to_other(false, error); }

bool fw_recovery_switch(const char **error) { return switch_to_other(true, error); }

namespace {
struct FwSwitchFinishArgs {
    char path[kInstallPathMax];
    SemaphoreHandle_t done;
};
}  // namespace

static void fw_switch_finish_task(void *arg)
{
    auto *args = static_cast<FwSwitchFinishArgs *>(arg);
    auto *settings = crosspad::CrosspadSettings::getInstance();
    crosspad::SettingsIoResult res = crosspad::settingsImportPerFirmware(*settings, args->path);
    if (res.ok) {
        settings->saveTo(NvsSettingsStore::getInstance());
    } else {
        ESP_LOGW(TAG, "per-firmware settings import failed: %s", res.error ? res.error : "?");
    }
    xSemaphoreGive(args->done);
    vTaskDeleteWithCaps(nullptr);
}

void fw_switch_finish_boot(void)
{
    nvs_handle_t h;
    if (nvs_open(kFwSlotsNs, NVS_READWRITE, &h) != ESP_OK) return;

    char pending[FW_PKG_NAME_LEN] = {0};
    size_t len = sizeof(pending);
    const bool hasPending = nvs_get_str(h, kKeyPending, pending, &len) == ESP_OK;

    if (hasPending && pending[0]) {
        FwSlotInfo slots[2];
        fw_slots_get(slots);
        const FwSlotInfo &running = slots[0].running ? slots[0] : slots[1];

        if (strcmp(pending, running.package) == 0) {
            FwSwitchFinishArgs args;
            snprintf(args.path, sizeof(args.path), "%s/%s/settings.json", FW_PKG_ROOT, running.package);
            if (access(args.path, F_OK) == 0) {
                args.done = xSemaphoreCreateBinary();
                if (args.done) {
                    /* settingsImportPerFirmware() runs ArduinoJson + stdio through
                     * the same shape fw_install_task()'s manifest read needed a
                     * dedicated task for above — app_main()'s own task is 4 kB
                     * internal RAM (sdkconfig CONFIG_ESP_MAIN_TASK_STACK_SIZE),
                     * nowhere near enough. Internal RAM, not PSRAM, for the
                     * stack itself: this task's tail is settings->saveTo(), a
                     * flash write, and a PSRAM-backed stack faults the instant
                     * its own call frame reaches one — see fw_switch_cmd_task()
                     * in hil_control.cpp for the coredump that first showed it
                     * (task=fw_switch_cmd, cause=0, vaddr=0x0). app_main() blocks
                     * on the handoff so a changed Kit is in place before the
                     * first load. */
                    if (xTaskCreatePinnedToCoreWithCaps(fw_switch_finish_task, "fw_switch_finish",
                                                        8192, &args, 1, nullptr, 0,
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
                        xSemaphoreTake(args.done, portMAX_DELAY);
                    } else {
                        ESP_LOGW(TAG, "no task for the per-firmware settings import");
                    }
                    vSemaphoreDelete(args.done);
                }
            }
        }
    }

    nvs_erase_key(h, kKeyPending);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Confirm ─────────────────────────────────────────────────────────── */

/* esp_ota_mark_app_valid_cancel_rollback() writes otadata — flash — directly
 * on the calling task, the same shape that coredumped fw_switch_cmd_task in
 * Task 6 (a PSRAM-backed stack faults the instant its own call frame reaches
 * a flash write's critical section, which disables the cache that stack
 * needs for every push/pop). The caller here can be an esp_timer callback
 * (esp_timer task, stack capability not this module's to assume) or the CDC
 * drain loop, so fw_confirm_running() never does the write itself — it
 * always hands off to a dedicated internal-RAM-stack task, making it safe to
 * call from anywhere. */
static std::atomic<bool> s_confirmBusy{false};

/* The periodic confirm timer main.cpp arms, handed over so the confirm task
 * can stop and delete it once there is nothing left to confirm. Periodic
 * rather than one-shot because the task creation below can fail (internal RAM
 * is tight with BLE up) — a dropped one-shot meant the image never confirmed,
 * the trial watchdog reset the board 30 s in, and a perfectly good slot was
 * quarantined as RolledBack. */
static esp_timer_handle_t s_confirmTimer = nullptr;

void fw_confirm_set_timer(esp_timer_handle_t timer)
{
    s_confirmTimer = timer;
}

/* Never from the timer's own callback: esp_timer re-arms a periodic timer
 * after the callback returns, so it must outlive it. The confirm task is a
 * task of its own, which is what makes this safe. */
static void confirm_timer_release(void)
{
    esp_timer_handle_t timer = s_confirmTimer;
    if (!timer) return;
    s_confirmTimer = nullptr;
    esp_timer_stop(timer);
    esp_timer_delete(timer);
}

/* Trial-boot safety net: this many seconds after fw_trial_watchdog_arm() (the
 * very first statement of app_main(), while the running image is still
 * PENDING_VERIFY), the RTC watchdog resets the chip if fw_confirm_running()
 * has not disabled it by then — a hang, a crash loop, or bsp_stm32_init()
 * failing (so the confirm timer in main.cpp is never armed) all end here
 * instead of stranding the board on a bad image forever. Comfortably above
 * the confirm timer's own kConfirmDelayMs (10 s, main.cpp) so a healthy boot
 * always confirms well before this fires. */
static constexpr uint32_t kTrialWatchdogTimeoutS = 30;

/* True only between fw_trial_watchdog_arm() actually arming and the next
 * trial_watchdog_disable(); what makes fw_trial_watchdog_feed() a no-op on
 * every boot that is not a trial. */
static bool s_trialWatchdogArmed = false;

/* Written at the top of app_main() and by the confirm task, read by the
 * firmware updater's worker job: atomic, unlike s_trialWatchdogArmed. */
static std::atomic<bool> s_onTrial{false};

/* Clears both trigger paths: the master enable bit (wdt_config0.en) that
 * wdt_hal_disable() gates the running countdown on, and flashboot_en, a
 * second and independent trigger — rwdt_ll_disable()'s own doc says as much
 * ("does not disable the flashboot mode ... a timeout can still occur if
 * flashboot mode is simultaneously enabled"). wdt_hal_feed() resets the
 * counter so a leftover in-flight countdown cannot reach either stage's
 * timeout after this returns. */
static void trial_watchdog_disable(void)
{
    wdt_hal_context_t ctx;
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_set_flashboot_en(&ctx, false);
    wdt_hal_feed(&ctx);
    wdt_hal_disable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    s_trialWatchdogArmed = false;
}

/* Trial-boot safety net: stage 0 of the RTC watchdog, armed only while the
 * running image is PENDING_VERIFY, reset by fw_confirm_running() once the
 * image proves itself (see kTrialWatchdogTimeoutS above). Only the normal
 * enable bit is set here — flashboot_en is deliberately left off: it is a
 * second, independent trigger that bypasses the enable bit entirely, so
 * disabling it later only ever clears the enable bit, not flashboot_en, and
 * a stray flashboot-mode countdown then fires on its own regardless of a
 * successful confirm. (flashboot_en is the bootloader's own flash-boot-phase
 * protection — esp_restart_noos() sets it right before jumping back into the
 * bootloader, which clears it again before the next app ever runs; app code
 * arming a trial window has no business touching it.) The unconditional
 * clear below runs before the PENDING_VERIFY check so any state leaked by a
 * previous boot's failed disable — including a still-set flashboot_en, which
 * plain wdt_hal_disable() never touched — can never reach into this boot. */
void fw_trial_watchdog_arm(void)
{
    trial_watchdog_disable();

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!running || esp_ota_get_state_partition(running, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    s_onTrial.store(true);

    wdt_hal_context_t ctx;
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    const uint32_t timeoutTicks = kTrialWatchdogTimeoutS * rtc_clk_slow_freq_get_hz();
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, timeoutTicks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    s_trialWatchdogArmed = true;
}

bool fw_running_on_trial(void)
{
    return s_onTrial.load();
}

void fw_trial_watchdog_feed(void)
{
    if (!s_trialWatchdogArmed) return;
    wdt_hal_context_t ctx;
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

static void fw_confirm_task(void *)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    bool pending = running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
                   st == ESP_OTA_IMG_PENDING_VERIFY;
    if (pending) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            /* Only now: an image that cannot mark itself valid must keep the
             * watchdog, or the retries below would run forever on an
             * unconfirmable image with nothing left to end the boot. */
            trial_watchdog_disable();
            s_onTrial.store(false);
            slot_set_state(partition_label(running), FwSlotState::Confirmed);
            ESP_LOGI(TAG, "slot %c confirmed", partition_label(running));
            pending = false;
        } else {
            /* Still PENDING_VERIFY and still watched: the next tick tries
             * again, and if none of them ever succeeds the 30 s RTC watchdog
             * resets the chip and the bootloader rolls back. */
            ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
        }
    }
    if (!pending) confirm_timer_release();
    s_confirmBusy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void fw_confirm_running(void)
{
    if (s_confirmBusy.exchange(true)) return;
    if (xTaskCreatePinnedToCoreWithCaps(fw_confirm_task, "fw_confirm", 4096,
                                        nullptr, 1, nullptr, 0,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "no task for the confirm write, retrying on the next tick");
        s_confirmBusy.store(false);
    }
}

/* CONFIG_BOOTLOADER_PROJECT_VER travels with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 * (sdkconfig.defaults/.v1/.v2) — bumped the same release rollback was turned
 * on, so a bootloader reporting at least this version is one built with
 * rollback support. Deterministic: unlike an NVS-history flag, this never
 * depends on whether a trial has actually run on this particular device. */
static constexpr uint32_t kMinRollbackBootloaderVer = 2;

bool fw_rollback_supported(void)
{
    esp_bootloader_desc_t desc;
    if (esp_ota_get_bootloader_description(nullptr, &desc) != ESP_OK) return false;
    return desc.version >= kMinRollbackBootloaderVer;
}

bool fw_rollback_toast_pending(char *failedPkg, size_t failedLen, char *runningPkg, size_t runningLen)
{
    if (!s_rollbackToastPending) return false;
    s_rollbackToastPending = false;
    strlcpy(failedPkg, s_rollbackToastFailedPkg, failedLen);
    strlcpy(runningPkg, s_rollbackToastRunningPkg, runningLen);
    return true;
}
