// SPDX-License-Identifier: GPL-3.0-or-later
//
// NvsWriteQueue — see include/NvsWriteQueue.hpp for the rules this implements.
// Pure C++: no ESP-IDF, no FreeRTOS, so main/tests can run it against a fake
// clock and a recording sink. The board's sink and its restart hook live in
// NvsSettingsStore.cpp.

#include "NvsWriteQueue.hpp"

#include <cstring>
#include <new>

namespace crosspad {
namespace {

constexpr size_t kNsLen = NvsWriteQueue::kNsRoom;
constexpr size_t kKeyLen = NvsWriteQueue::kKeyRoom;

bool fits(const char* s, size_t room)
{
    return s && std::strlen(s) < room;
}

/// Copy a namespace or key into the entry's room. Not strncpy: with both sides
/// an array of the same size, GCC's -Werror=stringop-truncation rightly refuses
/// to prove the copy is not cut, and here the source is always a name that
/// already fits (put() checked). Copy the bytes up to the NUL, then the NUL.
void copyName(char* dst, size_t room, const char* src)
{
    size_t n = 0;
    while (n + 1 < room && src[n]) ++n;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

/// NVS stores a bool as a u8, so those two are one type on flash and one entry
/// answers both — which is what the store's readU8 of a key written by saveBool
/// does without the queue. I16 and I32 are their own types: reading one as the
/// other is a type mismatch on flash, and the queue must not hide that.
bool sameNvsType(NvsKind a, NvsKind b)
{
    if (a == b) return true;
    const bool bothBytes = (a == NvsKind::Bool || a == NvsKind::U8) &&
                           (b == NvsKind::Bool || b == NvsKind::U8);
    return bothBytes;
}

} // namespace

NvsWriteQueue::NvsWriteQueue(void* table, size_t capacity, ClockFn clock, void* clockCtx,
                             const NvsQueueSink& sink)
    : capacity_(!table || !clock || !sink.write ? 0 : capacity),   // unusable: hold nothing, refuse everything
      slots_(static_cast<Entry*>(table)),
      clock_(clock), clockCtx_(clockCtx), sink_(sink)
{
    if (capacity_)
        tags_ = reinterpret_cast<uint32_t*>(static_cast<char*>(table) + capacity_ * sizeof(Entry));
    for (size_t i = 0; i < capacity_; ++i) {
        slots_[i].state = kEmpty;
        tags_[i] = 0;
    }
}

/// FNV-1a over "ns/key". Never 0: an empty entry is marked by a 0 tag, so a name
/// that hashed to 0 gets 1 instead (a collision with the marker, not a wrong
/// answer — the key comparison behind it is what decides).
uint32_t NvsWriteQueue::tagOf(const char* ns, const char* key)
{
    uint32_t h = 2166136261u;
    for (const char* s = ns; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    h ^= '/'; h *= 16777619u;
    for (const char* s = key; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h ? h : 1u;
}

NvsWriteQueue::Entry* NvsWriteQueue::findTagged(uint32_t tag, const char* ns, const char* key) const
{
    if (!slots_) return nullptr;
    for (size_t i = 0; i < capacity_; ++i) {
        if (tags_[i] != tag) continue;
        Entry& e = slots_[i];
        if (e.state == kEmpty) continue;
        if (std::strncmp(e.ns, ns, kNsLen) == 0 && std::strncmp(e.key, key, kKeyLen) == 0)
            return &e;
    }
    return nullptr;
}

NvsWriteQueue::Entry* NvsWriteQueue::find(const char* ns, const char* key) const
{
    if (!ns || !key) return nullptr;
    return findTagged(tagOf(ns, key), ns, key);
}

NvsWriteQueue::Entry* NvsWriteQueue::insert(const char* ns, const char* key, NvsKind kind,
                                            uint32_t tag)
{
    if (count_ >= capacity_) {
        // Room for one more, the safe way: drop the oldest entry that is
        // already on flash. Its value is not lost — the caller's settings
        // struct holds it and a later change writes it again, once.
        Entry* oldest = nullptr;
        size_t oldestAt = 0;
        for (size_t i = 0; i < capacity_; ++i) {
            Entry& e = slots_[i];
            if (e.state != kClean) continue;
            if (!oldest || e.changedUs < oldest->changedUs) { oldest = &e; oldestAt = i; }
        }
        if (!oldest) {
            stats_.refused++;
            return nullptr;
        }
        oldest->state = kEmpty;
        tags_[oldestAt] = 0;
        count_--;
        stats_.evictions++;
    }
    for (size_t i = 0; i < capacity_; ++i) {
        Entry& e = slots_[i];
        if (e.state != kEmpty) continue;
        copyName(e.ns, kNsLen, ns);
        copyName(e.key, kKeyLen, key);
        tags_[i] = tag;
        e.kind = kind;
        e.state = kDirty;
        e.value = 0;
        e.firstUs = e.changedUs = 0;
        e.gen++;
        count_++;
        pending_++;
        if (count_ > stats_.high) stats_.high = count_;
        if (pending_ > stats_.pendingHigh) stats_.pendingHigh = pending_;
        return &e;
    }
    stats_.refused++;
    return nullptr;
}

bool NvsWriteQueue::dueNow(const Entry& e, uint64_t now) const
{
    if (now >= e.firstUs && now - e.firstUs >= kMaxAgeUs) return true;
    return now >= e.changedUs && now - e.changedUs >= kQuietUs;
}

unsigned NvsWriteQueue::pending() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return pending_;
}

bool NvsWriteQueue::put(const char* ns, const char* key, NvsKind kind, int32_t value)
{
    std::lock_guard<std::mutex> lock(mtx_);
    stats_.puts++;
    if (!fits(ns, kNsLen) || !fits(key, kKeyLen)) {
        stats_.refused++;
        return false;
    }
    const uint32_t tag = tagOf(ns, key);
    Entry* e = findTagged(tag, ns, key);
    const uint64_t now = clock_(clockCtx_);
    if (!e) e = insert(ns, key, kind, tag);
    if (!e) return false;

    if (e->state != kInFlight && sameNvsType(e->kind, kind) && e->value == value) {
        // The value asked for is the value the queue already has — which is
        // what a settings save of an untouched blob is made of. No write, now
        // or later.
        stats_.unchanged++;
        return true;
    }
    if (e->state == kDirty) stats_.coalesced++;   // an edit already waiting: a write not done
    // One NVS type per key: two callers that disagree about it (a key written as
    // a bool and asked for as an int32) get the type the key already has, not a
    // flash write that would be refused as a type mismatch and retried forever.
    if (sameNvsType(e->kind, kind)) e->kind = kind;
    const State was = e->state;
    e->value = value;
    e->gen++;
    e->changedUs = now;
    if (was != kDirty) {
        e->firstUs = now;             // a new waiting period starts here
        if (was != kInFlight) pending_++;   // clean or fresh: it is owed now
    }
    e->state = kDirty;
    // Whether it was a clean entry (already on flash) or one still being
    // written, this value is now owed: kInFlight is left to the flush that is
    // running, which sees a different gen/value and will not clear it.
    if (pending_ > stats_.pendingHigh) stats_.pendingHigh = pending_;
    return true;
}

bool NvsWriteQueue::peek(const char* ns, const char* key, NvsKind kind, int32_t* out) const
{
    if (!out || !ns || !key) return false;
    std::lock_guard<std::mutex> lock(mtx_);
    const Entry* e = find(ns, key);
    if (!e || !sameNvsType(e->kind, kind)) return false;   // not ours, or not this type: the caller reads flash
    *out = e->value;
    return true;
}

void NvsWriteQueue::forget(const char* ns, const char* key)
{
    // flushMtx_ first, and held across the whole thing: an erase must not run
    // between a flush taking its entries and that flush recording them as
    // written, or the erase's namespaces would come back to life afterwards.
    std::lock_guard<std::mutex> flushLock(flushMtx_);
    std::lock_guard<std::mutex> lock(mtx_);
    if (!slots_ || !ns) return;
    for (size_t i = 0; i < capacity_; ++i) {
        Entry& e = slots_[i];
        if (e.state == kEmpty) continue;
        if (std::strncmp(e.ns, ns, kNsLen) != 0) continue;
        if (key && std::strncmp(e.key, key, kKeyLen) != 0) continue;
        if (e.state == kDirty || e.state == kInFlight) pending_--;
        e.state = kEmpty;
        tags_[i] = 0;
        count_--;
    }
}

unsigned NvsWriteQueue::drain(bool all)
{
    if (!slots_ || !sink_.write) return 0;
    std::lock_guard<std::mutex> flushLock(flushMtx_);   // the members below belong to one flush at a time

    unsigned nNs = 0, nItems = 0;

    const uint64_t takeUs = clock_(clockCtx_);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (size_t i = 0; i < capacity_ && nItems < kMaxBatch; ++i) {
            Entry& e = slots_[i];
            if (e.state != kDirty) continue;
            if (!all && !dueNow(e, takeUs)) continue;
            // The namespace has to be one this pass can batch. A namespace that
            // did not make the (short) list yet stays owed and is taken on the
            // next pass — which is why an owner that wants everything drains in
            // a loop rather than trusting one call.
            int at = -1;
            for (unsigned g = 0; g < nNs; ++g) {
                if (std::strncmp(nsList_[g], e.ns, kNsRoom) == 0) { at = (int)g; break; }
            }
            if (at < 0) {
                if (nNs >= kMaxNsPerFlush) continue;
                at = (int)nNs;
                copyName(nsList_[at], kNsRoom, e.ns);
                ++nNs;
            }
            Item& it = items_[nItems++];
            it.slot = &e;
            it.gen = e.gen;
            it.kind = e.kind;
            it.value = e.value;
            it.ok = false;
            e.state = kInFlight;
        }
    }

    unsigned wrote = 0, failed = 0;
    for (unsigned g = 0; g < nNs; ++g) {
        // One handle, one commit for every value of this namespace that came
        // out: the point of the batch is that saveTo() spans ~15 namespaces,
        // not its ~150 keys.
        bool any = false;
        for (unsigned i = 0; i < nItems && !any; ++i)
            any = std::strncmp(items_[i].slot->ns, nsList_[g], kNsRoom) == 0;
        if (!any) continue;
        const bool opened = sink_.open ? sink_.open(sink_.ctx, nsList_[g]) : false;
        if (opened) stats_.flushes++;
        for (unsigned i = 0; i < nItems; ++i) {
            if (std::strncmp(items_[i].slot->ns, nsList_[g], kNsRoom) != 0) continue;
            items_[i].ok = opened && sink_.write(sink_.ctx, nsList_[g], items_[i].kind,
                                                 items_[i].slot->key, items_[i].value);
            if (items_[i].ok) wrote++;
            else failed++;
        }
        if (sink_.close) sink_.close(sink_.ctx, nsList_[g]);
    }

    const uint64_t doneUs = clock_(clockCtx_);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (unsigned i = 0; i < nItems; ++i) {
            Entry& e = *items_[i].slot;
            if (e.state != kInFlight) continue;   // edited or erased since; leave it
            if (e.gen != items_[i].gen || e.value != items_[i].value ||
                e.kind != items_[i].kind) {
                // Changed while the write ran: keep it pending, and start its
                // max-age over so a stream of edits cannot pin it here.
                e.state = kDirty;
                e.firstUs = doneUs;
                continue;
            }
            if (!items_[i].ok) {
                e.state = kDirty;
                e.firstUs = doneUs;      // retry from now, not from the first attempt
            } else {
                e.state = kClean;
                e.changedUs = doneUs;
                pending_--;              // it is on flash: no longer owed
            }
        }
        stats_.written += wrote;
        stats_.errors += failed;
        if (wrote || failed) {
            stats_.lastWrote = wrote;
            stats_.lastFlushUs = doneUs;
        }
    }
    return wrote;
}

unsigned NvsWriteQueue::flushDue() { return drain(false); }
unsigned NvsWriteQueue::flushAll() { return drain(true); }

bool NvsWriteQueue::hasDue() const
{
    if (!slots_) return false;
    std::lock_guard<std::mutex> lock(mtx_);
    const uint64_t now = clock_(clockCtx_);
    for (size_t i = 0; i < capacity_; ++i) {
        const Entry& e = slots_[i];
        if (e.state == kDirty && dueNow(e, now)) return true;
    }
    return false;
}

uint64_t NvsWriteQueue::nextDueInUs() const
{
    if (!slots_) return 0;
    std::lock_guard<std::mutex> lock(mtx_);
    const uint64_t now = clock_(clockCtx_);
    uint64_t soonest = UINT64_MAX;
    for (size_t i = 0; i < capacity_; ++i) {
        const Entry& e = slots_[i];
        if (e.state != kDirty) continue;
        uint64_t at = e.changedUs + kQuietUs;
        if (e.firstUs + kMaxAgeUs < at) at = e.firstUs + kMaxAgeUs;
        const uint64_t left = at > now ? at - now : 0;
        if (left < soonest) soonest = left;
    }
    return soonest == UINT64_MAX ? 0 : soonest;
}

} // namespace crosspad
