#include "wifi_link.h"
#include "wifi_creds.h"
#include "usb_transfer.h"
#include "processMIDI.hpp"
#include "EspBleMidi.hpp"
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

extern crosspad::CrosspadStatus status;

using crosspad::WifiFail;
using crosspad::WifiLinkState;

static const char* TAG = "wifi_link";

static constexpr uint32_t kLingerMs = 30000;
static constexpr uint32_t kConnectTimeoutMs = 15000;
static constexpr uint32_t kRetryDelaysMs[] = {1000, 3000, 10000};
static constexpr uint32_t kRssiPollMs = 5000;
/* A verb that arrived over the BLE companion has its reply in the mailbox only
 * after the handler returns, and this task (higher priority) preempts the main
 * task the moment the verb posts; stopping BLE at once would drop that reply. */
static constexpr uint32_t kBleStopDelayMs = 200;
/* The deepest call is esp_wifi_init(): 2928 B used of 4096 (1168 B left);
 * the BLE handover before it takes 2608 B, and the connect, a TLS ping, a
 * scan, the stop and the BLE restart all stay above that peak (2026-10-04, v2,
 * BLE on, a high-water checkpoint after each step). That was ~150 B above the
 * 1024 B line, so one KiB more: ~2.2 kB left at the peak. */
static constexpr uint32_t kTaskStackHeadroom = 1024;
static constexpr uint32_t kTaskStack = 4096 + kTaskStackHeadroom;
static constexpr UBaseType_t kTaskPrio = 4;
static constexpr int kQueueLen = 16;
static constexpr uint32_t kWaitPollMs = 100;
/* The radio's memory comes from three places, each with its own gate before
 * anything is built (docs/subsystems/wifi.md, "Memory gates"). The thresholds
 * are the highest init - low of a start + connect + TLS ping over the launcher
 * (BLE on/off) and network mode, measured 2026-10-03 on v2 with the heaps' own
 * minimum (dips between samples count), plus a margin for the rest of the
 * firmware, rounded up to 1 KiB.
 *
 * Internal RAM the driver's and lwIP's fallbacks can use: they ask for
 * MALLOC_CAP_DEFAULT, which leaves out the pool SPIRAM_MALLOC_RESERVE_INTERNAL
 * keeps for DMA and stacks. heap_init - heap_low 1836 B + kSpawnMargin. */
static constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
static constexpr size_t kWifiMinInternalFree = 10240;
/* All internal RAM, that pool included: the driver's DMA buffers and task
 * stacks. int_init - int_low 39224 B + kSpawnMargin. */
static constexpr size_t kWifiMinInternalTotal = 48128;
/* PSRAM: the driver and lwIP allocate there first (SPIRAM_TRY_ALLOCATE_WIFI_LWIP),
 * mbedTLS only there. psram_init - psram_low 91792 B over a start + connect +
 * five TLS pings (launcher, BLE on, kit loaded; 2026-10-03, v2, image ba9a935)
 * + 24 KiB, rounded up to 1 KiB. The margin is wider than kSpawnMargin: the
 * rest of the firmware keeps allocating while the handshake holds its peak:
 * a start that passed with 100.3 kB free in a Sampler with a kit lost its
 * TLS ping to an SSL EOF. A Sampler with a kit leaves ~70 kB: refused here
 * by design, network mode is the fallback. */
static constexpr size_t kWifiMinPsramFree = 116736;
/* The largest single PSRAM allocation a TLS handshake can make: mbedTLS's
 * input buffer for a full record, ~16.7 kB (MBEDTLS_SSL_IN_CONTENT_LEN 16384
 * + record overhead; MBEDTLS_DYNAMIC_BUFFER sizes it to the record, so a
 * server sending 16 kB records gets all of it). TLSF rounds a request up to
 * its next size class, a few hundred bytes at this size, and the driver's
 * connect buffers (largest 4368 B) may split the block before the handshake,
 * hence 20 KiB. A test-only heap hook build saw mbedTLS below 8192 B
 * against crosspad.app, whose records are short; the gate covers servers
 * whose records are not. Enough free bytes in smaller pieces still fail.
 * 2026-10-03, v2, image ba9a935. */
static constexpr size_t kWifiMinPsramBlock = 20480;
/* PSRAM a running BLE stack hands back when the radio stops it: 16068 B
 * measured (launcher), taken down to whole KiB. The first-lease gate credits
 * it, since radio_init() checks PSRAM again once BLE has stopped. */
static constexpr size_t kBlePsramBytes = 15360;
/* What the first lease allocates before radio_init() can run that gate.
 * Measured 2026-10-02, v2, launcher, HEAP_TRACE around the first WIFI_UP. */
static constexpr size_t kNetStackBytes = 10680;      // esp_netif_init + default loop + handlers; never freed
static constexpr size_t kControlTaskBytes = 5164 + kTaskStackHeadroom;   // wifi_ctl stack + TCB, queue, four timers
/* Left for the rest of the firmware when a running BLE stack is about to hand
 * its memory over; the same margin kBleMinInternalFree keeps. */
static constexpr size_t kSpawnMargin = 8 * 1024;

enum class Msg : uint8_t {
    Acquire, Release, Linger, GotIp, Disconnected, ConnectTimeout, Retry, RssiPoll,
    ConnectNow, ConnectCandidate, EnableChanged, Scan,
};
struct Req { Msg msg; uint8_t arg; void* ptr; };   // GotIp: arg = event epoch, ptr = IPv4 address; Disconnected: arg = reason, ptr = event epoch

/* Nothing of this exists at boot. The first lease (or WIFI_SET) spawns the
 * control task with its queue and timers; after the last stop the task deletes
 * them and itself, so its internal stack comes back. s_q and s_task change only
 * under the guard, and post() sends only under it, so nothing lands in a queue
 * being deleted. */
static QueueHandle_t s_q;
static TaskHandle_t s_task;
static uint32_t s_stackFreeLast;   // the retired task's high-water mark, for WIFI_STATUS
/* Created with the network stack below and, like it, kept: a wifi_link_wait_up()
 * caller may be waiting on it while the task retires. */
