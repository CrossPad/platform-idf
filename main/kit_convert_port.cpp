// SPDX-License-Identifier: GPL-3.0-or-later
#include "kit_convert_port.h"

#include "engine_arena.hpp"
#include "lv_async_safe.h"
#include "psram_json_allocator.h"
#include "SamplerEnginePort.hpp"

#include "crosspad-sampler/logic/kit_convert_task.hpp"
#include "crosspad/dsp/FftFactory.hpp"
#include "crosspad/dsp/PortableFft.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/kit/KitRateConvert.hpp"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

/* main/sample_player_port.cpp */
extern uint8_t *sample_player_port_stage_take(bool urgent);
extern bool sample_player_port_direct_io(void);
extern void sample_player_port_stage_give(void);
extern void sample_player_port_yield_to_streamer(uint32_t weight);
extern void sample_player_port_forget_file(const char *path);

namespace {

const char *TAG = "KitConvert";

/* PSRAM the UI keeps whatever a conversion would like, when the arena has
 * nothing to lend: LVGL aborts on a failed allocation, a conversion only
 * reports "not enough memory". */
constexpr size_t kUiReserve = 96 * 1024;
constexpr uint32_t kStage = 4096;
constexpr int kMaxBlocks = 8;          // the converter holds five at most

struct Block { void *p; size_t n; bool lent; };

class IdfKitConvertHost : public crosspad::IKitConvertHost {
public:
    /* The engine arena's slack first -- it is not the UI's -- else the
     * general heap above the UI's reserve. What was lent has to be back
     * before the arena is reset: the reclaim hook stops the conversion and
     * waits for that (reclaim()). */
    void *alloc(size_t n) override {
        ensure_reclaim();
        int slot = -1;
        for (int i = 0; i < kMaxBlocks && slot < 0; ++i) if (!blocks_[i].p) slot = i;
        if (slot < 0) return nullptr;
        bool lent = true;
        void *p = engine_arena_lend(n);
        if (!p) {
            lent = false;
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < n + kUiReserve) return nullptr;
            p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        }
        if (!p) return nullptr;
        blocks_[slot] = {p, n, lent};
        if (lent) lentBytes_.fetch_add(n);
        live_ += n;
        if (live_ > peak_) peak_ = live_;
        if (lent) ++lentCount_;
        else ++heapCount_;
        return p;
    }

    void release(void *p) override {
        if (!p) return;
        for (auto &b : blocks_) {
            if (b.p != p) continue;
            live_ -= b.n;
            if (b.lent) lentBytes_.fetch_sub(b.n);
            b = {};
            break;
        }
        engine_arena_free(p);       // the arena's or the general heap's: it knows which
    }

    crosspad::IFft &fft() override {
        if (crosspad::IFft *f = crosspad::currentFft()) return *f;   // esp-dsp, if the pitched loader made it
        if (!portable_) portable_ = new crosspad::PortableFft(crosspad::kConvertFftMax);
        return *portable_;
    }

    void endJob() override {
        delete portable_;           // its twiddle tables are PSRAM nobody needs between conversions
        portable_ = nullptr;
    }

    /* Through the board's one DMA-capable stage: a piece read or written
     * straight into PSRAM is a CMD17 per sector through sdmmc's bounce. Around
     * stdio, so the FILE is put back where the descriptor ended. */
    size_t readIo(FILE *f, void *dst, size_t bytes) override {
        if (bytes > kStage || std::fflush(f) != 0) return fallbackRead(f, dst, bytes);
        const int fd = fileno(f);
        const off_t pos = lseek(fd, 0, SEEK_CUR);
        if (pos >= 0 && sample_player_port_direct_io()) {   // the sector layer bounces
            const ssize_t n = read(fd, dst, bytes);
            std::fseek(f, pos + (n > 0 ? n : 0), SEEK_SET);
            return n > 0 ? (size_t)n : 0;
        }
        uint8_t *stage = pos >= 0 ? sample_player_port_stage_take(false) : nullptr;
        if (!stage) return fallbackRead(f, dst, bytes);
        const ssize_t n = read(fd, stage, bytes);
        if (n > 0) std::memcpy(dst, stage, (size_t)n);
        sample_player_port_stage_give();
        std::fseek(f, pos + (n > 0 ? n : 0), SEEK_SET);
        return n > 0 ? (size_t)n : 0;
    }

    bool writeIo(FILE *f, const void *src, size_t bytes) override {
        if (bytes > kStage || std::fflush(f) != 0) return fallbackWrite(f, src, bytes);
        const int fd = fileno(f);
        const off_t pos = lseek(fd, 0, SEEK_CUR);
        if (pos >= 0 && sample_player_port_direct_io()) {   // the sector layer bounces
            const bool ok = write(fd, src, bytes) == (ssize_t)bytes;
            std::fseek(f, pos + (ok ? (off_t)bytes : 0), SEEK_SET);
            return ok;
        }
        uint8_t *stage = pos >= 0 ? sample_player_port_stage_take(false) : nullptr;
        if (!stage) return fallbackWrite(f, src, bytes);
        std::memcpy(stage, src, bytes);
        const bool ok = write(fd, stage, bytes) == (ssize_t)bytes;
        sample_player_port_stage_give();
        std::fseek(f, pos + (ok ? (off_t)bytes : 0), SEEK_SET);
        return ok;
    }

