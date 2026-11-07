/* SD_BENCH: how the card feeds many streaming voices at once.
 *
 * N files read in turn, a piece of each, the way a streamer refills N voices:
 * MB/s over the run, and each read's time -- average, 99th percentile and the
 * worst, which is what a voice's buffer has to outlast. Two destinations:
 *   psram  the piece read straight into PSRAM, as a buffer there would be
 *          (the S3's SDMMC cannot DMA to PSRAM: the driver then goes through
 *          a bounce buffer of its own)
 *   stage  through one internal, DMA-capable, 64-byte aligned buffer of
 *          kStageBytes, copied on to PSRAM -- the board's 4 kB stage, larger
 *   port   what sample_port_pread() does for a voice's refill: the file
 *          rewound to 0 and sought to the voice's place (its data from byte
 *          44, as a WAV's), then pieces that end on 4 kB of the file through a
 *          4 kB internal DMA stage. Without fast seek the seek walks the
 *          cluster chain from the file's first cluster every time
 * Files are /sdcard/crosspad/.sdbench/fNN.bin (SD_BENCH PREP writes them), read
 * with POSIX read() on descriptors opened once, O_RDONLY -- the case FatFs's
 * fast seek (CONFIG_FATFS_USE_FASTSEEK) builds its cluster table for at open.
 *
 * Its own task, PSRAM stack: card I/O never runs on the CDC loop. Rows arrive
 * as they are measured: "SDBENCH: ..." then "SDBENCH: done". */

#include "sd_bench.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

extern void sample_player_port_flush_files(void);

