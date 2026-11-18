/* CDC verbs: memory, tasks, resets, the IMU, the STM32, settings and the card. Dispatch order is in hil_control.cpp. */
#include "sd_sector_io.h"
#include "esp_heap_trace.h"
#include "bsp_worker.h"
#include "include/hil_control.h"
#include "include/usb_transfer.h"
#include "include/net_mode.h"
#include "include/net_mode_screen.h"
#include "include/fw_library.h"
#include "include/ota_cdc.h"
#include "crosspad/app/AppVersions.hpp"
#include "include/crosspad_deps.h"
#include "include/reset_log.h"
#include "stm32_fw_update.h"
#include "stm32_i2c.h"

#include "tinyusb.h"
#include "include/lv_async_safe.h"
#include "crosspad-gui/components/settings_ui.h"
#include "include/fw_updater.h"
#include "include/cp_account.h"
#include "include/wifi_creds.h"

#include "gui.h"
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad-mixer/MixerPresets.hpp>
#define HIL_HAS_MIXER 1
#endif
#include "CrosspadPlatform.hpp"
#include "imu_midi.h"
#include "NvsSettingsStore.hpp"
#include "bsp_sdmmc.h"
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
#include "crosspad-dawcontrol/DawControlState.hpp"
#endif

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "ml_smpl_stream_ply.h"
#include "MarcelSampleNode.hpp"
#endif
#include "bsp_ulp.h"
#include "performance_monitor.h"

#include "hil_verbs.h"

/* Whether a load is in flight and what waits behind it. KIT_LIST's current=
 * cannot tell "loading" from "the request went nowhere". */
/* Settings backup for the companion: the same JSON file the Settings → Backup
 * page writes. The work runs on the LVGL thread (an import applies brightness,
 * LEDs, vibration and the STM pad config through the GUI platform) and reports
 * through atomics like KIT_LOAD/KIT_STATUS, so the 5 kB main stack never
 * parses JSON. One operation at a time; SETTINGS_STATUS is the result. */
EXT_RAM_BSS_ATTR static char s_settings_path[96];
static std::atomic<bool> s_settings_busy{false};
static std::atomic<int> s_settings_ok{-1};              /* -1 never ran, 0 failed, 1 ok */
static std::atomic<const char *> s_settings_error{nullptr};
static std::atomic<char> s_settings_op{'-'};            /* 'e' export, 'i' import */

static void async_settings_backup(void *)
{
    const char *err = nullptr;
    const bool ok = s_settings_op.load() == 'e'
        ? crosspad_gui::settings_ui_backup_export(s_settings_path, &err)
        : crosspad_gui::settings_ui_backup_import(s_settings_path, &err);
    s_settings_error.store(err);
    s_settings_ok.store(ok ? 1 : 0);
    s_settings_busy.store(false);
}

static bool settings_backup_start(char op, const char *arg)
{
    if (s_settings_busy.load()) return false;
    const char *path = *arg ? arg : crosspad_gui::settings_ui_backup_default_path();
    if (!path || strlen(path) >= sizeof(s_settings_path) || strstr(path, "..")
        || (strncmp(path, "/sdcard/", 8) != 0 && strncmp(path, "/spiflash/", 10) != 0)) {
        return false;
    }
    strcpy(s_settings_path, path);
    s_settings_op.store(op);
    s_settings_ok.store(-1);
    s_settings_error.store(nullptr);
    s_settings_busy.store(true);
    return true;   /* the caller replies, then posts async_settings_backup */
}

static void handle_settings_status(void)
{
    char reply[192];
    const char *err = s_settings_error.load();
    snprintf(reply, sizeof(reply), "SETTINGS: busy=%d op=%c ok=%d err=%s path=%s\r\n",
             s_settings_busy.load() ? 1 : 0, s_settings_op.load(), s_settings_ok.load(),
             err ? err : "-", s_settings_path[0] ? s_settings_path : "-");
    cdc_reply(reply);
}

/* SHA-256 of the ELF this image was linked from, the way the build stamps it
 * into the app descriptor. The only thing that tells two builds of the same
 * commit apart: APP_VERSIONS/LIBVER report the commit, and the descriptor's
 * own compile time is not regenerated for every build, so both can read
 * identical while the board runs a different binary — which is exactly what
 * a silent rollback inside a trial window looks like. Compare against
 * `sha256sum build_<rev>/CrossPad.elf`. */
static void handle_app_sha(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char hex[sizeof(desc->app_elf_sha256) * 2 + 1];
    for (size_t i = 0; i < sizeof(desc->app_elf_sha256); i++) {
        snprintf(hex + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
    }
    char reply[96];
    snprintf(reply, sizeof(reply), "APPSHA: %s\r\n", hex);
    cdc_reply(reply);
}

// What went into this binary: which app submodules, at which commit, against
// which pin. Lines up one-to-one with the app manager's `status`.
static void handle_app_versions(void)
{
    char line[160];
    size_t count = crosspad::appVersionCount();
    for (size_t i = 0; i < count; ++i) {
        const crosspad::AppVersion *v = crosspad::appVersionGet(i);
        if (!v) continue;
        snprintf(line, sizeof(line),
                 "APPVER: %s id=%s commit=%s ref=%s dirty=%d\r\n",
                 v->component,
                 v->appId && v->appId[0] ? v->appId : "-",
                 v->commit,
                 v->ref && v->ref[0] ? v->ref : "-",
                 v->dirty ? 1 : 0);
        cdc_reply(line);
    }
    char summary[96];
    snprintf(line, sizeof(line), "LIBVER: %s\r\n", crosspad_deps_summary(summary, sizeof(summary)));
    cdc_reply(line);
    stm32_link_info_t link;
    if (stm32_hal_get_link_info(&link) == ESP_OK && link.linked) {
        snprintf(line, sizeof(line), "BOARD: fw=%s pcb=%u\r\n", BSP_BOARD_REV_NAME, (unsigned)link.pcb_rev);
    } else {
        snprintf(line, sizeof(line), "BOARD: fw=%s pcb=-\r\n", BSP_BOARD_REV_NAME);
    }
    cdc_reply(line);
    snprintf(line, sizeof(line), "APPVER: end count=%u\r\n", (unsigned)count);
    cdc_reply(line);
}

/* Erase the settings namespaces so the next boot takes its defaults.
 *
 * Without this there is no way to check any default on a board that has already
 * saved the key: a default can be read in the source and built into a binary,
 * and still not be *observed* anywhere, because the board already has the key
 * in NVS. A default nobody can observe
 * is a default nobody can trust.
 *
 * CDC only, never wired to the UI, and it wants FORCE spelled out — erasing a
 * board's configuration by a typo is not a thing that should be possible. The
 * reboot is part of the verb: CrosspadSettings::load() already ran, so the
 * living instance still holds the old values until the device comes back. */
static const char *const kSettingsNamespaces[] = {
    "cfg_audio", "cfg_audio_eng", "cfg_display", "cfg_esp_rt", "cfg_imu_midi",
    "cfg_keypad", "cfg_rgb", "cfg_stm_rt", "cfg_system", "cfg_vibrate",
    "cfg_wireless",
};

static void handle_settings_reset(void)
{
    unsigned erased = 0, missing = 0, failed = 0;
    for (const char *ns : kSettingsNamespaces) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND) { ++missing; continue; }
        if (err != ESP_OK) { ++failed; continue; }
        if (nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK) ++erased;
        else ++failed;
        nvs_close(h);
    }
    /* Saved WiFi networks too: a board that changes hands must not keep the
     * owner's password. Through wifi_creds, not a raw erase: under its lock
     * and with its RAM copy, so a save in the reply window (a connect's
     * promote) starts from the empty list instead of writing the old one back. */
    if (wifi_creds_clear()) ++erased;
    else ++failed;
    /* Update settings the same way: the RAM copies would write the old values
     * back from a check that ends in the reply window. */
    if (fw_updater_forget()) ++erased;
    else ++failed;
    /* The crosspad.app link: a board that changes hands must not keep the
     * owner's account. RAM now, NVS on its writer task, done well within the
     * reboot's wait below. */
    if (cp_account_forget()) ++erased;
    else ++failed;
    char reply[96];
    snprintf(reply, sizeof(reply), "SETTINGS_RESET: erased=%u absent=%u failed=%u reboot=1\r\n",
             erased, missing, failed);
    cdc_reply(reply);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* USB transfer mode: the SD card as a USB drive. Entering and leaving are both
 * reboots (usb_transfer.h), so the reply goes out first and the restart waits
 * for it. Deliberately CDC-only — there is no SysEx or BLE way in, because
 * handing a host the storage must need the user at the device. */
