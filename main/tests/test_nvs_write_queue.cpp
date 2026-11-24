// SPDX-License-Identifier: GPL-3.0-or-later
//
// The settings write queue, off the board: NvsWriteQueue (policy only) against a
// clock a test steps by hand and an NVS that only records. These are the rules
// main/NvsSettingsStore's wiring depends on and that no board run would name:
// what a gesture costs, what a read answers, what survives an erase, what
// happens when the flash refuses, and what happens when a value changes while
// its own write is on its way.
//
//   cmake -S main/tests -B main/tests/build && cmake --build main/tests/build
//   ctest --test-dir main/tests/build --output-on-failure

#include <catch2/catch_test_macros.hpp>

#include "NvsWriteQueue.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using crosspad::NvsKind;
using crosspad::NvsQueueSink;
using crosspad::NvsWriteQueue;

namespace {

uint64_t g_now = 0;
uint64_t fakeClock(void*) { return g_now; }

/// What the queue asked flash to do, in order, and what it is willing to say it
/// did. `fail` names keys that come back refused, the way a full NVS page does.
struct FakeNvs {
    struct Op { std::string ns, key; NvsKind kind; int32_t value; int handle; };
    std::vector<Op> ops;
    std::vector<std::string> opened, closed;
    std::vector<std::string> fail;
    bool openFails = false;
    int writesInside = 0;   // how many writes this flush has done so far

    bool open(const char* ns)
    {
        if (openFails) return false;
        opened.push_back(ns);
        writesInside = 0;
        return true;
    }
    bool write(const char* ns, NvsKind kind, const char* key, int32_t value)
    {
        writesInside++;
        // A host that moves the same fader again while its value is on the way:
        // legal here, and the queue must not lose it or mark it written.
        if (reroute && writesInside == reroute->afterWrite) reroute->fn(*reroute->slot);
        for (const std::string& f : fail)
            if (f == key) return false;
        ops.push_back({ns, key, kind, value, (int)opened.size()});
        return true;
    }
    void close(const char* ns) { closed.push_back(ns); }

    struct Reroute {
        int afterWrite = 0;
        NvsWriteQueue* slot = nullptr;
        void (*fn)(NvsWriteQueue&) = nullptr;
    };
    Reroute* reroute = nullptr;

    int count(const char* key) const
    {
        int n = 0;
        for (const Op& o : ops) if (o.key == key) n++;
        return n;
    }
    const Op* last(const char* key) const
    {
        const Op* found = nullptr;
        for (const Op& o : ops) if (o.key == key) found = &o;
        return found;
    }
};

NvsQueueSink sinkFor(FakeNvs& nvs)
{
    NvsQueueSink s;
    s.ctx = &nvs;
    s.open = [](void* c, const char* ns) { return static_cast<FakeNvs*>(c)->open(ns); };
    s.write = [](void* c, const char* ns, NvsKind k, const char* key, int32_t v) {
        return static_cast<FakeNvs*>(c)->write(ns, k, key, v);
    };
    s.close = [](void* c, const char* ns) { static_cast<FakeNvs*>(c)->close(ns); };
    return s;
}

/// The namespace an op says it went through: FakeNvs numbers its opens from 1
/// (opened.size() at the time of the write), so handle n is opened[n-1].
const char* openedOf(const FakeNvs* n, int handle)
{
    if (!n || handle < 1 || (size_t)handle > n->opened.size()) return "";
    return n->opened[(size_t)handle - 1].c_str();
}

/// A queue holding `cap` keys, on a buffer the test owns: exactly how the board
/// builds one (size it, align it, hand the table to the queue), except that the
/// table here is on the heap of the test rather than in PSRAM. The queue object
/// is a member, never inside that buffer — it guards the buffer with its mutexes.
class QueueOn {
public:
    QueueOn(FakeNvs& nvs, size_t cap)
        : store_(NvsWriteQueue::tableBytes(cap) + NvsWriteQueue::kTableAlign),
          nvs_(nvs),
          table_(alignedTo(store_)),
          q_(table_, cap, fakeClock, nullptr, sinkFor(nvs))
    {
        REQUIRE(table_);
    }

