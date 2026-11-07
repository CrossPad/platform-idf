#include "cp_remote.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "mqtt_client.h"

#include "bsp_worker.h"
#include "cp_account.h"
#include "crosspad/net/RemoteFrame.hpp"
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "fw_image_desc.h"
#include "hil_control.h"
#include "wifi_link.h"

#if !defined(CONFIG_MQTT_EVENT_QUEUE_SIZE) || CONFIG_MQTT_EVENT_QUEUE_SIZE < 2
#error "cp_remote needs CONFIG_MQTT_USE_CUSTOM_CONFIG=y and CONFIG_MQTT_EVENT_QUEUE_SIZE>=2 (esp-mqtt events raised by the sender task's publish while its own task posts)"
#endif

static const char* TAG = "cp_remote";

namespace {
enum class St : uint8_t { Off, Waiting, NoWifi, Connecting, Online, Backoff, Unlinked };
constexpr const char* kStNames[] = {"off", "waiting", "no_wifi", "connecting", "online", "backoff", "unlinked"};
constexpr const char* kLease = "remote";
constexpr int64_t kLeaseRetryUs = 60LL * 1000 * 1000;
constexpr uint32_t kLinkRetryMinS = 60;          // WiFi link failures: 60 s, doubling...
constexpr uint32_t kLinkRetryMaxS = 30 * 60;     // ...to 30 min (a failing join costs a radio start each time)
constexpr uint32_t kBackoffMinS = 5;
constexpr uint32_t kBackoffMaxS = 300;
constexpr size_t kQueue = 5;                     // the running one + 4 waiting (spec §3.5)
constexpr uint32_t kSupervisePeriodMs = 1000;
constexpr uint32_t kTeardownStack = 4096;
constexpr uint32_t kSenderStack = 6144;          // a TLS write on a PSRAM stack (fw_updater's install task: 6144)
constexpr int kKeepaliveS = 60;                  // Cloudflare closes a WebSocket idle for 100 s
constexpr int kNetworkTimeoutMs = 10000;         // esp-mqtt: each of DNS/TCP/TLS/WS/CONNACK
constexpr int64_t kConnectDeadlineUs = 30LL * 1000 * 1000;   // DNS + TCP/TLS (10 s) + CONNACK (10 s) can add up
constexpr size_t kOutboxLimit = 8 * 1024;        // unacknowledged replies (QoS 1); a full outbox drops the newest
constexpr size_t kReplyBuf = 8192;               // > kRemoteReplyMax: the companion overflow guard must not fire first

struct Slot { char id[crosspad::kRemoteIdMax + 1]; char command[crosspad::kRemoteCommandMax + 1]; };

/* Allocated in PSRAM when the client is built, freed after it is destroyed:
 * 15 kB that are not held while remote management is off. */
struct Bufs {
    Slot queue[kQueue];
    char reply[kReplyBuf];
    char frame[crosspad::kRemoteIdMax + 2 + crosspad::kRemoteReplyMax + 1];
};
}  // namespace

/* Locking. esp-mqtt runs on_event() on its own task WITH the client's api lock
 * held, and every esp_mqtt_client_* call from another task takes that lock. So
 * s_lock (queue, s_client, s_sender, s_qgen) is never held across a call into
 * esp-mqtt: the handler would wait for s_lock while the caller waits for the api
 * lock. Replies are published by a sender task of their own: the MQTT task polls
 * its socket for up to 1 s outside the lock and handles nothing queued for it
 * meanwhile, and the main loop must not wait for the lock. */