namespace {

void (*s_reply)(const char *) = nullptr;
void cdc_reply(const char *s) { if (s_reply) s_reply(s); }

constexpr const char *kDir = "/sdcard/crosspad/.sdbench";
constexpr int      kMaxFiles   = 20;
/* 8 kB, not 16: internal DMA memory is short on this board (~40 kB free, the
 * largest block ~20 kB, 5 kB at its low), and a 16 kB stage under 16 open
 * files left USB without its buffers -- the board fell off the bus. */
constexpr uint32_t kStageBytes = 8 * 1024;
constexpr uint32_t kMaxChunk   = 64 * 1024;
constexpr uint32_t kMaxTimes   = 8192;     // reads kept for the percentile, a run

enum class Job : uint8_t { Prep, Run };
enum class Mode : uint8_t { Psram, Stage, Port };
constexpr uint32_t kPortStage = 4096;
constexpr uint32_t kWavData   = 44;

struct Request {
    Job      job = Job::Run;
    int      files = 8;
    uint32_t chunk = 16 * 1024;
    Mode     mode = Mode::Psram;
    uint32_t ms = 3000;
    uint32_t fileMb = 2;
};

std::atomic<bool> s_busy{false};
Request s_req;

void row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void row(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    std::strcat(line, "\r\n");
    cdc_reply(line);
}

void path_of(char *out, size_t cap, int i) { std::snprintf(out, cap, "%s/f%02d.bin", kDir, i); }

/* kMaxFiles files of fileMb MB, written through the internal stage: a write
 * from PSRAM would go a sector at a time and take minutes. */
void prep(const Request &r)
{
    mkdir("/sdcard/crosspad", 0775);
    mkdir(kDir, 0775);
    auto *buf = (uint8_t *)heap_caps_aligned_alloc(64, kStageBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) { row("SDBENCH: ERR no internal DMA buffer"); return; }
    for (uint32_t k = 0; k < kStageBytes; ++k) buf[k] = (uint8_t)(k * 31u + 7u);
    const int64_t t0 = esp_timer_get_time();
    uint64_t written = 0;
    for (int i = 0; i < kMaxFiles; ++i) {
        char path[64];
        path_of(path, sizeof(path), i);
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
        if (fd < 0) { row("SDBENCH: ERR open %s errno=%d", path, errno); break; }
        const uint64_t size = (uint64_t)r.fileMb * 1024 * 1024;
        for (uint64_t at = 0; at < size; at += kStageBytes) {
            if (write(fd, buf, kStageBytes) != (ssize_t)kStageBytes) { row("SDBENCH: ERR write %s", path); break; }
            written += kStageBytes;
        }
        close(fd);
        vTaskDelay(1);
    }
    heap_caps_free(buf);
    const double s = (esp_timer_get_time() - t0) / 1e6;
    row("SDBENCH: prep files=%d mb=%u write_mbps=%.2f", kMaxFiles, (unsigned)r.fileMb,
        s > 0 ? written / 1048576.0 / s : 0.0);
}

void run(const Request &r)
{
    /* The streamer's cached handles go first: it keeps up to 18 open, and
     * the card's VFS holds 32 -- twenty files of ours did not fit (ENFILE).
     * Run with the song stopped; the cache fills again on the next refill. */
    sample_player_port_flush_files();
    int fd[kMaxFiles];
    for (int i = 0; i < r.files; ++i) {
        char path[64];
        path_of(path, sizeof(path), i);
        fd[i] = open(path, O_RDONLY);
        if (fd[i] < 0) {
            row("SDBENCH: ERR open %s errno=%d (SD_BENCH PREP first)", path, errno);
            for (int k = 0; k < i; ++k) close(fd[k]);
            return;
        }
    }
    auto *dst = (uint8_t *)heap_caps_aligned_alloc(64, kMaxChunk, MALLOC_CAP_SPIRAM);
    const uint32_t stageBytes = r.mode == Mode::Port ? kPortStage : kStageBytes;
    auto *stage = r.mode != Mode::Psram
                      ? (uint8_t *)heap_caps_aligned_alloc(64, stageBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                      : nullptr;
    uint32_t pos[kMaxFiles];
    for (int k = 0; k < kMaxFiles; ++k) pos[k] = kWavData;
    auto *times = (uint32_t *)heap_caps_malloc(kMaxTimes * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!dst || !times || (r.mode != Mode::Psram && !stage)) {
        row("SDBENCH: ERR no memory");
    } else {
        uint64_t bytes = 0, sum = 0;
        uint32_t worst = 0, reads = 0, wraps = 0;
        const int64_t start = esp_timer_get_time();
        const int64_t end = start + (int64_t)r.ms * 1000;
        int i = 0;
        while (esp_timer_get_time() < end) {
            const int64_t t0 = esp_timer_get_time();
            uint32_t got = 0;
            if (r.mode == Mode::Psram) {
                const ssize_t n = read(fd[i], dst, r.chunk);
                got = n > 0 ? (uint32_t)n : 0;
            } else if (r.mode == Mode::Stage) {
                while (got < r.chunk) {
                    const uint32_t want = std::min(kStageBytes, r.chunk - got);
                    const ssize_t n = read(fd[i], stage, want);
                    if (n <= 0) break;
                    std::memcpy(dst + got, stage, (size_t)n);
                    got += (uint32_t)n;
                    if ((uint32_t)n < want) break;
                }
            } else {
                lseek(fd[i], 0, SEEK_SET);              // the port's reuse of a cached handle
                lseek(fd[i], pos[i], SEEK_SET);
                while (got < r.chunk) {
                    const uint32_t at = pos[i] + got;
                    const uint32_t want = std::min(kPortStage - at % kPortStage, r.chunk - got);
                    const ssize_t n = read(fd[i], stage, want);
                    if (n <= 0) break;
                    std::memcpy(dst + got, stage, (size_t)n);
                    got += (uint32_t)n;
                    if ((uint32_t)n < want) break;
                }
                pos[i] += got;
            }
            const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
            if (got < r.chunk) {
                lseek(fd[i], 0, SEEK_SET);      // round again; the short read still counts
                pos[i] = kWavData;
                ++wraps;
            }
            bytes += got;
            sum += dt;
            worst = std::max(worst, dt);
            if (reads < kMaxTimes) times[reads] = dt;
            ++reads;
            i = (i + 1) % r.files;
        }
        const double s = (esp_timer_get_time() - start) / 1e6;
        const uint32_t kept = std::min(reads, kMaxTimes);
        std::sort(times, times + kept);
        const uint32_t p99 = kept ? times[std::min(kept - 1, (uint32_t)(kept * 0.99))] : 0;
        row("SDBENCH: mode=%s files=%d kb=%u mbps=%.2f avg_us=%u p99_us=%u worst_us=%u reads=%u wraps=%u",
            r.mode == Mode::Port ? "port" : r.mode == Mode::Stage ? "stage" : "psram", r.files, (unsigned)(r.chunk / 1024),
            s > 0 ? bytes / 1048576.0 / s : 0.0, reads ? (unsigned)(sum / reads) : 0u, (unsigned)p99,
            (unsigned)worst, (unsigned)reads, (unsigned)wraps);
    }
    heap_caps_free(times);
    heap_caps_free(stage);
    heap_caps_free(dst);
    for (int k = 0; k < r.files; ++k) close(fd[k]);
}

void task(void *)
{
    const Request r = s_req;
    if (r.job == Job::Prep) prep(r);
    else run(r);
    row("SDBENCH: done");
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

/* SD_BENCH PREP [mb]                       write the files
 * SD_BENCH <files> <kb> [psram|stage|port] [ms]  one run */
bool sd_bench_verb(const char *cmd, void (*reply)(const char *))
{
    if (std::strncmp(cmd, "SD_BENCH", 8) != 0 || (cmd[8] && cmd[8] != ' ')) return false;
    s_reply = reply;
    Request r;
    unsigned mb = 2, kb = 16, ms = 3000;
    int files = 8;
    char mode[8] = "psram";
    if (std::strncmp(cmd, "SD_BENCH PREP", 13) == 0) {
        std::sscanf(cmd, "SD_BENCH PREP %u", &mb);
        r.job = Job::Prep;
        r.fileMb = mb < 1 ? 1 : mb > 16 ? 16 : mb;
    } else if (std::sscanf(cmd, "SD_BENCH %d %u %7s %u", &files, &kb, mode, &ms) >= 2) {
        if (files < 1 || files > kMaxFiles || kb < 1 || kb * 1024 > kMaxChunk || ms < 200 || ms > 60000) {
            cdc_reply("SDBENCH: ERR files 1..20, kb 1..64, ms 200..60000\r\n");
            return true;
        }
        r.files = files;
        r.chunk = kb * 1024;
        r.mode = std::strcmp(mode, "stage") == 0 ? Mode::Stage
               : std::strcmp(mode, "port") == 0  ? Mode::Port
                                                 : Mode::Psram;
        r.ms = ms;
    } else {
        cdc_reply("SDBENCH: ERR usage SD_BENCH PREP [mb] | SD_BENCH files kb [psram|stage|port] [ms]\r\n");
        return true;
    }
    if (s_busy.exchange(true)) { cdc_reply("SDBENCH: ERR busy\r\n"); return true; }
    s_req = r;
    if (xTaskCreatePinnedToCoreWithCaps(task, "sd_bench", 8192, nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy.store(false);
        cdc_reply("SDBENCH: ERR no task\r\n");
        return true;
    }
    cdc_reply("SDBENCH: OK\r\n");
    return true;
}
