// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// NvsWriteQueue — the settings values a host or a finger changed, written to
// flash once per gesture instead of once per step.
//
// Why: a slider calls its setter for every step of the drag — the quick-settings
// drawer (crosspad-gui status_bar.cpp), Settings -> Audio's output rows
// (settings_ui_audio.cpp) and a host's fader (main/host_midi.cpp) all end in
// IOutputLevels::setLevel, and the Settings screen's rows end in
// CrosspadSettings::saveTo(), which saves every key it owns (~90 of them) each
// time one of them moves. Those setters persisted straight through to flash:
// nvs_set_u8 + nvs_commit per step, of every key, changed or not. NVS is
// append-per-set and compacts a page when it runs out, so a drag is not one
// write but a page's worth of writes and sometimes a move, and it happens on
// whoever's thread was moving the slider — the LVGL thread for a finger, the
// MIDI task for a host.
//
// This class holds the *policy* only, with no ESP and no FreeRTOS in it: the
// newest value of every key it has seen, the rule for when that value has to
// reach flash, and a sink it calls to do the writing. So it is testable off the
// board (main/tests/test_nvs_write_queue.cpp) with a clock a test steps by hand
// and a sink that records. The board's sink — NVS handles, the task that owns
// flash writes, and the hook that runs before a restart — is in
// NvsSettingsStore.cpp, next to the NVS access it replaces.
//
// The rules, in one place:
//   * A value that equals what the queue already holds is not a change and
//     costs nothing: saveTo() of a settings blob nobody touched writes nothing.
//   * A changed value is *pending* until either the quiet period has passed
//     since the last change to that key (so a whole drag is one write) or the
//     max-age has passed since it first went pending (so a host that keeps a
//     fader moving is answered within maxAge and not at every message).
//   * Reads answer from the queue: the value a host asked for a step ago is
//     what a read-back shows, whether or not it is on flash yet. A read-back
//     that showed the old value would make a control loop write twice as much.
//   * Pending values survive nothing but a flush: a restart, an OTA or a
//     power-off has to flush first (the store registers that hook); a crash
//     loses at most the quiet period of edits. NVS's own page format is
//     power-fail safe, so an interrupted commit costs the key, not the store.
//   * The table is fixed size. When it is full, the oldest *written* (clean)
//     entry is dropped: the value is on flash, so the cost is one more write
//     later for that key, never a lost one.

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace crosspad {

/// How the value reaches flash. Bool and U8 are the same NVS type (u8);
/// I16 and I32 are theirs. A key keeps one of them: mixing kinds on one key
/// would be a type mismatch on NVS's side, and the queue refuses to answer a
/// read of a different kind so the caller sees exactly what it sees today.
enum class NvsKind : uint8_t { Bool = 0, U8 = 1, I16 = 2, I32 = 3 };

/// The board's way of writing. Called with the queue's lock NOT held, so a
/// flash write never holds up the next slider step:
///   open(ns) once per namespace with due entries, write() for each of that
///   namespace's entries, close(ns) to commit and release the handle.
/// Keeping one namespace's entries in one batch is the point: it is one
/// commit, not one per key.
struct NvsQueueSink {
    void* ctx = nullptr;
    bool (*open)(void* ctx, const char* ns) = nullptr;
    bool (*write)(void* ctx, const char* ns, NvsKind kind, const char* key,
                  int32_t value) = nullptr;
    void (*close)(void* ctx, const char* ns) = nullptr;
};

struct NvsQueueStats {
    uint32_t puts = 0;         // save calls that reached the queue
    uint32_t unchanged = 0;    // of those, values that already matched: no write at all
    uint32_t coalesced = 0;    // changes folded into an edit already waiting: a write avoided
    uint32_t written = 0;      // values handed to the sink
    uint32_t errors = 0;       // the sink refused one
    uint32_t flushes = 0;      // sink batches opened
    uint32_t evictions = 0;    // clean entries dropped to make room
    uint32_t refused = 0;      // a change the table could not hold
    uint32_t high = 0;         // most entries held at once
    uint32_t pendingHigh = 0;  // most pending values at once
    uint32_t lastWrote = 0;    // values written by the last flush
    uint64_t lastFlushUs = 0;  // clock() at the last flush that wrote something
};

class NvsWriteQueue {
public:
    /// Microseconds, from whatever clock the owner supplies (esp_timer_get_time
    /// on the board; a variable in a test).
    using ClockFn = uint64_t (*)(void* ctx);

    /// Room for a namespace / a key: NVS's own limit is 15 characters for both.
    static constexpr size_t kNsRoom = 16;
    static constexpr size_t kKeyRoom = 16;

    /// The longest interval a value stays pending while its key keeps changing:
    /// a motorised fader that never stops still reaches flash within this.
    static constexpr uint64_t kMaxAgeUs = 120ull * 1000 * 1000;
    /// The quiet spell after the last change to a key before its value is
    /// written. A drag ends, this passes once, and the gesture is one write.
    static constexpr uint64_t kQuietUs = 5ull * 1000 * 1000;

    /// @p table is storage for @p capacity entries (the caller owns it; the
    /// board puts it in PSRAM, a test on the stack). capacity must be at least
    /// the number of keys that can be in flight at once — see NvsQueueStats'
    /// high/refused, which say what the real number was.
    NvsWriteQueue(void* table, size_t capacity, ClockFn clock, void* clockCtx,
                  const NvsQueueSink& sink);

    NvsWriteQueue(const NvsWriteQueue&) = delete;
    NvsWriteQueue& operator=(const NvsWriteQueue&) = delete;

    /// Remember @p value for ns/key and schedule the write. Returns false only
    /// when the table could not take the key at all (the value the caller keeps
    /// in its own settings struct is then simply not persisted early); the
    /// queue never blocks a caller on flash.
    bool put(const char* ns, const char* key, NvsKind kind, int32_t value);

    /// The value the queue holds for ns/key of this @p kind — written or still
    /// pending — into @p out. False when the queue does not know it (the caller
    /// reads it from flash as before) or when it knows it under another kind.
    bool peek(const char* ns, const char* key, NvsKind kind, int32_t* out) const;

    /// Forget entries: with @p key null, every entry of that namespace (an
    /// erase of the namespace behind the queue's back, so a later write of the
    /// same value must not be skipped as "already there"). Waits for a write in
    /// flight: an erase that ran beside one would be answered by it afterwards.
    void forget(const char* ns, const char* key = nullptr);

    /// Write every entry whose cooldown has elapsed. Returns how many went to
    /// the sink. Safe to call from several tasks: a flush is serialised, and a
    /// key edited during one is written again on the next pass.
    unsigned flushDue();

    /// Write everything pending, elapsed or not — the before-a-restart call.
    /// Returns how many went to the sink.
    unsigned flushAll();

    /// True when flushDue() would write something now.
    bool hasDue() const;
    /// Values not on flash yet.
    unsigned pending() const;
    /// Microseconds until the next entry becomes due; 0 = now. Lets an owner
    /// sleep sensibly instead of polling every second.
    uint64_t nextDueInUs() const;

    const NvsQueueStats& stats() const { return stats_; }

private:
    enum State : uint8_t { kEmpty = 0, kClean = 1, kDirty = 2, kInFlight = 3 };

    struct Entry {
        char ns[kNsRoom];
        char key[kKeyRoom];
        int32_t value = 0;
        uint64_t changedUs = 0;  // last change (or the flush that wrote it)
        uint64_t firstUs = 0;    // when this edit first went pending
        NvsKind kind = NvsKind::U8;
        State state = kEmpty;
        uint32_t gen = 0;        // bumped by every accepted put, to spot a change mid-flush
    };

    struct Item {          // a copy of one entry, taken to the sink
        Entry* slot = nullptr;
        uint32_t gen = 0;
        NvsKind kind = NvsKind::U8;
        int32_t value = 0;
        bool ok = false;   // the sink took it, set while the lock is not held
    };

    static constexpr size_t kMaxNsPerFlush = 8;
    /// Values one flush pass takes out of the table. Not per namespace — the
    /// board's `saveTo()` puts ~150 keys in ONE namespace, so a per-namespace
    /// cap would make it wait for several cooldowns. Members of the object (in
    /// the owner's block), not locals: the first build put this on a 4 kB task
    /// stack and the stack canary took it down.
    static constexpr size_t kMaxBatch = 96;

    Entry* find(const char* ns, const char* key) const;
    Entry* findTagged(uint32_t tag, const char* ns, const char* key) const;
    Entry* insert(const char* ns, const char* key, NvsKind kind, uint32_t tag);
    bool dueNow(const Entry& e, uint64_t now) const;
    unsigned drain(bool all);

    Item items_[kMaxBatch];
    char nsList_[kMaxNsPerFlush][kNsRoom];

    /// A hash of ns+key per entry, in its own compact array: `saveTo()` asks for
    /// ~150 keys in a row, and the first build of this feature spent more time
    /// walking the 64-byte entries (in PSRAM) than it saved on flash. Walking
    /// uint32 tags instead is a few hundred compares against 1 kB, and the entry
    /// itself is only touched when a tag matches.
    uint32_t* tags_ = nullptr;
    static uint32_t tagOf(const char* ns, const char* key);

    unsigned pending_ = 0;   // entries kDirty or kInFlight; kept as states change

    size_t capacity_ = 0;
    size_t count_ = 0;
    Entry* const slots_;
    ClockFn const clock_;
    void* const clockCtx_;
    NvsQueueSink sink_;
    mutable std::mutex mtx_;   // the table and the stats
    std::mutex flushMtx_;      // one flush at a time; held WITHOUT mtx_
    NvsQueueStats stats_;

public:
    /// The table is the caller's storage, so it can live where the owner wants:
    /// the board keeps its entries in PSRAM (a few kB is not internal-RAM money),
    /// a test keeps one on the stack. tableBytes() sizes it, kTableAlign says how
    /// it must be aligned, and the queue is then constructed with that pointer —
    /// Entry stays private, so the shape of an entry can change without touching
    /// an owner. The queue object itself is NOT in that buffer: it holds the
    /// mutexes that guard it.
    static constexpr size_t kTableAlign = alignof(Entry);
    /// Entries plus their tag array — one buffer, split by the queue itself.
    static size_t tableBytes(size_t capacity)
    {
        return capacity * (sizeof(Entry) + sizeof(uint32_t));
    }
};

} // namespace crosspad
