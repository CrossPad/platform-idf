/**
 * @file fw_updater.cpp
 * @brief Firmware update checks, downloads and installs -- see fw_updater.h.
 */
#include "fw_updater.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "crosspad/fw/FwCatalog.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/fw/FwKit.hpp"
#include "crosspad/fw/FwSchedule.hpp"
#include "crosspad/fw/Semver.hpp"
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/ui/kit_selector.hpp"
#include "bsp_worker.h"
#include "fw_catalog.h"
#include "fw_download.h"
#include "fw_flow_overlay.h"
#include "fw_image_desc.h"
#include "fw_library.h"
#include "fw_slots.h"
#include "fw_store_files.h"
#include "host_info.h"
#include "net_mode.h"
#include "usb_transfer.h"
#include "wifi_link.h"

extern crosspad::CrosspadStatus status;

static const char* TAG = "fw_updater";

static constexpr const char* kNs = "fwupd";
static constexpr const char* kKeyLatest = "latest";
static constexpr const char* kKeyLastCheck = "last_check";
static constexpr const char* kKeyChecked = "checked";
static constexpr const char* kKeyUrl = "url";
static constexpr const char* kKeyBeta = "beta_kits";
static constexpr const char* kKeyBoot = "boot_check";
static constexpr const char* kKeyRbKit = "rb_kit";
static constexpr const char* kKeyRbVersion = "rb_version";
static constexpr const char* kDefaultUrl = "https://crosspad.app";
static constexpr const char* kLeaseReason = "fw_update";
static constexpr const char* kRefusalBusy = "busy";
static constexpr char kHttp[] = "http://";
static constexpr char kHttps[] = "https://";
static constexpr size_t kEntriesMax = crosspad::kFwCatalogKits * crosspad::kFwCatalogVersions;
/* A persisted version is compared against core's, so the buffers must match. */
static_assert(FWU_VERSION_LEN == crosspad::kFwVersionMax, "FWU_VERSION_LEN must equal core's kFwVersionMax");
static_assert(FWU_KIT_LEN == crosspad::kFwKitMax, "FWU_KIT_LEN must equal core's kFwKitMax");
/* Longest wait for a running settings writer before a reset erases the keys. */
static constexpr uint32_t kForgetWaitMs = 500;
static constexpr uint32_t kForgetPollMs = 10;

/* PSRAM: the job is HTTP, TLS and FATFS and touches no flash (NVS goes
 * through persist()), and internal RAM is
 * what WiFi needs. Sized for esp_http_client's TLS handshake; FW_UPD_STATUS
 * stack_free= shows what the last job left unused. */
static constexpr uint32_t kJobStackBytes = 8192;
static constexpr UBaseType_t kJobPrio = 1;
static constexpr BaseType_t kJobCore = 0;
static constexpr uint32_t kWifiUpTimeoutMs = 20000;
/* A background check looks at its cancel flag this often while the link comes up. */
static constexpr uint32_t kWifiUpSliceMs = 250;
static constexpr uint32_t kInstallPollMs = 250;
/* fw_install_start() looks the package up (an image header off the card),
 * judges it against the next OTA partition and reads the charger over the STM
 * link: internal stack, like the settings writer. Transient, so the size is
 * what the CDC install command's own 8 kB showed to be comfortable. */
static constexpr uint32_t kInstallStartStackBytes = 6144;
static constexpr uint32_t kInstallStartPollMs = 20;
static constexpr uint32_t kPercent = 100;
/* The idle watch only reads state; once a second is plenty against a 2 min idle rule. */
static constexpr uint32_t kIdleJobPeriodMs = 1000;
static constexpr int64_t kUsPerS = 1000000;
/* An NVS write stalls the flash cache: never from a PSRAM stack, and never
 * long enough to hold up the LVGL thread or the CDC loop that asked for it. */
static constexpr uint32_t kPersistStackBytes = 3072;
static constexpr UBaseType_t kPersistPrio = 2;

enum class JobKind : uint8_t { Check, Download };
struct Job {
    JobKind kind;
    bool background;  // a trigger's, not the user's: abandoned on activity, never offers anything
    bool leaseFirst;  // the job's first step takes a WiFi lease (a check, a package seen missing)
    bool lease;       // a WiFi lease is held for it; the job task takes it and releases it
    bool install;
    char kit[FWU_KIT_LEN];
    char version[FWU_VERSION_LEN];
    char slotPkg[2][FW_PKG_NAME_LEN];   // filled by start_job(), see there
    uint32_t gen;     // s_forgetGen when the job was started: a forget() since makes it stale
};

static std::atomic<uint8_t> s_state{FWU_IDLE};
/* Bumped by fw_updater_forget(): a job started before it must not publish,
 * persist or show anything of its own afterwards. */
static std::atomic<uint32_t> s_forgetGen{0};
static std::atomic<bool> s_busy{false};
static std::atomic<uint32_t> s_stackFree{0};
static std::atomic<uint8_t> s_installPct{0};
static FwDlProgress s_dl;

struct InstallStart {
    char name[FW_PKG_NAME_LEN];
    char why[FW_INSTALL_ERROR_LEN];
    bool ok;
    std::atomic<bool> done;
};
static InstallStart s_installStart;
static std::atomic<uint32_t> s_instStackFree{0};
static std::atomic<bool> s_persistDirty{false};
static std::atomic<bool> s_persistRunning{false};
static bool s_publishLatest;   // under s_lock: a check finished, host_info takes its result
static Job s_job;
static bool s_bootCheck = true;                        // under s_lock
/* A background job is silent: while s_jobBackground is set its errors are
 * logged and nothing it does reaches the state, error or progress a user sees,
 * bar the result of a check that completed. s_cancel is honoured only through
 * cancelled(), which also tests s_job.background, so a stale set can never
 * abandon a user's job. */
