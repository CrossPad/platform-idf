/**
 * @file EspBleMidi.cpp
 * @brief NimBLE-backed BLE MIDI transport for ESP-IDF.
 *
 * Implements the BLE MIDI 1.0 specification (Apple/MMA): service
 * 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic
 * 7772E5DB-3868-4112-A1A9-F2669D106BF3, 13-bit millisecond timestamps.
 *
 * Sending never blocks the caller: messages go into a small queue that a
 * dedicated task drains, coalescing everything that piled up during the
 * previous connection interval into one packet. A pad hit therefore costs one
 * queue send whether BLE is connected, idle, or compiled out.
 */

#include "EspBleMidi.hpp"
#include "BleCompanion.hpp"
#include "BleControlMailbox.hpp"

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "BleMidi";

#if CONFIG_BT_NIMBLE_ENABLED

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace crosspad {
namespace {

/* ── UUIDs (BLE_UUID128_INIT takes little-endian bytes) ───────────────── */

const ble_uuid128_t kMidiSvcUuid = BLE_UUID128_INIT(
    0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7,
    0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03);

const ble_uuid128_t kMidiChrUuid = BLE_UUID128_INIT(
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1,
    0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77);

constexpr size_t   kTxQueueDepth  = 64;
constexpr size_t   kMaxPacketSize = 20;    ///< fits the 23-byte default ATT MTU
constexpr uint16_t kNoHandle      = 0xFFFF;

/// One outbound MIDI message (status + up to two data bytes).
struct TxMsg {
    uint8_t status;
    uint8_t d0;
    uint8_t d1;
    uint8_t len;    ///< total bytes on the wire, 1..3
    int64_t atUs;   ///< the event's time (esp_timer), stamped at enqueue
};

struct State {
    std::atomic<bool>     running{false};
    std::atomic<bool>     synced{false};
    std::atomic<bool>     connected{false};
    std::atomic<bool>     subscribed{false};   ///< peer enabled notifications
    std::atomic<bool>     advertising{false};
    std::atomic<bool>     scanning{false};
    std::atomic<uint16_t> connHandle{kNoHandle};
    std::atomic<uint16_t> connInterval{0};
    std::atomic<uint16_t> peerValHandle{kNoHandle};  ///< central mode

    BleMidiMode mode = BleMidiMode::Server;
    uint16_t    chrValHandle = 0;             ///< our own GATT value handle
    uint8_t     ownAddrType = 0;
    std::string deviceName = "CrossPad";

    ble_addr_t pendingConnAddr{};
    bool       havePendingConn = false;

    /* Written by the NimBLE host task, read by any (the Settings page polls
     * it from the LVGL task): a std::string copied mid-assignment is torn. */
    BleMidiDevice              peer;
    mutable std::mutex         peerMutex;
    std::vector<BleMidiDevice> scanResults;
    std::vector<ble_addr_t>    scanAddrs;
    mutable std::mutex         scanMutex;

    QueueHandle_t     txQueue = nullptr;
    TaskHandle_t      txTask = nullptr;
    std::atomic<bool> txRun{false};
    std::atomic<bool> txParked{false};

    EspBleMidi::Stats stats;

    /* Inbound parser state — touched only from the NimBLE host task. */
    uint8_t runningStatus = 0;
    bool    inSysEx = false;
};

State g;

/* ── Helpers ──────────────────────────────────────────────────────────── */

std::string addrToString(const ble_addr_t &a)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             a.val[5], a.val[4], a.val[3], a.val[2], a.val[1], a.val[0]);
    return std::string(buf);
}

bool parseAddr(const std::string &s, ble_addr_t &out)
{
    unsigned v[6];
    if (sscanf(s.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) out.val[i] = (uint8_t)v[5 - i];
    return true;
}

int midiDataBytes(uint8_t status)
{
    if (status >= 0xF8) return 0;
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        default: break;
    }
    switch (status) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        default: return 0;
    }
}

uint16_t nowMs13()
{
    return (uint16_t)((esp_timer_get_time() / 1000) & 0x1FFF);
}

uint16_t ms13(int64_t atUs)
{
    return (uint16_t)((atUs / 1000) & 0x1FFF);
}

/* ── Outbound: queue → BLE packet ─────────────────────────────────────── */