static StaticSemaphore_t s_lockBuf;
static SemaphoreHandle_t s_lock;
static Bufs* s_bufs;                             // under s_lock; freed by teardown_task() only
static size_t s_head, s_count;                   // ring of kQueue, under s_lock
static uint32_t s_qgen;                          // bumped when the queue is dropped, under s_lock
static esp_mqtt_client_handle_t s_client;        // under s_lock; written by the worker alone
static char s_uri[160], s_user[16], s_pass[80], s_topicReq[32], s_topicRsp[32], s_topicState[32], s_topicPeers[32];
static uint32_t s_authGen;
static std::atomic<St> s_state{St::Off};
static std::atomic<uint8_t> s_peers{0};
static std::atomic<uint32_t> s_done{0}, s_busy{0};
static std::atomic<bool> s_tearing{false};
static std::atomic<bool> s_pollBusy{false};      // cp_remote_poll() uses s_bufs / the client handle
static std::atomic<bool> s_frameBusy{false};     // the reply frame is s_bufs->frame[s_frameLen], not yet published
static size_t s_frameLen;                        // written under s_lock before s_frameBusy and the notify
static TaskHandle_t s_sender;                    // under s_lock; nulled by teardown() before the task is stopped
static TaskHandle_t s_tdSender;                  // teardown() -> teardown_task() (one teardown at a time: s_tearing)
static std::atomic<bool> s_senderStop{false};
static std::atomic<bool> s_senderAlive{false};
static std::atomic<const char*> s_reason{"-"};
static bool s_lease;
static int64_t s_retryAt, s_connectBy;           // worker only
static std::atomic<uint32_t> s_backoffS{kBackoffMinS};
static std::atomic<bool> s_backoffArm{false};    // a disconnect asks the worker for a retry time
static std::atomic<uint32_t> s_linkRetryS{kLinkRetryMinS};
static std::atomic<bool> s_retryNow{false};      // cp_remote_retry_now() -> the worker
static void (*s_wake)(void);

namespace {
struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};
}  // namespace

/* A stale event of a client that is being torn down must not move the state. */
static bool state_move(St from, St to) {
    St expected = from;
    return s_state.compare_exchange_strong(expected, to);
}

static bool wanted(void) {
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    return cp_account_linked() && cfg && cfg->wireless.enableWiFi && cfg->wireless.enableRemote;
}

/* The next attempt after the WiFi link failed or the lease was refused. A
 * memory gate is retried every minute (it clears when an app lets go); anything
 * else (no network, wrong password, lost link) backs off to kLinkRetryMaxS. */
static int64_t link_retry_at(int64_t now) {
    if (static_cast<crosspad::WifiFail>(wifi_link_fail()) == crosspad::WifiFail::NoMem) return now + kLeaseRetryUs;
    const uint32_t wait = s_linkRetryS;
    s_linkRetryS = wait * 2 > kLinkRetryMaxS ? kLinkRetryMaxS : wait * 2;
    ESP_LOGI(TAG, "link retry in %u s", (unsigned)wait);
    return now + (int64_t)wait * 1000 * 1000;
}

static void publish_state_online(esp_mqtt_client_handle_t c) {
    char st[64];
    snprintf(st, sizeof st, "online fw=%s rev=v%u", esp_app_get_description()->version,
             (unsigned)fw_image_desc_running()->board_rev);
    esp_mqtt_client_enqueue(c, s_topicState, st, 0, 0, 1, true);
}

static void answer_now(esp_mqtt_client_handle_t c, const char* id, const char* text) {
    char out[48];
    const size_t n = crosspad::formatRemoteReply(id, text, out, sizeof out);
    esp_mqtt_client_enqueue(c, s_topicRsp, out, (int)n, 1, 0, true);
}