static const char *usb_msc_state_name(usb_transfer_state_t state)
{
    switch (state) {
        case USB_TRANSFER_NO_CARD: return "no_card";
        case USB_TRANSFER_WAITING: return "waiting";
        case USB_TRANSFER_MOUNTED: return "mounted";
        case USB_TRANSFER_BUSY:    return "busy";
        case USB_TRANSFER_EJECTED: return "ejected";
        default:                   return "unknown";
    }
}

static void handle_usb_msc(const char *args)
{
    const bool active = usb_transfer_active();
    int on = -1;
    if (sscanf(args, "%d", &on) != 1) {
        usb_transfer_status_t st;
        usb_transfer_get_status(&st);
        /* Outside transfer mode the card is FATFS's, not the MSC layer's, so
         * its presence is the mount's answer — reporting the raw handle there
         * would say "no card" with one sitting in the slot. */
        const bool card = active ? st.cardPresent : status.sdCardDetected;
        int width = 0, khz = 0;
        bsp_sdcard_get_bus(&width, &khz);
        char reply[144];
        snprintf(reply, sizeof(reply),
                 "USB_MSC: active=%d card=%d state=%s size_mb=%llu read_kb=%u write_kb=%u width=%d freq=%d\r\n",
                 active ? 1 : 0, card ? 1 : 0, usb_msc_state_name(st.state),
                 (unsigned long long)(st.cardBytes >> 20),
                 (unsigned)st.readKiB, (unsigned)st.writeKiB, width, khz);
        cdc_reply(reply);
        return;
    }

    if ((on != 0) == active) {
        cdc_reply("OK unchanged\r\n");
        return;
    }
    cdc_reply("OK reboot=1\r\n");
    if (on) usb_transfer_request();
    else    usb_transfer_leave();
}

/* Network mode (net_mode.h): entering and leaving are both restarts, so the
 * reply goes out first and the restart waits for it. CDC-only like USB_MSC —
 * the companion allow-list has no NET_ entry, so there is no SysEx or BLE way
 * in. From transfer mode it is refused: the host may still be writing. */
static constexpr const char *kNetModeUsage = "ERR NET_MODE 0|1 [TAB=wifi|store|updates]\r\n";

static void handle_net_mode(const char *args)
{
    while (*args == ' ') args++;
    const bool active = net_mode_active();
    if (!*args) {
        char reply[64];
        snprintf(reply, sizeof(reply), "NET_MODE: active=%d tab=%s\r\n", active ? 1 : 0,
                 active ? net_tab_name(net_mode_start_tab()) : "-");
        cdc_reply(reply);
        return;
    }
    int on = -1;
    char tabArg[16] = "";
    if (sscanf(args, "%d %15s", &on, tabArg) < 1 || (on != 0 && on != 1)) {
        cdc_reply(kNetModeUsage);
        return;
    }
    net_tab_t tab = NET_TAB_WIFI;
    if (tabArg[0] && (strncmp(tabArg, "TAB=", 4) != 0 || !net_tab_parse(tabArg + 4, &tab))) {
        cdc_reply(kNetModeUsage);
        return;
    }
    if (usb_transfer_active()) { cdc_reply("ERR transfer_mode\r\n"); return; }
    if ((on == 1) == active) { cdc_reply("OK unchanged\r\n"); return; }
    cdc_reply("OK reboot=1\r\n");
    if (on) {
        net_mode_request(tab);
    } else {
        /* The screen lets go as Exit does, on the LVGL thread; the restart does
         * not wait for it, so a stuck UI cannot keep the board in this mode. */
        lv_async_call_locked([](void *) { net_mode_screen_close(); }, nullptr);
        net_mode_leave();
    }
}

/* Raw attitude and rate, because "is the IMU alive" and "is the mapping right"
 * were until now answerable only by the absence of an error line in the console,
 * which is a poor instrument. Values are the ones the mapper is fed. */