bool canSend()
{
    if (!g.running.load() || !g.connected.load()) return false;
    if (g.mode == BleMidiMode::Server) return g.subscribed.load();
    return g.peerValHandle.load() != kNoHandle;
}

void sendPacket(const uint8_t *buf, size_t len)
{
    uint16_t conn = g.connHandle.load();
    if (conn == kNoHandle) return;

    int rc;
    if (g.mode == BleMidiMode::Server) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        if (!om) { g.stats.txErrors++; return; }
        rc = ble_gatts_notify_custom(conn, g.chrValHandle, om);
    } else {
        rc = ble_gattc_write_no_rsp_flat(conn, g.peerValHandle.load(), buf, len);
    }
    if (rc == 0) g.stats.txPackets++;
    else         g.stats.txErrors++;
}

void txTaskFn(void *)
{
    uint8_t packet[kMaxPacketSize];
    TxMsg   msg;

    while (g.txRun.load()) {
        if (xQueueReceive(g.txQueue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!g.txRun.load()) break;

        uint16_t ts = nowMs13();
        uint8_t  tsHigh = (uint8_t)((ts >> 7) & 0x3F);
        size_t   n = 0;
        packet[n++] = (uint8_t)(0x80 | tsHigh);

        for (;;) {
            ts = ms13(msg.atUs);
            uint8_t high = (uint8_t)((ts >> 7) & 0x3F);
            /* Flush when the packet is full or the 13-bit timestamp rolled
             * into the next 128 ms window (the header holds the high bits for
             * the whole packet). */
            if (n + 1 + msg.len > kMaxPacketSize || high != tsHigh) {
                if (n > 1) sendPacket(packet, n);
                tsHigh = high;
                n = 0;
                packet[n++] = (uint8_t)(0x80 | tsHigh);
            }
            packet[n++] = (uint8_t)(0x80 | (ts & 0x7F));
            packet[n++] = msg.status;
            if (msg.len > 1) packet[n++] = msg.d0;
            if (msg.len > 2) packet[n++] = msg.d1;

            if (n + 4 > kMaxPacketSize) break;
            if (xQueueReceive(g.txQueue, &msg, 0) != pdTRUE) break;
        }
        if (n > 1) sendPacket(packet, n);
    }
    /* The stack lives in PSRAM, so it cannot be freed by the task standing
     * on it: park here and let end() release it with vTaskDeleteWithCaps(). */
    g.txParked.store(true);
    vTaskSuspend(nullptr);
}

void enqueue(uint8_t status, uint8_t d0, uint8_t d1, uint8_t len, int64_t atUs = 0)
{
    if (!g.txQueue || !canSend()) { g.stats.txDropped++; return; }
    /* Stamped here when the caller has no time of its own: every message in a
     * packet then carries a queue-order (monotonic) timestamp, and a clock tick
     * stamped with its own time never lands before a note stamped later. */
    TxMsg m{status, d0, d1, len, atUs ? atUs : esp_timer_get_time()};
    if (xQueueSend(g.txQueue, &m, 0) != pdTRUE) g.stats.txDropped++;
    else                                        g.stats.txMessages++;
}

/* ── GATT server ──────────────────────────────────────────────────────── */

int chrAccess(uint16_t conn_handle, uint16_t attr_handle,
              struct ble_gatt_access_ctxt *ctxt, void *arg);

const struct ble_gatt_chr_def kMidiChrs[] = {
    {
        .uuid         = &kMidiChrUuid.u,
        .access_cb    = chrAccess,
        .arg          = nullptr,
        .descriptors  = nullptr,
        .flags        = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
                        BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle   = &g.chrValHandle,
        .cpfd         = nullptr,
    },
    { },
};

BleControlMailbox controlMailbox;
void (*controlWake)() = nullptr;
// 7e400001/2/3-b5a3-f393-e0a9-e50e24dcca9e: control service, RX, reply.
const ble_uuid128_t controlService = BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x7e);
const ble_uuid128_t controlRx = BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x7e);
const ble_uuid128_t controlReply = BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x7e);
int controlAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t data[192]; uint16_t length = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &length) != 0 || !controlMailbox.write(data, length)) return BLE_ATT_ERR_UNLIKELY;
        // An accepted write ending in LF is a whole command; the mailbox holds one at a time.
        if (data[length - 1] == '\n' && controlWake) controlWake();
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static uint8_t reply[481];  // NimBLE host task only
        const size_t length = controlMailbox.read(reply);
        return os_mbuf_append(ctxt->om, reply, length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
const struct ble_gatt_chr_def controlChrs[] = {
    { .uuid = &controlRx.u, .access_cb = controlAccess, .arg = nullptr, .descriptors = nullptr, .flags = BLE_GATT_CHR_F_WRITE, .min_key_size = 0, .val_handle = nullptr, .cpfd = nullptr },
    { .uuid = &controlReply.u, .access_cb = controlAccess, .arg = nullptr, .descriptors = nullptr, .flags = BLE_GATT_CHR_F_READ, .min_key_size = 0, .val_handle = nullptr, .cpfd = nullptr },
    { }
};

const struct ble_gatt_svc_def kGattSvcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &kMidiSvcUuid.u,
        .includes        = nullptr,
        .characteristics = kMidiChrs,
    },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &controlService.u, .includes = nullptr, .characteristics = controlChrs },
    { },
};

