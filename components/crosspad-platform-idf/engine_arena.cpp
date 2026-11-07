// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine_arena.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <inttypes.h>

#include <atomic>

#include "SamplerEnginePort.hpp"  /* native_sampler_active(), _arena_keep(): lending */
#include "PitchedKitPort.hpp"     /* pitchedKitActive(): a ring beside a bank */
#include "crosspad/sampler/KitTake.hpp"   /* arenaMayLend(): the loan rule */

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "MarcelSampleNode.hpp"   /* marcelEngineReady(): engine_arena_lendable() */
#endif

namespace {

constexpr const char *TAG = "EngineArena";

/// PSRAM the engines never take: what the UI, the apps, WiFi and the
/// Recorder's ring are left at the least while the engines hold the most.
///
/// Until 2026-10-18 the arena was one block taken at boot -- 5424, 4848, 4608,
/// at last 4352 kB -- sized for the prebuilt streamer, which held 4252 kB
/// whatever the kit. The native engine holds what a kit needs (64 kB heads,
/// the pool's pages as voices play), so a fixed block left megabytes idle
/// while everything outside it ran short: every kB the image grew was a kB
/// the UI lost, and the Recorder was refused its take ring with 3 MB of the
/// arena free. Now the engines take PSRAM from the general heap as they need
/// it and give it back, and only this much is held back from them. With the
/// fixed block the launcher had 290-450 kB outside it; 512 kB is never less.
constexpr size_t kUiFloorBytes = 512u * 1024;

/// Alignment every arena block is handed out at: esp-dsp's aes3 FFT kernels
/// move complex pairs with 64-bit loads, and both the pitched bank's PCM and
/// the resampler's scratch are read by them directly.
constexpr size_t kBlockAlign = 16;

/// Every arena block carries one of these in front of it, linking it into
/// the list engine_arena_reset() frees. One alignment unit, so the block
/// after it keeps kBlockAlign.
struct Node {
    Node *prev;
    Node *next;
    uint32_t bytes;   ///< what the caller asked for
    uint32_t pad;
};
static_assert(sizeof(Node) == kBlockAlign, "the header must keep the payload aligned");

Node *s_head = nullptr;
size_t s_used = 0;     ///< payload bytes on the list
uint32_t s_blocks = 0;
uint32_t s_refused = 0;

/// Static so it exists before the first allocation, which happens at boot from
/// app_main with the loader tasks not yet created.
StaticSemaphore_t s_mutexBuf;
SemaphoreHandle_t s_mutex = nullptr;

/// Bumped by every engine_arena_reset(), under the guard.
std::atomic<uint32_t> s_generation{0};

/// The owner lock (engine_arena_owner_lock). Static storage, created eagerly
/// by engine_arena_init() at boot from app_main, before the kit loader, the
/// native take or any app task exists. ownerMutex() creates it there too for
/// a caller that comes first (still boot, still one task) -- never under a
/// critical section, where FreeRTOS calls do not belong.
StaticSemaphore_t s_ownerBuf;
SemaphoreHandle_t s_owner = nullptr;

SemaphoreHandle_t ownerMutex() {
    if (s_owner == nullptr) s_owner = xSemaphoreCreateRecursiveMutexStatic(&s_ownerBuf);
    return s_owner;
}

void lock() {
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutexBuf);
    }
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

void unlock() {
    xSemaphoreGiveRecursive(s_mutex);
}

class Guard {
public:
    Guard() { lock(); }
    ~Guard() { unlock(); }
};

size_t alignedSize(size_t bytes) {
    return (bytes + kBlockAlign - 1) & ~(kBlockAlign - 1);
}

/// What the engines may still take: the general heap's free bytes above the
/// floor. Under the guard.
size_t headroom() {
    const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return freeNow > kUiFloorBytes ? freeNow - kUiFloorBytes : 0;
}

/// The largest block a caller could be given now: the heap's largest, less
/// the header, and never past the floor. Under the guard.
size_t largestFree() {
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t fit = block > sizeof(Node) ? block - sizeof(Node) : 0;
    const size_t room = headroom();
    const size_t most = room > sizeof(Node) ? room - sizeof(Node) : 0;
    return fit < most ? fit : most;
}

