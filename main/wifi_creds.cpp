#include "wifi_creds.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "wifi_creds";
static constexpr const char* kNs = "wifi";
static constexpr const char* kKey = "nets";

/* The lists and the two blobs live in PSRAM: they exist in every boot and hold
 * every password. An NVS read or write from a PSRAM buffer is fine: esp_flash
 * copies through its own internal bounce buffer while the cache is off. The
 * blobs and the draft are zeroed after each use. */
EXT_RAM_BSS_ATTR static crosspad::WifiNetworkList s_cache;
/* A change is made here and copied into s_cache only once NVS has committed it,
 * so the cache never holds a list the flash does not. */
EXT_RAM_BSS_ATTR static crosspad::WifiNetworkList s_draft;                          // under s_lock
EXT_RAM_BSS_ATTR static uint8_t s_loadBlob[crosspad::WifiNetworkList::kBlobMax];   // under s_lock
EXT_RAM_BSS_ATTR static uint8_t s_saveBlob[crosspad::WifiNetworkList::kBlobMax];   // under s_lock
static SemaphoreHandle_t s_lock;

static void load_locked(void) {
    s_cache.clear();
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;   // never saved: empty
    size_t len = sizeof(s_loadBlob);
    esp_err_t err = nvs_get_blob(h, kKey, s_loadBlob, &len);
    nvs_close(h);
    if (err == ESP_OK && !s_cache.deserialize(s_loadBlob, len))
        ESP_LOGW(TAG, "saved networks unreadable (%u B), starting empty", (unsigned)len);
    memset(s_loadBlob, 0, sizeof(s_loadBlob));
}

static bool save_locked(const crosspad::WifiNetworkList& l) {
    size_t len = l.serialize(s_saveBlob, sizeof(s_saveBlob));
    nvs_handle_t h;
    esp_err_t err = len == 0 ? ESP_ERR_INVALID_SIZE : nvs_open(kNs, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, kKey, s_saveBlob, len);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    memset(s_saveBlob, 0, sizeof(s_saveBlob));
    return err == ESP_OK;
}

void wifi_creds_init(void) {
    s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_locked();
    xSemaphoreGive(s_lock);
}

void wifi_creds_snapshot(crosspad::WifiNetworkList& out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out = s_cache;
    xSemaphoreGive(s_lock);
}

template <typename F>
static bool mutate_locked(F f) {
    s_draft = s_cache;
    const bool ok = f(s_draft) && save_locked(s_draft);
    if (ok) s_cache = s_draft;
    s_draft = {};
    return ok;
}

template <typename F>
static bool mutate(F f) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = mutate_locked(f);
    xSemaphoreGive(s_lock);
    return ok;
}

bool wifi_creds_put(const char* ssid, const char* pass) {
    return mutate([&](crosspad::WifiNetworkList& l) { return l.put(ssid, pass); });
}
bool wifi_creds_promote(const char* ssid) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Already first (every reconnect to the usual network): nothing to write. */
    const int at = s_cache.find(ssid);
    const bool ok = at == 0 ||
                    (at > 0 && mutate_locked([&](crosspad::WifiNetworkList& l) { return l.promote(ssid); }));
    xSemaphoreGive(s_lock);
    return ok;
}
bool wifi_creds_forget(const char* ssid) {
    return mutate([&](crosspad::WifiNetworkList& l) { return l.forget(ssid); });
}

bool wifi_creds_clear(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) s_cache = {};
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}
