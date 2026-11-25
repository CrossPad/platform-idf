// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's IKeyValueStore: NVS, with a queue in front of it.
//
// Why the queue is here rather than in the callers: every settings writer ends
// in these functions — CrosspadSettings::saveTo() (all ~90 keys it owns, called
// by the Settings screen, the quick-settings drawer and the volume overlay),
// OutputLevelsPort::setLevel (the drawer's and Settings' output rows, a HIL
// verb, and a host's fader through main/host_midi.cpp), the transport's click
// settings, usb_config_manager's profile choice, a route preset. Put it here and
// all of them stop writing flash on every step of a gesture; put it in one of
// them and the others go on doing it.
//
// The policy — which value waits how long, what a read answers with, when a
// flush happens — is NvsWriteQueue (include/NvsWriteQueue.hpp), pure C++ and
// pinned by main/tests/test_nvs_write_queue.cpp. What is here is the part that
// belongs to this platform: NVS handles as the sink, the task that may write
// flash, and the hook that runs before a restart.
//
// A flash write needs a task whose stack is in internal RAM: the driver runs
// with the cache disabled and asserts on a PSRAM stack
// (esp_task_stack_is_sane_cache_disabled). The worker's stack is in PSRAM, so
// its job only asks for the write; the write runs on a short-lived task that has
// the stack for it — the same shape main.cpp's soft-off gate uses (see its
// comment over app_arm_softoff_boot_gate).

#include "NvsSettingsStore.hpp"
#include "NvsWriteQueue.hpp"

#include "bsp_worker.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

using crosspad::NvsKind;
using crosspad::NvsQueueSink;
using crosspad::NvsQueueStats;
using crosspad::NvsWriteQueue;

// One settings key, as the queue holds it: ns/key at NVS's own limits, the value,
// and the two stamps the cooldown reads, plus a 4-byte hash tag so a scan of the
// table walks bytes instead of entries. The board's own saveTo() asked for 128
// keys and hit the first version's capacity exactly (`high=128` in its counters),
// so the table is sized for the whole settings blob twice over: ~17 kB of PSRAM,
// and an entry that has to be dropped (the evictions counter) costs a later
// write, never a value.
static constexpr size_t kQueueCapacity = 256;

static const char* kQueueTag = "nvsq";

const char* NvsSettingsStore::TAG = "nvs_store";

static void* s_queueStorage = nullptr;
static NvsWriteQueue* s_queue = nullptr;
static bool s_jobRegistered = false;
static bool s_lazyCheck = false;

// One writer at a time: the worker job, a soft-off and a restart can all ask.
static std::atomic<bool> s_writerBusy{false};
static std::atomic<bool> s_writerDone{false};
static std::atomic<unsigned> s_writerWrote{0};