    NvsWriteQueue& q() { return q_; }
    const FakeNvs& nvs() const { return nvs_; }

private:
    static void* alignedTo(std::vector<unsigned char>& v)
    {
        uintptr_t p = (uintptr_t)v.data();
        const uintptr_t mask = NvsWriteQueue::kTableAlign - 1;
        if (p & mask) p = (p & ~mask) + NvsWriteQueue::kTableAlign;
        return (void*)p;
    }

    std::vector<unsigned char> store_;
    FakeNvs& nvs_;
    void* table_;
    NvsWriteQueue q_;
};

} // namespace

TEST_CASE("a change is not a write until its spell of quiet has passed")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 1'000'000;

    REQUIRE(qq.q().put("cfg_audio", "out_vol", NvsKind::U8, 60));
    REQUIRE(qq.q().pending() == 1);
    REQUIRE(qq.q().flushDue() == 0);          // same instant: nothing has waited yet
    REQUIRE(nvs.ops.empty());

    g_now += NvsWriteQueue::kQuietUs - 1;
    REQUIRE_FALSE(qq.q().hasDue());
    REQUIRE(qq.q().flushDue() == 0);

    g_now += 1;
    REQUIRE(qq.q().hasDue());
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.ops.size() == 1);
    REQUIRE(nvs.ops[0].value == 60);
    REQUIRE(nvs.opened.size() == 1);
    REQUIRE(nvs.closed.size() == 1);
    REQUIRE(qq.q().pending() == 0);
}

TEST_CASE("a whole drag is one write, and the value it ends on is the one that lands")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;

    // The drawer's output slider, ~30 steps a second of dragging.
    for (int step = 0; step < 40; ++step) {
        qq.q().put("cfg_audio", "ovc01", NvsKind::U8, (uint8_t)step);
        g_now += 33'000;                     // 30 Hz
    }
    REQUIRE(qq.q().pending() == 1);
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.count("ovc01") == 1);
    REQUIRE(nvs.last("ovc01")->value == 39);
    // 40 saves, one write: 39 fewer trips to flash than the setter used to make.
    REQUIRE(qq.q().stats().coalesced == 39);
    REQUIRE(qq.q().stats().written == 1);
}

TEST_CASE("saving a settings blob nobody touched writes nothing at all")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;
    const char* keys[] = { "brightness", "theme_clr", "home_view", "mute", "spk_en" };

    for (int round = 0; round < 5; ++round) {
        for (const char* k : keys) qq.q().put("cfg_audio", k, NvsKind::U8, 7);
        g_now += NvsWriteQueue::kQuietUs;
        qq.q().flushDue();
    }
    // Round 1 writes all five; the four later rounds change nothing, so they
    // write nothing. That is the ~90-key saveTo() of an untouched blob.
    REQUIRE(nvs.ops.size() == 5);
    REQUIRE(qq.q().stats().unchanged == 20);
    REQUIRE(qq.q().stats().written == 5);
    REQUIRE(qq.q().stats().refused == 0);
}

TEST_CASE("the second change to a key that is already on flash is written too")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 8);
    g_now = 0;
    qq.q().put("cfg_audio", "out_vol", NvsKind::U8, 40);
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.ops[0].value == 40);
    REQUIRE(qq.q().pending() == 0);

    // The key now sits in the table as "already on flash". Moving it again has to
    // make it owed again — a queue that only ever wrote a key the first time it
    // saw a change is a queue that silently stops saving the settings.
    g_now += 1'000'000;
    qq.q().put("cfg_audio", "out_vol", NvsKind::U8, 41);
    REQUIRE(qq.q().pending() == 1);
    g_now += NvsWriteQueue::kQuietUs - 1;
    REQUIRE_FALSE(qq.q().hasDue());
    g_now += 1;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.count("out_vol") == 2);
    REQUIRE(nvs.last("out_vol")->value == 41);

    // and the spell of quiet counts from this edit, not from the first one ever
    REQUIRE(qq.q().nextDueInUs() == 0);
    qq.q().put("cfg_audio", "out_vol", NvsKind::U8, 42);
    REQUIRE(qq.q().nextDueInUs() == NvsWriteQueue::kQuietUs);
    qq.q().flushAll();
    REQUIRE(nvs.last("out_vol")->value == 42);
}