static void on_event(void*, esp_event_base_t, int32_t id, void* data) {
    auto* e = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED:
            if (!state_move(St::Connecting, St::Online)) break;
            ESP_LOGI(TAG, "connected");
            s_backoffS = kBackoffMinS;
            s_linkRetryS = kLinkRetryMinS;
            esp_mqtt_client_subscribe(e->client, s_topicReq, 1);
            esp_mqtt_client_subscribe(e->client, s_topicPeers, 0);
            publish_state_online(e->client);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_peers = 0;
            {
                Lock l;                   // a request of this session is not run in the next one
                s_count = 0;
                ++s_qgen;
            }
            s_backoffArm = true;          // set before the state moves, so the worker sees both
            if (state_move(St::Online, St::Backoff) || state_move(St::Connecting, St::Backoff))
                ESP_LOGI(TAG, "disconnected");
            else
                s_backoffArm = false;
            break;
        case MQTT_EVENT_ERROR:
            if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED &&
                (state_move(St::Connecting, St::Unlinked) || state_move(St::Online, St::Unlinked))) {
                s_reason = "unlinked";         // unpaired on crosspad.app: drop the token, stop
                ESP_LOGW(TAG, "broker refused the token");
                cp_account_revoked(s_authGen);
            }
            break;
        case MQTT_EVENT_DATA: {
            if (e->current_data_offset != 0 || e->data_len != e->total_data_len) break;   // never > buffer
            if (e->topic_len == (int)strlen(s_topicPeers) && !memcmp(e->topic, s_topicPeers, e->topic_len)) {
                char n[4] = {};
                memcpy(n, e->data, e->data_len < 3 ? e->data_len : 3);
                s_peers = (uint8_t)atoi(n);
                break;
            }
            if (e->topic_len != (int)strlen(s_topicReq) || memcmp(e->topic, s_topicReq, e->topic_len)) break;
            crosspad::RemoteRequest r;
            if (!crosspad::parseRemoteRequest(e->data, (size_t)e->data_len, r)) break;
            bool queued = false, current;
            {
                Lock l;
                current = e->client == s_client;                             // not a client being torn down
                if (current && s_bufs && s_count < kQueue) {
                    Slot& s = s_bufs->queue[(s_head + s_count) % kQueue];
                    memcpy(s.id, r.id, sizeof s.id);
                    memcpy(s.command, r.command, sizeof s.command);
                    ++s_count;
                    queued = true;
                }
            }
            if (!queued) {
                if (current) {
                    ++s_busy;
                    answer_now(e->client, r.id, "ERR busy\r\n");
                }
            } else if (s_wake) s_wake();
            break;
        }
        default:
            break;
    }
}

static void free_bufs(void) {
    Bufs* b;
    {
        Lock l;
        b = s_bufs;
        s_bufs = nullptr;
    }
    heap_caps_free(b);
}

static void sender_task(void* arg);