namespace {

uint64_t queue_clock_us(void*) { return esp_timer_get_time(); }

// The sink: one NVS handle per namespace per flush, so a batch of changed keys
// is one commit instead of one per key. Only ever called from the queue's flush,
// which the queue serialises, so the handle needs no lock of its own.
struct NvsSinkCtx { nvs_handle_t h = 0; };
static NvsSinkCtx s_sink;

bool sink_open(void*, const char* ns)
{
    return nvs_open(ns, NVS_READWRITE, &s_sink.h) == ESP_OK;
}

bool sink_write(void*, const char*, NvsKind kind, const char* key, int32_t value)
{
    switch (kind) {
        case NvsKind::Bool: return nvs_set_u8(s_sink.h, key, value ? 1 : 0) == ESP_OK;
        case NvsKind::U8:   return nvs_set_u8(s_sink.h, key, (uint8_t)value) == ESP_OK;
        case NvsKind::I16:  return nvs_set_i16(s_sink.h, key, (int16_t)value) == ESP_OK;
        case NvsKind::I32:  return nvs_set_i32(s_sink.h, key, value) == ESP_OK;
    }
    return false;
}

void sink_close(void*, const char* ns)
{
    if (nvs_commit(s_sink.h) != ESP_OK) ESP_LOGW(kQueueTag, "%s: commit failed", ns);
    nvs_close(s_sink.h);
    s_sink.h = 0;
}

/// One pass of writing: up to a batch per namespace, everything pending for
/// @p all. Only ever called on a task with an internal stack.
unsigned flush_pass(bool all)
{
    if (!s_queue) return 0;
    return all ? s_queue->flushAll() : s_queue->flushDue();
}

unsigned flush_drain(bool all)
{
    unsigned wrote = 0, passes = 0;
    // One pass takes at most the queue's batch (96 values) from at most 8
    // namespaces; the board's own settings blob is ~150 keys over ~15 namespaces,
    // so a "write everything now" has to keep going until nothing is owed. The
    // bound is there so a sink that keeps refusing cannot spin this task forever:
    // 40 passes is 3 840 values, and the pending count in the log line says what
    // is left if it ever does stop early.
    while (passes++ < 40) {
        const unsigned n = flush_pass(all);
        wrote += n;
        if (!n) break;
    }
    return wrote;
}

/// One line per flush that did something, with the counters that say what the
/// queue cost the board and what it saved: this is the read-out the bench ladder
/// (.strata/tools/nvsqbench.py) greps for on the console port, so a PR's claim
/// "one gesture, one write" is a line on the wire and not a promise.
void log_flush(const char* reason, unsigned wrote, uint64_t t0)
{
    // Not on the writer task's stack: it only ever runs inside one flush at a
    // time (the queue serialises them), and 192 bytes of buffer on a task whose
    // other job is NVS's page machinery is how the first build of this feature
    // tripped its stack canary.
    static char stats[192];
    NvsSettingsStore::queueStatsText(stats, sizeof(stats));
    ESP_LOGI(kQueueTag, "%s: wrote=%u pending=%u in %u ms | %s", reason, wrote,
             NvsSettingsStore::queuePending(),
             (unsigned)((esp_timer_get_time() - t0) / 1000), stats);
}

/// The write, on a task with an internal stack, then this task is done with.
void nvsq_writer_task(void* arg)
{
    const bool all = arg != nullptr;
    const uint64_t t0 = esp_timer_get_time();
    const unsigned wrote = flush_drain(all);
    s_writerWrote.store(wrote);
    if (wrote || NvsSettingsStore::queuePending()) log_flush(all ? "flush-all" : "cooldown", wrote, t0);
    s_writerDone.store(true);
    s_writerBusy.store(false);
    vTaskDelete(nullptr);
}

bool spawn_writer(bool all)
{
    if (s_writerBusy.exchange(true)) return false;
    s_writerDone.store(false);
    s_writerWrote.store(0);
    // 6 kB, not the 4 kB main.cpp's single-write gate task uses: this body runs
    // NVS's page machinery (nvs_open/set/commit, which measures a page and may
    // compact one) and a log line with the counters. The first build of this
    // feature overflowed a 4 kB stack and the stack canary took it down
    // ("Stack canary watchpoint triggered (nvsq_flush)") — found on the bench,
    // not in review. The task is one-shot: the RAM is back between flushes.
    TaskHandle_t h = nullptr;
    if (xTaskCreatePinnedToCore(nvsq_writer_task, "nvsq_flush", 6144,
                                all ? (void*)1 : nullptr, 5, &h, tskNO_AFFINITY) != pdPASS) {
        s_writerBusy.store(false);
        ESP_LOGE(kQueueTag, "cannot start the settings writer");
        return false;
    }
    return true;
}

/// Ask for the write and wait for it, bounded. From a PSRAM-stack task — the
/// worker, the soft-off watch — this is the only safe way to want a flash write.
bool request_write(bool all, const char* reason, int maxWaitMs)
{
    const uint64_t t0 = esp_timer_get_time();
    if (!spawn_writer(all)) {
        // Someone else is writing already; that pass may not cover what we want
        // (an "all" ask behind a "due" one), so wait and look again.
        while (!s_writerDone.load() && (esp_timer_get_time() - t0) < (uint64_t)maxWaitMs * 1000)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (!NvsSettingsStore::queuePending()) return true;
        if (!spawn_writer(all)) return false;
    }
    for (int i = 0; i <= maxWaitMs / 10 && !s_writerDone.load(); ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!s_writerDone.load())
        ESP_LOGW(kQueueTag, "%s: still writing after %d ms, %u values pending",
                 reason, maxWaitMs, NvsSettingsStore::queuePending());
    // When it did finish, the writer task has already logged the line with the
    // counters in it; logging here too would read as two writes.
    return s_writerDone.load();
}

/// The worker's job: never writes, and never waits either (its stack is in PSRAM
/// and its contract is a short body — the task that also paints the pads waits
/// behind it). It only starts the writer and moves on; the writer logs what it
/// wrote. Waiting is left to the callers that have to: the soft-off and a
/// restart, which ask for it themselves.
uint32_t nvsq_job(void*)
{
    if (!s_queue || s_writerBusy.load() || !s_queue->hasDue()) return 0;
    spawn_writer(false);
    return 0;
}

/// esp_restart() runs the shutdown handlers first, with the scheduler still up,
/// and every way this firmware leaves — OTA, slot switch, transfer and network
/// mode, the RESTART verb, the soft-off — ends in esp_restart(). A panic does
/// not: a crash costs at most the quiet period of edits, which is the honest
/// trade for not writing flash per step. And NVS's own page format is
/// power-fail safe, so a commit cut short costs that key, not the store.
void nvsq_shutdown(void)
{
    if (!s_queue || !s_queue->pending()) return;
    // Always the one-shot task, never inline: who calls esp_restart() is not in
    // our hands (the CDC dispatcher's main task runs on a 4 kB stack, the worker
    // on a PSRAM one), and NVS's page machinery does not fit into what is left.
    request_write(true, "restart", 500);
}

/// Only when the worker has no job slot for us: look for a due write from the
/// setter, so a change still reaches flash on its own once something asks again.
void lazy_check(void)
{
    if (!s_lazyCheck || !s_queue || s_writerBusy.load() || !s_queue->hasDue()) return;
    if (!esp_ptr_internal(pxTaskGetStackStart(nullptr))) { request_write(false, "cooldown", 250); return; }
    flush_drain(false);
}

} // namespace