TEST_CASE("a fader that never stops still reaches flash within the max age")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;
    int flushed = 0;

    // One edit a second for 250 s: the quiet spell never runs out (the key keeps
    // being touched), so it is the max age that answers — a motorised controller
    // streaming CC reaches flash inside maxAge and not at every message.
    for (int i = 0; i < 250; ++i) {
        qq.q().put("cfg_audio", "out_vol", NvsKind::U8, (uint8_t)(i & 0x7F));
        g_now += 1'000'000;                       // one second
        if (qq.q().hasDue()) flushed += (int)qq.q().flushDue();
    }
    REQUIRE(flushed == 2);                        // ~120 s and ~240 s, not 250
    REQUIRE(nvs.count("out_vol") == 2);
    // 250 edits, and only three of them found the key not already waiting (the
    // first, and the first after each of the two writes): 247 writes not done.
    REQUIRE(qq.q().stats().coalesced == 247);
}

TEST_CASE("a read answers what was asked for, not what is on flash yet")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;

    qq.q().put("cfg_audio", "ovc10", NvsKind::U8, 30);
    int32_t v = 0;
    REQUIRE(qq.q().peek("cfg_audio", "ovc10", NvsKind::U8, &v));
    REQUIRE(v == 30);

    qq.q().put("cfg_audio", "ovc10", NvsKind::U8, 31);
    REQUIRE(qq.q().peek("cfg_audio", "ovc10", NvsKind::U8, &v));
    REQUIRE(v == 31);                      // the newest edit, mid-cooldown

    // A key the queue never saw is not the queue's to answer.
    REQUIRE_FALSE(qq.q().peek("cfg_audio", "never_seen", NvsKind::U8, &v));
    // And neither is a type it does not hold that way: an i16 key read as u8
    // would be a different NVS type, so the caller must go to flash.
    qq.q().put("audio_in", "g0", NvsKind::I16, -125);
    REQUIRE(qq.q().peek("audio_in", "g0", NvsKind::I16, &v));
    REQUIRE(v == -125);
    REQUIRE_FALSE(qq.q().peek("audio_in", "g0", NvsKind::U8, &v));
    // A bool and a u8 are the same NVS type, so they answer each other.
    qq.q().put("cfg_audio", "mute", NvsKind::Bool, 1);
    REQUIRE(qq.q().peek("cfg_audio", "mute", NvsKind::U8, &v));
    REQUIRE(v == 1);
}

TEST_CASE("namespaces are batched: one handle and one commit for a group of keys")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;
    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 10);
    qq.q().put("cfg_audio", "ovc01", NvsKind::U8, 20);
    qq.q().put("cfg_display", "brightness", NvsKind::U8, 55);
    qq.q().put("audio_out", "m1", NvsKind::Bool, 1);
    g_now += NvsWriteQueue::kQuietUs;

    REQUIRE(qq.q().flushDue() == 4);
    REQUIRE(nvs.opened.size() == 3);       // three namespaces, three handles
    REQUIRE(nvs.closed.size() == 3);
    REQUIRE(nvs.ops.size() == 4);
    // Every write carries the handle it was written through: the two cfg_audio
    // keys share one, the other two namespaces each have their own, and no key
    // was written outside its own namespace's handle.
    int audioHandle = -1, displayHandle = -1, outHandle = -1;
    for (const FakeNvs::Op& o : nvs.ops) {
        REQUIRE(o.ns == openedOf(&nvs, o.handle));
        if (o.ns == "cfg_audio") {
            if (audioHandle < 0) audioHandle = o.handle;
            REQUIRE(o.handle == audioHandle);
        } else if (o.ns == "cfg_display") {
            displayHandle = o.handle;
        } else {
            outHandle = o.handle;
        }
    }
    REQUIRE(displayHandle != audioHandle);
    REQUIRE(outHandle != audioHandle);
    REQUIRE(outHandle != displayHandle);
}