static bool build_client(int64_t now) {
    char serial[16];
    cp_account_serial(serial, sizeof serial);
    const char* base = cp_account_base();                 // https://host[:port] or http://…
    const bool tls = strncmp(base, "https://", 8) == 0;
    snprintf(s_uri, sizeof s_uri, "%s%s/api/device/mqtt/%s", tls ? "wss://" : "ws://",
             base + (tls ? 8 : 7), serial);
    char https[160];
    snprintf(https, sizeof https, "%s/api/device/mqtt/%s", base, serial);
    char header[80];
    if (!cp_account_auth_header(https, header, sizeof header, &s_authGen)) {
        s_reason = "no_token";
        return false;
    }
    strlcpy(s_pass, header + 7, sizeof s_pass);           // "Bearer <token>"
    mbedtls_platform_zeroize(header, sizeof header);
    strlcpy(s_user, serial, sizeof s_user);
    snprintf(s_topicReq, sizeof s_topicReq, "cp/%s/req", serial);
    snprintf(s_topicRsp, sizeof s_topicRsp, "cp/%s/rsp", serial);
    snprintf(s_topicState, sizeof s_topicState, "cp/%s/state", serial);
    snprintf(s_topicPeers, sizeof s_topicPeers, "cp/%s/peers", serial);

    auto* bufs = static_cast<Bufs*>(heap_caps_malloc(sizeof(Bufs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bufs) {
        mbedtls_platform_zeroize(s_pass, sizeof s_pass);
        s_reason = "no_mem";
        return false;
    }

    esp_mqtt_client_config_t c = {};
    c.broker.address.uri = s_uri;
    c.credentials.username = s_user;
    c.credentials.client_id = s_user;
    c.credentials.authentication.password = s_pass;
    c.session.keepalive = kKeepaliveS;
    c.session.last_will.topic = s_topicState;
    c.session.last_will.msg = "offline";
    c.session.last_will.retain = 1;
    c.network.disable_auto_reconnect = true;              // the supervisor backs off
    c.network.timeout_ms = kNetworkTimeoutMs;
    c.buffer.size = 1024;                                 // a request: topic + 8 + 512
    c.buffer.out_size = sizeof bufs->frame + 64;
    c.outbox.limit = kOutboxLimit;
    c.task.stack_size = 6144;
    if (tls) c.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    esp_mqtt_client_handle_t h = esp_mqtt_client_init(&c);
    mbedtls_platform_zeroize(s_pass, sizeof s_pass);      // esp-mqtt keeps its own copy
    if (!h) {
        heap_caps_free(bufs);
        s_reason = "mqtt_init";
        return false;
    }
    esp_mqtt_client_register_event(h, MQTT_EVENT_ANY, on_event, nullptr);
    {
        Lock l;
        s_bufs = bufs;
        s_head = s_count = 0;
        ++s_qgen;
        s_client = h;
    }
    s_frameBusy = false;
    s_backoffArm = false;
    s_connectBy = now + kConnectDeadlineUs;
    s_state = St::Connecting;
    s_senderStop = false;
    s_senderAlive = true;
    TaskHandle_t sender = nullptr;
    if (xTaskCreatePinnedToCoreWithCaps(sender_task, "cp_remote_tx", kSenderStack, h, 2, &sender, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_senderAlive = false;
        s_reason = "no_mem";
        return false;                                     // supervise() tears the client down
    }
    {
        Lock l;
        s_sender = sender;
    }
    if (esp_mqtt_client_start(h) == ESP_OK) return true;
    s_reason = "mqtt_start";
    return false;                                         // supervise() tears the client down
}

/* Publishes the reply frame cp_remote_poll() left in s_bufs->frame. The blocking
 * publish takes the client's api lock (free while the MQTT task sits in its
 * socket poll) and writes at once. Ends only when teardown asks, after which
 * it touches nothing: teardown_task() waits for s_senderAlive before destroying
 * the client and before freeing the buffers. */
static void sender_task(void* arg) {
    auto h = static_cast<esp_mqtt_client_handle_t>(arg);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_senderStop) break;
        if (!s_frameBusy) continue;
        Bufs* b;
        size_t n;
        {
            Lock l;
            b = s_bufs;
            n = s_frameLen;
        }
        if (b && esp_mqtt_client_publish(h, s_topicRsp, b->frame, (int)n, 1, 0) >= 0) ++s_done;
        s_frameBusy = false;                  // the outbox / the wire has its own copy
        bool more;
        {
            Lock l;
            more = s_count != 0;
        }
        if (more && s_wake) s_wake();
    }
    s_senderAlive = false;
    vTaskDeleteWithCaps(nullptr);
}

/* esp_mqtt_client_stop() waits for the client task to end (seconds inside a TLS
 * handshake): never in the worker job, a one-shot task does it. */
static void teardown_task(void* arg) {
    auto h = static_cast<esp_mqtt_client_handle_t>(arg);
    s_senderStop = true;
    if (s_tdSender) xTaskNotifyGive(s_tdSender);
    /* A publish already waiting for the api lock goes first: the lock is a mutex and the
     * sender (priority 2) outranks this task (1) when the MQTT task lets go of it. */
    esp_mqtt_client_stop(h);
    while (s_senderAlive) vTaskDelay(pdMS_TO_TICKS(10));  // the sender is gone: nothing calls into h any more
    esp_mqtt_client_destroy(h);   // no event handler and no reply frame can touch s_bufs after this
    while (s_pollBusy) vTaskDelay(pdMS_TO_TICKS(10));     // cp_remote_poll() may still be inside a command
    free_bufs();
    wifi_link_release();          // a client exists only while the lease is held
    s_tearing = false;
    vTaskDeleteWithCaps(nullptr);
}

static void teardown(St next) {
    esp_mqtt_client_handle_t h;
    TaskHandle_t sender;
    {
        Lock l;
        h = s_client;
        s_client = nullptr;
        sender = s_sender;
        s_sender = nullptr;                   // cp_remote_poll() notifies under this lock: never a stopped task
        s_count = 0;
        ++s_qgen;
    }
    s_peers = 0;
    const bool lease = s_lease;
    s_lease = false;
    if (!h) {
        free_bufs();              // a build that failed before the client existed
        s_state = next;
        if (lease) wifi_link_release();
        return;
    }
    s_tearing = true;
    s_tdSender = sender;
    if (xTaskCreatePinnedToCoreWithCaps(teardown_task, "cp_remote_off", kTeardownStack, h, 1, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_tearing = false;        // no memory for the task: leave the client to the next pass
        Lock l;
        s_client = h;
        s_sender = sender;
        s_lease = lease;
        return;
    }
    s_state = next;
}

/* bsp_worker job, every kSupervisePeriodMs: short, never blocks. */
static uint32_t supervise(void*) {
    if (s_tearing) return 0;
    const int64_t now = esp_timer_get_time();
    if (s_retryNow.exchange(false)) {
        s_retryAt = 0;
        s_linkRetryS = kLinkRetryMinS;
    }
    if (!wanted()) {
        const bool linked = cp_account_linked();
        const bool revoked = !linked && cp_account_notice();   // the server unpaired the board; else never paired / unlinked by hand
        s_reason = linked ? "-" : revoked ? "unlinked" : "not_paired";
        s_retryAt = 0;
        s_backoffS = kBackoffMinS;
        const St next = revoked ? St::Unlinked : St::Off;
        if (s_lease) teardown(next);
        else s_state = next;
        return 0;
    }
    if (s_state == St::Unlinked) {            // the broker refused a token the board has since replaced
        teardown(St::Off);
        return 0;
    }
    if (!s_lease) {
        if (now < s_retryAt) return 0;
        if (!wifi_link_acquire(kLease)) {
            s_reason = crosspad::wifiFailName(static_cast<crosspad::WifiFail>(wifi_link_fail()));
            s_state = St::NoWifi;
            s_retryAt = link_retry_at(now);
            return 0;
        }
        s_lease = true;
        s_state = St::Waiting;
    }
    const auto link = static_cast<crosspad::WifiLinkState>(wifi_link_state());
    if (link == crosspad::WifiLinkState::Failed) {
        s_reason = crosspad::wifiFailName(static_cast<crosspad::WifiFail>(wifi_link_fail()));
        s_retryAt = link_retry_at(now);
        teardown(St::NoWifi);
        return 0;
    }
    if (link != crosspad::WifiLinkState::Up) return 0;
    s_reason = "-";
    esp_mqtt_client_handle_t h;
    {
        Lock l;
        h = s_client;
    }
    if (!h) {
        if (!build_client(now)) {
            s_retryAt = now + kLeaseRetryUs;
            teardown(St::Backoff);
        }
        return 0;
    }
    if (s_state == St::Connecting && now > s_connectBy) {   // the client task may have ended without a DISCONNECTED
        s_reason = "mqtt_timeout";
        s_retryAt = now + kLeaseRetryUs;
        teardown(St::Backoff);
        return 0;
    }
    if (s_state == St::Backoff) {
        if (s_backoffArm.exchange(false)) {
            const uint32_t wait = s_backoffS;
            /* Plus 0-50 % at random: boards a broker restart dropped together do not come back together. */
            const uint32_t waitMs = wait * 1000 + esp_random() % (wait * 500 + 1);
            s_retryAt = now + (int64_t)waitMs * 1000;
            s_backoffS = wait * 2 > kBackoffMaxS ? kBackoffMaxS : wait * 2;
            ESP_LOGI(TAG, "retry in %u ms", (unsigned)waitMs);
        } else if (now >= s_retryAt) {
            s_connectBy = now + kConnectDeadlineUs;
            s_state = St::Connecting;
            if (esp_mqtt_client_reconnect(h) != ESP_OK) {   // h is the worker's own: only teardown() drops it
                s_reason = "reconnect";
                s_retryAt = now + kLeaseRetryUs;
                teardown(St::Backoff);
            }
        }
    }
    return 0;
}

void cp_remote_init(bool run) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lockBuf);
    if (!run) {
        s_reason = "boot_mode";
        return;
    }
    bsp_worker_add("cp_remote", kSupervisePeriodMs, supervise, nullptr);
}