/// A block from the general PSRAM heap, on the list; null where it would
/// take the heap below the floor. Under the guard.
void *takeBlock(size_t bytes) {
    const size_t need = sizeof(Node) + alignedSize(bytes);
    if (headroom() < need) return nullptr;
    Node *n = static_cast<Node *>(heap_caps_aligned_alloc(kBlockAlign, need, MALLOC_CAP_SPIRAM));
    if (n == nullptr) return nullptr;
    n->bytes = static_cast<uint32_t>(bytes);
    n->pad = 0;
    n->prev = nullptr;
    n->next = s_head;
    if (s_head) s_head->prev = n;
    s_head = n;
    s_used += bytes;
    ++s_blocks;
    return n + 1;
}

/// The list's node for @p ptr, or null when it is not an arena block (the
/// general-heap segments a ring falls back to are freed through here too).
/// A walk: the list is a kit's heads, the pool's pages and a few rings --
/// tens of blocks, and nothing on the render frees.
Node *findNode(const void *ptr) {
    for (Node *n = s_head; n != nullptr; n = n->next) {
        if (n + 1 == ptr) return n;
    }
    return nullptr;
}

void unlink(Node *n) {
    if (n->prev) n->prev->next = n->next;
    else s_head = n->next;
    if (n->next) n->next->prev = n->prev;
    s_used -= n->bytes;
    --s_blocks;
}

}  // namespace

bool engine_arena_init(void) {
    (void)ownerMutex();   /* eagerly, at boot: see s_owner */
    Guard g;
    ESP_LOGI(TAG, "dynamic: engines take PSRAM as they need it, %u kB free now, %u kB kept for the UI",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(kUiFloorBytes / 1024));
    return true;
}

void *engine_arena_alloc(size_t bytes) {
    if (bytes == 0) return nullptr;
    Guard g;
    void *p = takeBlock(bytes);
    if (p != nullptr) return p;
    /* The pitched bank's budget check and the native engine's head plan are
     * meant to stop a load from getting here; a WARN means one of them asked
     * for more than the floor leaves. */
    ++s_refused;
    ESP_LOGW(TAG, "refused %u B: %u kB above the floor, largest %u kB (refusal #%" PRIu32 ")",
             (unsigned)bytes, (unsigned)(headroom() / 1024), (unsigned)(largestFree() / 1024),
             s_refused);
    return nullptr;
}

void *engine_arena_try_alloc(size_t bytes) {
    if (bytes == 0) return nullptr;
    Guard g;
    return takeBlock(bytes);
}

void engine_arena_free(void *ptr) {
    if (ptr == nullptr) return;
    Guard g;
    if (Node *n = findNode(ptr)) {
        unlink(n);
        heap_caps_free(n);
    } else {
        heap_caps_free(ptr);
    }
}

/* More than one borrower.
 *
 * This was a single hook, documented as "set once, by whoever can do the
 * giving back", and the looper's long mode had it (that app is gone from
 * this firmware; the reason the hook is a list is not). Then a second borrower
 * arrived -- the native sampler's resident heads -- and the one slot meant
 * only one of them was ever told. A pitched kit loading between two sample
 * kits reset the arena behind the sampler's store, and the next take's
 * unload() freed heads the heap no longer knew: an assert inside
 * multi_heap_free on the `smpl_take` task, found by `kit_churn`.
 *
 * Eight slots, appended, duplicates ignored; the signature does not change, so
 * no caller does. Order is registration order, which is fine: each borrower
 * only has to let go of its own. */
#define ENGINE_ARENA_MAX_RECLAIM 8   /* in use: native heads, sample tools, pitched bank, song rings */
static engine_arena_reclaim_fn s_reclaim[ENGINE_ARENA_MAX_RECLAIM] = {NULL};
static void *s_reclaimCtx[ENGINE_ARENA_MAX_RECLAIM] = {NULL};

void engine_arena_set_reclaim(engine_arena_reclaim_fn fn, void *ctx)
{
    if (fn == NULL) return;
    for (int i = 0; i < ENGINE_ARENA_MAX_RECLAIM; ++i) {
        if (s_reclaim[i] == fn && s_reclaimCtx[i] == ctx) return;   /* already in */
        if (s_reclaim[i] == NULL) {
            s_reclaimCtx[i] = ctx;
            s_reclaim[i] = fn;
            return;
        }
    }
    ESP_LOGE(TAG, "reclaim hooks full (%d): a borrower will not be told to let go",
             ENGINE_ARENA_MAX_RECLAIM);
}