TEST_CASE("an erase behind the queue's back is not answered by the queue")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;
    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 42);
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);       // it is on flash, and the queue remembers

    // SETTINGS_RESET erases the namespace. If the queue still claimed the value,
    // the next save of 42 would be skipped as "already there" — a board erased
    // and then quietly told the old value back.
    int32_t v = 0;
    REQUIRE(qq.q().peek("cfg_audio", "ovc00", NvsKind::U8, &v));
    qq.q().forget("cfg_audio");
    REQUIRE_FALSE(qq.q().peek("cfg_audio", "ovc00", NvsKind::U8, &v));
    REQUIRE(qq.q().pending() == 0);

    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 42);   // the same value again
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);       // and it is written, because it has to be
    REQUIRE(nvs.count("ovc00") == 2);

    // Forgetting one key leaves the rest of the namespace alone.
    qq.q().put("cfg_audio", "ovc01", NvsKind::U8, 7);
    qq.q().put("cfg_audio", "mute", NvsKind::Bool, 0);
    g_now += NvsWriteQueue::kQuietUs;
    qq.q().flushDue();
    qq.q().forget("cfg_audio", "ovc01");
    REQUIRE(qq.q().peek("cfg_audio", "mute", NvsKind::Bool, &v));
    REQUIRE_FALSE(qq.q().peek("cfg_audio", "ovc01", NvsKind::U8, &v));
}

TEST_CASE("a refused write stays pending and is tried again")
{
    FakeNvs nvs;
    nvs.fail.push_back("ovc00");
    QueueOn qq(nvs, 16);
    g_now = 0;
    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 66);
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 0);       // the sink said no
    REQUIRE(qq.q().stats().errors == 1);
    REQUIRE(qq.q().pending() == 1);        // and the queue did not claim it landed
    int32_t v = 0;
    REQUIRE(qq.q().peek("cfg_audio", "ovc00", NvsKind::U8, &v));
    REQUIRE(v == 66);                      // a read still answers what was asked

    nvs.fail.clear();
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.last("ovc00")->value == 66);

    // A namespace that cannot be opened at all costs no entries and no claims.
    nvs.openFails = true;
    qq.q().put("cfg_audio", "ovc01", NvsKind::U8, 5);
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 0);
    REQUIRE(qq.q().pending() == 1);
    nvs.openFails = false;
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
}

TEST_CASE("a change while its own write is on the way is not lost or mislabelled")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;
    qq.q().put("cfg_audio", "out_vol", NvsKind::U8, 50);

    // During the write of the first value, the host (or a finger) sets 60.
    FakeNvs::Reroute rr;
    rr.afterWrite = 1;
    rr.slot = &qq.q();
    rr.fn = [](NvsWriteQueue& q) { q.put("cfg_audio", "out_vol", NvsKind::U8, 60); };
    nvs.reroute = &rr;

    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.last("out_vol")->value == 50);   // what was in flight did land
    REQUIRE(qq.q().pending() == 1);              // and 60 is still owed
    int32_t v = 0;
    REQUIRE(qq.q().peek("cfg_audio", "out_vol", NvsKind::U8, &v));
    REQUIRE(v == 60);

    nvs.reroute = nullptr;
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.last("out_vol")->value == 60);
    REQUIRE(qq.q().pending() == 0);
}

TEST_CASE("a full table drops a value already on flash, never one still owed")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 3);
    g_now = 0;
    REQUIRE(qq.q().put("cfg_display", "a", NvsKind::U8, 1));
    REQUIRE(qq.q().put("cfg_display", "b", NvsKind::U8, 2));
    REQUIRE(qq.q().put("cfg_display", "c", NvsKind::U8, 3));
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 3);       // all three are on flash now: clean
    REQUIRE(qq.q().stats().high == 3);

    // A fourth value, and only clean entries to make room from: the oldest goes.
    g_now += 1'000;
    REQUIRE(qq.q().put("cfg_display", "d", NvsKind::U8, 4));
    REQUIRE(qq.q().stats().evictions == 1);
    REQUIRE(qq.q().stats().refused == 0);
    REQUIRE(qq.q().pending() == 1);
    int32_t v = 0;
    REQUIRE_FALSE(qq.q().peek("cfg_display", "a", NvsKind::U8, &v));   // the one dropped
    REQUIRE(qq.q().peek("cfg_display", "c", NvsKind::U8, &v));         // the rest stay
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.last("d")->value == 4);

    // With every entry dirty and owed there is nothing safe to drop: the put is
    // refused and counted rather than losing someone else's value.
    FakeNvs nvs2;
    QueueOn full(nvs2, 3);
    g_now = 0;
    for (const char* k : {"x", "y", "z"}) REQUIRE(full.q().put("cfg_audio", k, NvsKind::U8, 1));
    REQUIRE_FALSE(full.q().put("cfg_audio", "w", NvsKind::U8, 1));
    REQUIRE(full.q().stats().refused == 1);
    REQUIRE(full.q().pending() == 3);
    REQUIRE(nvs2.ops.empty());
    full.q().flushAll();                   // and once the room is back, it fits
    REQUIRE(full.q().put("cfg_audio", "w", NvsKind::U8, 1));
    REQUIRE(full.q().stats().refused == 1);
}

