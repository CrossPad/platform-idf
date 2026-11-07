#include "include/reset_log.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_core_dump.h"

/* The summary API only exists for an ELF dump kept in flash; a revision
 * built without one (v1) still records the reset ring, just never a dump. */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#define RSTLOG_COREDUMP 1
#else
#define RSTLOG_COREDUMP 0
#endif

static bool coredump_present(void)
{
#if RSTLOG_COREDUMP
    return esp_core_dump_image_check() == ESP_OK;
#else
    return false;
#endif
}
#include "nvs.h"

#include "stm32_i2c.h"
#include "crosspad/stm32/reg_map.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/event/EventData.hpp"

static const char *TAG = "ResetLog";

#define RSTLOG_NS        "rstlog"
#define RSTLOG_KEY_RING  "ring"
#define RSTLOG_KEY_ALIVE "alive"
#define RSTLOG_KEY_CDSEQ "cd_seq"
#define RSTLOG_ENTRIES   8
#define RSTLOG_RING_VER  1

struct __attribute__((packed)) rstlog_entry_t {
    uint16_t seq;
    uint8_t  reason;          /* esp_reset_reason_t */
    uint8_t  stm_last_code;   /* REG_ESP_RST_LAST_CODE (ROM rst:0x..), 0xFF = STM unreadable */
    uint16_t stm_total;       /* REG_ESP_RST_TOTAL — banners the STM has parsed since its boot */
    uint16_t stm_wd_total;    /* REG_ESP_RST_WD_TOTAL — resets the STM watchdog forced */
    uint8_t  stm_wd_dead;
    uint8_t  coredump;        /* a core dump was found on this boot and belongs to this reset */
    uint16_t prev_uptime_min; /* how long the previous run lasted (5 min granularity) */
    char     prev_app[16];    /* app running when the previous run ended, "-" = launcher */
};

struct __attribute__((packed)) rstlog_ring_t {
    uint8_t  ver;
    uint8_t  count;                     /* entries ever written, stops at 0xFF */
    uint16_t next_seq;
    rstlog_entry_t e[RSTLOG_ENTRIES];   /* entry seq s lives in slot_of(s) */
};

struct __attribute__((packed)) rstlog_alive_t {
    uint16_t uptime_min;
    char     app[16];
};

static rstlog_ring_t  s_ring;
static rstlog_alive_t s_alive;
static uint16_t       s_cd_seq;          /* entry that owns the stored core dump, 0 = none */
static esp_timer_handle_t s_alive_timer;

/* Where entry @p seq lives. By its sequence, not by count: count stops at
 * 0xFF, and from then on count % N named the same slot on every boot -- each
 * new entry overwrote e[7] and e[6] was reported as the newest for good.
 * seq is a uint16_t and 65536 is a multiple of N, so the slots stay in step
 * across its wrap. A ring saturated under the old rule is out of order until
 * N boots have rewritten it: whoever reads an entry checks its seq. */
static rstlog_entry_t *slot_of(uint16_t seq)
{
    return &s_ring.e[(uint16_t)(seq - 1) % RSTLOG_ENTRIES];
}

static const char *reason_name(uint8_t r)
{
    switch ((esp_reset_reason_t)r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        case ESP_RST_EFUSE:     return "EFUSE";
        case ESP_RST_PWR_GLITCH:return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP:return "CPU_LOCKUP";
        default:                return "UNKNOWN";
    }
}

static bool nvs_get_blob_exact(nvs_handle_t h, const char *key, void *dst, size_t len)
{
    size_t got = len;
    return nvs_get_blob(h, key, dst, &got) == ESP_OK && got == len;
}

