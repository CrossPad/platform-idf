#pragma once

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "crosspad/settings/IKeyValueStore.hpp"

namespace crosspad { class NvsWriteQueue; }   // components/crosspad-platform-idf/NvsWriteQueue.hpp

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

    // An int16 NVS key, the type the input gains are stored as. Not on the
    // interface (no other platform uses it); the ports that have such a key
    // call these instead of opening an NVS handle of their own, so that every
    // settings value goes through the same queue.
    void saveI16(const char* ns, const char* key, int16_t value);
    int16_t readI16(const char* ns, const char* key, int16_t defaultVal);

    // ---- the write queue (see components/crosspad-platform-idf/NvsWriteQueue.hpp) ----

    /// Values the queue holds that are not on flash yet.
    static unsigned queuePending();
    /// Write everything pending now: the queue's own task does it (NVS's page
    /// machinery needs a task with an internal RAM stack, and the caller's stack
    /// is not ours to choose), and this waits for it, bounded. Returns false if
    /// the wait ran out — the values are still pending, and NVS's own page
    /// format is power-fail safe, so a commit cut short costs that key, not the
    /// store. Callers: the soft-off gate and the restart hook.
    static bool queueFlushWait(const char* reason, int maxWaitMs);
    /// One line of counters — what the queue saved the board, for the log and
    /// for a read-out over CDC or the console.
    static void queueStatsText(char* out, size_t len);
    /// Drop what the queue holds for a namespace it did not write: the SETTINGS_RESET
    /// and factory-reset paths erase namespaces directly, and an entry left
    /// "already on flash" would then skip the next write of that value.
    static void queueForget(const char* ns);
    /// Erase one namespace coherently: forget it in the queue, then erase it on
    /// flash. The settings-reset verb goes through here.
    static bool eraseNamespace(const char* ns);

private:
    NvsSettingsStore() = default;
    static const char* TAG;
    bool initialized_ = false;
};
