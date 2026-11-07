#include "include/net_mode.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net_mode";

/* RTC slow memory: kept across esp_restart(), uninitialised after a power
 * cycle — the lifetime the mode needs. Uninitialised RTC RAM can match the
 * magic by chance, so the reset reason is the second half of the test, as in
 * usb_transfer.cpp (whose magic is "CP:M"). */
static constexpr uint32_t kBootMagic = 0x43503A4Eu;   /* "CP:N" */
RTC_NOINIT_ATTR static uint32_t s_bootMagic;
RTC_NOINIT_ATTR static uint32_t s_bootTab;

/* Enough for a CDC reply to leave before the controller is reset. */
static constexpr uint32_t kRestartDelayMs = 150;

static const char *const kTabNames[NET_TAB_COUNT] = {"wifi", "store", "updates"};

static bool s_active;
static net_tab_t s_tab = NET_TAB_WIFI;

bool net_mode_boot_claim(void)
{
    s_active = (s_bootMagic == kBootMagic) && (esp_reset_reason() == ESP_RST_SW);
    s_tab = (s_active && s_bootTab < NET_TAB_COUNT) ? (net_tab_t)s_bootTab : NET_TAB_WIFI;
    s_bootMagic = 0;
    if (s_active) ESP_LOGW(TAG, "Booting into network mode (tab %s)", kTabNames[s_tab]);
    return s_active;
}

bool net_mode_active(void)
{
    return s_active;
}

net_tab_t net_mode_start_tab(void)
{
    return s_tab;
}

void net_mode_request(net_tab_t tab)
{
    ESP_LOGW(TAG, "Restarting into network mode");
    s_bootTab = (uint32_t)(tab < NET_TAB_COUNT ? tab : NET_TAB_WIFI);
    s_bootMagic = kBootMagic;
    vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
    esp_restart();
}

void net_mode_leave(void)
{
    ESP_LOGW(TAG, "Leaving network mode");
    s_bootMagic = 0;
    vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
    esp_restart();
}

const char *net_tab_name(net_tab_t tab)
{
    return tab < NET_TAB_COUNT ? kTabNames[tab] : "?";
}

bool net_tab_parse(const char *name, net_tab_t *out)
{
    for (int i = 0; i < NET_TAB_COUNT; ++i) {
        if (name && strcmp(name, kTabNames[i]) == 0) {
            if (out) *out = (net_tab_t)i;
            return true;
        }
    }
    return false;
}