bool cp_remote_wanted(void) { return wanted(); }

void cp_remote_retry_now(void) { s_retryNow = true; }

void cp_remote_set_wake(void (*wake)(void)) { s_wake = wake; }

static bool verb_is(const char* cmd, const char* verb) {
    const size_t n = strlen(verb);
    return strncmp(cmd, verb, n) == 0 && (cmd[n] == '\0' || cmd[n] == ' ');
}

/* The companion allowlist is what BLE and SysEx get from a hand-held phone; the
 * internet gets less (spec 3.5: no WiFi changes, no firmware upload, no file
 * writes, no bootloader, no USB drive mode). Of that allowlist only the WIFI_
 * verbs reach a WiFi change (SET, FORGET, FORGET_ALL, ENABLE; UP/DOWN/SCAN/PING
 * the companion already refuses): all but STATUS and LIST are denied. */
static bool remote_denied(const char* cmd) {
    /* The browser twin is a USB host's: it turns the touch mirror on. */
    if (strncmp(cmd, "TWIN", 4) == 0) return true;
    /* So is the motion stream: its frames go out on USB MIDI. */
    if (strncmp(cmd, "MOTION ", 7) == 0) return true;
    if (strncmp(cmd, "WIFI_", 5) != 0) return false;
    return !verb_is(cmd, "WIFI_STATUS") && !verb_is(cmd, "WIFI_LIST");
}