static std::atomic<bool> s_cancel{false};
static std::atomic<bool> s_jobBackground{false};
static std::atomic<int64_t> s_lastAttemptUs{-1};       // this boot's last check, esp_timer time
static std::atomic<bool> s_lastAttemptFailed{false};
static std::atomic<int64_t> s_lastPadUs{0};
static int64_t s_launcherSinceUs;                      // the idle job's own, one worker thread
/* The boot check of a trial boot, held until the image has confirmed itself:
 * a crash in the check (the radio, TLS) would otherwise roll a good image back. */
static std::atomic<bool> s_bootCheckHeld{false};
/* main.cpp: the boot's kit scan has finished. The boot check waits for it:
 * the WiFi radio and the scan's file handles asked internal RAM for the same
 * moment, and newlib aborted on a FILE lock it could not have (kit_scan,
 * lock_init_generic, about one boot in five -- 2026-10-28). */
extern std::atomic<bool> g_kit_scan_done;
/* main.cpp: a boot that mounted no card has probed it once more. A card that
 * came up a second late was mounted by the hotplug probe and rescanned with
 * the boot check's radio up -- the same abort, in kit_rescan (2026-10-30). */
extern std::atomic<bool> g_sd_probed;
static bsp_worker_job_t* s_idleJob;

static SemaphoreHandle_t s_lock;
static char s_err[FWU_ERROR_LEN];
static char s_latest[FWU_VERSION_LEN];
static char s_target[FWU_VERSION_LEN];
static char s_targetKit[FWU_KIT_LEN];
static char s_ready[FWU_VERSION_LEN];
static char s_url[FWU_URL_LEN];
static char s_betaKits[crosspad::kFwBetaListMax];
/* The kit and version the other slot last rolled back from (it did not start):
 * a check does not offer that version again, also after the slot was
 * overwritten -- hence persisted. Under s_lock. */
static char s_rbKit[FWU_KIT_LEN];
static char s_rbVersion[FWU_VERSION_LEN];
/* The running image is the recorded one but still on trial: the record goes
 * once it has confirmed itself (the idle job). */
static std::atomic<bool> s_rbClearOnConfirm{false};
static std::atomic<int64_t> s_lastCheck{0};   // atomic: the idle watch reads it without s_lock, which forget() holds across a flash erase
/* A catalog check has succeeded, also one whose response carried no usable
 * Date (s_lastCheck then stays as it was): host_info stops taking the hub's
 * FW_LATEST on this, not on the time. */
static std::atomic<bool> s_checked{false};
static FwCatalogEntry* s_entries;   // PSRAM: this boot's last fetch, compact
static size_t s_entryCount;

namespace {
struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};
}  // namespace

static bool stale() { return s_job.gen != s_forgetGen.load(); }

/* A stale job's states are dropped: forget() has already put the state back to idle. */
static void set_state(fwu_state_t s) {
    if (stale()) return;
    s_state.store(static_cast<uint8_t>(s));
}

/* The background job is asked to stop (a pad press, an app opening). */
static bool cancelled() { return s_job.background && s_cancel.load(); }

/* What the card fill stops at: an abandoned background check or a job a forget() outdated. */
static bool abandoned() { return cancelled() || stale(); }

/* One token on the FW_UPD_STATUS line: "<code>" or "<code>:<detail>", spaces as '_'. */
static void set_error(const char* code, const char* detail = nullptr) {
    char buf[FWU_ERROR_LEN];
    if (detail && detail[0]) snprintf(buf, sizeof buf, "%s:%s", code, detail);
    else strlcpy(buf, code, sizeof buf);
    for (char* p = buf; *p; ++p)
        if (*p == ' ') *p = '_';
    ESP_LOGW(TAG, "error: %s", buf);
    if (s_jobBackground.load() || stale()) return;
    Lock l;
    strlcpy(s_err, buf, sizeof s_err);
}

static void clear_error() {
    Lock l;
    s_err[0] = '\0';
}

static const char* running_kit() { return fw_image_desc_running()->kit_id; }
static const char* running_version() { return esp_app_get_description()->version; }

static void nvs_put_str(nvs_handle_t h, const char* key, const char* value) {
    const esp_err_t err = value[0] ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "nvs %s: %s", key, esp_err_to_name(err));
}

/* Writes every fwupd key from RAM, then -- after a successful check -- hands
 * its result to host_info (status.fwLatest, the orange glyph; it persists its
 * own copy) -- both NVS writers, both here on an internal stack. A change made while this
 * runs marks the keys dirty again and the loop writes once more, so nothing
 * is lost and at most one writer exists. */
static void persist_task(void*) {
    for (;;) {
        s_persistDirty.store(false);
        char latest[FWU_VERSION_LEN], url[FWU_URL_LEN], beta[crosspad::kFwBetaListMax];
        char rbKit[FWU_KIT_LEN], rbVersion[FWU_VERSION_LEN];
        int64_t lastCheck;
        bool checked;
        bool boot;
        bool publish;
        {
            Lock l;
            publish = s_publishLatest;   // taken with the copy it belongs to
            s_publishLatest = false;
            strlcpy(latest, s_latest, sizeof latest);
            strlcpy(url, s_url, sizeof url);
            strlcpy(beta, s_betaKits, sizeof beta);
            strlcpy(rbKit, s_rbKit, sizeof rbKit);
            strlcpy(rbVersion, s_rbVersion, sizeof rbVersion);
            lastCheck = s_lastCheck;
            checked = s_checked;
            boot = s_bootCheck;
        }
        nvs_handle_t h;
        if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_put_str(h, kKeyLatest, latest);
            nvs_put_str(h, kKeyUrl, url);
            nvs_put_str(h, kKeyBeta, beta);
            nvs_put_str(h, kKeyRbKit, rbKit);
            nvs_put_str(h, kKeyRbVersion, rbVersion);
            nvs_set_i64(h, kKeyLastCheck, lastCheck);
            nvs_set_u8(h, kKeyChecked, checked ? 1 : 0);
            nvs_set_u8(h, kKeyBoot, boot ? 1 : 0);
            nvs_commit(h);
            nvs_close(h);
        }
        if (publish) host_info_set_latest(latest);
        s_persistRunning.store(false);
        bool expected = false;
        if (!s_persistDirty.load() || !s_persistRunning.compare_exchange_strong(expected, true)) break;
    }
    vTaskDeleteWithCaps(nullptr);
}