static void handle_imu(void)
{
    bsp_imu_data_t d = {};
    char reply[192];
    if (bsp_imu_get_data(&d) != ESP_OK) {
        cdc_reply("IMU: err=1\r\n");
        return;
    }
    snprintf(reply, sizeof(reply),
             "IMU: pitch=%.2f roll=%.2f gyro=%.2f,%.2f,%.2f accel=%.3f,%.3f,%.3f "
             "temp=%.1f midi=%d sent=%u\r\n",
             d.fused_pitch, d.fused_roll, d.gyro_x, d.gyro_y, d.gyro_z,
             d.accel_x, d.accel_y, d.accel_z, d.temp,
             imu_midi_enabled() ? 1 : 0, (unsigned)imu_midi_sent());
    cdc_reply(reply);
    bsp_imu_stats_t st = {};
    bsp_imu_get_stats(&st);
    snprintf(reply, sizeof(reply), "IMU: drdy_line=%d drdy=%u samples=%u read_fail=%u\r\n",
             st.drdy_line ? 1 : 0, (unsigned)st.drdy_pulses, (unsigned)st.samples,
             (unsigned)st.read_failures);
    cdc_reply(reply);
    bsp_imu_event_counts_t ev = {};
    bsp_imu_get_events(&ev);
    snprintf(reply, sizeof(reply),
             "IMU: events single_tap=%u double_tap=%u free_fall=%u wake_up=%u orientation=%u d6d=%u "
             "still=%d still_periods=%u\r\n",
             (unsigned)ev.single_tap, (unsigned)ev.double_tap, (unsigned)ev.free_fall,
             (unsigned)ev.wake_up, (unsigned)ev.orientation, (unsigned)ev.orientation_6d,
             ev.still ? 1 : 0, (unsigned)ev.still_periods);
    cdc_reply(reply);

    snprintf(reply, sizeof(reply), "IMU: tap_gate mutes=%u taps_muted=%u mute_ms_left=%ld\r\n",
             (unsigned)st.tap_mutes, (unsigned)st.taps_muted, (long)st.tap_mute_ms_left);
    cdc_reply(reply);

    auto *cfg = crosspad::CrosspadSettings::getInstance();
    if (!cfg) return;
    for (size_t i = 0; i < crosspad::kImuMidiSlots; ++i) {
        const auto &s = cfg->imuMidi.slots[i];
        snprintf(reply, sizeof(reply),
                 "IMU: slot%u en=%d src=%u tgt=%u cc=%u ch=%u inv=%d span=%d\r\n",
                 (unsigned)i, s.enabled ? 1 : 0, (unsigned)s.source, (unsigned)s.target,
                 (unsigned)s.cc, (unsigned)s.channel, s.invert ? 1 : 0,
                 (int)s.inputSpan);
        cdc_reply(reply);
    }
    /* A trailer, because a reply that grew from one line to five needs a way for
     * the reader to know it has them all — the LIBVER row spent months being
     * dropped for want of exactly this. */
    snprintf(reply, sizeof(reply), "IMU: end slots=%u\r\n",
             (unsigned)crosspad::kImuMidiSlots);
    cdc_reply(reply);
}

/* ── Touch trace ───────────────────────────────────────────────────── */

/* The coordinate stream as the panel reports it, not as LVGL interprets it.
 * Recording is off at boot: the ring is 32 kB of PSRAM and the dump is up to
 * 4096 lines, neither of which any other scenario wants to pay for. */
/* The heap watch: a walk of every heap on the worker, until it finds a broken
 * block once. It is deliberately one-shot -- the first failure is the one with
 * information in it, and a corrupt heap tends to fail every walk after. */
static bool s_heap_watch = false;
static bsp_worker_job_t *s_heap_watch_job = nullptr;

static uint32_t heap_watch_job(void *)
{
    if (!s_heap_watch) return 0;
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE("HEAPCHK", "heap corrupt -- first seen now, so look at what just happened");
        s_heap_watch = false;
    }
    return 0;
}

static bool heap_watch_on(void) { return s_heap_watch; }

static void heap_watch_set(bool on)
{
    s_heap_watch = on;
    if (on && !s_heap_watch_job) {
        /* Two seconds: the walk is milliseconds of holding each heap's lock,
         * and anything faster is a worker overrun for no more information. */
        s_heap_watch_job = bsp_worker_add("heap_chk", 2000, heap_watch_job, nullptr);
    }
}

/* HEAP_TRACE START [records] | STOP | DUMP [min_bytes] [ALL]
 * Standalone heap tracing (CONFIG_HEAP_TRACING_STANDALONE) in LEAKS mode:
 * what was allocated between START and STOP and is still live. DUMP keeps
 * internal-RAM records unless ALL is given, groups them by call stack and prints
 * the groups largest first; the host resolves the PCs with addr2line on the
 * matching ELF. One reply shape for all three: rows only for DUMP, always the
 * trailer. The record buffer lives in PSRAM so the trace itself does not eat
 * what it is measuring. */
static heap_trace_record_t *s_heap_trace_buf = nullptr;
static size_t s_heap_trace_n = 0;
static bool s_heap_trace_on = false;