static std::atomic<EventGroupHandle_t> s_events{nullptr};
static bool s_netReady;            // esp_netif_init + default loop + our handlers
static constexpr EventBits_t kBitUp = BIT0;

static SemaphoreHandle_t guard_handle() {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

struct Guard {
    Guard()  { xSemaphoreTake(guard_handle(), portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(guard_handle()); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

static std::atomic<uint8_t> s_state{(uint8_t)WifiLinkState::Off};
static std::atomic<uint8_t> s_fail{(uint8_t)WifiFail::None};
static std::atomic<uint8_t> s_leases{0};
static std::atomic<int8_t> s_rssi{0};
static std::atomic<uint32_t> s_ip{0};
static std::atomic<uint8_t> s_lastReason{0};
static std::atomic<uint32_t> s_heapInit{0};
static std::atomic<uint32_t> s_heapLow{0};
static std::atomic<uint32_t> s_intInit{0};
static std::atomic<uint32_t> s_intLow{0};
static std::atomic<uint32_t> s_psramInit{0};
static std::atomic<uint32_t> s_psramLow{0};
/* Between watch_start() and watch_stop() the heaps track their own minimum
 * from the start on, so a dip between two samples (a TLS handshake) counts. */
static bool s_heapWatch;   // control task only
static portMUX_TYPE s_ssidMux = portMUX_INITIALIZER_UNLOCKED;
static char s_ssid[33];

/* Control-task-only state. */
static esp_netif_t* s_netif;
static std::atomic<bool> s_radio{false};   // esp_wifi_start() done; atomic for wifi_link_radio_idle()
static bool s_bleHeld;          // bleMidi_wifi_hold() taken, bleMidi_wifi_release() owed
static bool s_bleWasRunning;
/* Snapshot taken at the start of a connect sequence, zeroed at every stop.
 * PSRAM: it exists in every boot and holds passwords. */
EXT_RAM_BSS_ATTR static crosspad::WifiNetworkList s_try;
static size_t s_tryIdx;
static WifiFail s_lastFail;
static int s_retry;
static esp_timer_handle_t s_lingerTimer, s_connectTimer, s_retryTimer, s_rssiTimer;
/* esp_timer_stop() cannot take back a Linger already posted to the queue: the
 * flag says whether the linger that fired is still the one wanted. */
static bool s_lingerArmed;
/* A network picked on the device: tried alone, saved only once it has an IP
 * (spec §2). The caller leaves it in s_pendingCandidate and sets
 * s_pendingValid, both under the guard; the first ConnectCandidate the control
 * task handles copies it out, zeroes it and clears the flag. Picks made before
 * that overwrite one another, so the newest wins, and the messages they posted
 * after the first find the flag clear and do nothing. No heap payload rides
 * the queue, so a dropped message leaks nothing. */
EXT_RAM_BSS_ATTR static crosspad::WifiNetwork s_pendingCandidate;   // under the guard; PSRAM: exists in every boot
static bool s_pendingValid;                                         // under the guard
EXT_RAM_BSS_ATTR static crosspad::WifiNetwork s_candidate;          // control task only; read before any NVS write
static bool s_haveCandidate;                       // control task only

/* A candidate ends saved or not; either way its password is not kept here. */
static void drop_candidate() {
    s_haveCandidate = false;
    s_candidate = {};
}
/* Which attempt an IP or disconnect event belongs to, decided in event order,
 * not by when a task happens to run. The control task marks every attempt
 * boundary -- before each esp_wifi_connect(), after each esp_wifi_disconnect()
 * -- with a fence: an event of its own posted to the default event loop,
 * carrying the next value of s_fenceGen. The loop dispatches in post order, so
 * its handler stamps every driver and lwIP event with the last fence
 * dispatched before it (s_eventEpoch). An event is the current attempt's only
 * if no fence was posted after it, i.e. its stamp equals s_fenceGen. A stamp
 * read from the control task's counter instead was taken whenever the event
 * task ran, and an event delayed past a boundary took the new attempt's value.
 *
 * What remains assumed: the driver posts an attempt's events before the call
 * that ends it returns (wifi API calls run to completion on the WiFi task), so
 * our own disconnect's event lands before the fence that follows it -- the
 * same ordering the scan pause has relied on since 2026-10-03, weaker (posted,
 * not dispatched). DHCP is not covered by that: a lease can complete on the
 * lwIP task just after the station left, so on_got_ip() also checks the
 * address against the netif, which the stack zeroes on every disconnect. */
static const esp_event_base_t kFenceBase = "WIFI_LINK_FENCE";
static uint8_t s_fenceGen;     // control task only
static uint8_t s_eventEpoch;   // event loop task only
/* The loop task drains its queue at a priority far above this task's; a fence
 * not posted in this long leaves the epoch where it was (logged). */
static constexpr uint32_t kFenceWaitMs = 1000;

/* Dropped when the task is gone: then the radio is off and nothing but a
 * start (start_and_post) has anything to do. */
static void post(Msg m, uint8_t arg = 0, void* ptr = nullptr) {
    Req r{m, arg, ptr};
    Guard g;
    if (s_q && xQueueSend(s_q, &r, 0) != pdTRUE)
        ESP_LOGW(TAG, "control queue full, message %u dropped", (unsigned)m);
}

static size_t internal_free() { return heap_caps_get_free_size(kInternalCaps); }
static size_t internal_total_free() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
static size_t psram_free()    { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

static void lower_to(std::atomic<uint32_t>& low, size_t v) {
    if (v < low.load()) low.store((uint32_t)v);
}

/* Called at every step of a start and connect (driver up, connect issued,
 * IP, disconnect, RSSI poll) and at the stop: the gates are set from each
 * init - low. The minimum is the heaps' own since watch_start(), summed per
 * heap, so it may read a little low: the gates err on the safe side. */
static void sample_heap() {
    if (!s_heapWatch) return;
    lower_to(s_heapLow, heap_caps_get_minimum_free_size(kInternalCaps));
    lower_to(s_intLow, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    lower_to(s_psramLow, heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

/* The figures a start begins with; low == init until something is
 * allocated, so a refused start reads low == init. */
static void mark_start() {
    s_heapInit.store((uint32_t)internal_free());
    s_heapLow.store(s_heapInit.load());
    s_intInit.store((uint32_t)internal_total_free());
    s_intLow.store(s_intInit.load());
    s_psramInit.store((uint32_t)psram_free());
    s_psramLow.store(s_psramInit.load());
}

/* The local minimum is the heap component's one window: MEM's int_min reads
 * from the radio start while it is open, and the boot-time minimum again after
 * watch_stop(). */
static void watch_start() {
    heap_caps_monitor_local_minimum_free_size_start();
    s_heapWatch = true;
}

static void watch_stop() {
    if (!s_heapWatch) return;
    sample_heap();
    s_heapWatch = false;
    heap_caps_monitor_local_minimum_free_size_stop();
}

static void restart_timer(esp_timer_handle_t t, uint32_t ms) {
    esp_timer_stop(t);   // ESP_ERR_INVALID_STATE when idle: fine
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static void arm_linger() {
    s_lingerArmed = true;
    restart_timer(s_lingerTimer, kLingerMs);
}

static void cancel_linger() {
    s_lingerArmed = false;
    esp_timer_stop(s_lingerTimer);   // ESP_ERR_INVALID_STATE when idle: fine
}

static void set_state(WifiLinkState st) {
    s_state.store((uint8_t)st);
    status.wifiState = (uint8_t)st;
    status.wifiConnected = (st == WifiLinkState::Up);
    /* Absent when a start failed before radio_init() built the network stack. */
    EventGroupHandle_t ev = s_events.load();
    if (ev) {
        if (st == WifiLinkState::Up) xEventGroupSetBits(ev, kBitUp);
        else                         xEventGroupClearBits(ev, kBitUp);
    }
    /* WIFI_SET / a UI pick starts the radio without a lease; once that sequence
     * settles, nobody would ever arm the linger, so arm it here. */
    if ((st == WifiLinkState::Up || st == WifiLinkState::Failed) && s_leases.load() == 0)
        arm_linger();
}

static void fail(WifiFail f) {
    s_fail.store((uint8_t)f);
    set_state(WifiLinkState::Failed);
    ESP_LOGW(TAG, "failed: %s", crosspad::wifiFailName(f));
}

static void set_ssid(const char* ssid) {
    portENTER_CRITICAL(&s_ssidMux);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    portEXIT_CRITICAL(&s_ssidMux);
}

static WifiFail fail_from_reason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return WifiFail::Auth;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return WifiFail::NoNetwork;
        default:
            return WifiFail::Timeout;
    }
}

static void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == kFenceBase) {
        s_eventEpoch = (uint8_t)id;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGI(TAG, "disconnected, reason %u", (unsigned)d->reason);
        post(Msg::Disconnected, d->reason, (void*)(uintptr_t)s_eventEpoch);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* d = static_cast<ip_event_got_ip_t*>(data);
        post(Msg::GotIp, s_eventEpoch, (void*)(uintptr_t)d->ip_info.ip.addr);
    }
}

/* An attempt boundary (see s_fenceGen). */
static void fence() {
    const uint8_t next = s_fenceGen + 1;
    if (esp_event_post(kFenceBase, next, nullptr, 0, pdMS_TO_TICKS(kFenceWaitMs)) == ESP_OK)
        s_fenceGen = next;
    else
        ESP_LOGW(TAG, "attempt fence not posted: the previous attempt's events stay current");
}

static void timer_cb(void* arg) { post((Msg)(uintptr_t)arg); }

static esp_timer_handle_t make_timer(Msg m, const char* name) {
    esp_timer_create_args_t a = {};
    a.callback = timer_cb;
    a.arg = (void*)(uintptr_t)m;
    a.name = name;
    esp_timer_handle_t t = nullptr;
    esp_timer_create(&a, &t);
    return t;
}

static void stop_timer(esp_timer_handle_t t) { if (t) esp_timer_stop(t); }

static bool wifi_allowed() {
    if (!CROSSPAD_FW_RADIO) return false;   // the kit's app owns the radio
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    return cfg && cfg->wireless.enableWiFi;
}

/* BLE gets the radio back: restarted if it ran before, or started if someone
 * asked for it meanwhile (bleMidi_wifi_release()). */
static void after_radio_stopped() {
    if (!s_bleHeld) return;
    bleMidi_wifi_release(s_bleWasRunning);
    s_bleHeld = false;
    s_bleWasRunning = false;
}

/* esp_netif_create_default_wifi_sta() asserts on each of these steps; here a
 * failure is a failed start, not a reboot. */
static esp_err_t netif_create() {
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_netif = esp_netif_new(&ncfg);
    if (!s_netif) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_attach_wifi_station(s_netif);
    if (err == ESP_OK) err = esp_wifi_set_default_wifi_sta_handlers();
    return err;
}

/* Undoes any prefix of netif_create(): clears the driver and the default
 * handlers, then esp_netif_destroy(). */
static void netif_destroy() {
    if (!s_netif) return;
    esp_netif_destroy_default_wifi(s_netif);
    s_netif = nullptr;
}

/* Leaves the state at Stopping when the radio was on; the caller sets where it
 * ends (Off, or Failed through fail() — which must therefore come after this). */
static void radio_stop() {
    /* Every stop -- all networks failed, a lost link given up, WiFi switched
     * off, the linger -- ends a candidate unsaved. */
    drop_candidate();
    s_try = {};
    stop_timer(s_connectTimer); stop_timer(s_retryTimer); stop_timer(s_rssiTimer);
    if (s_radio) {
        set_state(WifiLinkState::Stopping);
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        netif_destroy();
        s_radio = false;
        ESP_LOGI(TAG, "radio stopped, internal free %u", (unsigned)internal_free());
    }
    watch_stop();
    s_rssi.store(0);
    s_ip.store(0);
    status.wifiRssi = 0;
    after_radio_stopped();
}

static void connect_index(size_t i) {
    stop_timer(s_connectTimer);
    if (i >= s_try.size()) {
        const WifiFail why = s_try.size() == 0 ? WifiFail::NoNetwork : s_lastFail;
        radio_stop();
        fail(why);
        return;
    }
    s_tryIdx = i;
    fence();
    const auto& n = s_try.at(i);
    wifi_config_t c = {};
    memcpy(c.sta.ssid, n.ssid, strlen(n.ssid));
    memcpy(c.sta.password, n.pass, strlen(n.pass));
    /* WPA2 at least: with WPA (TKIP) allowed, a same-name WPA1 access point
     * could take the password over a downgraded handshake. */
    c.sta.threshold.authmode = n.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &c);
    set_ssid(n.ssid);
    set_state(WifiLinkState::Connecting);
    ESP_LOGI(TAG, "connecting to \"%s\" (%u/%u)", n.ssid, (unsigned)i + 1, (unsigned)s_try.size());
    esp_wifi_connect();
    sample_heap();
    restart_timer(s_connectTimer, kConnectTimeoutMs);
}

static void start_sequence() {
    /* A reconnect retry still pending from the previous sequence would fire
     * into this one; a linger the previous Up/Failed armed would stop it. */
    stop_timer(s_retryTimer);
    cancel_linger();
    /* A sequence on a radio already up (WIFI_SET, a lease after a scan-only
     * start) would otherwise keep reporting the previous one's failure. */
    s_fail.store((uint8_t)WifiFail::None);
    if (s_haveCandidate) {
        s_try.clear();
        s_try.put(s_candidate.ssid, s_candidate.pass);
    } else {
        wifi_creds_snapshot(s_try);
    }
    s_lastFail = WifiFail::NoNetwork;
    s_retry = 0;
    connect_index(0);
}

static bool net_stack_locked();
static bool fits_locked(size_t taskBytes);

/* The radio's gates, before esp_wifi_init(): a start that cannot finish is
 * refused here rather than by the driver half way, which on hardware took
 * internal RAM down to 15 kB before handing it back. created: internal RAM
 * about to be built first (fits_locked()); psramCredit: PSRAM a running BLE
 * stack is about to hand back. */
static bool radio_fits(size_t created, size_t psramCredit) {
    const size_t needInt = created + kWifiMinInternalFree;
    const size_t needTotal = created + kWifiMinInternalTotal;
    const size_t freeInt = internal_free();
    const size_t freeTotal = internal_total_free();
    const size_t freePs = psram_free() + psramCredit;
    const size_t largestPs = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (freeInt >= needInt && freeTotal >= needTotal && freePs >= kWifiMinPsramFree &&
        largestPs >= kWifiMinPsramBlock)
        return true;
    ESP_LOGW(TAG, "not started: internal %u/%u B, internal with DMA pool %u/%u B, "
             "PSRAM %u/%u B, largest PSRAM block %u/%u B",
             (unsigned)freeInt, (unsigned)needInt, (unsigned)freeTotal, (unsigned)needTotal,
             (unsigned)freePs, (unsigned)kWifiMinPsramFree, (unsigned)largestPs,
             (unsigned)kWifiMinPsramBlock);
    return false;
}

/* Gate, network stack, netif, driver: the radio on, no connect. False: fail()
 * already says why and BLE is back if this stopped it. State is left to the
 * caller. */
static bool radio_init() {
    if (!wifi_allowed()) { fail(WifiFail::Disabled); return false; }
    if (usb_transfer_active()) { fail(WifiFail::TransferMode); return false; }
    /* Built here, on this task's stack, and tried again at every start until it
     * exists: the first lease comes from the LVGL thread or the 5 kB main task,
     * and esp_netif_init() + the default loop are the deepest calls of a start.
     * The gate comes first, as at spawn: lwIP's tcpip_init() asserts on a failed
     * allocation. */
    bool ready;
    {
        Guard g;
        ready = s_netReady || (fits_locked(0) && net_stack_locked());
    }
    if (!ready) { fail(WifiFail::NoMem); return false; }

    /* Held from here until the radio is off: a start asked for meanwhile
     * waits for after_radio_stopped(). */
    s_bleWasRunning = bleMidi_wifi_hold(kBleStopDelayMs);
    s_bleHeld = true;
    /* After BLE has handed its memory back: each init - low is then the
     * radio's own cost, which is what the gates below must cover. */
    mark_start();
    if (!radio_fits(0, 0)) {
        fail(WifiFail::NoMem);
        after_radio_stopped();
        return false;
    }
    watch_start();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;   // the driver keeps nothing in flash; saved networks are ours
    bool inited = false;
    esp_err_t err = netif_create();
    if (err == ESP_OK) { err = esp_wifi_init(&cfg); inited = (err == ESP_OK); }
    if (inited) sample_heap();
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        if (inited) esp_wifi_deinit();
        netif_destroy();
        watch_stop();
        fail(err == ESP_ERR_NO_MEM ? WifiFail::NoMem : WifiFail::Timeout);
        after_radio_stopped();
        return false;
    }
    sample_heap();
    s_radio = true;
    return true;
}

static void radio_start() {
    s_fail.store((uint8_t)WifiFail::None);
    set_state(WifiLinkState::Starting);
    if (radio_init()) start_sequence();
    else drop_candidate();
}

static void on_got_ip(uint8_t epoch, uint32_t ip) {
    sample_heap();
    /* Late events: an attempt already given up on, a radio already stopped, a
     * lease the station no longer holds (DHCP finishing after a disconnect,
     * past the fence), or an association that is no longer the network this
     * attempt is for. */
    if (!s_radio || (WifiLinkState)s_state.load() != WifiLinkState::Connecting ||
        epoch != s_fenceGen) return;
    esp_netif_ip_info_t held;
    if (ip == 0 || esp_netif_get_ip_info(s_netif, &held) != ESP_OK || held.ip.addr != ip) return;
    const char* ssid = s_try.at(s_tryIdx).ssid;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        strncmp((const char*)ap.ssid, ssid, sizeof(ap.ssid)) != 0) return;
    stop_timer(s_connectTimer);
    s_retry = 0;
    s_ip.store(ip);
    s_rssi.store(ap.rssi);
    status.wifiRssi = ap.rssi;
    if (s_haveCandidate) {
        wifi_creds_put(s_candidate.ssid, s_candidate.pass);   // control task: internal stack (wifi_creds.h)
        drop_candidate();
    } else {
        wifi_creds_promote(ssid);
    }
    set_state(WifiLinkState::Up);
    esp_timer_stop(s_rssiTimer);   // ESP_ERR_INVALID_STATE when idle: fine
    esp_timer_start_periodic(s_rssiTimer, (uint64_t)kRssiPollMs * 1000);
    ESP_LOGI(TAG, "up: \"%s\" rssi %d", ssid, (int)s_rssi.load());
}

static constexpr int kRetries = sizeof(kRetryDelaysMs) / sizeof(kRetryDelaysMs[0]);

/* A reconnect attempt after losing an up link did not get an IP. */
static void retry_failed() {
    stop_timer(s_connectTimer);
    if (s_retry >= kRetries) { radio_stop(); fail(WifiFail::Lost); return; }
    restart_timer(s_retryTimer, kRetryDelaysMs[s_retry]);
    ++s_retry;
}

/* Which reason to report when every saved network failed: a wrong password
 * says more than a timeout, a timeout more than "not found". */
static int fail_rank(WifiFail f) {
    switch (f) {
        case WifiFail::Auth:      return 3;
        case WifiFail::Timeout:   return 2;
        case WifiFail::NoNetwork: return 1;
        default:                  return 0;
    }
}

/* One saved network of the connect sequence did not get an IP. */
static void attempt_failed(WifiFail why) {
    if (fail_rank(why) > fail_rank(s_lastFail)) s_lastFail = why;
    connect_index(s_tryIdx + 1);
}

static void on_disconnected(uint8_t reason, uint8_t epoch) {
    sample_heap();
    /* The end of an attempt already moved past: a scan pauses one with a
     * disconnect the station reports as STA_LEAVING, not ASSOC_LEAVE, and it is
     * handled only after the resumed attempt has started. */
    if (epoch != s_fenceGen) return;
    /* Our own esp_wifi_disconnect() (ConnectNow, a timeout, a stop) reports
     * ASSOC_LEAVE; the code that called it has already moved on. */
    if (reason == WIFI_REASON_ASSOC_LEAVE) return;
    s_lastReason.store(reason);
    auto st = (WifiLinkState)s_state.load();
    if (st == WifiLinkState::Up) {
        /* A lost link stops reporting the address and signal it no longer has. */
        stop_timer(s_rssiTimer);
        s_ip.store(0);
        s_rssi.store(0);
        status.wifiRssi = 0;
        set_state(WifiLinkState::Connecting);
        s_retry = 1;
        restart_timer(s_retryTimer, kRetryDelaysMs[0]);
    } else if (st == WifiLinkState::Connecting) {
        if (s_retry > 0) retry_failed();
        else             attempt_failed(fail_from_reason(reason));
    }
}

/* Our own esp_wifi_disconnect() of an attempt still connecting reports
 * STA_LEAVING (36), which on_disconnected() would count as the next attempt
 * failing. The fence after the call makes that event stale whenever the queue
 * hands it over, also across a reconnect wait, before any connect fences. */
static void disconnect_attempt() {
    esp_wifi_disconnect();
    fence();
}

static void on_connect_timeout() {
    if ((WifiLinkState)s_state.load() != WifiLinkState::Connecting) return;
    disconnect_attempt();
    if (s_retry > 0) retry_failed();
    else             attempt_failed(WifiFail::Timeout);
}

static void on_retry() {
    fence();
    esp_wifi_connect();
    sample_heap();
    restart_timer(s_connectTimer, kConnectTimeoutMs);
}

static constexpr uint16_t kScanMax = WIFI_LINK_SCAN_MAX;
/* Radio start, a blocking active scan and one retry fit well inside this. */
static constexpr uint32_t kScanWaitMs = 15000;
/* The station refuses to scan for a moment after a disconnect. */
static constexpr uint32_t kScanRetryMs = 200;

/* One scan at a time: the caller holds scan_lock() for the whole request. The
 * control task fills s_scanOut and gives scan_done() after storing the sequence
 * number it served, so a caller that timed out never takes a later answer as
 * its own. Static, so a caller that gives up leaves nothing to free. */
static SemaphoreHandle_t scan_lock() {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}
static SemaphoreHandle_t scan_done() {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t s = xSemaphoreCreateBinaryStatic(&buf);
    return s;
}
EXT_RAM_BSS_ATTR static wifi_ap_t s_scanOut[kScanMax];
static int s_scanCount;
static std::atomic<uint8_t> s_scanServed{0};
static uint8_t s_scanSeq;   // under scan_lock()

/* Strongest record per SSID, hidden networks left out, strongest first. */
static int collapse_sorted(const wifi_ap_record_t* recs, uint16_t n, wifi_ap_t* out, int max) {
    int count = 0;
    for (uint16_t i = 0; i < n; ++i) {
        const char* ssid = (const char*)recs[i].ssid;
        if (!ssid[0]) continue;
        int at = -1;
        for (int j = 0; j < count; ++j) {
            if (strncmp(out[j].ssid, ssid, sizeof(out[j].ssid)) == 0) { at = j; break; }
        }
        if (at < 0) {
            if (count == max) continue;
            at = count++;
            strlcpy(out[at].ssid, ssid, sizeof(out[at].ssid));
            out[at].rssi = INT8_MIN;
        }
        if (recs[i].rssi > out[at].rssi) {
            out[at].rssi = recs[i].rssi;
            out[at].auth = (uint8_t)recs[i].authmode;
        }
    }
    std::sort(out, out + count, [](const wifi_ap_t& a, const wifi_ap_t& b) { return a.rssi > b.rssi; });
    return count;
}

/* A blocking active scan into s_scanOut. The station refuses to scan while it
 * connects, so an attempt in progress is paused and restarted on the same
 * network afterwards. With the radio off it comes up for the scan alone, no
 * connect. */
static void do_scan(uint8_t seq) {
    s_scanCount = WIFI_LINK_SCAN_REFUSED;   // radio_init()'s fail() says why
    const bool hadRadio = s_radio;
    if (s_radio || radio_init()) {
        const bool connecting = hadRadio && (WifiLinkState)s_state.load() == WifiLinkState::Connecting;
        if (connecting) {
            stop_timer(s_connectTimer);
            stop_timer(s_retryTimer);
            disconnect_attempt();
        }
        wifi_scan_config_t sc = {};
        esp_err_t err = esp_wifi_scan_start(&sc, true);
        if (err == ESP_ERR_WIFI_STATE) {
            vTaskDelay(pdMS_TO_TICKS(kScanRetryMs));
            err = esp_wifi_scan_start(&sc, true);
        }
        if (err == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            uint16_t cap = n < kScanMax ? n : kScanMax;
            auto* recs = static_cast<wifi_ap_record_t*>(
                heap_caps_calloc(cap ? cap : 1, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM));
            if (recs && esp_wifi_scan_get_ap_records(&cap, recs) == ESP_OK) {
                s_scanCount = collapse_sorted(recs, cap, s_scanOut, kScanMax);
            } else {
                ESP_LOGW(TAG, "scan: %s", recs ? "records not read" : "no memory for records");
                s_scanCount = WIFI_LINK_SCAN_FAILED;
                esp_wifi_clear_ap_list();
            }
            heap_caps_free(recs);
        } else {
            ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
            s_scanCount = WIFI_LINK_SCAN_FAILED;
        }
        if (connecting) {
            if (s_retry > 0) on_retry();
            else             connect_index(s_tryIdx);
        }
    }
    s_scanServed.store(seq);
    xSemaphoreGive(scan_done());
}

static void retire_if_idle();

/* Shared by both connect requests: the sequence starts over from its first
 * network, on a radio brought up for it if need be. */
static void restart_connect() {
    cancel_linger();
    if (!s_radio) radio_start();
    else { disconnect_attempt(); start_sequence(); }
}

static void handle(const Req& r) {
    auto st = (WifiLinkState)s_state.load();
    switch (r.msg) {
        case Msg::Acquire:
            cancel_linger();
            if (!s_radio) radio_start();
            /* The radio a scan brought up alone, under a Failed that
             * wifi_link_acquire() has just turned into Starting: a lease asks
             * for a connect, so give it one. */
            else if (st == WifiLinkState::Starting) start_sequence();
            break;
        case Msg::Release:
            if (s_leases.load() == 0) arm_linger();
            break;
        case Msg::Linger:
            if (s_lingerArmed && s_leases.load() == 0) {
                s_lingerArmed = false;
                bool failed = st == WifiLinkState::Failed;
                radio_stop();
                /* A radio a scan brought up alone stays on under Failed;
                 * radio_stop() then leaves Stopping. Failed comes back with its
                 * reason, stored directly: set_state() would arm the linger
                 * again. */
                if (failed) {
                    s_state.store((uint8_t)WifiLinkState::Failed);
                    status.wifiState = (uint8_t)WifiLinkState::Failed;
                } else {
                    set_state(WifiLinkState::Off);
                }
                retire_if_idle();
            }
            break;
        case Msg::GotIp:
            on_got_ip(r.arg, (uint32_t)(uintptr_t)r.ptr);
            break;
        case Msg::Disconnected:
            on_disconnected(r.arg, (uint8_t)(uintptr_t)r.ptr);
            break;
        case Msg::ConnectTimeout:
            on_connect_timeout();
            break;
        case Msg::Retry:
            on_retry();
            break;
        case Msg::RssiPoll: {
            sample_heap();
            /* A Release dropped by a full queue would leave the radio up for
             * good with no lease: an Up link with none arms the linger here. */
            if (s_leases.load() == 0 && !s_lingerArmed) arm_linger();
            wifi_ap_record_t ap;
            if (st == WifiLinkState::Up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                s_rssi.store(ap.rssi); status.wifiRssi = ap.rssi;
            }
            break;
        }
        case Msg::ConnectNow:
            drop_candidate();
            restart_connect();
            break;
        case Msg::ConnectCandidate: {
            /* handle() never runs under the guard: control_task() and the tail
             * of retire_if_idle() call it outside, so this does not nest. */
            bool picked;
            {
                Guard g;
                picked = s_pendingValid;
                if (picked) {
                    s_candidate = s_pendingCandidate;
                    s_pendingCandidate = {};   // the password stays in one place only
                    s_pendingValid = false;
                }
            }
            if (!picked) break;   // an earlier message already took the newest pick
            s_haveCandidate = true;
            restart_connect();
            break;
        }
        case Msg::EnableChanged:
            if (!wifi_allowed()) {
                if (s_radio) { radio_stop(); fail(WifiFail::Disabled); }
            } else if (!s_radio && s_leases.load() > 0) {
                /* Switched back on under a lease still held: that holder is
                 * waiting for a link. */
                radio_start();
            }
            break;
        case Msg::Scan:
            do_scan(r.arg);
            break;
    }
}

static void delete_objects_locked() {
    esp_timer_handle_t* timers[] = {&s_lingerTimer, &s_connectTimer, &s_retryTimer, &s_rssiTimer};
    for (auto* t : timers) {
        if (*t) { esp_timer_stop(*t); esp_timer_delete(*t); *t = nullptr; }
    }
    if (s_q) { vQueueDelete(s_q); s_q = nullptr; }
}

/* After the last stop. A start already queued (a lease, a WIFI_SET) keeps the
 * task: what was drained before it is handled in order. Otherwise the task
 * deletes its objects and itself; vTaskDelete() comes after the guard is
 * released, and nothing static is touched after that. */
static void retire_if_idle() {
    Req pending[kQueueLen];
    size_t n = 0;
    bool retire;
    {
        Guard g;
        bool keep = s_leases.load() > 0;
        Req r;
        while (!keep && n < kQueueLen && xQueueReceive(s_q, &r, 0) == pdTRUE) {
            pending[n++] = r;
            keep = (r.msg == Msg::Acquire || r.msg == Msg::ConnectNow || r.msg == Msg::ConnectCandidate);
        }
        retire = !keep;
        if (retire) {
            s_stackFreeLast = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
            delete_objects_locked();
            s_task = nullptr;
            ESP_LOGI(TAG, "control task retired, internal free %u", (unsigned)internal_free());
        }
    }
    if (retire) vTaskDelete(nullptr);
    for (size_t i = 0; i < n; ++i) handle(pending[i]);
}

static void control_task(void*) {
    Req r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) == pdTRUE) handle(r);
    }
}