int chrAccess(uint16_t conn_handle, uint16_t attr_handle,
              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            /* The spec says a read of the MIDI characteristic returns no data. */
            return 0;
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint8_t  buf[128];
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            getEspBleMidi().feedPacket(buf, len);
            return 0;
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ── GAP ──────────────────────────────────────────────────────────────── */

int  gapEvent(struct ble_gap_event *event, void *arg);
void startAdvertisingInternal();
void startScanInternal(uint16_t durationMs);

void requestFastConnParams(uint16_t conn)
{
    /* BLE MIDI wants the shortest interval the peer will grant. This is a
     * request — Windows and macOS renegotiate as they see fit. */
    struct ble_gap_upd_params p = {};
    p.itvl_min = 6;                 // 7.5 ms
    p.itvl_max = 12;                // 15 ms
    p.latency = 0;
    p.supervision_timeout = 400;    // 4 s
    ble_gap_update_params(conn, &p);
}

/* Central-mode discovery chain: service → characteristic → CCCD. */

int onDiscDsc(uint16_t conn_handle, const struct ble_gatt_error *error,
              uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg; (void)error;
    if (!dsc) return 0;
    if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
        uint8_t value[2] = {0x01, 0x00};
        ble_gattc_write_no_rsp_flat(conn_handle, dsc->handle, value, sizeof(value));
        ESP_LOGI(TAG, "subscribed to peer MIDI characteristic");
    }
    return 0;
}

int onDiscChr(uint16_t conn_handle, const struct ble_gatt_error *error,
              const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg; (void)error;
    if (chr && ble_uuid_cmp(&chr->uuid.u, &kMidiChrUuid.u) == 0) {
        g.peerValHandle.store(chr->val_handle);
        ble_gattc_disc_all_dscs(conn_handle, chr->val_handle,
                                chr->val_handle + 2, onDiscDsc, nullptr);
    }
    return 0;
}

int onDiscSvc(uint16_t conn_handle, const struct ble_gatt_error *error,
              const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (service) {
        ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                service->end_handle, onDiscChr, nullptr);
    } else if (error && error->status == BLE_HS_EDONE &&
               g.peerValHandle.load() == kNoHandle) {
        ESP_LOGW(TAG, "peer exposes no BLE MIDI service");
    }
    return 0;
}

bool advHasMidiSvc(const struct ble_hs_adv_fields &f)
{
    for (int i = 0; i < f.num_uuids128; ++i) {
        if (ble_uuid_cmp(&f.uuids128[i].u, &kMidiSvcUuid.u) == 0) return true;
    }
    return false;
}

void handleDisc(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) return;
    if (!advHasMidiSvc(fields)) return;

    std::string addr = addrToString(disc->addr);
    std::string name;
    if (fields.name_len > 0) name.assign((const char *)fields.name, fields.name_len);

    std::vector<BleMidiDevice> snapshot;
    {
        std::lock_guard<std::mutex> lock(g.scanMutex);
        for (auto &d : g.scanResults) {
            if (d.address == addr) { d.rssi = disc->rssi; return; }
        }
        BleMidiDevice dev;
        dev.name    = name.empty() ? std::string("BLE MIDI") : name;
        dev.address = addr;
        dev.rssi    = disc->rssi;
        g.scanResults.push_back(dev);
        g.scanAddrs.push_back(disc->addr);
        snapshot = g.scanResults;
        ESP_LOGI(TAG, "found %s (%s) rssi=%d", dev.name.c_str(), addr.c_str(),
                 disc->rssi);
    }
    getEspBleMidi().notifyScanResults(snapshot);
}

