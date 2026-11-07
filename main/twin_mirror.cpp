#include "include/twin_mirror.h"
#include "include/touch_trace.h"

#include <atomic>
#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <time.h>

#include "crosspad/platform/IClock.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

extern crosspad::CrosspadStatus status;

static const char *TAG = "twin_mirror";

namespace {

enum Kind : uint8_t { TOUCH = 0, ENCODER = 1, PAD = 2, POWER = 3, HOST = 4, CLOCK = 5, LOST = 6, STATUS = 8 };

void put7(uint8_t *p, uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) { p[i] = v & 0x7F; v >>= 7; } }

/* The status frame's payload (twin_mirror.h, kind 8); 16 bytes. */
void status_payload(uint8_t *p)
{
    const int b = status.batteryPct;
    p[0] = (uint8_t)(b < 0 ? 127 : (b > 126 ? 126 : b));
    p[1] = (uint8_t)((status.stm32.chargingStatus ? 1 : 0) | (status.stm32.sdDetected ? 2 : 0) |
                     (status.hostAlive(crosspad::getClock().getTimeUs()) ? 4 : 0) | (status.fwUpdateAvailable ? 8 : 0) |
                     (status.bluetoothActive ? 16 : 0) | (status.bluetoothConnected ? 32 : 0) |
                     (status.wifiConnected ? 64 : 0));
    p[2] = status.hostCpuPct > 127 ? 127 : status.hostCpuPct;
    put7(p + 3, status.hostRamUsedMb, 3);
    put7(p + 6, status.hostRamTotalMb, 3);
    p[9] = status.hostDawsInstalled & 0x7F;
    p[10] = status.hostDawsRunning & 0x7F;
    put7(p + 11, (uint64_t)time(nullptr), 5);
}

struct Event {
    uint32_t tick;
    uint8_t  kind;
    uint8_t  a;      // state / pad / hold
    int16_t  b;      // x / enc diff / velocity
    int16_t  c;      // y
};

/* Inputs arrive a few per second, a drag at the panel's ~100 Hz; the MIDI
 * task drains every millisecond. 256 is seconds of slack. */
constexpr unsigned kCap = 256;
/* The ring, the host bytes and the pump's frames live in PSRAM: tasks only
 * (no ISR, no DMA), and the 8 kB stay out of the internal RAM the image is short of. */
EXT_RAM_BSS_ATTR Event    s_ring[kCap];
unsigned s_head = 0, s_tail = 0;
bool     s_lost = false;

/* The host's messages ride beside the ring: a HOST event's length is in its b,
 * its bytes here, in the same order (a DAW's LCD row is ~60 bytes, SysEx at
 * most 256). */
constexpr unsigned kBytesCap = 4096;
EXT_RAM_BSS_ATTR uint8_t  s_bytes[kBytesCap];
unsigned s_bHead = 0, s_bTail = 0;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Keepalive: the host repeats TWIN 1 every couple of seconds. */
constexpr int64_t kAliveUs = 6 * 1000 * 1000;
std::atomic<int64_t> s_aliveUntil{0};

/* A clock frame this often while active: the twin's time moves in these steps. */
constexpr int64_t kClockUs = 20 * 1000;

constexpr unsigned kMaxIndev = 4;
/* hidden: the contact began on a text prompt (decided at the press, kept to
 * the end of its scroll, as touch_trace does for its own mirror). */
struct Tap { lv_indev_t *indev; lv_indev_read_cb_t inner; lv_indev_state_t state; bool hidden; };
Tap s_taps[kMaxIndev];
unsigned s_nTaps = 0;

/* Stamped and queued under one lock, and the pump's clock is read under it
 * after draining: every event still to come carries a tick >= the last clock. */