/* esp_netif_init() (lwIP's tcpip task) and the default event loop cannot be
 * undone: once WiFi has run they stay, with our two handlers on them. */
static bool net_stack_locked() {
    if (s_netReady) return true;
    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) {
        e = esp_event_loop_create_default();
        if (e == ESP_ERR_INVALID_STATE) e = ESP_OK;   // someone else made it: fine
    }
    if (e == ESP_OK) e = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, nullptr);
    if (e == ESP_OK) {
        e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr);
        if (e != ESP_OK) esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event);
    }
    if (e == ESP_OK) {
        e = esp_event_handler_register(kFenceBase, ESP_EVENT_ANY_ID, on_wifi_event, nullptr);
        if (e != ESP_OK) {
            esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event);
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event);
        }
    }
    if (e != ESP_OK) { ESP_LOGE(TAG, "network stack: %s", esp_err_to_name(e)); return false; }
    if (!s_events.load()) {
        EventGroupHandle_t ev = xEventGroupCreate();
        if (!ev) {
            esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event);
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event);
            esp_event_handler_unregister(kFenceBase, ESP_EVENT_ANY_ID, on_wifi_event);
            ESP_LOGE(TAG, "network stack: no event group");
            return false;
        }
        s_events.store(ev);
    }
    s_netReady = true;
    return true;
}