static void alive_store(void)
{
    nvs_handle_t h;
    if (nvs_open(RSTLOG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, RSTLOG_KEY_ALIVE, &s_alive, sizeof(s_alive));
    nvs_commit(h);
    nvs_close(h);
}

static void alive_tick(void *)
{
    uint16_t min = (uint16_t)(esp_timer_get_time() / 60000000LL);
    /* Coarse on purpose: one NVS write per 5 min keeps wear negligible, and
     * "died after ~35 min in Sampler" is all the ring needs to say. */
    if (min == 1 || (min % 5) == 0) {
        if (min != s_alive.uptime_min) {
            s_alive.uptime_min = min;
            alive_store();
        }
    }
}

static void on_app_changed(const void *data, crosspad::EventType, void *)
{
    auto *d = static_cast<const crosspad::AppChangedData *>(data);
    const char *name = (d && d->appName[0]) ? d->appName : "-";
    if (strncmp(s_alive.app, name, sizeof(s_alive.app)) == 0) return;
    strncpy(s_alive.app, name, sizeof(s_alive.app) - 1);
    s_alive.app[sizeof(s_alive.app) - 1] = '\0';
    alive_store();
}

static void read_stm(rstlog_entry_t *e)
{
    uint8_t blk[REG_ESP_RST_LEN];
    e->stm_last_code = 0xFF;
    if (stm32_i2c_read(REG_ESP_RST_BASE, blk, sizeof(blk)) != ESP_OK) return;
    e->stm_last_code = blk[REG_ESP_RST_LAST_CODE - REG_ESP_RST_BASE];
    e->stm_total     = (uint16_t)((blk[REG_ESP_RST_TOTAL_HI - REG_ESP_RST_BASE] << 8) |
                                   blk[REG_ESP_RST_TOTAL_LO - REG_ESP_RST_BASE]);
    e->stm_wd_total  = (uint16_t)((blk[REG_ESP_RST_WD_TOTAL_HI - REG_ESP_RST_BASE] << 8) |
                                   blk[REG_ESP_RST_WD_TOTAL_LO - REG_ESP_RST_BASE]);
    e->stm_wd_dead   = blk[REG_ESP_RST_WD_DEAD - REG_ESP_RST_BASE];
}

void reset_log_init(void)
{
    nvs_handle_t h;
    if (nvs_open(RSTLOG_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable — reset log disabled");
        return;
    }
    if (!nvs_get_blob_exact(h, RSTLOG_KEY_RING, &s_ring, sizeof(s_ring)) || s_ring.ver != RSTLOG_RING_VER) {
        memset(&s_ring, 0, sizeof(s_ring));
        s_ring.ver = RSTLOG_RING_VER;
        s_ring.next_seq = 1;
    }
    rstlog_alive_t prev = {};
    if (!nvs_get_blob_exact(h, RSTLOG_KEY_ALIVE, &prev, sizeof(prev))) {
        prev.uptime_min = 0;
        strcpy(prev.app, "?");
    }
    if (nvs_get_u16(h, RSTLOG_KEY_CDSEQ, &s_cd_seq) != ESP_OK) s_cd_seq = 0;

    rstlog_entry_t e = {};
    e.seq    = s_ring.next_seq++;
    e.reason = (uint8_t)esp_reset_reason();
    read_stm(&e);
    e.prev_uptime_min = prev.uptime_min;
    memcpy(e.prev_app, prev.app, sizeof(e.prev_app));
    e.prev_app[sizeof(e.prev_app) - 1] = '\0';

    /* A crash-class reset always rewrites the dump, so it is ours; any other
     * reset leaves an older dump with the entry that earned it (or, for a
     * dump nobody has claimed yet, with this one). */
    if (coredump_present()) {
        bool crash = e.reason == ESP_RST_PANIC || e.reason == ESP_RST_TASK_WDT ||
                     e.reason == ESP_RST_INT_WDT || e.reason == ESP_RST_WDT ||
                     e.reason == ESP_RST_CPU_LOCKUP;
        if (crash || s_cd_seq == 0) {
            e.coredump = 1;
            s_cd_seq = e.seq;
            nvs_set_u16(h, RSTLOG_KEY_CDSEQ, s_cd_seq);
        }
    }

    *slot_of(e.seq) = e;
    if (s_ring.count < 0xFF) s_ring.count++;
    nvs_set_blob(h, RSTLOG_KEY_RING, &s_ring, sizeof(s_ring));

    s_alive.uptime_min = 0;
    strcpy(s_alive.app, "-");
    nvs_set_blob(h, RSTLOG_KEY_ALIVE, &s_alive, sizeof(s_alive));
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "#%u reason=%s stm_last=0x%02X stm_wd=%u%s prev=%um in %s%s",
             e.seq, reason_name(e.reason), e.stm_last_code, e.stm_wd_total,
             e.stm_wd_dead ? "(dead)" : "", e.prev_uptime_min, e.prev_app,
             e.coredump ? " COREDUMP" : "");

    crosspad::getEventBus().subscribe(crosspad::EventType::AppChanged, on_app_changed, nullptr);

    const esp_timer_create_args_t args = {
        .callback = alive_tick,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rstlog_alive",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_alive_timer) == ESP_OK) {
        esp_timer_start_periodic(s_alive_timer, 60ULL * 1000000ULL);
    }
}

size_t reset_log_stm_format(char *out, size_t cap)
{
    rstlog_entry_t e = {};
    read_stm(&e);
    if (e.stm_last_code == 0xFF) return snprintf(out, cap, "RSTSTM: unreadable\r\n");
    return snprintf(out, cap, "RSTSTM: last=0x%02X total=%u wd=%u dead=%u\r\n",
                    e.stm_last_code, e.stm_total, e.stm_wd_total, e.stm_wd_dead);
}

size_t reset_log_format(char *out, size_t cap)
{
    size_t n = snprintf(out, cap, "RSTLOG: n=%u coredump_seq=%u\r\n", s_ring.count, s_cd_seq);
    uint8_t shown = s_ring.count < RSTLOG_ENTRIES ? s_ring.count : RSTLOG_ENTRIES;
    for (uint8_t i = 0; i < shown && n < cap; i++) {
        const rstlog_entry_t &e = *slot_of((uint16_t)(s_ring.next_seq - 1 - i));
        if (e.seq != (uint16_t)(s_ring.next_seq - 1 - i)) break;   /* the old rule's leftovers */
        n += snprintf(out + n, cap - n,
                      " #%u %s stm_last=0x%02X stm_total=%u stm_wd=%u%s prev=%um app=%s%s\r\n",
                      e.seq, reason_name(e.reason), e.stm_last_code, e.stm_total,
                      e.stm_wd_total, e.stm_wd_dead ? "(dead)" : "",
                      e.prev_uptime_min, e.prev_app, e.coredump ? " COREDUMP" : "");
    }
    return n < cap ? n : cap - 1;
}

size_t reset_log_coredump_format(char *out, size_t cap)
{
    if (!coredump_present()) {
        return snprintf(out, cap, "COREDUMP: none\r\n");
    }
#if RSTLOG_COREDUMP
    esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)calloc(1, sizeof(*s));
    if (!s) return snprintf(out, cap, "COREDUMP: present (no mem for summary)\r\n");
    size_t n;
    if (esp_core_dump_get_summary(s) != ESP_OK) {
        n = snprintf(out, cap, "COREDUMP: present, summary unreadable\r\n");
    } else {
        n = snprintf(out, cap, "COREDUMP: seq=%u task=%s pc=0x%08" PRIx32 " cause=%" PRIu32
                     " vaddr=0x%08" PRIx32 " depth=%" PRIu32 "%s\r\n bt:",
                     s_cd_seq, s->exc_task, s->exc_pc, s->ex_info.exc_cause,
                     s->ex_info.exc_vaddr, s->exc_bt_info.depth,
                     s->exc_bt_info.corrupted ? " (corrupted)" : "");
        for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16 && n < cap; i++) {
            n += snprintf(out + n, cap - n, " 0x%08" PRIx32, s->exc_bt_info.bt[i]);
        }
        /* pc above is the frame's PC - 3 (esp_cpu_process_stack_pc(), made for
         * return addresses); after an exception epc1 = pc + 3 is the faulting
         * instruction itself (stale after an abort or a watchdog). a0..a3 are
         * the frame's, elf the crashed image's ELF SHA prefix (it need not be
         * the running one: a crash on a trial boot rolls back). */
        s->app_elf_sha256[sizeof(s->app_elf_sha256) - 1] = '\0';
        if (n < cap) {
            n += snprintf(out + n, cap - n,
                          "\r\n exc: tcb=0x%08" PRIx32 " a0=0x%08" PRIx32 " a1=0x%08" PRIx32
                          " a2=0x%08" PRIx32 " a3=0x%08" PRIx32 " epc1=0x%08" PRIx32
                          " elf=%s\r\n",
                          s->exc_tcb, s->ex_info.exc_a[0], s->ex_info.exc_a[1],
                          s->ex_info.exc_a[2], s->ex_info.exc_a[3], s->ex_info.epcx[0],
                          (const char *)s->app_elf_sha256);
        }
    }
    free(s);
    return n < cap ? n : cap - 1;
#else
    return snprintf(out, cap, "COREDUMP: none\r\n");
#endif
}

void reset_log_clear(void)
{
    memset(&s_ring, 0, sizeof(s_ring));
    s_ring.ver = RSTLOG_RING_VER;
    s_ring.next_seq = 1;
    s_cd_seq = 0;
#if RSTLOG_COREDUMP
    esp_core_dump_image_erase();
#endif
    nvs_handle_t h;
    if (nvs_open(RSTLOG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, RSTLOG_KEY_RING, &s_ring, sizeof(s_ring));
    nvs_erase_key(h, RSTLOG_KEY_CDSEQ);
    nvs_commit(h);
    nvs_close(h);
}
