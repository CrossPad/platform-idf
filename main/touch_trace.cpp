#include "include/touch_trace.h"

#include <atomic>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"

#include "crosspad-gui/theme/crosspad_theme.h"

static const char *TAG = "touch_trace";

/* The gui has no "is a prompt open" call, so this looks for the prompt's
 * keyboard among the overlay layer's children and grandchildren. */
bool touch_trace_text_entry_open(void)
{
    lv_obj_t *layer = crosspad_gui::getOverlayParent();
    if (!layer) return false;
    for (uint32_t i = 0, n = lv_obj_get_child_count(layer); i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(layer, (int32_t)i);
        if (lv_obj_check_type(c, &lv_keyboard_class)) return true;
        for (uint32_t j = 0, m = lv_obj_get_child_count(c); j < m; ++j)
            if (lv_obj_check_type(lv_obj_get_child(c, (int32_t)j), &lv_keyboard_class)) return true;
    }
    return false;
}

namespace {

struct Sample {
    uint32_t t_ms;
    int16_t  x;
    int16_t  y;
    uint8_t  pressed;
};

/* The panel is read every ~9 ms, not once per refresh period, so 512 samples
 * turned out to be 13 s of history — a batch of six gestures overwrote itself
 * before it could be dumped. 4096 is 32 kB of PSRAM and holds minutes once
 * repeated points are dropped. */
constexpr unsigned kCapacity = 4096;

/* A finger resting still produces one identical sample every 9 ms and nothing
 * else; keeping them buys no information and costs the whole ring. One is kept
 * every kStillMs so a pause is still visible as a pause. */
constexpr uint32_t kStillMs = 100;

/* The host mirror sends a press and a release at once, a drag at most this often. */
constexpr int64_t kMirrorMoveUs = 25000;

Sample            *s_ring    = nullptr;
unsigned           s_head    = 0;   /* next write slot */
unsigned           s_count   = 0;
bool               s_on      = false;

lv_indev_read_cb_t s_inner   = nullptr;
lv_indev_t        *s_indev   = nullptr;
bool               s_wasDown = false;

/* Live touch for the host (touch_mirror_pump): the latest sample, packed so the
 * LVGL task can publish it and the MIDI task read it without a lock.
 * bit 31 down | bits 30..22 x | bits 21..14 y | bits 13..0 sequence */
std::atomic<uint32_t> s_live{0};
bool                  s_liveDown = false;

/* Decided at the press and kept for the whole contact, so a prompt that opens
 * under a finger still gets its lift mirrored. */
bool s_liveBlocked = false;

void publish(int x, int y, bool down)
{
    x = x < 0 ? 0 : (x > 511 ? 511 : x);
    y = y < 0 ? 0 : (y > 255 ? 255 : y);
    const uint32_t prev = s_live.load(std::memory_order_relaxed);
    const uint32_t body = (down ? 0x80000000u : 0u) | ((uint32_t)x << 22) | ((uint32_t)y << 14);
    if ((prev & ~0x3FFFu) == body) return;              /* a finger resting still: nothing new */
    s_live.store(body | ((prev + 1) & 0x3FFFu), std::memory_order_release);
}

int16_t  s_lastX = 0, s_lastY = 0;
uint32_t s_lastT = 0;
bool     s_haveLast = false;

/* The synthetic contact. Written by the CDC task while `s_inj` is Idle,
 * published by storing Armed; the read callback (LVGL task) starts it on its
 * next sample, plays it, sends one release and goes back to Idle. */
enum class Inj : uint8_t { Idle, Armed, Running, Releasing };
std::atomic<Inj> s_inj{Inj::Idle};
int32_t  s_injX1 = 0, s_injY1 = 0, s_injX2 = 0, s_injY2 = 0;
uint32_t s_injMove = 0, s_injRest = 0, s_injHold = 0, s_injT0 = 0;

uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

/* The panel's indev runs in LVGL's event mode, read when the FT6x36's
 * interrupt says a finger moved: with no finger nothing reads it, and a
 * synthetic contact would sit Armed forever. While one plays, this reads the
 * indev the way the interrupt does, every 10 ms. */
lv_timer_t *s_pump = nullptr;
constexpr uint32_t kPumpMs = 10;

void pump_cb(lv_timer_t *)
{
    if (s_indev) lv_indev_read(s_indev);
    if (s_inj.load(std::memory_order_acquire) == Inj::Idle && s_pump) {
        lv_timer_delete(s_pump);
        s_pump = nullptr;
    }
}

/* Overwrite @p data with the synthetic contact while one is playing. */
void inject(lv_indev_data_t *data)
{
    Inj st = s_inj.load(std::memory_order_acquire);
    if (st == Inj::Idle) return;
    if (st == Inj::Armed) {
        s_injT0 = now_ms();
        s_inj.store(Inj::Running, std::memory_order_relaxed);
        st = Inj::Running;
    }
    if (st == Inj::Releasing) {
        data->point.x = s_injX2;
        data->point.y = s_injY2;
        data->state = LV_INDEV_STATE_RELEASED;
        s_inj.store(Inj::Idle, std::memory_order_release);
        return;
    }
    const uint32_t held = now_ms() - s_injT0;
    if (held < s_injHold) {
        data->point.x = s_injX1;
        data->point.y = s_injY1;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    const uint32_t t = held - s_injHold;
    int32_t x = s_injX2, y = s_injY2;
    if (t < s_injMove) {
        x = s_injX1 + (int32_t)((int64_t)(s_injX2 - s_injX1) * t / s_injMove);
        y = s_injY1 + (int32_t)((int64_t)(s_injY2 - s_injY1) * t / s_injMove);
    }
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    if (t >= s_injMove + s_injRest) s_inj.store(Inj::Releasing, std::memory_order_relaxed);
}

void record(int16_t x, int16_t y, bool pressed)
{
    if (!s_ring) return;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (pressed && s_haveLast && x == s_lastX && y == s_lastY && (now - s_lastT) < kStillMs) return;
    s_lastX = x; s_lastY = y; s_lastT = now; s_haveLast = pressed;

    s_ring[s_head] = Sample{now, x, y, (uint8_t)pressed};
    s_head = (s_head + 1) % kCapacity;
    if (s_count < kCapacity) s_count++;
}

void trace_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_inner) s_inner(indev, data);
    inject(data);