static void persist() {
    s_persistDirty.store(true);
    bool expected = false;
    if (!s_persistRunning.compare_exchange_strong(expected, true)) return;   // the running writer sees it
    if (xTaskCreatePinnedToCoreWithCaps(persist_task, "fw_upd_nvs", kPersistStackBytes, nullptr, kPersistPrio,
                                        nullptr, kJobCore, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_persistRunning.store(false);
        ESP_LOGW(TAG, "update settings not saved: no memory for the writer");
    }
}

static void load_settings() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;   // never written: defaults
    size_t len = sizeof s_url;
    if (nvs_get_str(h, kKeyUrl, s_url, &len) != ESP_OK) s_url[0] = '\0';
    len = sizeof s_betaKits;
    if (nvs_get_str(h, kKeyBeta, s_betaKits, &len) != ESP_OK) s_betaKits[0] = '\0';
    len = sizeof s_latest;
    if (nvs_get_str(h, kKeyLatest, s_latest, &len) != ESP_OK) s_latest[0] = '\0';
    len = sizeof s_rbKit;
    if (nvs_get_str(h, kKeyRbKit, s_rbKit, &len) != ESP_OK) s_rbKit[0] = '\0';
    len = sizeof s_rbVersion;
    if (nvs_get_str(h, kKeyRbVersion, s_rbVersion, &len) != ESP_OK) s_rbVersion[0] = '\0';
    int64_t lastCheck = 0;
    if (nvs_get_i64(h, kKeyLastCheck, &lastCheck) == ESP_OK) s_lastCheck.store(lastCheck);
    uint8_t checked = 0;
    // Saved before the flag existed: a check time is a succeeded check.
    s_checked = (nvs_get_u8(h, kKeyChecked, &checked) == ESP_OK && checked) || lastCheck > 0;
    uint8_t boot = 1;
    if (nvs_get_u8(h, kKeyBoot, &boot) == ESP_OK) s_bootCheck = boot != 0;
    nvs_close(h);
}

/* The compact list FW_CATALOG and the views read. Whether a version is on
 * the card means touching the SD card, so that is worked out before the lock. */
static void publish_entries(const crosspad::FwCatalog& c) {
    if (!s_entries) return;
    bool onCard[kEntriesMax] = {};
    size_t n = 0;
    for (size_t k = 0; k < c.kitCount; ++k) {
        for (size_t i = 0; i < c.kits[k].count && n < kEntriesMax; ++i) {
            char name[FW_PKG_NAME_LEN];
            fw_library_package_name(c.kits[k].kit, c.kits[k].versions[i].version, name, sizeof name);
            onCard[n++] = fw_library_is_complete(name);
        }
    }
    Lock l;
    n = 0;
    for (size_t k = 0; k < c.kitCount; ++k) {
        for (size_t i = 0; i < c.kits[k].count && n < kEntriesMax; ++i, ++n) {
            const crosspad::FwCatalogVersion& v = c.kits[k].versions[i];
            FwCatalogEntry& e = s_entries[n];
            strlcpy(e.kit, c.kits[k].kit, sizeof e.kit);
            strlcpy(e.version, v.version, sizeof e.version);
            e.beta = v.channel == crosspad::FwChannel::Beta;
            e.community = v.tier == crosspad::FwTier::Community;
            e.onCard = onCard[n];
        }
    }
    s_entryCount = n;
}

static crosspad::FwCatalog* alloc_catalog() {
    return static_cast<crosspad::FwCatalog*>(heap_caps_calloc(1, sizeof(crosspad::FwCatalog), MALLOC_CAP_SPIRAM));
}

/* Into `cat` (the caller's PSRAM); publishes the compact list. */
static bool fetch_catalog(crosspad::FwCatalog& cat, int64_t* fetchedAt) {
    char url[FWU_URL_LEN];
    fw_updater_get_url(url, sizeof url);
    const FwCatalogFetch r = fw_catalog_fetch(url, cat, fetchedAt, cancelled);
    if (r != FwCatalogFetch::Ok) {
        if (r != FwCatalogFetch::Cancelled) set_error("catalog", fw_catalog_fetch_name(r));
        return false;
    }
    if (cancelled()) return false;
    publish_entries(cat);
    return true;
}

struct RolledBack {
    char kit[FWU_KIT_LEN];
    crosspad::Semver version;
    bool known;
};

/* Blocked this boot, or the version the other slot rolled back from. */
static bool skip_version(const char* kit, const char* version, void* ctx) {
    const auto* rb = static_cast<const RolledBack*>(ctx);
    crosspad::Semver v;
    return fw_download_blocked(kit, version) ||
           (rb->known && strcmp(rb->kit, kit) == 0 && crosspad::Semver::parse(version, v) && v == rb->version);
}

static bool find_update(const crosspad::FwCatalog& cat, crosspad::FwCatalogVersion* out) {
    crosspad::Semver running;
    crosspad::Semver::parse(running_version(), running);   // unparseable: 0.0.0, every release is newer
    const auto channel = fw_updater_channel_beta(running_kit()) ? crosspad::FwChannel::Beta
                                                                : crosspad::FwChannel::Stable;
    RolledBack rb{};
    {
        Lock l;
        strlcpy(rb.kit, s_rbKit, sizeof rb.kit);
        rb.known = s_rbKit[0] && crosspad::Semver::parse(s_rbVersion, rb.version);
    }
    const crosspad::FwCatalogVersion* v = crosspad::fwCatalogPickUpdate(cat, running_kit(), channel, running, skip_version, &rb);
    if (!v) return false;
    *out = *v;
    return true;
}

static const char* fail_name() {
    return crosspad::wifiFailName(static_cast<crosspad::WifiFail>(wifi_link_fail()));
}

/* A failed or abandoned check changes nothing that persists: the last result
 * and the card's cached catalog stay as they were. A background one also shows
 * nothing (see s_jobBackground); both kinds of end count as a failed attempt,
 * which the idle watch retries after an hour. */
