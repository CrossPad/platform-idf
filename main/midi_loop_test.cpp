#include "midi_loop_test.h"

#include <atomic>
#include <cstdio>

#include "CrosspadPlatform.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

// Channel 1: the STM's MIDI stream is parsed listening on channel 1 only.
constexpr uint8_t  kStatus    = 0x90;      // note-on, channel 1
constexpr uint8_t  kChannel   = 1;
constexpr uint32_t kNotes     = 32;
constexpr uint32_t kVelocities = 127;
constexpr uint32_t kPeriod    = kNotes * kVelocities;

std::atomic<bool>     s_capture{false};
std::atomic<uint32_t> s_rx{0};
std::atomic<uint32_t> s_in_order{0};
std::atomic<uint32_t> s_gaps{0};          // forward jumps
std::atomic<uint32_t> s_skipped{0};       // messages the jumps passed over
std::atomic<uint32_t> s_back{0};          // a step back: corrupted or repeated
std::atomic<int32_t>  s_last{-1};

std::atomic<bool>     s_sending{false};
std::atomic<bool>     s_stop{false};
std::atomic<uint32_t> s_sent{0};
std::atomic<uint32_t> s_send_fail{0};
std::atomic<uint32_t> s_send_rate{0};
std::atomic<uint32_t> s_send_count{0};
std::atomic<int64_t>  s_send_us{0};       // how long the last run took

struct SendArgs {
    uint32_t count;
    uint32_t rate;
};

// Paced by the tick: every tick sends what has fallen due since the start,
// so the rate holds without a busy wait (a few messages a tick at most).
void send_task(void *arg)
{
    const SendArgs a = *static_cast<SendArgs *>(arg);
    delete static_cast<SendArgs *>(arg);
    auto &stm = crosspad::getStm32Transport();
    const int64_t t0 = esp_timer_get_time();
    uint32_t seq = 0;
    while (seq < a.count && !s_stop.load()) {
        const uint64_t due = (uint64_t)(esp_timer_get_time() - t0) * a.rate / 1000000ULL + 1;
        for (; seq < a.count && seq < due; ++seq) {
            const uint32_t p = seq % kPeriod;
            const uint8_t msg[3] = {kStatus, (uint8_t)(p % kNotes), (uint8_t)(1 + p / kNotes)};
            if (stm.send(msg, sizeof msg)) s_sent.fetch_add(1);
            else s_send_fail.fetch_add(1);
        }
        vTaskDelay(1);
    }
    s_send_us.store(esp_timer_get_time() - t0);
    s_sending.store(false);
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

extern "C" bool midi_loop_take(uint8_t channel, uint8_t note, uint8_t velocity)
{
    if (!s_capture.load(std::memory_order_relaxed) || channel != kChannel ||
        note >= kNotes || velocity == 0) {
        return false;
    }
    const int32_t seq = (int32_t)((velocity - 1) * kNotes + note);
    const int32_t last = s_last.exchange(seq);
    s_rx.fetch_add(1);
    if (last >= 0) {
        const uint32_t step = (uint32_t)(seq - last + (int32_t)kPeriod) % kPeriod;
        if (step == 1) {
            s_in_order.fetch_add(1);
        } else if (step != 0 && step < kPeriod / 2) {
            s_gaps.fetch_add(1);
            s_skipped.fetch_add(step - 1);
        } else {
            s_back.fetch_add(1);
        }
    }
    return true;
}

extern "C" void midi_loop_capture(bool on)
{
    s_rx = 0;
    s_in_order = 0;
    s_gaps = 0;
    s_skipped = 0;
    s_back = 0;
    s_last = -1;
    s_capture = on;
}

extern "C" bool midi_loop_send(uint32_t count, uint32_t rate)
{
    if (count == 0 || rate == 0) return false;
    bool idle = false;
    if (!s_sending.compare_exchange_strong(idle, true)) return false;
    s_stop = false;
    s_sent = 0;
    s_send_fail = 0;
    s_send_us = 0;
    s_send_rate = rate;
    s_send_count = count;
    auto *a = new SendArgs{count, rate};
    /* PSRAM stack: internal RAM is a few kB while WiFi is up, and this task
     * touches no flash. */
    if (xTaskCreatePinnedToCoreWithCaps(send_task, "midi_loop", 3072, a, 4, nullptr, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        delete a;
        s_sending = false;
        return false;
    }
    return true;
}

extern "C" void midi_loop_stop(void)
{
    s_stop = true;
}

extern "C" void midi_loop_format(char *out, size_t n)
{
    snprintf(out, n,
             "MIDI_LOOP: capture=%d rx=%lu in_order=%lu gaps=%lu skipped=%lu back=%lu last=%ld "
             "sending=%d sent=%lu fail=%lu count=%lu rate=%lu send_us=%lld\r\n",
             s_capture.load() ? 1 : 0, (unsigned long)s_rx.load(), (unsigned long)s_in_order.load(),
             (unsigned long)s_gaps.load(), (unsigned long)s_skipped.load(), (unsigned long)s_back.load(),
             (long)s_last.load(), s_sending.load() ? 1 : 0, (unsigned long)s_sent.load(),
             (unsigned long)s_send_fail.load(), (unsigned long)s_send_count.load(),
             (unsigned long)s_send_rate.load(), (long long)s_send_us.load());
}
