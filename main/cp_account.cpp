#include "cp_account.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "crosspad/fw/FwAccount.hpp"
#include "crosspad/net/WifiTypes.hpp"
#include "device_serial.h"
#include "fw_http.h"
#include "fw_image_desc.h"
#include "wifi_link.h"

static const char* TAG = "cp_account";
static constexpr const char* kNs = "cpacct";            // shared by both slots, erased by a factory reset
static constexpr const char* kKeyToken = "token";
static constexpr const char* kKeyName = "name";
static constexpr const char* kLeaseReason = "cp_pair";
static constexpr uint32_t kPairStackBytes = 8192;       // a TLS handshake, as fw_upd: PSRAM, never flash
static constexpr uint32_t kNvsStackBytes = 3072;        // NVS writes: internal stack
static constexpr UBaseType_t kTaskPrio = 1;
static constexpr BaseType_t kTaskCore = 0;
static constexpr uint32_t kSliceMs = 250;               // how soon a cancel is seen
static constexpr uint32_t kLinkWaitMs = 20000;
static constexpr uint32_t kPairTtlMaxS = 600;           // never trust a longer code than the contract allows
static constexpr int kPollFailuresMax = 3;              // a dropped poll is retried, three in a row end it
static constexpr int kStatusOk = 200;
static constexpr size_t kReplyMax = 512;
static constexpr size_t kUrlMax = 128;
static constexpr size_t kBodyMax = 160;   // pair/token: serial, code and the 64-hex poll secret
static constexpr size_t kMacBytes = 6;
static constexpr int64_t kUsPerS = 1000000;
static constexpr char kRefusalBusy[] = "busy";
static constexpr char kRefusalNoMem[] = "no_mem";
static constexpr char kCancelled[] = "cancelled";
static constexpr char kExpired[] = "expired";

struct Account {
    char token[crosspad::kFwDeviceTokenLen + 1];
    char name[crosspad::kFwAccountNameMax];
    char serial[crosspad::kFwBoardSerialLen + 1];
    char label[sizeof("CP0000")];
    char code[crosspad::kFwPairCodeLen + 1];
    char err[CPA_ERROR_LEN];
    int64_t deadlineUs;
    uint32_t gen;   // bumped whenever the token changes; a request remembers the one it carried
};

/* PSRAM: copied out under the lock before anything writes flash. */
EXT_RAM_BSS_ATTR static Account s;
EXT_RAM_BSS_ATTR static char s_reply[kReplyMax];
EXT_RAM_BSS_ATTR static char s_url[kUrlMax];
EXT_RAM_BSS_ATTR static char s_body[kBodyMax];
static StaticSemaphore_t s_lockBuf;
static SemaphoreHandle_t s_lock;
static std::atomic<uint8_t> s_pair{CPA_IDLE};
static std::atomic<bool> s_busy{false};
static std::atomic<bool> s_cancel{false};
static std::atomic<bool> s_notice{false};
static std::atomic<bool> s_nvsDirty{false};
static std::atomic<bool> s_nvsRunning{false};
static std::atomic<uint32_t> s_stackFree{0};

namespace {
struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
}  // namespace