int gapEvent(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g.connected.store(true);
            g.connHandle.store(event->connect.conn_handle);
            g.runningStatus = 0;
            g.inSysEx = false;

            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                std::lock_guard<std::mutex> lock(g.peerMutex);
                g.peer.address = addrToString(desc.peer_id_addr);
                g.peer.connected = true;
                g.connInterval.store(desc.conn_itvl);
            }
            if (g.mode == BleMidiMode::Server) {
                g.advertising.store(false);
            } else {
                g.peerValHandle.store(kNoHandle);
                ble_gattc_disc_svc_by_uuid(event->connect.conn_handle,
                                           &kMidiSvcUuid.u, onDiscSvc, nullptr);
            }
            requestFastConnParams(event->connect.conn_handle);
            ESP_LOGI(TAG, "connected to %s (itvl=%u)", g.peer.address.c_str(),
                     (unsigned)g.connInterval.load());
            getEspBleMidi().notifyConnectionChanged(true, g.peer);
        } else if (g.mode == BleMidiMode::Server) {
            startAdvertisingInternal();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        controlMailbox.reset();
        BleMidiDevice was = g.peer;
        was.connected = false;
        g.connected.store(false);
        g.subscribed.store(false);
        g.connHandle.store(kNoHandle);
        g.peerValHandle.store(kNoHandle);
        g.connInterval.store(0);
        {
            std::lock_guard<std::mutex> lock(g.peerMutex);
            g.peer = BleMidiDevice{};
        }
        getEspBleMidi().notifyConnectionChanged(false, was);
        if (g.running.load() && g.mode == BleMidiMode::Server) {
            startAdvertisingInternal();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            g.connInterval.store(desc.conn_itvl);
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g.chrValHandle) {
            g.subscribed.store(event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "peer %s notifications",
                     event->subscribe.cur_notify ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (g.running.load() && g.mode == BleMidiMode::Server) {
            startAdvertisingInternal();
        }
        return 0;

    case BLE_GAP_EVENT_DISC:
        handleDisc(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        g.scanning.store(false);
        if (g.havePendingConn) {
            g.havePendingConn = false;
            ble_gap_connect(g.ownAddrType, &g.pendingConnAddr, 10000, nullptr,
                            gapEvent, nullptr);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t  buf[128];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &len) == 0) {
            getEspBleMidi().feedPacket(buf, len);
        }
        return 0;
    }

    default:
        return 0;
    }
}

void startAdvertisingInternal()
{
    if (!g.synced.load() || g.connected.load()) return;

    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&kMidiSvcUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    /* The 128-bit UUID already fills the 31-byte advertisement, so the name
     * goes into the scan response. */
    struct ble_hs_adv_fields rsp = {};
    rsp.name = (uint8_t *)g.deviceName.c_str();
    rsp.name_len = (uint8_t)g.deviceName.size();
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv = {};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(g.ownAddrType, nullptr, BLE_HS_FOREVER, &adv,
                           gapEvent, nullptr);
    if (rc != 0) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    g.advertising.store(true);
    ESP_LOGI(TAG, "advertising as '%s'", g.deviceName.c_str());
}

void startScanInternal(uint16_t durationMs)
{
    if (!g.synced.load()) return;
    struct ble_gap_disc_params p = {};
    p.filter_duplicates = 1;
    int32_t duration = durationMs ? (int32_t)durationMs : BLE_HS_FOREVER;
    int rc = ble_gap_disc(g.ownAddrType, duration, &p, gapEvent, nullptr);
    if (rc != 0) { ESP_LOGE(TAG, "scan rc=%d", rc); return; }
    g.scanning.store(true);
    ESP_LOGI(TAG, "scanning for BLE MIDI peripherals");
}

void onSync()
{
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &g.ownAddrType) != 0) {
        g.ownAddrType = BLE_OWN_ADDR_PUBLIC;
    }
    g.synced.store(true);
    ESP_LOGI(TAG, "stack synced (own addr type %d)", g.ownAddrType);
    if (g.mode == BleMidiMode::Server) startAdvertisingInternal();
    else                               startScanInternal(0);
}