    {
        const bool down = data->state == LV_INDEV_STATE_PRESSED;
        if (down && !s_liveDown) s_liveBlocked = touch_trace_text_entry_open();
        if ((down || s_liveDown) && !s_liveBlocked) publish(data->point.x, data->point.y, down);
        s_liveDown = down;
    }

    if (!s_on) { s_wasDown = data->state == LV_INDEV_STATE_PRESSED; return; }

    const bool down = data->state == LV_INDEV_STATE_PRESSED;
    /* Every sample while down, plus the one release edge — a lift reports the
     * last coordinate, which is what a swipe's end point actually is. */
    if (down || s_wasDown) record((int16_t)data->point.x, (int16_t)data->point.y, down);
    s_wasDown = down;
}

} // namespace

void touch_mirror_pump(void (*send)(const uint8_t *frame, size_t len))
{
    static uint32_t s_sentSeq = 0;
    static bool     s_sentDown = false;
    static int64_t  s_sentAt = 0;
    const uint32_t v = s_live.load(std::memory_order_acquire);
    const uint32_t seq = v & 0x3FFFu;
    if (seq == s_sentSeq || !send) return;
    const bool down = v >> 31;
    const int64_t now = esp_timer_get_time();
    if (down && s_sentDown && now - s_sentAt < kMirrorMoveUs) return;   /* drags: rate-limited */
    const unsigned x = (v >> 22) & 0x1FF, y = (v >> 14) & 0xFF;
    const uint8_t st = down ? (s_sentDown ? 2 : 1) : 0;
    const uint8_t frame[] = { 0xF0, 0x7D, 0x1F, 0x05, st,
                              (uint8_t)(x >> 7), (uint8_t)(x & 0x7F), (uint8_t)(y >> 7), (uint8_t)(y & 0x7F), 0xF7 };
    send(frame, sizeof frame);
    s_sentSeq = seq; s_sentDown = down; s_sentAt = now;
}

void touch_trace_init(lv_indev_t *indev)
{
    if (!indev || s_inner) return;
    s_indev = indev;
    s_inner = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, trace_read_cb);
    ESP_LOGI(TAG, "tapped the touch read_cb");
}

bool touch_trace_set(bool on)
{
    if (on && !s_ring) {
        s_ring = (Sample *)heap_caps_calloc(kCapacity, sizeof(Sample), MALLOC_CAP_SPIRAM);
        if (!s_ring) { ESP_LOGE(TAG, "no PSRAM for %u samples", kCapacity); return false; }
    }
    if (on) touch_trace_clear();
    s_on = on;
    return true;
}

bool     touch_trace_is_on(void) { return s_on; }

bool touch_trace_inject(int x1, int y1, int x2, int y2, unsigned move_ms, unsigned rest_ms, unsigned hold_ms)
{
    if (!s_inner) return false;   // no panel: nothing reads the contact
    if (s_inj.load(std::memory_order_acquire) != Inj::Idle) return false;
    s_injX1 = x1; s_injY1 = y1; s_injX2 = x2; s_injY2 = y2;
    s_injMove = move_ms ? move_ms : 1;
    s_injRest = rest_ms;
    s_injHold = hold_ms;
    if (!lvgl_port_lock(1000)) return false;   // nothing would read it
    if (!s_pump) s_pump = lv_timer_create(pump_cb, kPumpMs, nullptr);
    s_inj.store(Inj::Armed, std::memory_order_release);
    lvgl_port_unlock();
    return true;
}

bool touch_trace_injecting(void) { return s_inj.load(std::memory_order_acquire) != Inj::Idle; }
unsigned touch_trace_count(void) { return s_count; }

void touch_trace_clear(void)
{
    s_head     = 0;
    s_count    = 0;
    s_haveLast = false;
}

bool touch_trace_get(unsigned idx, unsigned *t_ms, int *x, int *y, unsigned *pressed)
{
    if (!s_ring || idx >= s_count) return false;
    const unsigned start = (s_head + kCapacity - s_count) % kCapacity;
    const Sample  &s     = s_ring[(start + idx) % kCapacity];
    *t_ms    = s.t_ms;
    *x       = s.x;
    *y       = s.y;
    *pressed = s.pressed;
    return true;
}