static void handle_heap_trace(const char *arg)
{
    char reply[160];
    bool dump = false;
    bool with_psram = false;
    size_t min_bytes = 0;
    if (strncmp(arg, "START", 5) == 0) {
        size_t want = (size_t)atoi(arg + 5);
        if (want == 0) want = 768;
        if (s_heap_trace_on) heap_trace_stop();
        if (s_heap_trace_buf && s_heap_trace_n != want) {
            heap_trace_init_standalone(nullptr, 0);
            heap_caps_free(s_heap_trace_buf);
            s_heap_trace_buf = nullptr;
        }
        if (!s_heap_trace_buf) {
            s_heap_trace_buf = (heap_trace_record_t *)heap_caps_calloc(
                want, sizeof(heap_trace_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_heap_trace_buf) { cdc_reply("ERR HEAP_TRACE no_mem\r\n"); return; }
            s_heap_trace_n = want;
            heap_trace_init_standalone(s_heap_trace_buf, s_heap_trace_n);
        }
        const esp_err_t e = heap_trace_start(HEAP_TRACE_LEAKS);
        if (e != ESP_OK) {
            snprintf(reply, sizeof(reply), "ERR HEAP_TRACE start %s\r\n", esp_err_to_name(e));
            cdc_reply(reply);
            return;
        }
        s_heap_trace_on = true;
    } else if (strncmp(arg, "STOP", 4) == 0) {
        if (s_heap_trace_on) heap_trace_stop();
        s_heap_trace_on = false;
    } else if (strncmp(arg, "DUMP", 4) == 0) {
        dump = true;
        min_bytes = (size_t)atoi(arg + 4);
        with_psram = strstr(arg, "ALL") != nullptr;
    } else {
        cdc_reply("ERR HEAP_TRACE START|STOP|DUMP\r\n");
        return;
    }

    const size_t count = s_heap_trace_buf ? heap_trace_get_count() : 0;
    unsigned rows = 0;
    if (dump && count) {
        struct Group { void *pc[CONFIG_HEAP_TRACING_STACK_DEPTH]; uint32_t bytes; uint32_t n; };
        constexpr size_t kGroups = 512;
        Group *g = (Group *)heap_caps_calloc(kGroups, sizeof(Group), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g) { cdc_reply("ERR HEAP_TRACE no_mem\r\n"); return; }
        size_t used = 0;
        uint32_t other_bytes = 0, other_n = 0;
        for (size_t i = 0; i < count; i++) {
            heap_trace_record_t r;
            if (heap_trace_get(i, &r) != ESP_OK || !r.address) continue;
            if (!with_psram && esp_ptr_external_ram(r.address)) continue;
            size_t k = 0;
            for (; k < used; k++) {
                if (memcmp(g[k].pc, r.alloced_by, sizeof(g[k].pc)) == 0) break;
            }
            if (k == used) {
                if (used == kGroups) { other_bytes += r.size; other_n++; continue; }
                memcpy(g[used].pc, r.alloced_by, sizeof(g[used].pc));
                used++;
            }
            g[k].bytes += r.size;
            g[k].n++;
        }
        /* Largest first: a selection sort over at most 512 entries. */
        for (size_t a = 0; a < used; a++) {
            size_t best = a;
            for (size_t b = a + 1; b < used; b++) if (g[b].bytes > g[best].bytes) best = b;
            if (best != a) { Group t = g[a]; g[a] = g[best]; g[best] = t; }
        }
        for (size_t a = 0; a < used; a++) {
            if (g[a].bytes < min_bytes) break;
            int len = snprintf(reply, sizeof(reply), "HEAP_TRACE: bytes=%lu n=%lu pc=",
                               (unsigned long)g[a].bytes, (unsigned long)g[a].n);
            for (size_t d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH && len < (int)sizeof(reply) - 16; d++) {
                len += snprintf(reply + len, sizeof(reply) - len, "%s%p", d ? "," : "", g[a].pc[d]);
            }
            snprintf(reply + len, sizeof(reply) - len, "\r\n");
            cdc_reply(reply);
            rows++;
        }
        if (other_n) {
            snprintf(reply, sizeof(reply), "HEAP_TRACE: bytes=%lu n=%lu pc=other\r\n",
                     (unsigned long)other_bytes, (unsigned long)other_n);
            cdc_reply(reply);
            rows++;
        }
        heap_caps_free(g);
    }
    snprintf(reply, sizeof(reply), "HEAP_TRACE: end n=%u records=%u on=%d\r\n",
             rows, (unsigned)count, s_heap_trace_on ? 1 : 0);
    cdc_reply(reply);
}

/* int_* counts MALLOC_CAP_INTERNAL, which includes ~8 kB of RTC fast memory
 * no DMA can reach; dma_* is the internal RAM a DMA buffer can come from --
 * the SD stage, a fopen's sector buffer, BLE's controller -- and the unit the
 * 63 kB floor is about. */
static void handle_mem(void)
{
    multi_heap_info_t iram, psram, dma;
    heap_caps_get_info(&iram, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);
    heap_caps_get_info(&dma, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    char reply[220];
    snprintf(reply, sizeof(reply),
             "MEM: int_free=%u int_largest=%u int_min=%u int_blocks=%u "
             "psram_free=%u psram_largest=%u psram_blocks=%u dma_free=%u dma_largest=%u dma_min=%u\r\n",
             (unsigned)iram.total_free_bytes, (unsigned)iram.largest_free_block,
             (unsigned)iram.minimum_free_bytes, (unsigned)iram.allocated_blocks,
             (unsigned)psram.total_free_bytes, (unsigned)psram.largest_free_block,
             (unsigned)psram.allocated_blocks, (unsigned)dma.total_free_bytes,
             (unsigned)dma.largest_free_block, (unsigned)dma.minimum_free_bytes);
    cdc_reply(reply);
}

/* Internal-heap size histogram: "18 kB free, largest block 4 kB" is a
 * fragmentation problem, not a shortage, and only this tells them apart. */

struct MemHist {
    static constexpr int kBuckets = 8;   /* <=64,128,256,512,1k,2k,8k,rest */
    uint32_t usedCount[kBuckets] = {};
    uint32_t usedBytes[kBuckets] = {};
    uint32_t freeCount[kBuckets] = {};
    uint32_t freeBytes[kBuckets] = {};
    uint32_t biggestUsed = 0;
    /* The blocks that decide whether a task can still be created. */
    static constexpr int kBigMax = 16;
    uint32_t  bigSize[kBigMax] = {};
    uintptr_t bigAddr[kBigMax] = {};
    int       bigCount = 0;
    bool      psram = false;   /* which memory this histogram is of */
};

static int mem_bucket(size_t size)
{
    if (size <= 64)   return 0;
    if (size <= 128)  return 1;
    if (size <= 256)  return 2;
    if (size <= 512)  return 3;
    if (size <= 1024) return 4;
    if (size <= 2048) return 5;
    if (size <= 8192) return 6;
    return 7;
}

static bool mem_walker(walker_heap_into_t heap, walker_block_info_t block, void *ctx)
{
    /* One memory at a time: PSRAM is mapped at 0x3C000000-0x3DFFFFFF on the
     * S3, internal DRAM sits at 0x3FC88000 and up. */
    auto *h = static_cast<MemHist *>(ctx);
    if (((intptr_t)block.ptr < 0x3F000000) != h->psram) return true;
    int b = mem_bucket(block.size);
    if (block.used) {
        h->usedCount[b]++;
        h->usedBytes[b] += block.size;
        if (block.size > h->biggestUsed) h->biggestUsed = block.size;
        if (block.size >= 8192 && h->bigCount < MemHist::kBigMax) {
            h->bigSize[h->bigCount] = block.size;
            h->bigAddr[h->bigCount] = (uintptr_t)block.ptr;
            h->bigCount++;
        }
    } else {
        h->freeCount[b]++;
        h->freeBytes[b] += block.size;
    }
    (void)heap;
    return true;
}

/* One row per registered worker job; then pad_anim's slowest run since the
 * previous WORKER, split (main.cpp g_padanim_worst; no name=, so a job-table
 * parser skips it); then an end line with the count. */
static void handle_worker(void)
{
    bsp_worker_stat_t st[BSP_WORKER_MAX_JOBS];
    const size_t n = bsp_worker_stats(st, BSP_WORKER_MAX_JOBS);
    char line[160];
    for (size_t i = 0; i < n; i++) {
        snprintf(line, sizeof(line),
                 "WORKER: name=%s period=%lu runs=%lu last_us=%lu max_us=%lu overruns=%lu\r\n",
                 st[i].name, (unsigned long)st[i].period_ms, (unsigned long)st[i].runs,
                 (unsigned long)st[i].last_us, (unsigned long)st[i].max_us,
                 (unsigned long)st[i].overruns);
        cdc_reply(line);
    }
    const uint32_t *w = g_padanim_worst;
    snprintf(line, sizeof(line),
             "WORKER: split job=pad_anim worst_us=%lu frame_us=%lu report_us=%lu stm_us=%lu at_ms=%lu\r\n",
             (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
             (unsigned long)w[4]);
    g_padanim_worst[0] = 0;
    cdc_reply(line);
    snprintf(line, sizeof(line), "WORKER: end n=%u\r\n", (unsigned)n);
    cdc_reply(line);
}

/* MEM_BLOCKS [PSRAM] -- internal RAM by default; PSRAM lists the big
 * blocks that are eating the UI's budget, the engine arena first. */
static void handle_mem_blocks(bool psram)
{
    static const char *kNames[MemHist::kBuckets] = {
        "64", "128", "256", "512", "1k", "2k", "8k", "big"
    };
    MemHist h;
    h.psram = psram;
    heap_caps_walk_all(mem_walker, &h);
    /* Static: 900 B is too much for the main task's stack under the heap walk. */
    EXT_RAM_BSS_ATTR static char reply[900];
    int n = snprintf(reply, sizeof(reply), "MEMBLK: biggest_used=%u",
                     (unsigned)h.biggestUsed);
    for (int i = 0; i < MemHist::kBuckets; ++i) {
        n += snprintf(reply + n, sizeof(reply) - n, " | <=%s u=%u/%uB f=%u/%uB",
                      kNames[i], (unsigned)h.usedCount[i], (unsigned)h.usedBytes[i],
                      (unsigned)h.freeCount[i], (unsigned)h.freeBytes[i]);
    }
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);

    /* Second line: ~512 B per CDC write, and the block list would be cut. */
    n = snprintf(reply, sizeof(reply), "MEMBIG:");
    for (int i = 0; i < h.bigCount && n < (int)sizeof(reply) - 40; ++i) {
        n += snprintf(reply + n, sizeof(reply) - n, " @%08x=%u",
                      (unsigned)h.bigAddr[i], (unsigned)h.bigSize[i]);
    }
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);
}

/* STM_FW_UPDATE: ~10 s on the STM link (stm32_fw_update_ex owns it for the
 * duration). Run where the update overlay runs it -- a task of its own with
 * an internal stack, because the image is read off LittleFS (flash) -- and
 * answered by that task with the same "OK STM_FW" / "ERR STM_FW" line. */
static std::atomic<bool> s_stm_upd_busy{false};
EXT_RAM_BSS_ATTR static char s_stm_upd_path[96];
static bool s_stm_upd_force = false;

static void stm_fw_update_task(void *)
{
    esp_err_t uerr = stm32_fw_update_ex(s_stm_upd_path, s_stm_upd_force, nullptr, nullptr);
    char out[96];
    if (uerr == ESP_OK) {
        stm32_link_info_t li = {};
        stm32_hal_get_link_info(&li);
        snprintf(out, sizeof(out), "OK STM_FW %u.%u/0x%04X\r\n", li.fw_major, li.fw_minor, li.proto_version);
    } else {
        snprintf(out, sizeof(out), "ERR STM_FW %s\r\n", esp_err_to_name(uerr));
    }
    cdc_reply(out);
    s_stm_upd_busy.store(false);
    vTaskDelete(nullptr);
}

/* STM_FW_INFO reads the bundled image's descriptor off LittleFS, which wants
 * more stack than the CDC loop (the 4 kB main task) has left at this depth:
 * the verb overflowed it, a reboot per call. The read runs on a short task
 * with an internal stack (a flash read) while the caller waits, so the reply
 * still leaves from the caller -- the companion captures it there. */
struct StmFwInfoJob {
    stm32_fw_image_info_t img;
    esp_err_t err;
    StaticSemaphore_t doneBuf;
    SemaphoreHandle_t done;
};

static void stm_fw_info_task(void *arg)
{
    auto *job = static_cast<StmFwInfoJob *>(arg);
    job->err = stm32_fw_image_info(STM32_FW_BUNDLED_PATH, &job->img);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

static esp_err_t stm_fw_bundled_info(stm32_fw_image_info_t *img)
{
    StmFwInfoJob job = {};
    job.done = xSemaphoreCreateBinaryStatic(&job.doneBuf);
    if (xTaskCreate(stm_fw_info_task, "stm_fw_info", 4096, &job, 5, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    xSemaphoreTake(job.done, portMAX_DELAY);
    *img = job.img;
    return job.err;
}

bool verb_usb_msc(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "USB_MSC", 7) == 0) {
        handle_usb_msc(cmd + 7);
        return true;
    }
    return false;
}

bool verb_net_mode(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "NET_MODE", 8) == 0 && (cmd[8] == '\0' || cmd[8] == ' ')) {
        handle_net_mode(cmd + 8);
        return true;
    }
    return false;
}