TEST_CASE("pending() counts what is owed, and does not drift")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 8);
    g_now = 0;
    REQUIRE(qq.q().pending() == 0);

    // three new keys: three owed
    qq.q().put("cfg_audio", "a", NvsKind::U8, 1);
    qq.q().put("cfg_audio", "b", NvsKind::U8, 2);
    qq.q().put("cfg_audio", "c", NvsKind::U8, 3);
    REQUIRE(qq.q().pending() == 3);
    // changed again: still three, not six
    qq.q().put("cfg_audio", "a", NvsKind::U8, 9);
    REQUIRE(qq.q().pending() == 3);
    // unchanged: nothing owed
    qq.q().put("cfg_audio", "b", NvsKind::U8, 2);
    REQUIRE(qq.q().pending() == 3);

    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 3);
    REQUIRE(qq.q().pending() == 0);

    // a clean key edited again is owed again
    qq.q().put("cfg_audio", "a", NvsKind::U8, 10);
    REQUIRE(qq.q().pending() == 1);

    // forgetting drops it from the owed count too
    qq.q().forget("cfg_audio", "a");
    REQUIRE(qq.q().pending() == 0);
    // and forgetting a whole namespace with several owed keys clears them all
    qq.q().put("cfg_audio", "x", NvsKind::U8, 1);
    qq.q().put("cfg_audio", "y", NvsKind::U8, 1);
    REQUIRE(qq.q().pending() == 2);
    qq.q().forget("cfg_audio");
    REQUIRE(qq.q().pending() == 0);
    REQUIRE(qq.q().nextDueInUs() == 0);

    // an entry dropped by eviction was on flash, so it was not owed: the count
    // must not go down (and must not go negative).
    QueueOn small(nvs, 2);
    g_now += 10'000'000;
    small.q().put("cfg_display", "p", NvsKind::U8, 1);
    small.q().put("cfg_display", "q", NvsKind::U8, 1);
    g_now += NvsWriteQueue::kQuietUs;
    small.q().flushDue();
    REQUIRE(small.q().pending() == 0);
    small.q().put("cfg_display", "r", NvsKind::U8, 5);   // evicts one clean entry
    REQUIRE(small.q().stats().evictions == 1);
    REQUIRE(small.q().pending() == 1);
    g_now += NvsWriteQueue::kQuietUs;
    small.q().flushDue();
    REQUIRE(small.q().pending() == 0);
}