bool NvsSettingsStore::init() {
    if (initialized_) return true;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        initialized_ = true;
        ESP_LOGI(TAG, "NVS initialized");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return initialized_;
    }

    // One block holds the entries and the queue object itself, so there is one
    // allocation and it all lands in the same memory: 128 entries + the object's
    // locks, counters and flush working set is ~10 kB, which is PSRAM money and
    // not internal-RAM money. (If PSRAM is somehow not available, fall back to
    // internal; if that fails too, keep writing straight through to flash — the
    // old behaviour, and a line in the log saying so.)
    const size_t tableBytes = NvsWriteQueue::tableBytes(kQueueCapacity);
    const size_t blockBytes = tableBytes + sizeof(NvsWriteQueue) + alignof(NvsWriteQueue);
    void* raw = heap_caps_malloc(blockBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char* where = "psram";
    if (!raw) {
        raw = heap_caps_malloc(blockBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        where = "internal";
    }
    if (!raw) {
        ESP_LOGE(kQueueTag, "no memory for the settings queue (%u B): writing straight through",
                 (unsigned)blockBytes);
        return initialized_;
    }
    // Align the object that follows the table, and keep both pointers for the log.
    uintptr_t base = (uintptr_t)raw;
    uintptr_t objAt = base + tableBytes;
    const uintptr_t mask = alignof(NvsWriteQueue) - 1;
    if (objAt & mask) objAt = (objAt & ~mask) + alignof(NvsWriteQueue);
    s_queueStorage = (void*)base;
    {
        NvsQueueSink sink;
        sink.ctx = &s_sink;
        sink.open = sink_open;
        sink.write = sink_write;
        sink.close = sink_close;
        s_queue = new ((void*)objAt) NvsWriteQueue(s_queueStorage, kQueueCapacity,
                                                   queue_clock_us, nullptr, sink);
    }
    ESP_LOGI(kQueueTag, "settings writes queued: table %u B + queue %u B in %s, cap %u keys",
             (unsigned)tableBytes, (unsigned)sizeof(NvsWriteQueue), where, (unsigned)kQueueCapacity);

    s_jobRegistered = bsp_worker_add("nvsq", 1000, nvsq_job, nullptr) != nullptr;
    if (!s_jobRegistered) {
        // BSP_WORKER_MAX_JOBS is 12 and the table is shared; with no slot the
        // due check moves into the setters, so a change still reaches flash,
        // just only when something asks again.
        s_lazyCheck = true;
        ESP_LOGW(kQueueTag, "no worker slot: checking for due writes on each save");
    }
    const esp_err_t hooked = esp_register_shutdown_handler(nvsq_shutdown);
    if (hooked != ESP_OK)
        ESP_LOGW(kQueueTag, "no shutdown slot (%s): the periodic flush and the soft-off hook are all there is",
                 esp_err_to_name(hooked));
    return initialized_;
}

// ---- Per-key helpers ----
//
// Reads answer from the queue when it holds the key: a host that moves a fader
// and reads its own map back must see what it asked for, not what was last on
// flash — a read-back of the old value is how a control loop ends up writing
// twice as much. A key the queue never saw reads from flash exactly as before.

bool NvsSettingsStore::readBool(const char* ns, const char* key, bool defaultVal) {
    int32_t v;
    if (s_queue && s_queue->peek(ns, key, NvsKind::Bool, &v)) return v != 0;
    nvs_handle_t h;
    uint8_t val = defaultVal ? 1 : 0;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    return val != 0;
}

uint8_t NvsSettingsStore::readU8(const char* ns, const char* key, uint8_t defaultVal) {
    int32_t v;
    if (s_queue && s_queue->peek(ns, key, NvsKind::U8, &v)) return (uint8_t)v;
    nvs_handle_t h;
    uint8_t val = defaultVal;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    return val;
}

int32_t NvsSettingsStore::readI32(const char* ns, const char* key, int32_t defaultVal) {
    int32_t v;
    if (s_queue && s_queue->peek(ns, key, NvsKind::I32, &v)) return v;
    nvs_handle_t h;
    int32_t val = defaultVal;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &val);
        nvs_close(h);
    }
    return val;
}