/* Writes what RAM holds now; a change made while it writes is written again. */
static void nvs_task(void*) {
    for (;;) {
        s_nvsDirty = false;
        char token[sizeof s.token];
        char name[sizeof s.name];
        {
            Lock l;
            memcpy(token, s.token, sizeof token);
            memcpy(name, s.name, sizeof name);
        }
        nvs_handle_t h;
        // Nothing to erase on a board that never linked: opening for writing would create the namespace.
        esp_err_t probe = ESP_OK;
        if (!token[0]) {
            probe = nvs_open(kNs, NVS_READONLY, &h);
            if (probe == ESP_OK) nvs_close(h);
        }
        if (probe != ESP_ERR_NVS_NOT_FOUND && nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t e = token[0] ? nvs_set_str(h, kKeyToken, token) : nvs_erase_all(h);
            if (e == ESP_OK && token[0]) e = nvs_set_str(h, kKeyName, name);
            if (e != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGE(TAG, "cpacct not written");
            nvs_close(h);
        }
        memset(token, 0, sizeof token);
        if (s_nvsDirty) continue;
        s_nvsRunning = false;
        bool idle = false;
        if (!s_nvsDirty || !s_nvsRunning.compare_exchange_strong(idle, true)) break;
    }
    vTaskDeleteWithCaps(nullptr);
}

/* False: no writer could be started, RAM and NVS now differ until the next change. */
static bool persist(void) {
    s_nvsDirty = true;
    bool idle = false;
    if (!s_nvsRunning.compare_exchange_strong(idle, true)) return true;   // the running writer sees the flag
    if (xTaskCreatePinnedToCoreWithCaps(nvs_task, "cp_acct_nvs", kNvsStackBytes, nullptr, kTaskPrio, nullptr,
                                        kTaskCore, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_nvsRunning = false;
        ESP_LOGE(TAG, "no task to write cpacct");
        return false;
    }
    return true;
}

static void copy_out(const char* src, char* out, size_t n) {
    if (n == 0) return;
    Lock l;
    strlcpy(out, src, n);
}

/* Token and name gone, under the lock; a pairing in flight is told to drop
 * whatever it receives (run_pair() checks s_cancel under the same lock). */
static bool wipe(bool notice) {
    bool had;
    {
        Lock l;
        had = s.token[0] != '\0';
        s_cancel = true;
        ++s.gen;
        memset(s.token, 0, sizeof s.token);
        memset(s.name, 0, sizeof s.name);
    }
    s_notice = notice;
    if (!s_busy) s_pair = CPA_IDLE;
    return had;
}

void cp_account_init(void) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lockBuf);
    uint8_t mac[kMacBytes] = {};
    esp_efuse_mac_get_default(mac);
    snprintf(s.serial, sizeof s.serial, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;   // never linked
    size_t len = sizeof s.token;
    if (nvs_get_str(h, kKeyToken, s.token, &len) != ESP_OK) s.token[0] = '\0';
    len = sizeof s.name;
    if (nvs_get_str(h, kKeyName, s.name, &len) != ESP_OK) s.name[0] = '\0';
    nvs_close(h);
    if (s.token[0] && !crosspad::fwDeviceTokenValid(s.token)) {
        ESP_LOGW(TAG, "stored device token is malformed: dropped");
        memset(s.token, 0, sizeof s.token);
        s.name[0] = '\0';
        persist();
    }
    ESP_LOGI(TAG, "%s, account host %s", s.token[0] ? "linked" : "not linked", CROSSPAD_ACCOUNT_URL);
}

bool cp_account_linked(void) {
    Lock l;
    return s.token[0] != '\0';
}

void cp_account_name(char* out, size_t n) { copy_out(s.name, out, n); }

void cp_account_serial(char* out, size_t n) { copy_out(s.serial, out, n); }

void cp_account_label(char* out, size_t n) { copy_out(s.label, out, n); }

const char* cp_account_base(void) { return CROSSPAD_ACCOUNT_URL; }

bool cp_account_auth_header(const char* url, char* out, size_t n, uint32_t* gen) {
    Lock l;
    if (!s.token[0] || !crosspad::fwAccountUrlAllowed(CROSSPAD_ACCOUNT_URL, url)) return false;
    *gen = s.gen;
    const int w = snprintf(out, n, "Bearer %s", s.token);
    return w > 0 && static_cast<size_t>(w) < n;
}

/* A 401 that left with an older token (a catalog check in flight while the
 * board paired again) must not erase the token it holds now. */
void cp_account_revoked(uint32_t gen) {
    {
        Lock l;
        if (!s.token[0] || gen != s.gen) return;
        ++s.gen;
        memset(s.token, 0, sizeof s.token);
        memset(s.name, 0, sizeof s.name);
    }
    s_notice = true;
    ESP_LOGW(TAG, "device token refused (401): unlinked");
    persist();
}

bool cp_account_notice(void) { return s_notice; }

static bool sleep_or_cancel(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += kSliceMs) {
        if (s_cancel) return false;
        vTaskDelay(pdMS_TO_TICKS(kSliceMs));
    }
    return !s_cancel;
}