TEST_CASE("the settings blob's own shape: ~150 keys in one namespace")
{
    // The board's real saveTo() asks for about 150 keys every time it is called
    // (92 lines in CrosspadSettings.cpp, several of them loops, plus stm.save()).
    // This is that shape: the queue must hold it without thrashing the table,
    // and the second save of an untouched blob must write nothing.
    FakeNvs nvs;
    QueueOn qq(nvs, 256);
    g_now = 0;
    char key[16];
    for (int i = 0; i < 150; ++i) {
        snprintf(key, sizeof(key), "k%03d", i);
        qq.q().put("cfg_audio", key, NvsKind::U8, (int16_t)i);
    }
    REQUIRE(qq.q().pending() == 150);
    REQUIRE(qq.q().stats().high == 150);
    REQUIRE(qq.q().stats().evictions == 0);

    // One pass takes the batch, not the whole blob: this is why the store drains
    // in a loop instead of trusting a single call.
    g_now += NvsWriteQueue::kQuietUs;
    unsigned wrote = qq.q().flushDue(), passes = 1;
    REQUIRE(wrote > 0);
    REQUIRE(wrote <= 96);
    while (passes++ < 40) {
        const unsigned n = qq.q().flushDue();
        wrote += n;
        if (!n) break;
    }
    REQUIRE(wrote == 150);
    REQUIRE(qq.q().pending() == 0);

    for (int i = 0; i < 150; ++i) {
        snprintf(key, sizeof(key), "k%03d", i);
        qq.q().put("cfg_audio", key, NvsKind::U8, (int16_t)i);
    }
    REQUIRE(qq.q().pending() == 0);
    REQUIRE(qq.q().flushDue() == 0);
    // Two handles for the whole blob, not 150: its 150 keys came out in two
    // passes (the batch is 96), and a pass opens its namespace once, commits,
    // and closes. The untouched save below added a third at most — check the
    // count did not move.
    REQUIRE(nvs.opened.size() == 2);
    REQUIRE(nvs.closed.size() == 2);
    REQUIRE(nvs.ops.size() == 150);           // ...and nothing written since
}

TEST_CASE("ns and key longer than the entry's room are refused, not silently truncated")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 4);
    g_now = 0;
    // NVS itself caps a namespace and a key at 15 characters, which is what the
    // entry's buffers are sized for. A longer one must be a refusal, not a key
    // that quietly became another key (a truncated one would be written under a
    // name nobody asked for).
    REQUIRE(qq.q().put("cfg_abcdefghijk", "abcdefghijklmno", NvsKind::U8, 9));   // 15 and 15: fits
    REQUIRE_FALSE(qq.q().put("cfg_abcdefghijkl", "ok", NvsKind::U8, 1));          // 16-char ns
    REQUIRE_FALSE(qq.q().put("cfg_audio", "abcdefghijklmnop", NvsKind::U8, 1));   // 16-char key
    REQUIRE(qq.q().stats().refused == 2);
    g_now += NvsWriteQueue::kQuietUs;
    REQUIRE(qq.q().flushDue() == 1);
    REQUIRE(nvs.opened[0] == "cfg_abcdefghijk");
    REQUIRE(nvs.ops[0].key == "abcdefghijklmno");
    REQUIRE(nvs.ops[0].value == 9);
}

TEST_CASE("flushAll writes what flushDue would make wait, and a name is only a name")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 16);
    g_now = 0;
    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 1);
    qq.q().put("cfg_stm", "audio_en", NvsKind::U8, 0);
    // No time has passed at all — a restart does not get to wait for the quiet
    // period, so everything pending goes now.
    REQUIRE(qq.q().flushDue() == 0);
    REQUIRE(qq.q().flushAll() == 2);
    REQUIRE(nvs.ops.size() == 2);
    REQUIRE(qq.q().pending() == 0);
    REQUIRE(nvs.opened.size() == 2);
    // Same keys again: the values match, so nothing is owed.
    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 1);
    qq.q().put("cfg_stm", "audio_en", NvsKind::U8, 0);
    REQUIRE(qq.q().flushAll() == 0);
    REQUIRE(qq.q().pending() == 0);
}

TEST_CASE("nextDueInUs tells an owner how long it may sleep")
{
    FakeNvs nvs;
    QueueOn qq(nvs, 8);
    g_now = 10'000'000;
    REQUIRE(qq.q().nextDueInUs() == 0);
    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 3);
    REQUIRE(qq.q().nextDueInUs() == NvsWriteQueue::kQuietUs);
    g_now += 2'000'000;
    REQUIRE(qq.q().nextDueInUs() == NvsWriteQueue::kQuietUs - 2'000'000);
    qq.q().put("cfg_audio", "ovc00", NvsKind::U8, 4);   // touched again: the spell restarts
    REQUIRE(qq.q().nextDueInUs() == NvsWriteQueue::kQuietUs);
    // but the max age never moves, and it is what answers for a stream of edits
    g_now += NvsWriteQueue::kQuietUs - 1;
    REQUIRE(qq.q().nextDueInUs() == 1);
}