/* Before anything is created: lwIP's tcpip_init() asserts on a failed
 * allocation, and the network stack, once built, never comes back. radio_init()
 * stops a running BLE stack before its own gate, so then only the margin is
 * asked for on top of what is about to be created: taskBytes (the control task
 * at spawn, nothing when it runs already) and the network stack until built. */
static bool fits_locked(size_t taskBytes) {
    mark_start();   // a refusal here leaves WIFI_STATUS its evidence
    const size_t created = taskBytes + (s_netReady ? 0 : kNetStackBytes);
    if (!crosspad::getEspBleMidi().isRunning()) return radio_fits(created, 0);
    /* BLE's internal RAM comes back before radio_init() checks the radio's
     * share, so only what is created first, with the margin, is asked for
     * here; its PSRAM is credited. */
    const size_t need = created + kSpawnMargin;
    const size_t freeInt = internal_free();
    if (freeInt < need) {
        ESP_LOGW(TAG, "not started: %u B internal free, needs %u B",
                 (unsigned)freeInt, (unsigned)need);
        return false;
    }
    const size_t freePs = psram_free() + kBlePsramBytes;
    if (freePs < kWifiMinPsramFree) {
        ESP_LOGW(TAG, "not started: %u B PSRAM free with BLE's back, needs %u B",
                 (unsigned)freePs, (unsigned)kWifiMinPsramFree);
        return false;
    }
    /* Not credited: BLE's PSRAM may come back in pieces, so only a block free
     * now is sure. Checked here, before the network stack, which is never
     * freed: a start radio_fits() would refuse once BLE has stopped should not
     * leave it behind. What this branch cannot see is the all-internal gate
     * (kWifiMinInternalTotal): BLE hands back far more than the stack takes,
     * so it is left to radio_fits() -- if that refuses, the network stack
     * (kNetStackBytes) stays built and BLE comes back. */
    const size_t largestPs = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largestPs < kWifiMinPsramBlock) {
        ESP_LOGW(TAG, "not started: largest PSRAM block %u B, needs %u B",
                 (unsigned)largestPs, (unsigned)kWifiMinPsramBlock);
        return false;
    }
    return true;
}