void onReset(int reason)
{
    ESP_LOGW(TAG, "stack reset, reason %d", reason);
    g.synced.store(false);
    g.connected.store(false);
    g.subscribed.store(false);
}

void hostTask(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

} // namespace

/* ── EspBleMidi ───────────────────────────────────────────────────────── */

EspBleMidi &EspBleMidi::instance()
{
    static EspBleMidi inst;
    return inst;
}

EspBleMidi &getEspBleMidi() { return EspBleMidi::instance(); }

bool EspBleMidi::isSupported() { return true; }
bool EspBleMidi::isRunning() const { return g.running.load(); }
EspBleMidi::Stats EspBleMidi::getStats() const { return g.stats; }
void EspBleMidi::resetStats() { g.stats = Stats{}; }
std::string EspBleMidi::getOwnAddress() const
{
    if (!g.running.load()) return {};
    /* ownAddrType is a BLE_OWN_ADDR_* value; ble_hs_id_copy_addr() wants a
     * BLE_ADDR_* one. The low bit carries public-vs-random in both, and the
     * RPA variants still resolve to the identity address we want here. */
    const uint8_t idType = (g.ownAddrType & 1) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    ble_addr_t a{};
    a.type = idType;
    if (ble_hs_id_copy_addr(idType, a.val, nullptr) != 0) return {};
    return addrToString(a);
}
uint16_t EspBleMidi::getConnInterval() const { return g.connInterval.load(); }

const char *EspBleMidi::getStateName() const
{
    if (!g.running.load())   return "off";
    if (g.connected.load())  return g.subscribed.load() || g.mode == BleMidiMode::Host
                                    ? "connected" : "connected-nosub";
    if (g.scanning.load())   return "scanning";
    if (g.advertising.load())return "advertising";
    if (!g.synced.load())    return "starting";
    return "idle";
}

void EspBleMidi::feedPacket(const uint8_t *p, size_t len)
{
    g.stats.rxPackets++;
    if (len < 2 || !(p[0] & 0x80)) return;   // no header byte → not BLE MIDI

    size_t  i = 1;
    uint8_t status = g.runningStatus;
    bool    inSysEx = g.inSysEx;
    uint8_t data[2] = {0, 0};
    int     dataIdx = 0;
    int     dataNeeded = status ? midiDataBytes(status) : 0;

    while (i < len) {
        uint8_t b = p[i];
        if (b & 0x80) {
            /* A timestamp byte. What follows is either a status byte or, under
             * running status, the first data byte of the next message. */
            if (++i >= len) break;
            uint8_t s = p[i];
            if (s & 0x80) {
                ++i;
                if (s == 0xF0) { inSysEx = true;  status = 0; continue; }
                if (s == 0xF7) { inSysEx = false; status = 0; continue; }
                if (s >= 0xF8) { continue; }              // realtime — ignored
                status = s;
                dataNeeded = midiDataBytes(s);
                dataIdx = 0;
                if (dataNeeded == 0) dispatchParsed(status, 0, 0);
                continue;
            }
            b = s;   // running status: this is a data byte
        }
        ++i;
        if (inSysEx || status == 0) continue;
        data[dataIdx++] = b;
        if (dataIdx >= dataNeeded) {
            dispatchParsed(status, data[0], dataNeeded > 1 ? data[1] : 0);
            dataIdx = 0;
        }
    }

    g.runningStatus = status;
    g.inSysEx = inSysEx;
}

void EspBleMidi::dispatchParsed(uint8_t status, uint8_t d0, uint8_t d1)
{
    g.stats.rxMessages++;
    uint8_t ch = status & 0x0F;
    switch (status & 0xF0) {
        case 0x90:
            if (d1 == 0) dispatchNoteOff(ch, d0, 0);
            else         dispatchNoteOn(ch, d0, d1);
            break;
        case 0x80: dispatchNoteOff(ch, d0, d1); break;
        case 0xB0: dispatchCC(ch, d0, d1);      break;
        default: break;
    }
}

void EspBleMidi::notifyConnectionChanged(bool connected, const BleMidiDevice &dev)
{
    dispatchConnectionChanged(connected, dev);
}