bool verb_tasks(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "TASKS", 5) == 0) {
        /* To the console, not CDC: the table is long and the console survives
         * a starved main loop. Read twice and diff to see who owns a core. */
        perf_print_task_stats();
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_mem_blocks(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "MEM_BLOCKS", 10) == 0) {
        handle_mem_blocks(strstr(cmd, "PSRAM") != nullptr);
        return true;
    }
    return false;
}

bool verb_worker(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "WORKER", 6) == 0) {
        handle_worker();
        return true;
    }
    return false;
}

bool verb_ulp(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "ULP") == 0) {
        /* What the ULP RISC-V program has counted: a heartbeat that moves
         * proves the coprocessor runs beside the main CPU. */
        bsp_ulp_status_t u;
        bsp_ulp_get_status(&u);
        char out[120];
        snprintf(out, sizeof(out), "ULP: running=%d period_ms=%d heartbeat=%lu\r\n",
                 u.running ? 1 : 0, BSP_ULP_PERIOD_MS, (unsigned long)u.heartbeat);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_reset_log(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "RESET_LOG CLEAR", 15) == 0) {
        reset_log_clear();
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_imu_regw(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "IMU_REGW ", 9) == 0) {
        // Raw LSM6DS3TR-C register write for bench experiments: IMU_REGW <hex addr> <hex value>.
        unsigned addr = 0, val = 0;
        if (sscanf(cmd + 9, "%x %x", &addr, &val) != 2 || addr > 0x7F || val > 0xFF) {
            cdc_reply("ERR IMU_REGW <hexaddr> <hexval>\r\n");
            return true;
        }
        cdc_reply(bsp_imu_write_register((uint8_t)addr, (uint8_t)val) == ESP_OK ? "OK\r\n" : "ERR i2c\r\n");
        return true;
    }
    return false;
}