static bool spawn_locked() {
    if (!fits_locked(kControlTaskBytes)) return false;
    s_lingerArmed = false;
    s_q = xQueueCreate(kQueueLen, sizeof(Req));
    s_lingerTimer  = make_timer(Msg::Linger, "wifi_linger");
    s_connectTimer = make_timer(Msg::ConnectTimeout, "wifi_conn");
    s_retryTimer   = make_timer(Msg::Retry, "wifi_retry");
    s_rssiTimer    = make_timer(Msg::RssiPoll, "wifi_rssi");
    if (s_q && s_lingerTimer && s_connectTimer && s_retryTimer && s_rssiTimer &&
        xTaskCreatePinnedToCore(control_task, "wifi_ctl", kTaskStack, nullptr, kTaskPrio,
                                &s_task, 0) == pdPASS) {
        return true;
    }
    ESP_LOGE(TAG, "control task not created: %u B internal free", (unsigned)internal_free());
    s_task = nullptr;
    delete_objects_locked();
    return false;
}

/* False when there is not enough internal RAM to start or the control task
 * could not be created. */
static bool start_and_post(Msg m) {
    Req r{m, 0, nullptr};
    Guard g;
    if (!s_q && !spawn_locked()) return false;
    if (xQueueSend(s_q, &r, 0) != pdTRUE)
        ESP_LOGW(TAG, "control queue full, message %u dropped", (unsigned)m);
    return true;
}