/* Main loop (5 kB stack): the buffers are in s_bufs, the handler is the companion's.
 * Runs only while Online. The finished frame goes to the sender task (a notify,
 * non-blocking): publishing here would wait for the client's api lock, which the
 * MQTT task holds through connect and TLS. */
void cp_remote_poll(void) {
    EXT_RAM_BSS_ATTR static Slot cur;
    static bool pending;                      // a frame is in s_bufs->frame, not yet handed over
    static size_t pendingLen;
    static uint32_t pendingGen;
    if (s_frameBusy) return;                  // the one frame buffer is still on its way out
    Bufs* b;
    uint32_t gen;
    {
        Lock l;
        if (!s_client || !s_bufs || s_state != St::Online) return;
        b = s_bufs;
        gen = s_qgen;
        if (pending && pendingGen != gen) pending = false;       // the session it belonged to is gone
        if (!pending) {
            if (!s_count) return;
            cur = b->queue[s_head];
        }
        s_pollBusy = true;                    // teardown_task() waits for this before freeing b
    }
    if (!pending) {
        if (remote_denied(cur.command)) snprintf(b->reply, kReplyBuf, "ERR not_remote\r\n");
        else {
            b->reply[0] = 0;                  // the handler's reply callback appends
            hil_control_companion_handle(cur.command, b->reply, kReplyBuf);
        }
        pendingLen = crosspad::formatRemoteReply(cur.id, b->reply, b->frame, sizeof b->frame);
        pendingGen = gen;
        pending = true;
    }
    {
        Lock l;
        if (gen != s_qgen || !s_client) {     // dropped while the command ran
            pending = false;
        } else {
            if (!s_sender) {
                pending = false;
            } else {
                /* Length and busy first, then the notify: the sender (other core) may publish and
                 * clear the flag before xTaskNotifyGive returns. Neither call waits. */
                s_frameLen = pendingLen;
                s_frameBusy = true;
                s_head = (s_head + 1) % kQueue;
                --s_count;
                pending = false;
                xTaskNotifyGive(s_sender);
            }
        }
    }
    s_pollBusy = false;
}

uint8_t cp_remote_peers(void) { return s_peers; }

const char* cp_remote_state_name(void) { return kStNames[static_cast<int>(s_state.load())]; }

static __attribute__((noinline)) void status_line(void (*reply)(const char*)) {
    static char line[320];
    size_t queued;
    {
        Lock l;
        queued = s_count;
    }
    snprintf(line, sizeof line, "REMOTE: state=%s reason=%s peers=%u queued=%u done=%u busy=%u uri=%s\r\n",
             cp_remote_state_name(), s_reason.load(), (unsigned)s_peers.load(), (unsigned)queued,
             (unsigned)s_done.load(), (unsigned)s_busy.load(), s_uri[0] ? s_uri : "-");
    reply(line);
}

bool cp_remote_handle(const char* cmd, void (*reply)(const char*)) {
    if (strcmp(cmd, "REMOTE_STATUS") != 0) return false;
    status_line(reply);
    return true;
}