static void end_attempt(bool failed) {
    s_lastAttemptFailed.store(failed);
    s_lastAttemptUs.store(esp_timer_get_time());
}

/* The job's WiFi lease, taken on its own task: the verb or press that started
 * it has had its answer already, and wifi_link_acquire() may wait on the
 * link's guard. True when held. A refusal (WiFi off, transfer mode, no_mem)
 * ends a user's job `failed` with the link's reason as err; a background
 * job's changes nothing the user sees, and the next trigger tries again. */
static bool take_lease() {
    if (s_job.lease) return true;
    if (wifi_link_acquire(kLeaseReason)) {
        s_job.lease = true;
        return true;
    }
    const char* why = fail_name();
    if (s_job.background) {
        end_attempt(true);
        ESP_LOGI(TAG, "background check skipped: %s", why);
        return false;
    }
    set_error(why);
    set_state(FWU_FAILED);
    return false;
}

/* The job's lease was taken by take_lease(); this waits for the link. A radio
 * that could not start after all (the memory gate inside the control task)
 * ends here as no_mem too. */
static bool wait_link() {
    for (uint32_t waited = 0; waited < kWifiUpTimeoutMs; waited += kWifiUpSliceMs) {
        if (cancelled()) return false;
        if (wifi_link_wait_up(kWifiUpSliceMs)) return true;
        if (wifi_link_state() == static_cast<uint8_t>(crosspad::WifiLinkState::Failed)) break;
    }
    const auto fail = static_cast<crosspad::WifiFail>(wifi_link_fail());
    set_error(fail == crosspad::WifiFail::None ? "wifi_timeout" : fail_name());
    return false;
}

/* After a download or a cleanup: which listed versions are whole on the card.
 * Only this (the job) task changes the entries' names, so reading them here
 * needs no lock; the flags are written under it. */
static void refresh_on_card() {
    bool onCard[kEntriesMax] = {};
    const size_t n = s_entryCount;
    for (size_t i = 0; i < n; ++i) {
        char name[FW_PKG_NAME_LEN];
        fw_library_package_name(s_entries[i].kit, s_entries[i].version, name, sizeof name);
        onCard[i] = fw_library_is_complete(name);
    }
    Lock l;
    for (size_t i = 0; i < n && i < s_entryCount; ++i) s_entries[i].onCard = onCard[i];
}

static FwDlResult download_version(const Job& job, const crosspad::FwCatalogVersion& v) {
    s_dl.done.store(0);
    s_dl.total.store(v.bin.size + v.assets.size + v.icon.size);
    set_state(FWU_DOWNLOADING);
    char url[FWU_URL_LEN];
    fw_updater_get_url(url, sizeof url);
    const FwDlResult r = fw_download_run(url, job.kit, v, s_dl, job.slotPkg[0], job.slotPkg[1]);
    if (r == FwDlResult::Ok) return r;
    set_error(fw_dl_result_name(r));
    set_state(FWU_FAILED);
    return r;
}

static void install_start_task(void*) {
    const char* why = nullptr;
    s_installStart.ok = fw_install_start(s_installStart.name, false, &why);
    strlcpy(s_installStart.why, why ? why : "", sizeof s_installStart.why);
    s_instStackFree.store(uxTaskGetStackHighWaterMark(nullptr));
    s_installStart.done.store(true);
    vTaskDeleteWithCaps(nullptr);
}

/* Same kit, and the same version as a Semver ("v1.3.0" = "1.3.0"). */
static bool is_build(const char* kit, const char* version, const char* rbKit, const char* rbVersion) {
    crosspad::Semver a, b;
    return strcmp(kit, rbKit) == 0 && crosspad::Semver::parse(version, a) && crosspad::Semver::parse(rbVersion, b) &&
           a == b;
}

/* The lock is only tried: forget() holds it across a flash erase, and the idle
 * worker must not wait for that. False = not now, the caller tries again later. */
static bool clear_rolled_back(const char* why) {
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return false;
    s_rbKit[0] = s_rbVersion[0] = '\0';
    xSemaphoreGive(s_lock);
    s_rbClearOnConfirm.store(false);
    ESP_LOGI(TAG, "rolled-back version offered again: %s", why);
    persist();
    return true;
}

/* The job's stack is PSRAM and must not reach flash, and fw_install_start()
 * reaches the OTA partition table and the STM link: it runs on a task of its
 * own and the job waits for its answer. fw_install_start() then writes the
 * slot on yet another task, stopping the sample engine meanwhile and
 * reloading the kit after (network mode has no engine to stop); this waits
 * for that too. */
static bool install_package(const char* kit, const char* version) {
    char name[FW_PKG_NAME_LEN];
    fw_library_package_name(kit, version, name, sizeof name);
    s_installPct.store(0);
    set_state(FWU_INSTALLING);
    strlcpy(s_installStart.name, name, sizeof s_installStart.name);
    s_installStart.done.store(false);
    if (xTaskCreatePinnedToCoreWithCaps(install_start_task, "fw_upd_inst", kInstallStartStackBytes, nullptr,
                                        kPersistPrio, nullptr, kJobCore,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        set_error("no_mem", "install_task");
        set_state(FWU_FAILED);
        return false;
    }
    while (!s_installStart.done.load()) vTaskDelay(pdMS_TO_TICKS(kInstallStartPollMs));
    if (!s_installStart.ok) {
        set_error("install", s_installStart.why);
        set_state(FWU_FAILED);
        return false;
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kInstallPollMs));
        const FwInstallStatus st = fw_install_status();
        s_installPct.store(st.percent);
        if (st.phase == FwInstallPhase::Done) break;
        if (st.phase == FwInstallPhase::Failed) {
            set_error("install", st.error);
            set_state(FWU_FAILED);
            return false;
        }
    }
    char rbKit[FWU_KIT_LEN], rbVersion[FWU_VERSION_LEN];
    {
        Lock l;
        strlcpy(s_ready, version, sizeof s_ready);
        strlcpy(rbKit, s_rbKit, sizeof rbKit);
        strlcpy(rbVersion, s_rbVersion, sizeof rbVersion);
    }
    // The user installed the version that rolled back once: they want it, so checks offer it (and Run) again.
    if (rbKit[0] && is_build(kit, version, rbKit, rbVersion)) clear_rolled_back("installed again on purpose");
    set_state(FWU_READY_TO_RESTART);
    ESP_LOGI(TAG, "%s installed in the other slot: run it, or exit to keep this one", name);
    return true;
}

