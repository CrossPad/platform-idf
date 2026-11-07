/**
 * @file lvgl_mem_psram.c
 * @brief LVGL allocator backed by PSRAM.
 *
 * LVGL widgets are small, and CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL keeps small
 * allocations internal — so the whole UI was built from the scarcest memory on
 * the chip. That capped the kit list around 18 kits, where lv_realloc() returns
 * NULL and LVGL writes into it anyway (StoreProhibited, not a clean failure).
 * Lowering the Kconfig globally would also move driver DMA buffers to PSRAM;
 * routing only LVGL's allocations does not.
 *
 * The routing is applied with -Wl,--wrap (see main/CMakeLists.txt) rather than
 * only by implementing LVGL's LV_STDLIB_CUSTOM hooks, because which allocator
 * LVGL compiles is a Kconfig *choice* and sdkconfig.defaults pins it to
 * LV_USE_CLIB_MALLOC. With just the hooks, this file compiled to nothing in
 * every build directory and the UI kept allocating internal RAM: measured on
 * hardware, one open app cost ~48 B of internal RAM per widget — 22 kB for the
 * Sampler, on a board with 28 kB free. Wrapping lv_malloc_core & friends works
 * whichever allocator the choice lands on, so a regenerated sdkconfig cannot
 * silently switch the routing back off.
 *
 * Only LVGL's own allocations move. The display draw buffers are allocated by
 * esp_lvgl_port with explicit DMA caps and handed over with
 * lv_display_set_buffers(), so they never pass through here.
 *
 * Falls back to the default heap when PSRAM is exhausted or absent.
 *
 * Emergency pool. PSRAM is the scarce memory here: the sample engines share
 * the 6.2 MB pool with LVGL, taking what a kit needs down to a 512 kB floor
 * (engine_arena.cpp), and a kit list, a loaded kit, the boot's update check
 * on WiFi and an open app can leave nothing of the rest. An LVGL
 * allocation that fails then is not clean: the draw-task, style and
 * animation allocations assert (lvgl_assert/lvgl_assert_handler.h), and a
 * layer buffer that cannot be had is retried inside the refresh loop without
 * yielding. So 64 kB of PSRAM, taken at LVGL's first allocation, is made a
 * heap of its own that only LVGL draws on, once PSRAM has said no and before
 * the internal fallback: the screen finishes drawing, the app can be
 * closed, and the memory comes back. It used to be handed to the general heap
 * on the first failure, where the next kit copy, TLS record or PNG decode
 * took it within milliseconds and LVGL failed regardless -- a KIT_LOAD under
 * the boot's update check, back_churn after kit_churn, both with the board
 * dead in an assert. Nothing above LVGL_POOL_MAX_ALLOC comes out of it: one
 * decode of a picture too big to fit would empty it for nothing.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "multi_heap.h"
#include "lvgl.h"

#define LVGL_MEM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* A 320x7 ARGB layer chunk is 8960 B and a screen may need a few at once. */
#define LVGL_POOL_BYTES      (64 * 1024)
#define LVGL_POOL_MAX_ALLOC  (16 * 1024)
/* The internal-RAM fallback must not drain what the audio tasks, sdmmc and the
 * CDC loop live on. Measured: with PSRAM gone the kit browser took internal
 * from 26 kB to 143 B through this fallback. Below this floor LVGL gets NULL. */
#define LVGL_INTERNAL_FLOOR  (12 * 1024)

static const char *TAG = "lvgl_mem";
static uint8_t *s_poolMem = NULL;
static multi_heap_handle_t s_pool = NULL;
static portMUX_TYPE s_poolLock = portMUX_INITIALIZER_UNLOCKED;
static size_t s_poolWhole = 0;   /* its free size with nothing in it */
static bool s_poolTried = false;
static uint32_t s_poolEpisodes = 0;

/* At LVGL's first allocation, from lv_init() -- one thread, PSRAM still whole. */
static void pool_init(void)
{
    if (s_poolTried) return;
    s_poolTried = true;
    s_poolMem = (uint8_t *)heap_caps_malloc(LVGL_POOL_BYTES, LVGL_MEM_CAPS);
    if (s_poolMem) s_pool = multi_heap_register(s_poolMem, LVGL_POOL_BYTES);
    if (s_pool) {
        multi_heap_set_lock(s_pool, &s_poolLock);
        s_poolWhole = multi_heap_free_size(s_pool);
    } else {
        ESP_LOGE(TAG, "no %u B emergency pool: an exhausted PSRAM will stop LVGL", (unsigned)LVGL_POOL_BYTES);
    }
}