bool verb_imu_reg(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "IMU_REG ", 8) == 0) {
        // Raw LSM6DS3TR-C register read: IMU_REG <hex addr> [len<=64] -> hex bytes.
        unsigned addr = 0, len = 1;
        if (sscanf(cmd + 8, "%x %u", &addr, &len) < 1 || addr > 0x7F || len < 1 || len > 64) {
            cdc_reply("ERR IMU_REG <hexaddr> [len]\r\n");
            return true;
        }
        uint8_t buf[64];
        if (bsp_imu_read_registers((uint8_t)addr, buf, len) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        char out[12 + 64 * 3];
        size_t n = snprintf(out, sizeof(out), "IMUREG %02X:", addr);
        for (unsigned i = 0; i < len; i++) n += snprintf(out + n, sizeof(out) - n, " %02X", buf[i]);
        snprintf(out + n, sizeof(out) - n, "\r\n");
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_stm_regw(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STM_REGW ", 9) == 0) {
        // Raw STM32 register-map write for bench experiments: STM_REGW <hex addr> <hex value>.
        // Not saved: the next settings push (an STM reset, Settings) restores the stored value.
        unsigned addr = 0, val = 0;
        if (sscanf(cmd + 9, "%x %x", &addr, &val) != 2 || addr > 0xFFFF || val > 0xFF) {
            cdc_reply("ERR STM_REGW <hexaddr> <hexval>\r\n");
            return true;
        }
        cdc_reply(stm32_i2c_write_u8((uint16_t)addr, (uint8_t)val) == ESP_OK ? "OK\r\n" : "ERR i2c\r\n");
        return true;
    }
    return false;
}

bool verb_stm_reg(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STM_REG ", 8) == 0) {
        // Raw STM32 register-map read: STM_REG <hex addr> [len<=64] → hex bytes.
        unsigned addr = 0, len = 1;
        if (sscanf(cmd + 8, "%x %u", &addr, &len) < 1 || len < 1 || len > 64) {
            cdc_reply("ERR STM_REG <hexaddr> [len]\r\n");
            return true;
        }
        uint8_t buf[64];
        if (stm32_i2c_read((uint16_t)addr, buf, len) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        char out[8 + 64 * 3];
        size_t n = snprintf(out, sizeof(out), "REG %04X:", addr);
        for (unsigned i = 0; i < len; i++) n += snprintf(out + n, sizeof(out) - n, " %02X", buf[i]);
        snprintf(out + n, sizeof(out) - n, "\r\n");
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_reset_log_2(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "RESET_LOG STM", 13) == 0) {
        char out[96];
        reset_log_stm_format(out, sizeof(out));
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_reset_log_3(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "RESET_LOG", 9) == 0) {
        EXT_RAM_BSS_ATTR static char out[1024];
        reset_log_format(out, sizeof(out));
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_coredump_info(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "COREDUMP_INFO", 13) == 0) {
        EXT_RAM_BSS_ATTR static char out[512];
        reset_log_coredump_format(out, sizeof(out));
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_imu_midi(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "IMU_MIDI SLOT ", 14) == 0) {
        /* One slot per call: src tgt cc ch inv span. Everything is validated
         * against the enums rather than trusted, because a bad source index
         * would otherwise silently map to Pitch and look like a wiring bug. */
        int idx = -1, src = -1, tgt = -1, cc = -1, ch = -1, inv = -1, span = -1;
        if (sscanf(cmd, "IMU_MIDI SLOT %d %d %d %d %d %d %d",
                   &idx, &src, &tgt, &cc, &ch, &inv, &span) != 7 ||
            idx < 0 || idx >= (int)crosspad::kImuMidiSlots ||
            src < 0 || src >= (int)crosspad::ImuMidiSource::Count ||
            tgt < 0 || tgt >= (int)crosspad::ImuMidiTarget::Count ||
            cc < 0 || cc > 127 || ch < 0 || ch > 15 ||
            (inv != 0 && inv != 1) || span <= 0) {
            cdc_reply("ERR IMU_MIDI SLOT idx src tgt cc ch inv span "
                      "(src 0..4 pitch/roll/gx/gy/gz, tgt 0=cc 1=bend)\r\n");
            return true;
        }
        auto *cfg = crosspad::CrosspadSettings::getInstance();
        if (!cfg) { cdc_reply("ERR no settings\r\n"); return true; }
        auto &slot = cfg->imuMidi.slots[idx];
        slot.enabled   = true;
        slot.source    = (uint8_t)src;
        slot.target    = (uint8_t)tgt;
        slot.cc        = (uint8_t)cc;
        slot.channel   = (uint8_t)ch;
        slot.invert    = (inv == 1);
        slot.inputSpan = (float)span;
        imu_midi_apply_settings();
        cfg->saveTo(NvsSettingsStore::getInstance());
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_imu_midi_2(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "IMU_MIDI ", 9) == 0) {
        int on = -1;
        if (sscanf(cmd, "IMU_MIDI %d", &on) == 1 && (on == 0 || on == 1)) {
            auto *cfg = crosspad::CrosspadSettings::getInstance();
            if (cfg) {
                cfg->imuMidi.enabled = (on == 1);
                cfg->saveTo(NvsSettingsStore::getInstance());
            }
            imu_midi_set_enabled(on == 1);
            cdc_reply("OK\r\n");
        } else {
            cdc_reply("ERR IMU_MIDI 0|1\r\n");
        }
        return true;
    }
    return false;
}

bool verb_heap_trace(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "HEAP_TRACE ", 11) == 0) {
        handle_heap_trace(cmd + 11);
        return true;
    }
    return false;
}

bool verb_imu_gesture(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "IMU_GESTURE ", 12) == 0) {
        /* The board cannot be knocked from the bench; this posts what the IMU
         * driver would have, on the event bus, so an app's reaction to a
         * gesture (sampler Advanced, DAW Control mode) is testable. */
        static const struct { const char *name; crosspad::MotionGesture g; } kNames[] = {
            {"TAP",         crosspad::MotionGesture::SingleTap},
            {"DOUBLE_TAP",  crosspad::MotionGesture::DoubleTap},
            {"FREE_FALL",   crosspad::MotionGesture::FreeFall},
            {"WAKE_UP",     crosspad::MotionGesture::WakeUp},
            {"ORIENTATION", crosspad::MotionGesture::Orientation},
            {"STILL",       crosspad::MotionGesture::Still},
        };
        const char *arg = cmd + 12;
        for (const auto &n : kNames) {
            if (strncmp(arg, n.name, strlen(n.name)) == 0) {
                crosspad::getEventBus().postMotionGesture(n.g, 0, crosspad::EventSource::Cli);
                cdc_reply("OK\r\n");
                return true;
            }
        }
        cdc_reply("ERR IMU_GESTURE TAP|DOUBLE_TAP|FREE_FALL|WAKE_UP|ORIENTATION|STILL\r\n");
        return true;
    }
    return false;
}