static void run_download(const Job& job) {
    /* The version the caller typed may be a spelling of the catalog's (`1.1` for
     * `1.1.0`): once the catalog has answered, `version` is the catalog's own
     * string, and the package directory, the install and the status all use it. */
    char version[FWU_VERSION_LEN];
    strlcpy(version, job.version, sizeof version);
    {
        Lock l;
        strlcpy(s_target, version, sizeof s_target);
        strlcpy(s_targetKit, job.kit, sizeof s_targetKit);
    }
    // Seen missing: the radio comes up while the card is verified.
    if (job.leaseFirst && !take_lease()) return;
    // Already whole and still verifying: no network needed to install it.
    if (!fw_download_verify_on_card(job.kit, version)) {
        // Seen whole or not known when the job started, and not whole now: fetch it.
        if (!take_lease()) return;
        crosspad::FwCatalog* cat = alloc_catalog();
        if (!cat) {
            set_error("no_mem", "catalog");
            set_state(FWU_FAILED);
            return;
        }
        crosspad::FwCatalogVersion v;
        bool found = false;
        int64_t fetchedAt = 0;
        if (wait_link() && fetch_catalog(*cat, &fetchedAt)) {
            const crosspad::FwCatalogVersion* hit = crosspad::fwCatalogFind(*cat, job.kit, job.version);
            if (hit) {
                v = *hit;
                found = true;
                strlcpy(version, hit->version, sizeof version);
            } else {
                set_error("not_in_catalog");
            }
        }
        heap_caps_free(cat);
        if (!found) {
            set_state(FWU_FAILED);
            return;
        }
        {
            Lock l;
            strlcpy(s_target, version, sizeof s_target);
        }
        const FwDlResult r = download_version(job, v);
        refresh_on_card();
        if (r != FwDlResult::Ok) return;
    }
    // The download is whole and verified: the install and the wait for a restart
    // need no network, so the radio goes back to its linger now. job_task() releases
    // only a lease still held, which makes this the one release on every path.
    if (s_job.lease) {
        wifi_link_release();
        s_job.lease = false;
    }
    if (job.install) {
        install_package(job.kit, version);
        return;
    }
    char latest[FWU_VERSION_LEN];
    fw_updater_latest(latest, sizeof latest);
    set_state(strcmp(latest, version) == 0 ? FWU_AVAILABLE : FWU_IDLE);
}

static void run_check() {
    if (!take_lease()) return;
    crosspad::FwCatalog* cat = alloc_catalog();
    if (!cat) {
        set_error("no_mem", "catalog");
        end_attempt(true);
        if (!s_job.background) set_state(FWU_FAILED);
        return;
    }
    int64_t fetchedAt = 0;
    const bool fetched = wait_link() && !cancelled() && fetch_catalog(*cat, &fetchedAt);
    if (!fetched || cancelled()) {
        heap_caps_free(cat);
        end_attempt(true);
        if (cancelled()) ESP_LOGI(TAG, "background check abandoned: the board is in use");
        else if (!s_job.background) set_state(FWU_FAILED);
        return;
    }
    end_attempt(false);
    // The Store's icons and kit text, while this job's lease still holds the link.
    char base[FWU_URL_LEN];
    fw_updater_get_url(base, sizeof base);
    fw_store_files_fetch(base, *cat, fw_updater_channel_beta, abandoned);
    crosspad::FwCatalogVersion update;
    const bool found = find_update(*cat, &update);
    heap_caps_free(cat);
    {
        Lock l;
        if (stale()) return;   // a factory reset ran meanwhile: its result is not to be written back
        strlcpy(s_latest, found ? update.version : "", sizeof s_latest);
        if (fetchedAt > 0) s_lastCheck.store(fetchedAt);
        s_checked = true;
        s_publishLatest = true;
    }
    persist();
    // Installed earlier and still the newest: it waits for Run, not for Install. The other
    // slot says so across a restart too, where this boot's s_ready knows nothing of it.
    const bool installed = found && fw_slots_other_holds(running_kit(), update.version);
    if (installed) {
        Lock l;
        strlcpy(s_ready, update.version, sizeof s_ready);
    }
    char ready[FWU_VERSION_LEN];
    fw_updater_ready_version(ready, sizeof ready);
    if (installed || (ready[0] && (!found || strcmp(ready, update.version) == 0))) set_state(FWU_READY_TO_RESTART);
    else set_state(found ? FWU_AVAILABLE : FWU_UP_TO_DATE);
    ESP_LOGI(TAG, "kit %s: %s (running %s)", running_kit(), found ? update.version : "up to date", running_version());
}

