#include "wifi_verbs.h"
#include "wifi_creds.h"
#include "wifi_link.h"
#include "wifi_ping.h"
#include "device_serial.h"
#include "NvsSettingsStore.hpp"
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/util/QuotedArgs.hpp"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <atomic>
#include <cstdio>
#include <cstring>

/* The host's own lease, so WIFI_UP/WIFI_DOWN pair up however often they are sent. */
static bool s_benchLease;

static const char* args_of(const char* cmd, const char* verb) {
    size_t n = strlen(verb);
    if (strncmp(cmd, verb, n) != 0 || (cmd[n] != '\0' && cmd[n] != ' ')) return nullptr;
    return cmd + n;
}

/* Reply text that carries an SSID: an SSID may hold any byte, and the host
 * reads fields apart by `"` and ` | `, a line by CR/LF. So backslash, `"` and
 * `|` get a backslash in front, CR/LF/other control bytes become \r, \n,
 * \xHH; the host undoes exactly this. Worst case four output bytes per input. */
static constexpr size_t kEscapedSsidMax = 4 * crosspad::WifiNetworkList::kMaxSsid + 1;

static void escape_ssid(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 5 <= cap; ++in) {
        const unsigned char c = (unsigned char)*in;
        if (c == '\\' || c == '"' || c == '|') n += (size_t)snprintf(out + n, cap - n, "\\%c", c);
        else if (c == '\r')                     n += (size_t)snprintf(out + n, cap - n, "\\r");
        else if (c == '\n')                     n += (size_t)snprintf(out + n, cap - n, "\\n");
        else if (c < 0x20 || c == 0x7f)          n += (size_t)snprintf(out + n, cap - n, "\\x%02x", c);
        else                                     out[n++] = (char)c;
    }
    out[n] = '\0';
}

static void reply_status(void (*reply)(const char*)) {
    char ssid[33]; int8_t rssi; uint32_t ip;
    wifi_link_info(ssid, sizeof(ssid), &rssi, &ip);
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    char ssidText[kEscapedSsidMax];
    escape_ssid(ssid, ssidText, sizeof(ssidText));
    char ipText[16];
    esp_ip4_addr_t a = {ip};
    snprintf(ipText, sizeof(ipText), IPSTR, IP2STR(&a));
    /* The SSID is quoted (and escaped) so a name with spaces stays one field;
     * none is ssid="". Static, as in WIFI_SET: the longest line (every byte of a
     * 32-byte SSID escaped, every field at its widest) is ~460 B, and the main
     * task's 5 kB stack has none to spare. PSRAM, as every static here: they
     * exist in every boot, and internal RAM is what the WiFi gates count. */
    EXT_RAM_BSS_ATTR static char line[512];
    snprintf(line, sizeof(line),
             "WIFI: state=%s ssid=\"%s\" rssi=%d ip=%s leases=%u fail=%s reason=%u "
             "heap_int=%u heap_init=%u heap_low=%u int_init=%u int_low=%u psram_init=%u psram_low=%u "
             "stack_free=%u enabled=%u\r\n",
             crosspad::wifiLinkStateName((crosspad::WifiLinkState)wifi_link_state()),
             ssidText, (int)rssi, ipText, (unsigned)wifi_link_leases(),
             crosspad::wifiFailName((crosspad::WifiFail)wifi_link_fail()),
             (unsigned)wifi_link_last_reason(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT),
             (unsigned)wifi_link_heap_init(), (unsigned)wifi_link_heap_low(),
             (unsigned)wifi_link_int_init(), (unsigned)wifi_link_int_low(),
             (unsigned)wifi_link_psram_init(), (unsigned)wifi_link_psram_low(),
             (unsigned)wifi_link_stack_free(), cfg && cfg->wireless.enableWiFi ? 1u : 0u);
    reply(line);
}

/* WIFI_SCAN and WIFI_PING take seconds; the CDC drain loop must not wait for
 * them, so each runs on a one-shot task that replies for itself (the FW_LIST
 * shape in hil_control.cpp). PSRAM stacks: the scan only waits on the control
 * task, and esp-tls writes no flash. The companion cannot take a reply
 * from another task, so hil_control_companion_handle() refuses both (R2). */
static constexpr uint32_t kScanTaskStack = 4096;
static constexpr uint32_t kPingTaskStack = 8192;    // an esp-tls handshake: mbedTLS's deepest calls
static constexpr UBaseType_t kVerbTaskPrio = 1;
static constexpr BaseType_t kVerbTaskCore = 0;
static constexpr size_t kApLineMax = 1024;
static constexpr size_t kApHeadMax = 24;            // "WIFI_AP: count=NN"
static constexpr int kHttpReachableMin = 200;       // a 404 before sub-project B adds the
static constexpr int kHttpReachableEnd = 500;       // endpoint still proves DNS + TLS + Worker

static std::atomic<bool> s_scanBusy{false};
static std::atomic<bool> s_pingBusy{false};
static void (*s_scanReply)(const char*);
static void (*s_pingReply)(const char*);