void EspBleMidi::notifyScanResults(const std::vector<BleMidiDevice> &devices)
{
    dispatchScanResults(devices);
}

bool EspBleMidi::begin(BleMidiMode mode)
{
    if (g.running.load()) {
        if (g.mode == mode) return true;
        end();
    }

    g.mode = mode;
    g.stats = Stats{};
    g.runningStatus = 0;
    g.inSysEx = false;
    g.peerValHandle.store(kNoHandle);
    g.connHandle.store(kNoHandle);
    {
        std::lock_guard<std::mutex> lock(g.scanMutex);
        g.scanResults.clear();
        g.scanAddrs.clear();
    }

    g.txQueue = xQueueCreate(kTxQueueDepth, sizeof(TxMsg));
    if (!g.txQueue) { ESP_LOGE(TAG, "tx queue alloc failed"); return false; }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        vQueueDelete(g.txQueue);
        g.txQueue = nullptr;
        return false;
    }

    ble_hs_cfg.reset_cb  = onReset;
    ble_hs_cfg.sync_cb   = onSync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(kGattSvcs);
    if (rc == 0) rc = ble_gatts_add_svcs(kGattSvcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed rc=%d", rc);
        nimble_port_deinit();
        vQueueDelete(g.txQueue);
        g.txQueue = nullptr;
        return false;
    }

    ble_svc_gap_device_name_set(g.deviceName.c_str());

    g.txRun.store(true);
    g.txParked.store(false);
    if (xTaskCreatePinnedToCoreWithCaps(txTaskFn, "ble_midi_tx", 3072, nullptr, 5,
                                       &g.txTask, 0,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "tx task creation failed");
        g.txRun.store(false);
        nimble_port_deinit();
        vQueueDelete(g.txQueue);
        g.txQueue = nullptr;
        return false;
    }

    g.running.store(true);
    nimble_port_freertos_init(hostTask);
    ESP_LOGI(TAG, "BLE MIDI started in %s mode",
             mode == BleMidiMode::Server ? "Server" : "Host");
    return true;
}

void EspBleMidi::end()
{
    if (!g.running.load()) return;
    ESP_LOGI(TAG, "stopping BLE MIDI");

    g.running.store(false);

    if (g.scanning.load())    { ble_gap_disc_cancel(); g.scanning.store(false); }
    if (g.advertising.load()) { ble_gap_adv_stop();    g.advertising.store(false); }
    uint16_t conn = g.connHandle.load();
    if (conn != kNoHandle) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    g.txRun.store(false);
    if (g.txTask) {
        if (g.txQueue) {
            TxMsg wake{0, 0, 0, 2};      // unblocks the task so it can retire
            xQueueSend(g.txQueue, &wake, 0);
        }
        for (int i = 0; i < 30 && !g.txParked.load(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
        for (int i = 0; i < 50 && eTaskGetState(g.txTask) != eSuspended; ++i)
            vTaskDelay(pdMS_TO_TICKS(2));
        if (eTaskGetState(g.txTask) == eSuspended) {
            vTaskDeleteWithCaps(g.txTask);
            g.txTask = nullptr;
        } else {
            ESP_LOGW(TAG, "tx task never parked — stack left allocated");
        }
    }

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGW(TAG, "nimble_port_stop rc=%d — stack left running", rc);
    }

    if (g.txQueue) { vQueueDelete(g.txQueue); g.txQueue = nullptr; }

    g.synced.store(false);
    g.connected.store(false);
    g.subscribed.store(false);
    g.connHandle.store(kNoHandle);
    g.peerValHandle.store(kNoHandle);
    g.connInterval.store(0);
    {
        std::lock_guard<std::mutex> lock(g.peerMutex);
        g.peer = BleMidiDevice{};
    }
    {
        std::lock_guard<std::mutex> lock(g.scanMutex);
        g.scanResults.clear();
        g.scanAddrs.clear();
    }
}

bool EspBleMidi::isConnected() const { return g.connected.load(); }
BleMidiMode EspBleMidi::getMode() const { return g.mode; }

void EspBleMidi::startScan(uint16_t durationMs)
{
    if (!g.running.load() || g.mode != BleMidiMode::Host) return;
    {
        std::lock_guard<std::mutex> lock(g.scanMutex);
        g.scanResults.clear();
        g.scanAddrs.clear();
    }
    startScanInternal(durationMs);
}

void EspBleMidi::stopScan()
{
    if (!g.scanning.load()) return;
    ble_gap_disc_cancel();
    g.scanning.store(false);
}

bool EspBleMidi::isScanning() const { return g.scanning.load(); }

std::vector<BleMidiDevice> EspBleMidi::getScanResults() const
{
    std::lock_guard<std::mutex> lock(g.scanMutex);
    return g.scanResults;
}

bool EspBleMidi::connectToDevice(const std::string &address)
{
    if (!g.running.load() || g.mode != BleMidiMode::Host) return false;

    ble_addr_t addr{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g.scanMutex);
        for (size_t i = 0; i < g.scanResults.size(); ++i) {
            if (address.empty() || g.scanResults[i].address == address) {
                addr = g.scanAddrs[i];
                found = true;
                break;
            }
        }
    }
    if (!found) {
        addr.type = BLE_ADDR_PUBLIC;
        if (!parseAddr(address, addr)) return false;
    }

    if (g.scanning.load()) {
        /* A scan must end before a connect can start; DISC_COMPLETE picks the
         * pending address up. */
        g.pendingConnAddr = addr;
        g.havePendingConn = true;
        ble_gap_disc_cancel();
        g.scanning.store(false);
    }
    if (g.havePendingConn) {
        g.havePendingConn = false;
        return ble_gap_connect(g.ownAddrType, &addr, 10000, nullptr, gapEvent,
                               nullptr) == 0;
    }
    return ble_gap_connect(g.ownAddrType, &addr, 10000, nullptr, gapEvent,
                           nullptr) == 0;
}