static void job_task(void*) {
    const Job job = s_job;
    if (job.kind == JobKind::Check) run_check();
    else run_download(job);
    // s_job, not the copy: take_lease() records the lease there.
    if (s_job.lease) wifi_link_release();
    s_stackFree.store(uxTaskGetStackHighWaterMark(nullptr));
    ESP_LOGI(TAG, "job done, %u B of stack never used", static_cast<unsigned>(s_stackFree.load()));
    s_cancel.store(false);
    s_jobBackground.store(false);   // before s_busy: a user refusal right after must not be silenced
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

/* The slots' package names into s_job, for the cleanup to protect.
 * fw_slots_get() reads NVS and the partitions -- flash -- so it runs on
 * start_job()'s caller's internal stack, never on the job's PSRAM one.
 * noinline: the FwSlotInfo pair is on that stack only while the slots are
 * read -- FW_DOWNLOAD calls it from the 5 kB main task. */
__attribute__((noinline)) static void read_slot_packages() {
    FwSlotInfo slots[2];
    fw_slots_get(slots);
    strlcpy(s_job.slotPkg[0], slots[0].package, sizeof s_job.slotPkg[0]);
    strlcpy(s_job.slotPkg[1], slots[1].package, sizeof s_job.slotPkg[1]);
}

/* One job at a time: this compare-exchange is the whole arbitration between
 * the background checks, the views, Settings, FW_CHECK and FW_DOWNLOAD.
 * Nothing here waits for the link: FW_CHECK and FW_DOWNLOAD call it on the
 * CDC task, which must answer at once. The WiFi lease is the job's own first step (take_lease()),
 * so a lease refused (WiFi off, no_mem) is a failed job with err=, read back
 * with FW_UPD_STATUS or seen by the views when the job ends. `refusal` (when
 * given) gets "busy" or "no_mem" (no job task), and the caller answers it (a
 * toast, an ERR line) with the state and error a user sees as they were.
 * Without `refusal` (a view's automatic check) that refusal is shown as a
 * failed job. A background job's refusal or failure is only logged and
 * counted, and it never shows "checking". A user job's first state is
 * published before this returns, so a poller never reads the previous result
 * as this job's. */
static bool start_job(const Job& job, fwu_state_t first, char* refusal = nullptr, size_t refusalLen = 0) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        if (refusal) strlcpy(refusal, kRefusalBusy, refusalLen);
        return false;
    }
    s_job = job;
    s_job.gen = s_forgetGen.load();
    s_job.lease = false;   // taken by the job task
    if (job.kind == JobKind::Download) read_slot_packages();
    s_cancel.store(false);
    s_jobBackground.store(job.background);
    /* What a refused start puts back. Static: only the holder of s_busy gets
     * here, and FW_CHECK calls this on the 5 kB main task. */
    static char prevErr[FWU_ERROR_LEN];
    const uint8_t prevState = s_state.load();
    if (!job.background) {
        fw_updater_error(prevErr, sizeof prevErr);
        clear_error();
        set_state(first);
    }
    if (xTaskCreatePinnedToCoreWithCaps(job_task, "fw_upd", kJobStackBytes, nullptr, kJobPrio, nullptr, kJobCore,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        if (refusal) strlcpy(refusal, "no_mem", refusalLen);
        if (job.background) {
            end_attempt(true);
        } else if (refusal) {
            {
                Lock l;
                strlcpy(s_err, prevErr, sizeof s_err);
            }
            set_state(static_cast<fwu_state_t>(prevState));
        } else {
            set_error("no_mem", "job_task");
            set_state(FWU_FAILED);
        }
        s_jobBackground.store(false);
        s_busy.store(false);
        return false;
    }
    return true;
}

/* Background work runs only with the launcher in front: no app (so no
 * recording -- the Recorder and the Sequencer are apps), no kit selector (the
 * gate a kit-less Sampler or Sequencer opens, and the one an app opens over
 * itself, both with no running app), no kit loading (one asked for over CDC or
 * SysEx opens nothing, and a kit's copy beside the check's TLS session was
 * more PSRAM than there is), no firmware flow overlay, not in USB transfer or
 * network mode. getRunningApp() and kit_selector_is_open() are pointer reads,
 * the same kind APP_LIST makes from the CDC loop; kit_load_task_is_loading()
 * a flag. */
static bool launcher_in_front() {
    return status.isGuiInitialised && !usb_transfer_active() && !net_mode_active() && !fw_flow_active() &&
           !crosspad_sampler::kit_selector_is_open() && !crosspad_sampler::kit_load_task_is_loading() &&
           crosspad_gui::AppOrchestrator::getInstance().getRunningApp() == nullptr;
}

/* A kit scan -- the boot's, or the rescan of a card mounted later -- or the
 * boot kit load after it is running, or a card-less boot has not probed the
 * card again yet: no background check takes the radio now. */
static bool kits_settling() {
    return !g_kit_scan_done.load() || !g_sd_probed.load() || crosspad_sampler::kit_load_task_is_loading();
}

static bool start_background_check() {
    Job job{};
    job.kind = JobKind::Check;
    job.background = true;
    job.leaseFirst = true;
    return start_job(job, FWU_CHECKING);
}

/* Any pad press -- the user's or the sequencer's -- abandons a background
 * check. PadPressed carries isOn, so a release can arrive as one too: a hand
 * still on the pads is activity, so any of them restarts the idle clock. */
static void on_pad_pressed(const void*, crosspad::EventType, void*) {
    s_lastPadUs.store(esp_timer_get_time());
    if (s_busy.load() && s_jobBackground.load()) s_cancel.store(true);
}

/* An app opening abandons it at once; the idle job's 1 s poll is the backstop
 * for the event arriving before the orchestrator has a running app. */
static void on_app_changed(const void*, crosspad::EventType, void*) {
    if (!launcher_in_front() && s_busy.load() && s_jobBackground.load()) s_cancel.store(true);
}

/* rb_kit / rb_version follow fw_slots: recorded while the other slot is marked
 * rolled back (fw_slots_init()), so no check offers that version again until a
 * newer one is out; dropped once it no longer applies -- the other slot is not
 * rolled back any more (overwritten, or the build installed again on purpose),
 * or this very build runs and has confirmed itself. */