/* No task, so the radio is off: the outcome can be set from here. */
static void refuse_no_task() {
    s_fail.store((uint8_t)WifiFail::NoMem);
    s_state.store((uint8_t)WifiLinkState::Failed);
    status.wifiState = (uint8_t)WifiLinkState::Failed;
}

bool wifi_link_acquire(const char* reason) {
    if (!wifi_allowed()) { s_fail.store((uint8_t)WifiFail::Disabled); return false; }
    if (usb_transfer_active()) { s_fail.store((uint8_t)WifiFail::TransferMode); return false; }
    s_leases.fetch_add(1);
    /* A Failed left by an earlier attempt would answer wifi_link_wait_up() before
     * the control task has even seen this lease. Under Failed the radio is off,
     * or on for a scan alone (do_scan()); either way this lease is about to
     * start a connect sequence (radio_start(), or start_sequence() from
     * Msg::Acquire): say so now. */
    uint8_t failed = (uint8_t)WifiLinkState::Failed;
    if (s_state.compare_exchange_strong(failed, (uint8_t)WifiLinkState::Starting))
        status.wifiState = (uint8_t)WifiLinkState::Starting;
    ESP_LOGI(TAG, "lease +%s (%u)", reason ? reason : "?", (unsigned)s_leases.load());
    if (!start_and_post(Msg::Acquire)) {
        s_leases.fetch_sub(1);
        refuse_no_task();
        return false;
    }
    return true;
}