bool verb_imu_ths(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "IMU_THS ", 8) == 0) {
        /* IMU_THS <tap 0-31> <wake 0-63>: the embedded tap and wake-up thresholds,
         * for tuning on hardware (not persisted). */
        int tap = -1, wake = -1;
        if (sscanf(cmd + 8, "%d %d", &tap, &wake) != 2 || tap < 0 || wake < 0 ||
            bsp_imu_set_thresholds((uint8_t)tap, (uint8_t)wake) != ESP_OK) {
            cdc_reply("ERR IMU_THS <tap 0-31> <wake 0-63>\r\n");
        } else {
            cdc_reply("OK\r\n");
        }
        return true;
    }
    return false;
}

bool verb_imu(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "IMU", 3) == 0) {
        handle_imu();
        return true;
    }
    return false;
}

bool verb_settings_reset(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SETTINGS_RESET", 14) == 0) {
        if (strstr(cmd, "FORCE") == nullptr) {
            cdc_reply("ERR say FORCE — this erases every saved setting and reboots\r\n");
            return true;
        }
        handle_settings_reset();
        return true;
    }
    return false;
}

bool verb_reset_test(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "RESET_TEST ", 11) == 0) {
        // Kill the device three ways, to check the black box end to end.
        // Each one reboots the ESP.
        const char *how = cmd + 11;
        if (strncmp(how, "panic", 5) == 0) {
            cdc_reply("OK panic\r\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            abort();
        }
        if (strncmp(how, "wdt", 3) == 0) {
            // Starve IDLE0 until the task watchdog fires — a core dump,
            // with TASK_WDT_PANIC.
            cdc_reply("OK wdt\r\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            for (;;) { }
        }
        if (strncmp(how, "stmwd", 5) == 0) {
            // Silence the heartbeat past the STM watchdog: it pulses EN and
            // the ESP returns with a POWERON reason.
            cdc_reply("OK stmwd\r\n");
            stm32_hal_heartbeat_pause_ms(15000);
            return true;
        }
        cdc_reply("ERR panic|wdt|stmwd\r\n");
        return true;
    }
    return false;
}

bool verb_mem(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "MEM", 3) == 0) {
        handle_mem();
        return true;
    }
    return false;
}

bool verb_cdc_stats(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "CDC_STATS", 9) == 0) {
        /* The three fates of a command with no reply: rx = USB reads queued,
         * cmds = complete lines dispatched, drop = lost to a full queue. */
        char reply[256];
        /* maxcmd/maxgap are read-and-cleared: bracket a window with two reads
         * and the second one is that window's worst case. maxcmd is time spent
         * inside one handler, maxgap is time a command already queued waited
         * for the drain loop — a late reply is one or the other. */
        /* stack_min= is the least stack, in bytes, any command since boot
         * left free on the main task, stack_verb= the verb that did: not
         * cleared, the worst case is the one that matters. stack_last= is
         * what the CDC command before this one left. Ahead of maxcmd=, whose
         * value can hold spaces. */
        snprintf(reply, sizeof(reply),
                 "CDCSTATS: rx=%u cmds=%u drop=%u maxcmd_us=%u maxgap_us=%u stack_min=%u stack_verb=%s "
                 "stack_last=%u maxcmd=%s\r\n",
                 (unsigned)g_cdc_rx_chunks.load(std::memory_order_relaxed),
                 (unsigned)g_cdc_commands.load(std::memory_order_relaxed),
                 (unsigned)g_cdc_dropped.load(std::memory_order_relaxed),
                 (unsigned)g_cdc_max_cmd_us.exchange(0, std::memory_order_relaxed),
                 (unsigned)g_cdc_max_gap_us.exchange(0, std::memory_order_relaxed),
                 (unsigned)s_stack_min, s_stack_verb,
                 (unsigned)g_cdc_last_stack_free.load(std::memory_order_relaxed), g_cdc_max_cmd_verb);
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_usb_guard(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (strncmp(cmd, "USB_GUARD", 9) == 0) {
        /* The USB serial guard (usb_serial_prompt): with it on, a host's SysEx
         * request for the CDC profile is confirmed on the device. A bench
         * board turns it off once, persisted — or every unattended
         * `usb-mode set default` stops at the prompt. */
        auto *cfg = crosspad::CrosspadSettings::getInstance();
        if (!cfg) { cdc_reply("ERR no settings\r\n"); return true; }
        if (sscanf(cmd, "USB_GUARD %d", &a) == 1) {
            if (a != 0 && a != 1) { cdc_reply("ERR usage USB_GUARD [0|1]\r\n"); return true; }
            cfg->usbSerialGuard = (a == 1);
            cfg->saveTo(NvsSettingsStore::getInstance());
        }
        char out[24];
        snprintf(out, sizeof(out), "USB_GUARD: %d\r\n", cfg->usbSerialGuard ? 1 : 0);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_ota_prefs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "OTA_PREFS") == 0) {
        /* Read-only: whether an OTA over CDC will stop at the on-screen Allow
         * prompt (auto_accept=0) or start at once. Set only from the Firmware
         * app's checkbox. */
        char out[32];
        snprintf(out, sizeof(out), "OTA_PREFS: auto_accept=%d\r\n", ota_auto_accept_enabled() ? 1 : 0);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_app_versions(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "APP_VERSIONS", 12) == 0) {
        handle_app_versions();
        return true;
    }
    return false;
}

bool verb_app_sha(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "APP_SHA", 7) == 0) {
        handle_app_sha();
        return true;
    }
    return false;
}