static bool in_pool(const void *p)
{
    return s_pool && (const uint8_t *)p >= s_poolMem && (const uint8_t *)p < s_poolMem + LVGL_POOL_BYTES;
}

static void *pool_malloc(size_t size)
{
    if (!s_pool || size > LVGL_POOL_MAX_ALLOC) return NULL;
    /* Said once as the pool starts being used, not per allocation: a starved
     * screen makes hundreds a second. */
    const bool wasIdle = multi_heap_free_size(s_pool) == s_poolWhole;
    void *p = multi_heap_malloc(s_pool, size);
    if (p && wasIdle) {
        ESP_LOGW(TAG, "PSRAM exhausted (%u B wanted): drawing from the %u B emergency pool (#%" PRIu32 ")",
                 (unsigned)size, (unsigned)LVGL_POOL_BYTES, ++s_poolEpisodes);
    }
    return p;
}

static bool internal_can_spare(size_t size)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >
           size + LVGL_INTERNAL_FLOOR;
}

static void *lvgl_psram_malloc(size_t size)
{
    pool_init();
    void *p = heap_caps_malloc(size, LVGL_MEM_CAPS);
    if (p == NULL) {
        p = pool_malloc(size);
    }
    if (p == NULL && internal_can_spare(size)) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void lvgl_psram_free(void *p)
{
    if (in_pool(p)) multi_heap_free(s_pool, p);
    else heap_caps_free(p);
}

/* A block moved between the pool and the heaps: the new one first, then the
 * old one copied and freed. */
static void *move_block(void *p, size_t oldSize, size_t newSize)
{
    void *n = lvgl_psram_malloc(newSize);
    if (n == NULL) return NULL;
    memcpy(n, p, oldSize < newSize ? oldSize : newSize);
    lvgl_psram_free(p);
    return n;
}

static void *lvgl_psram_realloc(void *p, size_t new_size)
{
    if (p == NULL) return lvgl_psram_malloc(new_size);
    if (new_size == 0) {
        lvgl_psram_free(p);
        return NULL;
    }
    if (in_pool(p)) {
        /* Out of the pool as soon as the heap can take it back. */
        void *n = move_block(p, multi_heap_get_allocated_size(s_pool, p), new_size);
        return n ? n : multi_heap_realloc(s_pool, p, new_size);
    }
    void *n = heap_caps_realloc(p, new_size, LVGL_MEM_CAPS);
    if (n == NULL && new_size <= LVGL_POOL_MAX_ALLOC) {
        n = pool_malloc(new_size);
        if (n) {
            const size_t old = heap_caps_get_allocated_size(p);
            memcpy(n, p, old < new_size ? old : new_size);
            heap_caps_free(p);
        }
    }
    if (n == NULL && internal_can_spare(new_size)) {
        n = heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
    }
    return n;
}

/* --- Link-time routing -----------------------------------------------------
 * These intercept every LVGL allocation regardless of LV_USE_STDLIB_MALLOC.
 * heap_caps_free() is capability-agnostic, so it releases blocks that came
 * from either heap; only the pool's own go back through multi_heap_free().
 */

void *__wrap_lv_malloc_core(size_t size)
{
    return lvgl_psram_malloc(size);
}

void *__wrap_lv_realloc_core(void *p, size_t new_size)
{
    return lvgl_psram_realloc(p, new_size);
}

void __wrap_lv_free_core(void *p)
{
    lvgl_psram_free(p);
}

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

void lv_mem_init(void)
{
    /* Nothing to init — the ESP heap allocator is already up. */
}

void lv_mem_deinit(void)
{
    /* Nothing to deinit. */
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL; /* Not supported — the ESP heap owns the pools. */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return lvgl_psram_malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return lvgl_psram_realloc(p, new_size);
}

void lv_free_core(void *p)
{
    lvgl_psram_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (mon_p == NULL) return;

    multi_heap_info_t info;
    heap_caps_get_info(&info, LVGL_MEM_CAPS);

    lv_memzero(mon_p, sizeof(*mon_p));
    mon_p->total_size = heap_caps_get_total_size(LVGL_MEM_CAPS);
    mon_p->free_cnt = info.free_blocks;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->used_pct = mon_p->total_size
                          ? (uint8_t)(100 - (info.total_free_bytes * 100 / mon_p->total_size))
                          : 0;
    mon_p->frag_pct = info.total_free_bytes
                          ? (uint8_t)(100 - (info.largest_free_block * 100 / info.total_free_bytes))
                          : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(LVGL_MEM_CAPS, true) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