void wifi_link_release(void) {
    uint8_t cur = s_leases.load();
    while (cur > 0 && !s_leases.compare_exchange_weak(cur, cur - 1)) {}
    if (cur == 0) return;   // unbalanced release: ignore, never underflow
    post(Msg::Release);
}

uint8_t wifi_link_state(void) { return s_state.load(); }
uint8_t wifi_link_fail(void)  { return s_fail.load(); }
uint8_t wifi_link_leases(void) { return s_leases.load(); }

bool wifi_link_radio_idle(void) {
    const auto st = (WifiLinkState)s_state.load();
    return s_leases.load() == 0 && !s_radio.load() && st != WifiLinkState::Starting && st != WifiLinkState::Stopping;
}

bool wifi_link_wait_up(uint32_t timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        auto st = (WifiLinkState)s_state.load();
        if (st == WifiLinkState::Up) return true;
        if (st == WifiLinkState::Failed) return false;
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return false;
        EventGroupHandle_t ev = s_events.load();
        if (ev) xEventGroupWaitBits(ev, kBitUp, pdFALSE, pdTRUE, pdMS_TO_TICKS(kWaitPollMs));
        else    vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
    }
}

bool wifi_link_info(char* ssid, size_t n, int8_t* rssi, uint32_t* ip) {
    if (ssid && n) {
        portENTER_CRITICAL(&s_ssidMux);
        strlcpy(ssid, s_ssid, n);
        portEXIT_CRITICAL(&s_ssidMux);
    }
    if (rssi) *rssi = s_rssi.load();
    if (ip) *ip = s_ip.load();
    return (WifiLinkState)s_state.load() == WifiLinkState::Up;
}