bool verb_heap_check(VerbLine &l)
{
    char *cmd = l.cmd;
    /* STOOL [IN MIC|LINE|PADS|USB|REC|SYNC bar [beats]|DIR [path]|STOP|RELEASE|LIST|OV path [a b]|ONSETS sens|EQ n|
     *        PLAY path a b loop|SLICE path a b|ATK ms|PITCH st|PSTOP|SAVE path a b|EXPORT name|HANDOFF path a b|
     *        FX path out st hp lp rev [keep]] --
     * the sample tools (Recorder, Slicer). SYNC is a take with the song from
     * that bar (0-based), after beats of count-in; song=/wait= say so. DIR
     * lists a folder of the card (none: the recordings); dir=/n=/folders= and
     * the names after "names=". Slices from ONSETS/EQ are kept here
     * for EXPORT; ATK and PITCH (semitones, varispeed) are what EXPORT and
     * SLICE (a slice's audition) use. FX's keep=1 transposes and keeps the
     * length (the Recorder's KEEP), 0 or none varispeeds. Bare STOOL reports;
     * names last, like ASONG. */
    /* HEAP_CHECK [WATCH 0|1] -- walk every heap and say whether its blocks are
     * still whole. Light poisoning only notices a broken block when something
     * frees it, which can be minutes and two apps away from the write that
     * broke it: that is how an overrun somewhere else comes out as an assert
     * inside lv_label_set_text_fmt. WATCH 1 puts the walk on a worker and logs
     * the first failure, which pins the corruption to whatever was happening
     * seconds earlier instead of to the innocent free that noticed. */
    if (strncmp(cmd, "HEAP_CHECK", 10) == 0) {
        int on = -1;
        if (sscanf(cmd, "HEAP_CHECK WATCH %d", &on) == 1) heap_watch_set(on != 0);
        const bool ok = heap_caps_check_integrity_all(true);
        EXT_RAM_BSS_ATTR static char reply[96];
        snprintf(reply, sizeof(reply), "HEAPCHK: ok=%d watch=%d\r\n",
                 ok ? 1 : 0, heap_watch_on() ? 1 : 0);
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_settings_export(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SETTINGS_EXPORT", 15) == 0 || strncmp(cmd, "SETTINGS_IMPORT", 15) == 0) {
        char path[96] = {};
        const char *arg = cmd + 15;
        while (*arg == ' ') arg++;
        size_t n = 0;
        while (n < sizeof(path) - 1 && arg[n] && arg[n] != '\r' && arg[n] != '\n') {
            path[n] = arg[n];
            ++n;
        }
        if (!settings_backup_start(cmd[9] == 'E' ? 'e' : 'i', path)) {
            cdc_reply(s_settings_busy.load() ? "ERR busy\r\n" : "ERR path\r\n");
            return true;
        }
        char out[128];
        snprintf(out, sizeof(out), "OK %s\r\n", s_settings_path);
        cdc_reply(out);   /* reply first */
        lv_async_call_locked(async_settings_backup, nullptr);
        return true;
    }
    return false;
}

bool verb_settings_status(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SETTINGS_STATUS", 15) == 0) {
        handle_settings_status();
        return true;
    }
    return false;
}

bool verb_sd_cache(VerbLine &l)
{
    char *cmd = l.cmd;
    /* SD_CACHE [0|1] -- the card layer's cache of single sectors (directory
     * and FAT reads, sd_sector_io.cpp), for an A/B; emptied either way. */
    if (strncmp(cmd, "SD_CACHE", 8) == 0 && (cmd[8] == 0 || cmd[8] == ' ')) {
        if (cmd[8] == ' ') sd_cache_enable(cmd[9] == '1');
        cdc_reply(sd_cache_on() ? "SDCACHE: on=1\r\n" : "SDCACHE: on=0\r\n");
        return true;
    }
    return false;
}

bool verb_sd_trace(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "SD_TRACE") == 0) {
        /* Every card transaction since the last call, by task (sd_sector_io.cpp). */
        EXT_RAM_BSS_ATTR static char reply[1024];
        sd_trace_cmd(reply, sizeof(reply));
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_sd_bounce(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SD_BOUNCE ", 10) == 0) {
        /* The sector bounce off (0) or on (1), for an A/B. */
        sd_bounce_enable(cmd[10] == '1');
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_stm_fw_info(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STM_FW_INFO", 11) == 0) {
        // Running STM firmware vs the image bundled in LittleFS.
        stm32_link_info_t li = {};
        stm32_hal_get_link_info(&li);
        stm32_fw_image_info_t img = {};
        esp_err_t ierr = stm_fw_bundled_info(&img);
        char out[160];
        if (ierr == ESP_OK) {
            snprintf(out, sizeof(out), "STM_FW running=%u.%u/0x%04X%s board=r%u fw_pcb=r%u bundled=%u.%u/0x%04X pcb=r%u size=%u\r\n",
                     li.fw_major, li.fw_minor, li.proto_version, li.linked ? "" : "(down)",
                     li.pcb_rev, li.fw_pcb, img.major, img.minor, img.proto, img.pcb, (unsigned)img.size);
        } else {
            snprintf(out, sizeof(out), "STM_FW running=%u.%u/0x%04X%s bundled=none(%s)\r\n",
                     li.fw_major, li.fw_minor, li.proto_version, li.linked ? "" : "(down)",
                     esp_err_to_name(ierr));
        }
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_stm_fw_update(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STM_FW_UPDATE", 13) == 0) {
        // Reflash the STM from a file (default: the bundled image). ~10 s,
        // on its own task, which sends the reply. A trailing FORCE overrides
        // the board-revision gate.
        char path[96];
        const char *arg = cmd + 13;
        while (*arg == ' ') arg++;
        strncpy(path, *arg ? arg : STM32_FW_BUNDLED_PATH, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        for (char *q = path; *q; ++q) {
            if (*q == '\r' || *q == '\n') { *q = '\0'; break; }
        }
        bool force = false;
        char *sp = strrchr(path, ' ');
        if (sp && strcmp(sp + 1, "FORCE") == 0) { force = true; *sp = '\0'; }
        if (strcmp(path, "FORCE") == 0) { force = true; strcpy(path, STM32_FW_BUNDLED_PATH); }
        /* ~10 s: on a task of its own, which answers (stm_fw_update_task). */
        if (s_stm_upd_busy.exchange(true)) { cdc_reply("ERR STM_FW busy\r\n"); return true; }
        strncpy(s_stm_upd_path, path, sizeof(s_stm_upd_path) - 1);
        s_stm_upd_path[sizeof(s_stm_upd_path) - 1] = '\0';
        s_stm_upd_force = force;
        if (xTaskCreate(stm_fw_update_task, "stm_fw_cdc", 6144, nullptr, 5, nullptr) != pdPASS) {
            s_stm_upd_busy.store(false);
            cdc_reply("ERR STM_FW no task\r\n");
        }
        return true;
    }
    return false;
}

bool verb_stm_reset(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STM_RESET", 9) == 0) {
        /* Hardware reset of the STM through the ESP's reset line, a 100 ms
         * pulse. An ST-LINK without an NRST wire cannot reset it, so when the STM sits in
         * STOP1 with debug off (a release build after a dirty jump out of the ROM DFU),
         * this is the only reset a host can reach. The STM console and the
         * link go away for a moment; the ESP stays up. */
        gpio_set_level((gpio_num_t)BSP_STM_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level((gpio_num_t)BSP_STM_RST_PIN, 0);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_stm_dfu(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STM_DFU", 7) == 0) {
        // Drop the STM into USB DFU; the bridge is gone until it reboots.
        cdc_reply(crosspad::getStm32CommandSender().sendDfuRequest(true)
                      ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    return false;
}
