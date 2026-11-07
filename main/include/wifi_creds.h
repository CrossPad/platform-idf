#pragma once
#include "crosspad/net/WifiNetworkList.hpp"
#include <cctype>
#include <cstring>

/* Saved WiFi networks: NVS namespace "wifi", key "nets", one blob in
 * crosspad::WifiNetworkList's format. Outside CrosspadSettings on purpose — the
 * per-firmware settings import of an A/B switch never touches it, so both slots
 * share the list. put/promote/forget/clear write NVS, so call them only from a task
 * with an INTERNAL stack (the WiFi control task, the CDC drain loop, the
 * companion main task) — never from the LVGL thread or a task whose stack lives
 * in PSRAM, where a flash write is not allowed. wifi_creds_snapshot() is safe
 * from anywhere. */

void wifi_creds_init(void);                        // once at boot, after nvs_flash_init
void wifi_creds_snapshot(crosspad::WifiNetworkList& out);
/* put/promote/forget/clear change the list in RAM only once NVS has committed
 * the change: false leaves both as they were. */
bool wifi_creds_put(const char* ssid, const char* pass);
bool wifi_creds_promote(const char* ssid);
bool wifi_creds_forget(const char* ssid);
/* Every saved network gone, from NVS and from RAM (zeroed). Writes NVS, as above. */
bool wifi_creds_clear(void);

/* A WPA/WPA2/WPA3 passphrase is 8..63 characters, or exactly 64 hex digits (a
 * raw PSK). A shorter one can never join, and as a candidate it drops a link
 * that works, so no entry path sends one. Empty means an open network, which
 * the callers that know the network is secured refuse on their own. */
static constexpr size_t kWifiPassMin = 8;
static constexpr size_t kWifiPskHexLen = 64;

inline bool wifi_pass_acceptable(const char* pass) {
    const size_t n = strlen(pass);
    if (n == 0) return true;
    if (n < kWifiPassMin) return false;
    if (n < kWifiPskHexLen) return true;
    if (n > kWifiPskHexLen) return false;
    for (size_t i = 0; i < n; ++i)
        if (!isxdigit((unsigned char)pass[i])) return false;
    return true;
}
