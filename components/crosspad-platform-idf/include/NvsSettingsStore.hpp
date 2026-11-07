#pragma once

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "crosspad/settings/IKeyValueStore.hpp"

class NvsSettingsStore : public crosspad::IKeyValueStore {
public:
    static NvsSettingsStore& getInstance() {
        static NvsSettingsStore instance;
        return instance;
    }

    // IKeyValueStore interface
    bool init() override;

    void saveBool(const char* ns, const char* key, bool value) override;
    void saveU8(const char* ns, const char* key, uint8_t value) override;
    void saveI32(const char* ns, const char* key, int32_t value) override;

    bool readBool(const char* ns, const char* key, bool defaultVal) override;
    uint8_t readU8(const char* ns, const char* key, uint8_t defaultVal) override;
    int32_t readI32(const char* ns, const char* key, int32_t defaultVal) override;

    void eraseAll() override;

    // Legacy alias
    void factoryReset() { eraseAll(); }

private:
    NvsSettingsStore() = default;
    static const char* TAG;
    bool initialized_ = false;
};
