#include "NvsSettingsStore.hpp"

const char* NvsSettingsStore::TAG = "nvs_store";

bool NvsSettingsStore::init() {
    if (initialized_) return true;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        initialized_ = true;
        ESP_LOGI(TAG, "NVS initialized");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }
    return initialized_;
}

// ---- Per-key helpers ----

bool NvsSettingsStore::readBool(const char* ns, const char* key, bool defaultVal) {
    nvs_handle_t h;
    uint8_t val = defaultVal ? 1 : 0;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    return val != 0;
}

uint8_t NvsSettingsStore::readU8(const char* ns, const char* key, uint8_t defaultVal) {
    nvs_handle_t h;
    uint8_t val = defaultVal;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    return val;
}

int32_t NvsSettingsStore::readI32(const char* ns, const char* key, int32_t defaultVal) {
    nvs_handle_t h;
    int32_t val = defaultVal;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &val);
        nvs_close(h);
    }
    return val;
}

void NvsSettingsStore::saveBool(const char* ns, const char* key, bool value) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void NvsSettingsStore::saveU8(const char* ns, const char* key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

void NvsSettingsStore::saveI32(const char* ns, const char* key, int32_t value) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- Factory reset ----

void NvsSettingsStore::eraseAll() {
    ESP_LOGW(TAG, "Factory reset - erasing all settings namespaces");
    const char* namespaces[] = {
        "cfg_display", "cfg_audio", "cfg_rgb", "cfg_keypad",
        "cfg_vibrate", "cfg_wireless", "cfg_stm_rt", "cfg_esp_rt",
        "cfg_system", "fwupd", "cpacct"
    };
    for (const char* ns : namespaces) {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ESP_LOGW(TAG, "Factory reset complete - restart to apply defaults");
}