void push(uint8_t kind, uint8_t a, int16_t b, int16_t c, const uint8_t *bytes = nullptr)
{
    if (!twin_mirror_active()) return;
    portENTER_CRITICAL(&s_lock);
    const unsigned next = (s_head + 1) % kCap;
    const unsigned used = (s_bHead + kBytesCap - s_bTail) % kBytesCap;
    if (next == s_tail || (bytes && used + (unsigned)b >= kBytesCap)) {
        s_lost = true;
    } else {
        s_ring[s_head] = Event{lv_tick_get(), kind, a, b, c};
        s_head = next;
        for (int i = 0; bytes && i < b; ++i) { s_bytes[s_bHead] = bytes[i]; s_bHead = (s_bHead + 1) % kBytesCap; }
    }
    portEXIT_CRITICAL(&s_lock);
}

/* An input on a text prompt goes out as a LOST, never as itself: the twin
 * resyncs from TWIN_STATE, which holds structure and no text. */
void push_hidden()
{
    if (!twin_mirror_active()) return;
    portENTER_CRITICAL(&s_lock);
    s_lost = true;
    portEXIT_CRITICAL(&s_lock);
}

void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    Tap *t = nullptr;
    for (unsigned i = 0; i < s_nTaps; ++i) if (s_taps[i].indev == indev) t = &s_taps[i];
    if (!t) return;
    if (t->inner) t->inner(indev, data);
    if (!twin_mirror_active()) { t->state = data->state; return; }

    /* One frame per read that can matter, so the twin reads at the same ticks:
     * every read while held (long press, drag, the encoder's hold), and after
     * a touch release every read while a scroll is still being thrown (the
     * momentum decays per read, not per millisecond). An idle, released read
     * changes nothing in LVGL and is not sent. */
    const bool held = data->state == LV_INDEV_STATE_PRESSED;
    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        if (data->enc_diff != 0 || data->state != t->state || held) {
            int d = data->enc_diff;
            d = d < -63 ? -63 : (d > 63 ? 63 : d);
            if (touch_trace_text_entry_open()) push_hidden();
            else push(ENCODER, data->state == LV_INDEV_STATE_PRESSED, (int16_t)(d + 64), 0);
        }
    } else {
        if (held && t->state != LV_INDEV_STATE_PRESSED) t->hidden = touch_trace_text_entry_open();
        if (data->state != t->state || held || lv_indev_get_scroll_obj(indev) != nullptr) {
            if (t->hidden) push_hidden();
            else push(TOUCH, data->state == LV_INDEV_STATE_PRESSED, (int16_t)data->point.x, (int16_t)data->point.y);
        }
    }
    t->state = data->state;
}

void put_tick(uint8_t *f, uint32_t t)
{
    f[0] = (t >> 21) & 0x7F; f[1] = (t >> 14) & 0x7F; f[2] = (t >> 7) & 0x7F; f[3] = t & 0x7F;
}

} // namespace

void twin_mirror_attach(lv_indev_t *indev)
{
    if (!indev || s_nTaps >= kMaxIndev) return;
    s_taps[s_nTaps++] = Tap{indev, lv_indev_get_read_cb(indev), LV_INDEV_STATE_RELEASED};
    lv_indev_set_read_cb(indev, read_cb);
}

void twin_mirror_pad(uint8_t pad, uint8_t velocity) { push(PAD, pad, velocity, 0); }
void twin_mirror_power(bool hold)                   { push(POWER, hold ? 1 : 0, 0, 0); }

void twin_mirror_host(const uint8_t *msg, size_t len)
{
    if (msg && len && len <= 300) push(HOST, 0, (int16_t)len, 0, msg);
}

void twin_mirror_enable(bool on)
{
    const bool was = twin_mirror_active();
    s_aliveUntil.store(on ? esp_timer_get_time() + kAliveUs : 0);
    if (on && !was) ESP_LOGI(TAG, "twin attached");
}

bool twin_mirror_active(void) { return esp_timer_get_time() < s_aliveUntil.load(std::memory_order_relaxed); }

