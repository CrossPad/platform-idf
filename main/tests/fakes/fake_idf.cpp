// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-ins for FreeRTOS queues and tasks, esp_lvgl_port's display lock
// and lv_async_call(). Every object lives until the process ends: the
// forwarder task is a detached thread blocked in a receive at exit.
#include "esp_lvgl_port.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "lvgl.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct FakeQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<unsigned char>> items;
    UBaseType_t length;
    UBaseType_t itemSize;
};

namespace {
std::mutex& criticalMutex() { static auto* m = new std::mutex; return *m; }
std::recursive_timed_mutex& displayLock() { static auto* m = new std::recursive_timed_mutex; return *m; }

template <class Pred>
bool waitTicks(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, TickType_t ticks, Pred pred) {
    if (ticks == portMAX_DELAY) { cv.wait(lock, pred); return true; }
    return cv.wait_for(lock, std::chrono::milliseconds(ticks), pred);
}
} // namespace

void fake_critical_enter(portMUX_TYPE *) { criticalMutex().lock(); }
void fake_critical_exit(portMUX_TYPE *) { criticalMutex().unlock(); }

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize) {
    auto* q = new FakeQueue;
    q->length = length;
    q->itemSize = itemSize;
    return q;
}

void vQueueDelete(QueueHandle_t) {}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks) {
    std::unique_lock<std::mutex> lock(q->m);
    if (!waitTicks(q->cv, lock, ticks, [q] { return q->items.size() < q->length; })) return pdFALSE;
    const auto* p = static_cast<const unsigned char*>(item);
    q->items.emplace_back(p, p + q->itemSize);
    q->cv.notify_all();
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks) {
    std::unique_lock<std::mutex> lock(q->m);
    if (!waitTicks(q->cv, lock, ticks, [q] { return !q->items.empty(); })) return pdFALSE;
    std::memcpy(item, q->items.front().data(), q->itemSize);
    q->items.pop_front();
    q->cv.notify_all();
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    std::lock_guard<std::mutex> lock(q->m);
    return static_cast<UBaseType_t>(q->items.size());
}

TickType_t xTaskGetTickCount(void) {
    using namespace std::chrono;
    return static_cast<TickType_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *, uint32_t, void *arg,
                                           UBaseType_t, TaskHandle_t *, BaseType_t, uint32_t) {
    std::thread(fn, arg).detach();
    return pdPASS;
}

bool lvgl_port_lock(uint32_t timeout_ms) {
    if (timeout_ms == 0) { displayLock().lock(); return true; }
    return displayLock().try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void lvgl_port_unlock(void) { displayLock().unlock(); }

extern "C" int lv_async_call(lv_async_cb_t cb, void *user_data) {
    cb(user_data);
    return 0;
}