bool engine_arena_owner_lock(uint32_t waitMs) {
    SemaphoreHandle_t m = ownerMutex();
    if (waitMs != 0) {
        return xSemaphoreTakeRecursive(m, pdMS_TO_TICKS(waitMs)) == pdTRUE;
    }
    /* Unbounded, but not silent: a wait this long is a take or a bank build
     * that never ended, and the console should say who is stuck. */
    uint32_t waited = 0;
    while (xSemaphoreTakeRecursive(m, pdMS_TO_TICKS(5000)) != pdTRUE) {
        waited += 5;
        TaskHandle_t holder = xSemaphoreGetMutexHolder(m);
        ESP_LOGW(TAG, "owner lock: '%s' waiting %u s on '%s'", pcTaskGetName(nullptr),
                 (unsigned)waited, holder ? pcTaskGetName(holder) : "?");
    }
    return true;
}

void engine_arena_owner_unlock(void) {
    xSemaphoreGiveRecursive(ownerMutex());
}

uint32_t engine_arena_generation(void) {
    return s_generation.load(std::memory_order_acquire);
}

void engine_arena_reset(void) {
    /* The owner lock across the whole reset: a native take or a pitched build
     * on another task finishes first, and nobody fills the arena between the
     * borrowers letting go and the list being freed. The callers that hand
     * the arena over hold it already (recursive). */
    EngineArenaOwner owner;

    /* Borrowers first: what they hold is freed below, and a free of theirs
     * that came later would be a second one. */
    for (int i = 0; i < ENGINE_ARENA_MAX_RECLAIM; ++i) {
        if (s_reclaim[i] != NULL) s_reclaim[i](s_reclaimCtx[i]);
    }

    Guard g;
    const size_t wasUsed = s_used;
    const uint32_t wasBlocks = s_blocks;
    while (s_head != nullptr) {
        Node *n = s_head;
        unlink(n);
        heap_caps_free(n);
    }
    const uint32_t gen = s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    ESP_LOGI(TAG, "reset: %u kB in %u blocks back to PSRAM (%u kB free), generation %" PRIu32,
             (unsigned)(wasUsed / 1024), (unsigned)wasBlocks,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), gen);
}

size_t engine_arena_capacity(void) {
    Guard g;
    return s_used + headroom();
}

bool engine_arena_lendable(void) {
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (crosspad::marcelEngineReady().load(std::memory_order_acquire)) return true;
#endif
    return native_sampler_active();
}

void *engine_arena_lend(size_t bytes) {
    if (bytes == 0) return nullptr;
    Guard g;
    if (!engine_arena_lendable()) return nullptr;
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    /* The prebuilt streamer takes all its memory in Init(), so nothing it
     * does grows into the slack. */
    if (crosspad::marcelEngineReady().load(std::memory_order_acquire)) {
        return takeBlock(bytes);
    }
#endif
    /* Not the owner's reserve. A borrower that took the pages the native
     * engine's pool may still want would turn a later SMPL_POLY raise or a
     * pad edit into a voice with no ring or a pad with no head -- refusing
     * the borrow is the simpler half: the ring goes to the general heap.
     *
     * On the largest free block, not the free total: the pool grows by whole
     * pages and a head is one block, and free bytes that add up in pieces
     * give neither. The loan is cut from that block too, so it has to hold
     * both. Aligned size, as the heap will cut it. */
    const size_t want = sizeof(Node) + alignedSize(bytes);
    if (!crosspad::arenaMayLend(want, largestFree(), native_sampler_arena_keep())) {
        return nullptr;
    }
    return takeBlock(bytes);
}

void *engine_arena_lend_ring(size_t bytes) {
    /* What a ring leaves free while a pitched bank owns the arena: the next
     * instrument is built beside the one playing before that one goes.
     * Refusing outright left a Pitch track's bounce, and the Recorder beside
     * an instrument, the PSRAM heap alone -- "not enough memory" with 1.6 MB
     * of the arena idle. */
    constexpr size_t kPitchedKeepFree = 512 * 1024;
    if (crosspad::pitchedBankInArena()) {
        if (engine_arena_capacity() - engine_arena_used() < bytes + kPitchedKeepFree) return nullptr;
        return engine_arena_try_alloc(bytes);
    }
    return engine_arena_lend(bytes);
}

size_t engine_arena_largest_free(void) {
    Guard g;
    return largestFree();
}

size_t engine_arena_used(void) {
    Guard g;
    return s_used;
}