/* The lease was taken by cp_account_pair_start(); this waits for the link. */
static const char* wait_link(void) {
    for (uint32_t waited = 0; waited < kLinkWaitMs; waited += kSliceMs) {
        if (s_cancel) return kCancelled;
        if (wifi_link_wait_up(kSliceMs)) return nullptr;
        if (wifi_link_state() == static_cast<uint8_t>(crosspad::WifiLinkState::Failed)) {
            const auto fail = static_cast<crosspad::WifiFail>(wifi_link_fail());
            if (fail != crosspad::WifiFail::None) return crosspad::wifiFailName(fail);
            break;
        }
    }
    return "no_network";
}

/* nullptr: linked. Otherwise the error token; "expired" and "cancelled" pick their own states. */
static const char* run_pair(void) {
    if (const char* why = wait_link()) return why;
    const unsigned rev = fw_image_desc_running()->board_rev;
    snprintf(s_url, sizeof s_url, "%s/api/device/pair/start", CROSSPAD_ACCOUNT_URL);
    const char* sn = device_serial();   // CP0001..CP9999 once the STM answered, else the MAC: no label
    if (sn[0] == 'C' && sn[1] == 'P') {
        Lock l;
        snprintf(s.label, sizeof s.label, "%s", sn);
    }
    if (s.label[0])   // the STM's PCB serial, the website's name for this board (Decision 11)
        snprintf(s_body, sizeof s_body, "{\"serial\":\"%s\",\"rev\":\"v%u\",\"label\":\"%s\"}", s.serial, rev, s.label);
    else
        snprintf(s_body, sizeof s_body, "{\"serial\":\"%s\",\"rev\":\"v%u\"}", s.serial, rev);
    size_t len = 0;
    FwHttpReply r;
    if (!fw_http_post_json(s_url, s_body, s_reply, sizeof s_reply, &len, &r)) return "network";
    if (r.status != kStatusOk) return "http";
    crosspad::FwPairStart start{};
    if (!crosspad::parseFwPairStart(s_reply, len, start)) return "parse";
    {
        Lock l;
        memcpy(s.code, start.code, sizeof s.code);
        const uint32_t ttl = start.expiresIn < kPairTtlMaxS ? start.expiresIn : kPairTtlMaxS;
        s.deadlineUs = esp_timer_get_time() + static_cast<int64_t>(ttl) * kUsPerS;
    }
    s_pair = CPA_CODE;
    snprintf(s_url, sizeof s_url, "%s/api/device/pair/token", CROSSPAD_ACCOUNT_URL);
    // The poll secret goes only into this request body: never shown, never logged,
    // wiped with the body when pairing ends (the server mints a token only for it).
    snprintf(s_body, sizeof s_body, "{\"serial\":\"%s\",\"code\":\"%s\",\"poll\":\"%s\"}", s.serial,
             start.code, start.poll);
    memset(start.poll, 0, sizeof start.poll);
    int failures = 0;
    for (;;) {
        if (!sleep_or_cancel(crosspad::kFwPairPollMs)) return kCancelled;
        if (esp_timer_get_time() >= s.deadlineUs) return kExpired;
        if (!fw_http_post_json(s_url, s_body, s_reply, sizeof s_reply, &len, &r)) {
            if (++failures >= kPollFailuresMax) return "network";
            continue;
        }
        failures = 0;
        crosspad::FwPairToken tok{};
        const crosspad::FwPairReply reply = crosspad::parseFwPairToken(r.status, s_reply, len, tok);
        memset(s_reply, 0, sizeof s_reply);   // a token may have passed through here
        switch (reply) {
            case crosspad::FwPairReply::Waiting: continue;
            case crosspad::FwPairReply::Expired: return kExpired;
            case crosspad::FwPairReply::Bad: return "http";
            case crosspad::FwPairReply::Token: {
                bool kept;
                {
                    Lock l;
                    // Cancelled, unlinked or reset while this reply was on its way: never stored.
                    kept = !s_cancel;
                    if (kept) {
                        ++s.gen;
                        memcpy(s.token, tok.token, sizeof s.token);
                        memcpy(s.name, tok.account, sizeof s.name);
                    }
                }
                memset(&tok, 0, sizeof tok);
                if (!kept) return kCancelled;
                s_notice = false;
                persist();
                return nullptr;
            }
        }
    }
}