void EspBleMidi::startAdvertising(const std::string &deviceName)
{
    if (!deviceName.empty() && deviceName != g.deviceName) {
        g.deviceName = deviceName;
        if (g.running.load()) ble_svc_gap_device_name_set(g.deviceName.c_str());
    }
    if (!g.running.load() || g.mode != BleMidiMode::Server) return;
    if (g.advertising.load()) return;
    startAdvertisingInternal();
}

void EspBleMidi::stopAdvertising()
{
    if (!g.advertising.load()) return;
    ble_gap_adv_stop();
    g.advertising.store(false);
}

bool EspBleMidi::isAdvertising() const { return g.advertising.load(); }

void EspBleMidi::disconnect()
{
    uint16_t conn = g.connHandle.load();
    if (conn != kNoHandle) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

BleMidiDevice EspBleMidi::getConnectedDevice() const
{
    std::lock_guard<std::mutex> lock(g.peerMutex);
    return g.peer;
}

/* ── Outbound API ─────────────────────────────────────────────────────── */

void EspBleMidi::sendNoteOn(uint8_t note, uint8_t channel)  { sendNoteOn(note, 127, channel); }
void EspBleMidi::sendNoteOff(uint8_t note, uint8_t channel) { sendNoteOff(note, 0, channel); }

void EspBleMidi::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel)
{
    uint8_t out;
    if (!applyTxOffset(note & 0x7F, out)) { g.stats.txDropped++; return; }
    enqueue((uint8_t)(0x90 | (channel & 0x0F)), out, velocity & 0x7F, 3);
}

void EspBleMidi::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel)
{
    uint8_t out;
    if (!applyTxOffset(note & 0x7F, out)) { g.stats.txDropped++; return; }
    enqueue((uint8_t)(0x80 | (channel & 0x0F)), out, velocity & 0x7F, 3);
}

void EspBleMidi::sendControlChange(uint8_t controller, uint8_t value, uint8_t channel)
{
    enqueue((uint8_t)(0xB0 | (channel & 0x0F)), controller & 0x7F, value & 0x7F, 3);
}

void EspBleMidi::sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel)
{
    /* Poly aftertouch is addressed by note, so it has to travel with the same
     * transpose or it lands on a note the peer is not holding. */
    uint8_t out;
    if (!applyTxOffset(note & 0x7F, out)) { g.stats.txDropped++; return; }
    enqueue((uint8_t)(0xA0 | (channel & 0x0F)), out, pressure & 0x7F, 3);
}

void EspBleMidi::sendProgramChange(uint8_t program, uint8_t channel)
{
    enqueue((uint8_t)(0xC0 | (channel & 0x0F)), program & 0x7F, 0, 2);
}