    bool syncFile(FILE *f) override { return std::fflush(f) == 0 && fsync(fileno(f)) == 0; }

    void giveWay() override { sample_player_port_yield_to_streamer(1); }

    uint64_t freeBytes(const char *fsDir) override {
        static const char kCard[] = "/sdcard";
        if (strncmp(fsDir, kCard, sizeof(kCard) - 1) != 0) return UINT64_MAX;
        uint64_t total = 0, free = 0;
        return esp_vfs_fat_info(kCard, &total, &free) == ESP_OK ? free : UINT64_MAX;
    }

    ArduinoJson::Allocator *jsonAllocator() override { return PsramJsonAllocator::instance(); }

    /* Not while a sample of the store reads it: a voice of the outgoing kit
     * streams its tail from the original until it ends, and a removed file is
     * a voice cut short. */
    bool releaseFile(const char *fsPath) override {
        if (native_sampler_reads(fsPath)) return false;
        sample_player_port_forget_file(fsPath);
        return true;
    }

    size_t peak() const { return peak_; }
    size_t live() const { return live_; }
    uint32_t lentCount() const { return lentCount_; }
    uint32_t heapCount() const { return heapCount_; }
    uint32_t fallbacks() const { return fallbacks_; }
    void resetPeak() { peak_ = live_; lentCount_ = heapCount_ = fallbacks_ = 0; }

private:
    size_t fallbackRead(FILE *f, void *dst, size_t bytes) {
        ++fallbacks_;
        return std::fread(dst, 1, bytes, f);
    }
    bool fallbackWrite(FILE *f, const void *src, size_t bytes) {
        ++fallbacks_;
        return std::fwrite(src, 1, bytes, f) == bytes;
    }

