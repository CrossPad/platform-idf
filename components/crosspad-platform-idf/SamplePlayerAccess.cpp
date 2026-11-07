// SPDX-License-Identifier: GPL-3.0-or-later

#include "SamplePlayerAccess.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

namespace crosspad {

namespace {

// Static, so it cannot fail for want of heap and is ready before any caller.
StaticSemaphore_t g_fileMutexBuf;
SemaphoreHandle_t g_fileMutex = nullptr;

// Serializes mutators against each other — the gate flag only excludes the
// renderer, so without this the second mutator's exit would let it back in
// while the first is still restructuring.
StaticSemaphore_t g_gateMutexBuf;
SemaphoreHandle_t g_gateMutex = nullptr;

// How long to wait for the RT task to leave Process(). Far more than one
// render pass needs; the bound only stops a wedged audio task from hanging a
// kit load forever.
constexpr int kGateWaitTicks = 40;

// Held by audio_bg for one BkgProc() and by the engine teardown; the count is
// the teardown announcing itself, so the streamer stands aside rather than
// racing for a mutex it would always win.
StaticSemaphore_t g_streamMutexBuf;
SemaphoreHandle_t g_streamMutex = nullptr;
std::atomic<int> g_streamDemand{0};

// Gate hold accounting. Only the outermost hold of a recursive take counts,
// so a MutationGuard nested in another reports one hold, not two. All under
// g_gateMutex except the published totals, which a CDC verb reads.
int g_gateDepth = 0;
int64_t g_gateEnterUs = 0;
std::atomic<uint32_t> g_gateMaxUs{0};
std::atomic<uint32_t> g_gateTotalUs{0};
std::atomic<uint32_t> g_gateCount{0};

}  // namespace

void samplePlayerAccessInit() {
    if (g_fileMutex == nullptr) {
        g_fileMutex = xSemaphoreCreateRecursiveMutexStatic(&g_fileMutexBuf);
    }
    if (g_gateMutex == nullptr) {
        g_gateMutex = xSemaphoreCreateRecursiveMutexStatic(&g_gateMutexBuf);
    }
    if (g_streamMutex == nullptr) {
        g_streamMutex = xSemaphoreCreateRecursiveMutexStatic(&g_streamMutexBuf);
    }
}

void samplePlayerFileLock() {
    // Before init there is one thread, and blocking would deadlock the setup
    // that creates the mutex.
    if (g_fileMutex != nullptr) {
        xSemaphoreTakeRecursive(g_fileMutex, portMAX_DELAY);
    }
}

bool samplePlayerFileTryLock(uint32_t ms) {
    if (g_fileMutex == nullptr) return true;
    return xSemaphoreTakeRecursive(g_fileMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void samplePlayerFileUnlock() {
    if (g_fileMutex != nullptr) {
        xSemaphoreGiveRecursive(g_fileMutex);
    }
}

bool samplePlayerStreamBegin() {
    if (g_streamDemand.load(std::memory_order_seq_cst) > 0) {
        return false;
    }
    if (g_streamMutex != nullptr) {
        xSemaphoreTakeRecursive(g_streamMutex, portMAX_DELAY);
    }
    // Demand arriving between the check and the take costs one refill's wait,
    // not the unbounded starvation this exists to prevent.
    return true;
}

void samplePlayerStreamEnd() {
    if (g_streamMutex != nullptr) {
        xSemaphoreGiveRecursive(g_streamMutex);
    }
}

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
bool samplePlayerStreamHeld() {
    return g_streamDemand.load(std::memory_order_relaxed) > 0;
}
#endif

void samplePlayerStreamExcludeLock() {
    // Published before blocking, or the streamer never lets the loader in.
    g_streamDemand.fetch_add(1, std::memory_order_seq_cst);
    if (g_streamMutex != nullptr) {
        xSemaphoreTakeRecursive(g_streamMutex, portMAX_DELAY);
    }
}

void samplePlayerStreamExcludeUnlock() {
    if (g_streamMutex != nullptr) {
        xSemaphoreGiveRecursive(g_streamMutex);
    }
    g_streamDemand.fetch_sub(1, std::memory_order_seq_cst);
}

void samplePlayerGateEnter() {
    if (g_gateMutex != nullptr) {
        xSemaphoreTakeRecursive(g_gateMutex, portMAX_DELAY);
    }
    if (g_gateDepth++ == 0) {
        g_gateEnterUs = esp_timer_get_time();
    }
    detail::samplePlayerGated().store(true, std::memory_order_seq_cst);

    // Wait out a render pass that started before the flag went up. Yield
    // rather than spin — the RT task is on the other core at a higher priority.
    for (int i = 0; i < kGateWaitTicks; ++i) {
        if (!detail::samplePlayerRendering().load(std::memory_order_seq_cst)) {
            return;
        }
        vTaskDelay(1);
    }
}

void samplePlayerGateExit() {
    detail::samplePlayerGated().store(false, std::memory_order_seq_cst);
    if (--g_gateDepth == 0) {
        const uint32_t held = (uint32_t)(esp_timer_get_time() - g_gateEnterUs);
        g_gateTotalUs.fetch_add(held, std::memory_order_relaxed);
        g_gateCount.fetch_add(1, std::memory_order_relaxed);
        uint32_t prev = g_gateMaxUs.load(std::memory_order_relaxed);
        while (held > prev &&
               !g_gateMaxUs.compare_exchange_weak(prev, held, std::memory_order_relaxed)) {
        }
    }
    if (g_gateMutex != nullptr) {
        xSemaphoreGiveRecursive(g_gateMutex);
    }
}

void samplePlayerGateStats(uint32_t* maxUs, uint32_t* totalUs, uint32_t* count) {
    if (maxUs)   *maxUs   = g_gateMaxUs.exchange(0, std::memory_order_relaxed);
    if (totalUs) *totalUs = g_gateTotalUs.load(std::memory_order_relaxed);
    if (count)   *count   = g_gateCount.load(std::memory_order_relaxed);
}

}  // namespace crosspad