static void pair_task(void*) {
    const char* why = run_pair();
    memset(s_body, 0, sizeof s_body);   // held the poll secret while polling
    s_stackFree = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    wifi_link_release();
    {
        Lock l;
        strlcpy(s.err, why && strcmp(why, kCancelled) != 0 && strcmp(why, kExpired) != 0 ? why : "", sizeof s.err);
        s.code[0] = '\0';
    }
    if (!why) s_pair = CPA_LINKED;
    else if (strcmp(why, kCancelled) == 0) s_pair = CPA_IDLE;
    else if (strcmp(why, kExpired) == 0) s_pair = CPA_EXPIRED;
    else s_pair = CPA_FAILED;
    ESP_LOGI(TAG, "pairing ended: %s", why ? why : "linked");
    s_busy = false;
    vTaskDeleteWithCaps(nullptr);
}

bool cp_account_pair_start(char* refusal, size_t n) {
    bool idle = false;
    if (!s_busy.compare_exchange_strong(idle, true)) {
        strlcpy(refusal, kRefusalBusy, n);
        return false;
    }
    // On the caller's task, so a refusal (WiFi off, no_mem) is its answer at once.
    if (!wifi_link_acquire(kLeaseReason)) {
        strlcpy(refusal, crosspad::wifiFailName(static_cast<crosspad::WifiFail>(wifi_link_fail())), n);
        s_busy = false;
        return false;
    }
    {
        Lock l;
        s.code[0] = '\0';
        s.err[0] = '\0';
        s.deadlineUs = 0;
    }
    s_cancel = false;
    s_pair = CPA_STARTING;
    if (xTaskCreatePinnedToCoreWithCaps(pair_task, "cp_pair", kPairStackBytes, nullptr, kTaskPrio, nullptr, kTaskCore,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        wifi_link_release();
        s_pair = CPA_IDLE;
        strlcpy(refusal, kRefusalNoMem, n);
        s_busy = false;
        return false;
    }
    return true;
}

void cp_account_pair_cancel(void) {
    s_cancel = true;
    if (!s_busy) s_pair = CPA_IDLE;   // a running pairing ends in idle itself, within one slice
}

cpa_pair_t cp_account_pair_state(void) { return static_cast<cpa_pair_t>(s_pair.load()); }

const char* cp_account_pair_state_name(cpa_pair_t st) {
    switch (st) {
        case CPA_IDLE: return "idle";
        case CPA_STARTING: return "starting";
        case CPA_CODE: return "code";
        case CPA_LINKED: return "linked";
        case CPA_EXPIRED: return "expired";
        case CPA_FAILED: return "failed";
    }
    return "?";
}

void cp_account_pair_code(char* out, size_t n) {
    if (n == 0) return;
    Lock l;
    strlcpy(out, s_pair == CPA_CODE ? s.code : "", n);
}

uint32_t cp_account_pair_left_s(void) {
    if (s_pair != CPA_CODE) return 0;
    int64_t deadline;
    {
        Lock l;
        deadline = s.deadlineUs;
    }
    const int64_t left = deadline - esp_timer_get_time();
    return left > 0 ? static_cast<uint32_t>((left + kUsPerS - 1) / kUsPerS) : 0;
}

void cp_account_pair_error(char* out, size_t n) { copy_out(s.err, out, n); }

bool cp_account_unlink(void) {
    if (!wipe(false)) return false;
    ESP_LOGI(TAG, "unlinked");
    persist();
    return true;
}

bool cp_account_forget(void) {
    wipe(false);
    return persist();
}

uint32_t cp_account_stack_free(void) { return s_stackFree; }