    /* engine_arena_reset()'s borrower hook: give back what was lent, now. The
     * converter's buffers are in use for the whole of a file, so the
     * conversion is stopped (it resumes later from what it finished) and
     * this waits for the worker to drop them -- one step, a few ms of card
     * I/O. Unbounded, because a reset over memory still being written is
     * corruption; the worker waits on nothing the resetting thread holds. */
    static void reclaim(void *ctx) {
        auto *self = static_cast<IdfKitConvertHost *>(ctx);
        if (self->lentBytes_.load() == 0) return;
        ESP_LOGW(TAG, "arena reset: stopping the conversion to give back %u B", (unsigned)self->lentBytes_.load());
        crosspad_sampler::kit_convert_cancel();
        for (uint32_t waited = 0; self->lentBytes_.load() != 0; waited += 10) {
            if (waited && waited % 5000 == 0) ESP_LOGW(TAG, "arena reset: still waiting for the conversion");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void ensure_reclaim() {
        if (reclaimSet_) return;
        engine_arena_set_reclaim(reclaim, this);
        reclaimSet_ = true;
    }

    Block blocks_[kMaxBlocks] = {};
    std::atomic<size_t> lentBytes_{0};
    size_t live_ = 0, peak_ = 0;
    uint32_t lentCount_ = 0, heapCount_ = 0, fallbacks_ = 0;
    crosspad::PortableFft *portable_ = nullptr;
    bool reclaimSet_ = false;
};

IdfKitConvertHost s_host;

/* The verbs' requests, carried to the LVGL thread. */
int s_reqKit = -1;
uint32_t s_reqRate = 0;
bool s_reqKeep = false;
std::atomic<int> s_reqResult{0};    // 1 accepted, -1 refused, 0 none yet

const char *phase_name(crosspad::KitRateConverter::Phase p) {
    using P = crosspad::KitRateConverter::Phase;
    switch (p) {
    case P::Idle: return "idle";
    case P::Planning: return "planning";
    case P::Converting: return "converting";
    case P::Committing: return "committing";
    case P::Done: return "done";
    case P::Failed: return "failed";
    case P::Cancelled: return "cancelled";
    }
    return "?";
}

const char *job_name(crosspad_sampler::KitConvertJob j) {
    using J = crosspad_sampler::KitConvertJob;
    switch (j) {
    case J::Convert: return "convert";
    case J::Tidy: return "tidy";
    default: return "none";
    }
}

/* A name in quotes, for a reply read apart by spaces. */
void quoted(const char *in, char *out, size_t cap) {
    size_t n = 0;
    if (cap < 3) { if (cap) out[0] = '\0'; return; }
    out[n++] = '"';
    for (; *in && n + 3 < cap; ++in) {
        if (*in == '"' || *in == '\\') out[n++] = '\\';
        out[n++] = *in;
    }
    out[n++] = '"';
    out[n] = '\0';
}

const char *args_of(const char *cmd, const char *verb) {
    const size_t n = strlen(verb);
    if (strncmp(cmd, verb, n) != 0 || (cmd[n] != '\0' && cmd[n] != ' ')) return nullptr;
    return cmd + n;
}

} // namespace

crosspad::IKitConvertHost *kit_convert_host(void) { return &s_host; }

bool kit_convert_verbs_handle(const char *cmd, void (*reply)(const char *text)) {
    const char *a;
    /* Static: a reply is built on the 4 kB main task, and these are long;
     * PSRAM, as the other verbs' reply buffers. */
    EXT_RAM_BSS_ATTR static char out[512];
    if ((a = args_of(cmd, "KIT_RATES"))) {
        uint16_t samples = 0, off = 0;
        uint32_t rate = 0, er = 0;
        auto *km = crosspad::getKitManager();
        crosspad::KitInfo *kit = km ? km->getCurrentKit() : nullptr;
        if (!native_sampler_rate_report(&samples, &off, &rate, &er)) {
            reply("KITRATES: pending\r\n");
            return true;
        }
        snprintf(out, sizeof(out), "KITRATES: kit=%d samples=%u off=%u rate=%u engine=%u keep=%d\r\n",
                 km ? km->getCurrentKitId() : -1, (unsigned)samples, (unsigned)off, (unsigned)rate,
                 (unsigned)er, kit && kit->keepRate ? 1 : 0);
        reply(out);
        return true;
    }
    if ((a = args_of(cmd, "KIT_CONVERT_STATUS"))) {
        const crosspad_sampler::KitConvertStatus st = crosspad_sampler::kit_convert_status();
        const auto &p = st.progress;
        char cur[64], err[64];
        quoted(p.current, cur, sizeof(cur));
        quoted(p.error, err, sizeof(err));
        const int req = s_reqResult.load();
        snprintf(out, sizeof(out),
                 "KITCONV: job=%s busy=%d kit=%d phase=%s pct=%u file=%u files=%u to_convert=%u converted=%u "
                 "skipped=%u from=%u to=%u frames=%u/%u ms=%u cur=%s err=%s reloaded=%d "
                 "tidy_removed=%u tidy_kept=%u tidy_pending=%u seq=%u req=%s mem_live=%u mem_peak=%u "
                 "lent=%u heap=%u io_fb=%u\r\n",
                 job_name(st.job), st.busy ? 1 : 0, st.kitId, phase_name(p.phase), (unsigned)p.percent(),
                 (unsigned)p.fileIndex, (unsigned)p.files, (unsigned)p.toConvert, (unsigned)p.converted,
                 (unsigned)p.skipped, (unsigned)p.fromRate, (unsigned)p.toRate, (unsigned)p.framesDone,
                 (unsigned)p.framesTotal, (unsigned)st.elapsedMs, cur, err, st.reloaded ? 1 : 0,
                 (unsigned)st.tidy.removed, (unsigned)st.tidy.kept, (unsigned)st.tidy.pending,
                 (unsigned)st.seq, req > 0 ? "ok" : req < 0 ? "refused" : "none", (unsigned)s_host.live(),
                 (unsigned)s_host.peak(), (unsigned)s_host.lentCount(), (unsigned)s_host.heapCount(),
                 (unsigned)s_host.fallbacks());
        reply(out);
        return true;
    }
    if ((a = args_of(cmd, "KIT_CONVERT_STOP"))) {
        crosspad_sampler::kit_convert_cancel();
        reply("OK\r\n");
        return true;
    }
    int kit = -1, v = -1;
    unsigned rate = 0;
    if ((a = args_of(cmd, "KIT_CONVERT")) && sscanf(a, "%d %u", &kit, &rate) >= 1 && kit >= 0) {
        s_reqKit = kit;
        s_reqRate = rate;
        s_reqResult.store(0);
        s_host.resetPeak();
        reply("OK\r\n");     /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked([](void *) {
            s_reqResult.store(crosspad_sampler::kit_convert_start(s_reqKit, s_reqRate) ? 1 : -1);
        }, nullptr);
        return true;
    }
    if ((a = args_of(cmd, "KIT_KEEP_RATE")) && sscanf(a, "%d %d", &kit, &v) == 2 && kit >= 0 && (v == 0 || v == 1)) {
        s_reqKit = kit;
        s_reqKeep = v == 1;
        s_reqResult.store(0);
        reply("OK\r\n");
        lv_async_call_locked([](void *) {
            s_reqResult.store(crosspad_sampler::kit_convert_keep_rate(s_reqKit, s_reqKeep) ? 1 : -1);
        }, nullptr);
        return true;
    }
    if ((a = args_of(cmd, "KIT_CONVERT_TIDY")) && sscanf(a, "%d", &kit) == 1 && kit >= 0) {
        s_reqKit = kit;
        s_reqResult.store(0);
        reply("OK\r\n");
        lv_async_call_locked([](void *) {
            s_reqResult.store(crosspad_sampler::kit_convert_tidy(s_reqKit) ? 1 : -1);
        }, nullptr);
        return true;
    }
    return false;
}