static void scan_task(void*) {
    EXT_RAM_BSS_ATTR static wifi_ap_t aps[WIFI_LINK_SCAN_MAX];   // one scan task at a time (s_scanBusy)
    const int n = wifi_link_scan(aps, WIFI_LINK_SCAN_MAX);
    char* line = static_cast<char*>(heap_caps_malloc(kApLineMax, MALLOC_CAP_SPIRAM));
    if (!line) {
        s_scanReply("ERR no_mem\r\n");
    } else if (n == WIFI_LINK_SCAN_TIMEOUT) {
        s_scanReply("ERR timeout\r\n");
    } else if (n == WIFI_LINK_SCAN_FAILED) {
        s_scanReply("ERR scan\r\n");
    } else if (n < 0) {
        snprintf(line, kApLineMax, "ERR %s\r\n",
                 crosspad::wifiFailName((crosspad::WifiFail)wifi_link_fail()));
        s_scanReply(line);
    } else {
        /* Rows first, behind room for the head, so count= is the rows that fit. */
        char* body = line + kApHeadMax;
        const size_t cap = kApLineMax - kApHeadMax - 3;   // "\r\n" + NUL
        size_t used = 0;
        int rows = 0;
        for (int i = 0; i < n; ++i) {
            char ssidText[kEscapedSsidMax];
            escape_ssid(aps[i].ssid, ssidText, sizeof(ssidText));
            int w = snprintf(body + used, cap - used, " | %d %s %s", (int)aps[i].rssi,
                             wifi_link_auth_name(aps[i].auth), ssidText);
            if (w < 0 || used + (size_t)w >= cap) break;
            used += (size_t)w;
            ++rows;
        }
        char head[kApHeadMax];
        int h = snprintf(head, sizeof(head), "WIFI_AP: count=%d", rows);
        memmove(line + h, body, used);
        memcpy(line, head, (size_t)h);
        memcpy(line + h + used, "\r\n", 3);
        s_scanReply(line);
    }
    heap_caps_free(line);
    s_scanBusy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

static void ping_task(void*) {
    char line[96];
    if (!wifi_link_acquire("ping")) {
        snprintf(line, sizeof(line), "ERR %s\r\n",
                 crosspad::wifiFailName((crosspad::WifiFail)wifi_link_fail()));
    } else {
        uint32_t ms = 0;
        int tls = 0;
        const int r = wifi_ping(&ms, &tls);
        wifi_link_release();
        if (r >= kHttpReachableMin && r < kHttpReachableEnd)
            snprintf(line, sizeof(line), "WIFI_PING: ok ms=%u status=%d sn=%s\r\n", (unsigned)ms, r,
                     device_serial());
        else if (r > 0)
            snprintf(line, sizeof(line), "ERR status=%d\r\n", r);
        else   // mbedTLS codes are negative: tls=-0x2700
            snprintf(line, sizeof(line), "ERR err=%s tls=%s0x%04X\r\n",
                     esp_err_to_name((esp_err_t)-r), tls < 0 ? "-" : "",
                     (unsigned)(tls < 0 ? -tls : tls));
    }
    s_pingReply(line);
    s_pingBusy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

static void start_verb_task(std::atomic<bool>& busy, void (**slot)(const char*),
                            void (*reply)(const char*), TaskFunction_t fn, const char* name,
                            uint32_t stack) {
    if (busy.exchange(true)) { reply("ERR busy\r\n"); return; }
    *slot = reply;
    if (xTaskCreatePinnedToCoreWithCaps(fn, name, stack, nullptr, kVerbTaskPrio, nullptr,
                                        kVerbTaskCore, MALLOC_CAP_SPIRAM) != pdPASS) {
        busy.store(false);
        reply("ERR no_task\r\n");
    }
}

bool wifi_verbs_handle(const char* cmd, void (*reply)(const char*)) {
    const char* a;
    if ((a = args_of(cmd, "WIFI_STATUS"))) { reply_status(reply); return true; }

    if ((a = args_of(cmd, "WIFI_ENABLE"))) {
        int on;
        auto* cfg = crosspad::CrosspadSettings::getInstance();
        if (sscanf(a, " %d", &on) != 1 || (on != 0 && on != 1)) { reply("ERR WIFI_ENABLE 0|1\r\n"); return true; }
        if (!cfg) { reply("ERR no settings\r\n"); return true; }
        cfg->wireless.enableWiFi = (on == 1);
        cfg->saveTo(NvsSettingsStore::getInstance());
        /* Reply before posting: the control task preempts this one the moment
         * it has work (see kBleStopDelayMs in wifi_link.cpp). */
        reply("OK\r\n");
        wifi_link_enable_changed();
        return true;
    }

    if ((a = args_of(cmd, "WIFI_UP"))) {
        /* Before the held-lease shortcut: with WiFi switched off a held lease
         * means nothing, and OK would say the link is coming. */
        auto* cfg = crosspad::CrosspadSettings::getInstance();
        if (!cfg || !cfg->wireless.enableWiFi) { reply("ERR disabled\r\n"); return true; }
        if (!s_benchLease) {
            if (!wifi_link_acquire("bench")) {
                char line[64];
                snprintf(line, sizeof(line), "ERR %s\r\n",
                         crosspad::wifiFailName((crosspad::WifiFail)wifi_link_fail()));
                reply(line);
                return true;
            }
            s_benchLease = true;
        }
        reply("OK\r\n");
        return true;
    }

    if ((a = args_of(cmd, "WIFI_DOWN"))) {
        if (s_benchLease) { wifi_link_release(); s_benchLease = false; }
        reply("OK\r\n");
        return true;
    }

    if ((a = args_of(cmd, "WIFI_SET"))) {
        // Static: the main task has 5 kB of stack (see bleCompanionPoll), and
        // wifi_creds_put() puts an NVS write on top. Only the main loop runs verbs.
        EXT_RAM_BSS_ATTR static char ssid[crosspad::WifiNetworkList::kMaxSsid + 1];
        EXT_RAM_BSS_ATTR static char pass[crosspad::WifiNetworkList::kMaxPass + 1];
        /* The password is copied into the saved list; it is not kept here. */
        struct Wipe { ~Wipe() { memset(pass, 0, sizeof(pass)); } } wipe;
        pass[0] = '\0';
        if (!crosspad::nextQuotedArg(a, ssid, sizeof(ssid))) { reply("ERR WIFI_SET <ssid> [pass]\r\n"); return true; }
        if (*a && !crosspad::nextQuotedArg(a, pass, sizeof(pass))) { reply("ERR bad_pass\r\n"); return true; }
        /* WIFI_SET cannot know the network's security, so an empty password stays
         * an open network; a short or malformed one is refused before it can
         * replace a working candidate. */
        if (!wifi_pass_acceptable(pass)) { reply("ERR short_pass\r\n"); return true; }
        if (!wifi_creds_put(ssid, pass)) { reply("ERR save\r\n"); return true; }
        reply("OK\r\n");   // before posting, as in WIFI_ENABLE
        wifi_link_connect_now();
        return true;
    }

    if ((a = args_of(cmd, "WIFI_LIST"))) {
        // Static: ~750 bytes the 5 kB main task stack cannot spare (see WIFI_SET).
        EXT_RAM_BSS_ATTR static crosspad::WifiNetworkList l;
        wifi_creds_snapshot(l);
        /* 5 × (" | " + 32 SSID bytes, each escaped to up to 4) + head fits; the
         * bound is there for the next limit change. */
        EXT_RAM_BSS_ATTR static char line[768];
        size_t n = (size_t)snprintf(line, sizeof(line), "WIFI_NET: count=%u", (unsigned)l.size());
        for (size_t i = 0; i < l.size(); ++i) {
            char ssidText[kEscapedSsidMax];
            escape_ssid(l.at(i).ssid, ssidText, sizeof(ssidText));
            int w = snprintf(line + n, sizeof(line) - n, " | %s", ssidText);
            if (w < 0 || n + (size_t)w >= sizeof(line) - 3) break;
            n += (size_t)w;
        }
        memcpy(line + n, "\r\n", 3);
        l = {};   // the snapshot carries every password
        reply(line);
        return true;
    }

    if ((a = args_of(cmd, "WIFI_FORGET_ALL"))) {
        /* The settings factory reset's WiFi step (wifi_factory_reset()), so a
         * bench can check it without resetting everything else. Run here, not
         * on that step's task, so the reply is the erase's own result and a
         * WIFI_LIST after it sees the empty list: one namespace erase is short
         * for the drain loop, and this task has the internal stack an NVS
         * write needs (wifi_creds.h). */
        const bool ok = wifi_creds_clear();
        reply(ok ? "OK\r\n" : "ERR erase\r\n");
        wifi_link_enable_changed();   // after the reply, as in WIFI_ENABLE
        return true;
    }

    if ((a = args_of(cmd, "WIFI_FORGET"))) {
        char ssid[crosspad::WifiNetworkList::kMaxSsid + 1];
        if (!crosspad::nextQuotedArg(a, ssid, sizeof(ssid))) { reply("ERR WIFI_FORGET <ssid>\r\n"); return true; }
        reply(wifi_creds_forget(ssid) ? "OK\r\n" : "ERR not_found\r\n");
        return true;
    }
    if ((a = args_of(cmd, "WIFI_SCAN"))) {
        start_verb_task(s_scanBusy, &s_scanReply, reply, scan_task, "wifi_scan", kScanTaskStack);
        return true;
    }
    if ((a = args_of(cmd, "WIFI_PING"))) {
        start_verb_task(s_pingBusy, &s_pingReply, reply, ping_task, "wifi_ping", kPingTaskStack);
        return true;
    }
    return false;
}