static void note_rolled_back(void) {
    FwSlotInfo slots[2];
    fw_slots_get(slots);
    const FwSlotInfo& other = slots[slots[0].running ? 1 : 0];
    const char* otherVersion = other.version[0] == 'v' ? other.version + 1 : other.version;   // catalog versions carry no 'v'
    const bool otherRolledBack = other.state == FwSlotState::RolledBack && other.hasDesc &&
                                 crosspad::fwKitIdValid(other.kit) && crosspad::fwVersionValid(otherVersion);
    if (otherRolledBack && !is_build(running_kit(), running_version(), other.kit, otherVersion)) {
        {
            Lock l;
            if (strcmp(s_rbKit, other.kit) == 0 && strcmp(s_rbVersion, otherVersion) == 0) return;
            strlcpy(s_rbKit, other.kit, sizeof s_rbKit);
            strlcpy(s_rbVersion, otherVersion, sizeof s_rbVersion);
        }
        ESP_LOGW(TAG, "%s %s did not start in the other slot: not offered again", other.kit, otherVersion);
        persist();
        return;
    }
    char rbKit[FWU_KIT_LEN], rbVersion[FWU_VERSION_LEN];
    {
        Lock l;
        strlcpy(rbKit, s_rbKit, sizeof rbKit);
        strlcpy(rbVersion, s_rbVersion, sizeof rbVersion);
    }
    if (!rbKit[0]) return;
    if (is_build(running_kit(), running_version(), rbKit, rbVersion)) {
        // On a trial boot the idle job clears it once the image has confirmed itself.
        if (fw_running_on_trial() || !clear_rolled_back("it runs here, confirmed")) s_rbClearOnConfirm.store(true);
        return;
    }
    if (other.state != FwSlotState::RolledBack)
        clear_rolled_back("the other slot is no longer rolled back");   // busy: the next boot
}

/* bsp_worker job: short and non-blocking -- it reads state and, at most,
 * creates the job task (which takes the lease). An app opening is seen here within
 * one period (and sooner through on_app_changed). */
static uint32_t idle_job(void*) {
    // No background check on a trial boot: a crash in it would roll a good image back.
    if (fw_running_on_trial()) return 0;
    if (s_rbClearOnConfirm.load()) clear_rolled_back("it runs here, confirmed");
    // Once, as at a boot that is not a trial: dropped if the board is in use by then (the daily check covers it).
    /* The boot's kit load follows the scan: waited out too, it is no player. */
    const bool settling = kits_settling();
    if (!settling && s_bootCheckHeld.exchange(false) && launcher_in_front() && !s_busy.load() &&
        !start_background_check())
        ESP_LOGI(TAG, "boot check not started");
    const int64_t now = esp_timer_get_time();
    if (!launcher_in_front()) {
        s_launcherSinceUs = 0;
        if (s_busy.load() && s_jobBackground.load()) s_cancel.store(true);
        return 0;
    }
    if (s_launcherSinceUs == 0) s_launcherSinceUs = now;
    if (settling || s_busy.load() || fw_updater_state() == FWU_READY_TO_RESTART) return 0;
    const int64_t quietSince = std::max(s_launcherSinceUs, s_lastPadUs.load());
    const int64_t attempt = s_lastAttemptUs.load();
    crosspad::FwDailyInputs in{};
    in.nowUnix = static_cast<int64_t>(time(nullptr));
    in.lastCheckUnix = fw_updater_last_check();
    in.idleS = (now - quietSince) / kUsPerS;
    in.sinceLastAttemptS = attempt < 0 ? -1 : (now - attempt) / kUsPerS;
    in.lastAttemptFailed = s_lastAttemptFailed.load();
    if (crosspad::fwDailyCheckDue(in)) start_background_check();
    return 0;
}

void fw_updater_init(void) {
    s_lock = xSemaphoreCreateMutex();
    load_settings();
    note_rolled_back();
    crosspad::getEventBus().subscribe(crosspad::EventType::PadPressed, on_pad_pressed, nullptr);
    crosspad::getEventBus().subscribe(crosspad::EventType::AppChanged, on_app_changed, nullptr);
    s_entries = static_cast<FwCatalogEntry*>(heap_caps_calloc(kEntriesMax, sizeof(FwCatalogEntry), MALLOC_CAP_SPIRAM));
    // The last check's update already sits in the other slot (installed, "Later", a restart since): offer Run.
    if (s_latest[0] && fw_slots_other_holds(running_kit(), s_latest)) {
        strlcpy(s_ready, s_latest, sizeof s_ready);
        s_state.store(FWU_READY_TO_RESTART);
    }
    char url[FWU_URL_LEN];
    fw_updater_get_url(url, sizeof url);
    ESP_LOGI(TAG, "kit %s, running %s, latest %s, catalog %s", running_kit(), running_version(),
             s_latest[0] ? s_latest : "-", url);
}

fwu_state_t fw_updater_state(void) { return static_cast<fwu_state_t>(s_state.load()); }

const char* fw_updater_state_name(fwu_state_t s) {
    switch (s) {
        case FWU_IDLE:             return "idle";
        case FWU_CHECKING:         return "checking";
        case FWU_UP_TO_DATE:       return "up_to_date";
        case FWU_AVAILABLE:        return "available";
        case FWU_DOWNLOADING:      return "downloading";
        case FWU_INSTALLING:       return "installing";
        case FWU_READY_TO_RESTART: return "ready_to_restart";
        case FWU_FAILED:           return "failed";
    }
    return "?";
}

bool fw_updater_busy(void) { return s_busy.load(); }

bool fw_updater_background_busy(void) { return s_busy.load() && s_jobBackground.load(); }

bool fw_updater_check_start(char* refusal, size_t n) {
    Job job{};
    job.kind = JobKind::Check;
    job.leaseFirst = true;
    return start_job(job, FWU_CHECKING, refusal, n);
}

uint8_t fw_updater_progress(void) {
    switch (fw_updater_state()) {
        case FWU_DOWNLOADING: {
            const uint32_t total = s_dl.total.load();
            return total ? static_cast<uint8_t>(static_cast<uint64_t>(s_dl.done.load()) * kPercent / total) : 0;
        }
        case FWU_INSTALLING:       return s_installPct.load();
        case FWU_READY_TO_RESTART: return kPercent;
        default:                   return 0;
    }
}

void fw_updater_catalog_each(bool (*fn)(const FwCatalogEntry* entry, void* ctx), void* ctx) {
    Lock l;
    for (size_t i = 0; i < s_entryCount && fn(&s_entries[i], ctx); ++i) {}
}

void fw_updater_error(char* out, size_t n) {
    Lock l;
    strlcpy(out, s_err, n);
}

void fw_updater_latest(char* out, size_t n) {
    Lock l;
    strlcpy(out, s_latest, n);
}

int64_t fw_updater_last_check(void) { return s_lastCheck.load(); }

bool fw_updater_checked(void) { return s_checked.load(); }

