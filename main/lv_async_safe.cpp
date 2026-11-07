// SPDX-License-Identifier: GPL-3.0-or-later
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "lv_async_safe.h"

#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdlib>

static const char *TAG = "lv_async";

static constexpr UBaseType_t kQueueLength = 24;
static constexpr TickType_t  kWarnInterval = pdMS_TO_TICKS(1000);
/* Only when the forwarder could not be started: how long a caller may wait
 * for the display lock itself before its call is dropped. */
static constexpr uint32_t    kInlineLockMs = 100;

typedef struct {
    lv_async_cb_t cb;
    void         *user_data;
} lv_async_item_t;

typedef struct lv_async_node {
    lv_async_item_t       item;
    struct lv_async_node *next;
} lv_async_node_t;

static std::atomic<QueueHandle_t> s_queue{nullptr};
static std::atomic<bool> s_failed{false};

/* Calls that found the queue full, or the forwarder not started yet, wait
 * here in order. While the list is not empty every new call joins it, and
 * the forwarder runs it only once the queue is empty: one FIFO. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static lv_async_node_t *s_head = nullptr;
static lv_async_node_t *s_tail = nullptr;

static std::atomic<uint32_t> s_overflowed{0};
static std::atomic<uint32_t> s_dropped{0};
static std::atomic<TickType_t> s_lastWarn{0};

static void warn_rate_limited(void)
{
    const TickType_t now = xTaskGetTickCount();
    TickType_t last = s_lastWarn.load();
    if (now - last < kWarnInterval) return;
    if (!s_lastWarn.compare_exchange_strong(last, now)) return;
    ESP_LOGW(TAG, "LVGL busy: %u calls past the queue, %u dropped (totals)",
             (unsigned)s_overflowed.load(), (unsigned)s_dropped.load());
}

static void run_locked(const lv_async_item_t &item)
{
    if (lvgl_port_lock(0)) {   /* 0 = block until acquired */
        lv_async_call(item.cb, item.user_data);
        lvgl_port_unlock();
    }
}

static void run_overflow(void)
{
    taskENTER_CRITICAL(&s_mux);
    lv_async_node_t *n = s_head;
    s_head = s_tail = nullptr;
    taskEXIT_CRITICAL(&s_mux);

    while (n) {
        lv_async_node_t *next = n->next;
        run_locked(n->item);
        free(n);
        n = next;
    }
}

static void lv_async_fwd_task(void *arg)
{
    const QueueHandle_t queue = static_cast<QueueHandle_t>(arg);
    lv_async_item_t item;
    for (;;) {
        if (uxQueueMessagesWaiting(queue) == 0) run_overflow();
        if (xQueueReceive(queue, &item, portMAX_DELAY) == pdTRUE) run_locked(item);
    }
}

extern "C" void lv_async_safe_start(void)
{
    if (s_queue.load() || s_failed.load()) return;
    QueueHandle_t q = xQueueCreate(kQueueLength, sizeof(lv_async_item_t));
    if (q && xTaskCreatePinnedToCoreWithCaps(lv_async_fwd_task, "lv_async_fwd", 2560, q, 3,
                                             nullptr, 0, MALLOC_CAP_SPIRAM) == pdPASS) {
        s_queue.store(q);
        return;
    }
    ESP_LOGE(TAG, "forwarder failed to start; callers take the lock inline, bounded");
    if (q) vQueueDelete(q);
    s_failed.store(true);
    run_overflow();
}

extern "C" void lv_async_call_locked(lv_async_cb_t cb, void *user_data)
{
    const lv_async_item_t item = { cb, user_data };

    if (s_failed.load()) {
        if (lvgl_port_lock(kInlineLockMs)) {
            lv_async_call(cb, user_data);
            lvgl_port_unlock();
            return;
        }
        s_dropped.fetch_add(1);
        warn_rate_limited();
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    const bool listEmpty = s_head == nullptr;
    taskEXIT_CRITICAL(&s_mux);
    const QueueHandle_t queue = s_queue.load();
    if (listEmpty && queue && xQueueSend(queue, &item, 0) == pdTRUE) return;

    auto *node = static_cast<lv_async_node_t *>(malloc(sizeof(lv_async_node_t)));
    if (!node) {
        s_dropped.fetch_add(1);
        warn_rate_limited();
        return;
    }
    node->item = item;
    node->next = nullptr;
    taskENTER_CRITICAL(&s_mux);
    if (s_tail) s_tail->next = node; else s_head = node;
    s_tail = node;
    taskEXIT_CRITICAL(&s_mux);

    if (queue) {
        s_overflowed.fetch_add(1);
        warn_rate_limited();
    }
}