void EspBleMidi::sendPitchBend(int16_t value, uint8_t channel)
{
    int v = value + 8192;
    if (v < 0) v = 0;
    if (v > 16383) v = 16383;
    enqueue((uint8_t)(0xE0 | (channel & 0x0F)), (uint8_t)(v & 0x7F),
            (uint8_t)((v >> 7) & 0x7F), 3);
}

void EspBleMidi::sendRealTime(uint8_t status, int64_t atUs)
{
    if (status < 0xF8) return;
    enqueue(status, 0, 0, 1, atUs);
}

void bleCompanionSetWake(void (*wake)()) { controlWake = wake; }

void bleCompanionPoll(void (*handler)(const char *, char *, size_t)) {
    // Static, not on the caller's stack: the main task has 4 kB, and these
    // 673 bytes on top of a HIL handler's own frame (STM_FW_INFO reads a
    // file) overflowed it. Only the main loop calls this.
    static char command[192];
    static char reply[481];
    uint32_t generation;
    if (!controlMailbox.take(command, generation)) return;
    reply[0] = 0;
    handler(command, reply, sizeof(reply));
    controlMailbox.finish(reply, generation);
}

} // namespace crosspad

#else  /* !CONFIG_BT_NIMBLE_ENABLED — BLE compiled out of this build */

namespace crosspad {
void bleCompanionPoll(void (*)(const char *, char *, size_t)) {}
void bleCompanionSetWake(void (*)()) {}

EspBleMidi &EspBleMidi::instance() { static EspBleMidi inst; return inst; }
EspBleMidi &getEspBleMidi() { return EspBleMidi::instance(); }

bool EspBleMidi::isSupported() { return false; }
bool EspBleMidi::begin(BleMidiMode)
{
    ESP_LOGW(TAG, "BLE MIDI requested but NimBLE is not enabled in this build");
    return false;
}
void EspBleMidi::end() {}
bool EspBleMidi::isConnected() const { return false; }
BleMidiMode EspBleMidi::getMode() const { return BleMidiMode::Server; }
void EspBleMidi::startScan(uint16_t) {}
void EspBleMidi::stopScan() {}
bool EspBleMidi::isScanning() const { return false; }
std::vector<BleMidiDevice> EspBleMidi::getScanResults() const { return {}; }
bool EspBleMidi::connectToDevice(const std::string &) { return false; }
void EspBleMidi::startAdvertising(const std::string &) {}
void EspBleMidi::stopAdvertising() {}
bool EspBleMidi::isAdvertising() const { return false; }
void EspBleMidi::disconnect() {}
BleMidiDevice EspBleMidi::getConnectedDevice() const { return {}; }
void EspBleMidi::sendNoteOn(uint8_t, uint8_t) {}
void EspBleMidi::sendNoteOff(uint8_t, uint8_t) {}
void EspBleMidi::sendNoteOn(uint8_t, uint8_t, uint8_t) {}
void EspBleMidi::sendNoteOff(uint8_t, uint8_t, uint8_t) {}
void EspBleMidi::sendControlChange(uint8_t, uint8_t, uint8_t) {}
void EspBleMidi::sendAftertouch(uint8_t, uint8_t, uint8_t) {}
void EspBleMidi::sendProgramChange(uint8_t, uint8_t) {}
void EspBleMidi::sendPitchBend(int16_t, uint8_t) {}
void EspBleMidi::sendRealTime(uint8_t, int64_t) {}
void EspBleMidi::feedPacket(const uint8_t *, size_t) {}
void EspBleMidi::dispatchParsed(uint8_t, uint8_t, uint8_t) {}
void EspBleMidi::notifyConnectionChanged(bool, const BleMidiDevice &) {}
void EspBleMidi::notifyScanResults(const std::vector<BleMidiDevice> &) {}
bool EspBleMidi::isRunning() const { return false; }
EspBleMidi::Stats EspBleMidi::getStats() const { return {}; }
void EspBleMidi::resetStats() {}
uint16_t EspBleMidi::getConnInterval() const { return 0; }
const char *EspBleMidi::getStateName() const { return "unsupported"; }
std::string EspBleMidi::getOwnAddress() const { return {}; }

} // namespace crosspad

#endif /* CONFIG_BT_NIMBLE_ENABLED */