bool fw_updater_channel_beta(const char* kit) {
    Lock l;
    return crosspad::fwBetaListHas(s_betaKits, kit);
}

bool fw_updater_set_channel(const char* kit, bool beta) {
    {
        Lock l;   // one section: two callers never drop each other's change
        char list[crosspad::kFwBetaListMax];
        strlcpy(list, s_betaKits, sizeof list);
        if (!crosspad::fwBetaListSet(list, sizeof list, kit, beta)) return false;
        strlcpy(s_betaKits, list, sizeof s_betaKits);
    }
    persist();
    return true;
}

void fw_updater_get_url(char* out, size_t n) {
    Lock l;
    strlcpy(out, s_url[0] ? s_url : kDefaultUrl, n);
}

bool fw_updater_set_url(const char* url) {
    char clean[FWU_URL_LEN] = "";
    if (url && url[0]) {
        const size_t len = strlen(url);
        const size_t schemeLen = strncmp(url, kHttp, sizeof kHttp - 1) == 0    ? sizeof kHttp - 1
                                 : strncmp(url, kHttps, sizeof kHttps - 1) == 0 ? sizeof kHttps - 1
                                                                                  : 0;
        const bool hasHost = schemeLen > 0 && url[schemeLen] != '\0' && url[schemeLen] != '/';
        if (len >= sizeof clean || !hasHost || strpbrk(url, " \t\"?#")) return false;
        memcpy(clean, url, len + 1);
        for (size_t n = len; n > 0 && clean[n - 1] == '/'; --n) clean[n - 1] = '\0';
    }
    {
        Lock l;
        strlcpy(s_url, clean, sizeof s_url);
    }
    persist();
    return true;
}

uint32_t fw_updater_stack_free(void) { return s_stackFree.load(); }

uint32_t fw_updater_inst_stack_free(void) { return s_instStackFree.load(); }

bool fw_updater_forget(void) {
    /* A settings write in flight copied its values before this reset and
     * would put them back after the erase. */
    for (uint32_t waited = 0; s_persistRunning.load() && waited < kForgetWaitMs; waited += kForgetPollMs)
        vTaskDelay(pdMS_TO_TICKS(kForgetPollMs));
    {
        Lock l;
        s_forgetGen.fetch_add(1);   // first: a job past its stale() check is overwritten below
        s_url[0] = s_betaKits[0] = s_latest[0] = s_err[0] = s_rbKit[0] = s_rbVersion[0] = '\0';
        s_rbClearOnConfirm.store(false);
        s_lastCheck.store(0);
        s_checked = false;
        s_bootCheck = true;
        s_entryCount = 0;
        s_publishLatest = false;
        s_persistDirty.store(false);
        s_state.store(FWU_IDLE);
    }
    host_info_set_latest("");   // the status bar's glyph and status.fwLatest follow the erased result
    Lock l;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

enum class CardHint : uint8_t { Unknown, Whole, Missing };

/* What this boot's last fetch saw on the card (RAM, so no SD access on the
 * caller's thread). Unknown: not listed -- no fetch since the boot, or a
 * spelling of the version the catalog does not use. */
static CardHint card_hint(const char* kit, const char* version) {
    Lock l;
    for (size_t i = 0; i < s_entryCount; ++i)
        if (strcmp(s_entries[i].kit, kit) == 0 && strcmp(s_entries[i].version, version) == 0)
            return s_entries[i].onCard ? CardHint::Whole : CardHint::Missing;
    return CardHint::Unknown;
}

bool fw_updater_download_start(const char* kit, const char* version, bool install, char* refusal, size_t n) {
    if (version && version[0] == 'v') ++version;   // catalog versions carry no 'v'
    // fwVersionValid: the version becomes part of a directory name and a URL.
    if (!crosspad::fwKitIdValid(kit) || !version || !crosspad::fwVersionValid(version)) {
        if (refusal) strlcpy(refusal, "bad_args", n);
        return false;
    }
    if (fw_download_blocked(kit, version)) {
        if (refusal) strlcpy(refusal, "blocked", n);
        return false;
    }
    Job job{};
    job.kind = JobKind::Download;
    // Seen missing from the card: it must be fetched, so the job leases first and
    // the radio comes up while the card is verified. Seen whole, or not known
    // (the list is empty after a reboot): it may install with no WiFi at all --
    // run_download() verifies the card first and takes the lease only to fetch.
    // Either way a refused lease ends the job failed with the lease's reason as err.
    job.leaseFirst = card_hint(kit, version) == CardHint::Missing;
    job.install = install;
    strlcpy(job.kit, kit, sizeof job.kit);
    strlcpy(job.version, version, sizeof job.version);
    return start_job(job, FWU_DOWNLOADING, refusal, n);
}

void fw_updater_target(char* out, size_t n) {
    Lock l;
    strlcpy(out, s_target, n);
}

void fw_updater_target_kit(char* out, size_t n) {
    Lock l;
    strlcpy(out, s_targetKit, n);
}

void fw_updater_ready_version(char* out, size_t n) {
    Lock l;
    strlcpy(out, s_ready, n);
}

bool fw_updater_boot_check(void) {
    Lock l;
    return s_bootCheck;
}

void fw_updater_set_boot_check(bool on) {
    {
        Lock l;
        s_bootCheck = on;
    }
    persist();
}

void fw_updater_launcher_ready(void) {
    if (s_idleJob || net_mode_active()) return;
    s_idleJob = bsp_worker_add("fw_upd_idle", kIdleJobPeriodMs, idle_job, nullptr);
    if (!s_idleJob) ESP_LOGW(TAG, "idle watch not started: no worker slot");
    if (!fw_updater_boot_check()) return;
    if (fw_running_on_trial() || kits_settling()) {   // the idle job starts it once both have landed
        s_bootCheckHeld.store(true);
        ESP_LOGI(TAG, "boot check held: %s", fw_running_on_trial() ? "this image has not confirmed itself yet"
                                                                    : "the kits are still settling");
        return;
    }
    if (launcher_in_front() && !start_background_check()) ESP_LOGI(TAG, "boot check not started");
}