uint8_t  wifi_link_last_reason(void) { return s_lastReason.load(); }
uint32_t wifi_link_heap_init(void)   { return s_heapInit.load(); }
uint32_t wifi_link_heap_low(void)    { return s_heapLow.load(); }
uint32_t wifi_link_int_init(void)    { return s_intInit.load(); }
uint32_t wifi_link_int_low(void)     { return s_intLow.load(); }
uint32_t wifi_link_psram_init(void)  { return s_psramInit.load(); }
uint32_t wifi_link_psram_low(void)   { return s_psramLow.load(); }

uint32_t wifi_link_stack_free(void) {
    Guard g;
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : s_stackFreeLast;
}

void wifi_link_connect_now(void) {
    if (!start_and_post(Msg::ConnectNow)) refuse_no_task();
}
void wifi_link_connect_candidate(const char* ssid, const char* pass) {
    {
        Guard g;   // released before start_and_post(), which takes it again
        snprintf(s_pendingCandidate.ssid, sizeof(s_pendingCandidate.ssid), "%s", ssid ? ssid : "");
        snprintf(s_pendingCandidate.pass, sizeof(s_pendingCandidate.pass), "%s", pass ? pass : "");
        s_pendingValid = true;
    }
    if (!start_and_post(Msg::ConnectCandidate)) {
        /* No task to take it: the pick is refused, not left for a later one. */
        {
            Guard g;
            s_pendingCandidate = {};
            s_pendingValid = false;
        }
        refuse_no_task();
    }
}
void wifi_link_enable_changed(void) { post(Msg::EnableChanged); }

int wifi_link_scan(wifi_ap_t* out, int max) {
    if (!wifi_link_acquire("scan")) return WIFI_LINK_SCAN_REFUSED;
    xSemaphoreTake(scan_lock(), portMAX_DELAY);
    const uint8_t seq = ++s_scanSeq;
    xSemaphoreTake(scan_done(), 0);   // a give left by a request whose caller gave up
    post(Msg::Scan, seq);
    int result = WIFI_LINK_SCAN_TIMEOUT;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kScanWaitMs);
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0 || xSemaphoreTake(scan_done(), deadline - now) != pdTRUE) break;
        if (s_scanServed.load() != seq) continue;   // an older request's answer
        if (s_scanCount >= 0) {
            result = s_scanCount < max ? s_scanCount : max;
            memcpy(out, s_scanOut, (size_t)result * sizeof(wifi_ap_t));
        } else {
            result = s_scanCount;
        }
        break;
    }
    xSemaphoreGive(scan_lock());
    wifi_link_release();
    return result;
}

const char* wifi_link_auth_name(uint8_t auth) {
    switch ((wifi_auth_mode_t)auth) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2e";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2wpa3";   // the WiFi page tells SAE-only and mixed apart
        case WIFI_AUTH_OWE:             return "owe";
        default:                        return "other";
    }
}

/* An NVS erase: internal stack, as wifi_creds.h asks. */
static constexpr uint32_t kResetTaskStack = 4096;
static constexpr UBaseType_t kResetTaskPrio = 2;
static constexpr BaseType_t kResetTaskCore = 1;

static void factory_reset_task(void*) {
    wifi_creds_clear();   // logs a failure itself
    wifi_link_enable_changed();
    ESP_LOGW(TAG, "factory reset: saved networks forgotten");
    vTaskDelete(nullptr);
}

bool wifi_factory_reset(void) {
    if (xTaskCreatePinnedToCore(factory_reset_task, "wifi_reset", kResetTaskStack, nullptr,
                                kResetTaskPrio, nullptr, kResetTaskCore) == pdPASS)
        return true;
    ESP_LOGE(TAG, "factory reset: no task, %u B internal free", (unsigned)internal_free());
    return false;
}