void twin_mirror_pump(void (*send)(const uint8_t *frame, size_t len))
{
    static int64_t s_clockAt = 0;
    if (!send || !twin_mirror_active()) return;
    EXT_RAM_BSS_ATTR static uint8_t f[16 + 300 + 300 / 7 + 2];
    EXT_RAM_BSS_ATTR static uint8_t host[300];
    for (;;) {
        Event e;
        bool have = false, lost = false;
        uint32_t clock = 0;
        portENTER_CRITICAL(&s_lock);
        if (s_tail != s_head) {
            e = s_ring[s_tail]; s_tail = (s_tail + 1) % kCap; have = true;
            if (e.kind == HOST)
                for (int i = 0; i < e.b; ++i) { host[i] = s_bytes[s_bTail]; s_bTail = (s_bTail + 1) % kBytesCap; }
        }
        else { lost = s_lost; s_lost = false; clock = lv_tick_get(); }
        portEXIT_CRITICAL(&s_lock);

        if (have) {
            size_t n = 0;
            f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 0x1F; f[n++] = 0x09; f[n++] = e.kind;
            put_tick(f + n, e.tick); n += 4;
            switch (e.kind) {
            case TOUCH: {
                const unsigned x = e.b < 0 ? 0 : (unsigned)e.b, y = e.c < 0 ? 0 : (unsigned)e.c;
                f[n++] = e.a; f[n++] = (x >> 7) & 0x7F; f[n++] = x & 0x7F; f[n++] = (y >> 7) & 0x7F; f[n++] = y & 0x7F;
                break;
            }
            case ENCODER: f[n++] = e.a; f[n++] = (uint8_t)e.b & 0x7F; break;
            case PAD:     f[n++] = e.a & 0x7F; f[n++] = (uint8_t)e.b & 0x7F; break;
            case POWER:   f[n++] = e.a; break;
            case HOST:
                for (int i = 0; i < e.b; i += 7) {
                    uint8_t hi = 0;
                    const int m = e.b - i < 7 ? e.b - i : 7;
                    for (int k = 0; k < m; ++k) hi |= (uint8_t)((host[i + k] >> 7) << k);
                    f[n++] = hi;
                    for (int k = 0; k < m; ++k) f[n++] = host[i + k] & 0x7F;
                }
                break;
            }
            f[n++] = 0xF7;
            send(f, n);
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (lost) {
            f[0] = 0xF0; f[1] = 0x7D; f[2] = 0x1F; f[3] = 0x09; f[4] = LOST; put_tick(f + 5, clock); f[9] = 0xF7;
            send(f, 10);
        }
        /* The status goes out when anything in it but the seconds moved (the
         * clock shows minutes), and once more whenever a twin attaches. */
        static uint8_t s_sent[16];
        static int64_t s_sentAliveUntil = 0;
        if (now - s_clockAt >= kClockUs) {
            uint8_t p[16];
            status_payload(p);
            const int64_t alive = s_aliveUntil.load(std::memory_order_relaxed);
            const bool fresh = alive - s_sentAliveUntil > kAliveUs;   /* a new attach, not a keepalive */
            s_sentAliveUntil = alive;
            static time_t s_sentMinute = 0;
            const time_t minute = time(nullptr) / 60;
            if (fresh || memcmp(p, s_sent, 11) != 0 || minute != s_sentMinute) {
                s_sentMinute = minute;
                memcpy(s_sent, p, sizeof s_sent);
                f[0] = 0xF0; f[1] = 0x7D; f[2] = 0x1F; f[3] = 0x09; f[4] = STATUS; put_tick(f + 5, clock);
                memcpy(f + 9, p, 16); f[25] = 0xF7;
                send(f, 26);
            }
        }
        if (lost || now - s_clockAt >= kClockUs) {
            f[0] = 0xF0; f[1] = 0x7D; f[2] = 0x1F; f[3] = 0x09; f[4] = CLOCK; put_tick(f + 5, clock); f[9] = 0xF7;
            send(f, 10);
            s_clockAt = now;
        }
        return;
    }
}