int16_t NvsSettingsStore::readI16(const char* ns, const char* key, int16_t defaultVal) {
    int32_t v;
    if (s_queue && s_queue->peek(ns, key, NvsKind::I16, &v)) return (int16_t)v;
    nvs_handle_t h;
    int16_t val = defaultVal;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i16(h, key, &val);
        nvs_close(h);
    }
    return val;
}

void NvsSettingsStore::saveBool(const char* ns, const char* key, bool value) {
    if (s_queue) { s_queue->put(ns, key, NvsKind::Bool, value ? 1 : 0); lazy_check(); return; }
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void NvsSettingsStore::saveU8(const char* ns, const char* key, uint8_t value) {
    if (s_queue) { s_queue->put(ns, key, NvsKind::U8, value); lazy_check(); return; }
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

void NvsSettingsStore::saveI32(const char* ns, const char* key, int32_t value) {
    if (s_queue) { s_queue->put(ns, key, NvsKind::I32, value); lazy_check(); return; }
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

void NvsSettingsStore::saveI16(const char* ns, const char* key, int16_t value) {
    if (s_queue) { s_queue->put(ns, key, NvsKind::I16, value); lazy_check(); return; }
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i16(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- Factory reset ----

void NvsSettingsStore::eraseAll() {
    ESP_LOGW(TAG, "Factory reset - erasing all settings namespaces");
    const char* namespaces[] = {
        "cfg_display", "cfg_audio", "cfg_rgb", "cfg_keypad",
        "cfg_vibrate", "cfg_wireless", "cfg_stm_rt", "cfg_esp_rt",
        "cfg_system", "fwupd", "cpacct"
    };
    for (const char* ns : namespaces) {
        // Forget first: an entry claiming a value is on flash would skip the
        // next write of it, and a board erased under its own queue would then
        // boot into a default the queue keeps answering as the old value.
        if (!eraseNamespace(ns)) ESP_LOGW(TAG, "could not erase %s", ns);
    }
    ESP_LOGW(TAG, "Factory reset complete - restart to apply defaults");
}

// ---- the queue's doors ----

unsigned NvsSettingsStore::queuePending()
{
    return s_queue ? s_queue->pending() : 0;
}

bool NvsSettingsStore::queueFlushWait(const char* reason, int maxWaitMs)
{
    if (!s_queue || !s_queue->pending()) return true;
    // Always on the queue's own internal-stack task, whatever the caller's stack
    // is: a soft-off arrives from the worker (a PSRAM stack) and an OTA
    // hand-over from the CDC dispatcher (a 4 kB one), and neither is a place to
    // run NVS's page machinery inline.
    return request_write(true, reason, maxWaitMs);
}

void NvsSettingsStore::queueStatsText(char* out, size_t len)
{
    if (!out || !len) return;
    if (!s_queue) { snprintf(out, len, "queue=none"); return; }
    const NvsQueueStats& s = s_queue->stats();
    // %u with uint32_t: on the S3 uint32_t is `long unsigned int`, so every
    // counter is cast rather than the format widened (%lu reads worse on a board
    // where these are small counts).
    snprintf(out, len,
             "puts=%u same=%u coalesced=%u written=%u errors=%u flushes=%u "
             "pending=%u high=%u evict=%u refused=%u job=%s",
             (unsigned)s.puts, (unsigned)s.unchanged, (unsigned)s.coalesced,
             (unsigned)s.written, (unsigned)s.errors, (unsigned)s.flushes,
             s_queue->pending(), (unsigned)s.high, (unsigned)s.evictions,
             (unsigned)s.refused,
             s_jobRegistered ? "worker" : (s_lazyCheck ? "lazy" : "none"));
}

void NvsSettingsStore::queueForget(const char* ns)
{
    if (s_queue && ns) s_queue->forget(ns);
}

bool NvsSettingsStore::eraseNamespace(const char* ns)
{
    if (!ns) return false;
    queueForget(ns);
    nvs_handle_t h;
    const esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;   // nothing there, nothing to forget
    if (err != ESP_OK) return false;
    const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
