#include "hil_verbs.h"

static const char *TAG = "hil_control";
std::atomic<uint32_t> g_hil_pad_press_count{0};
std::atomic<uint32_t> g_hil_pad_release_count{0};
std::atomic<uint32_t> g_hil_note_on_count{0};
std::atomic<uint16_t> g_hil_last_note_on{0};
/* Serialises cdc_reply(). Two tasks write the same FIFO: the CDC drain loop
 * and PITCHED_BENCH's own task, which emits a row per measurement while the
 * loop keeps answering other verbs. Without this a "BENCH:" row lands inside
 * another reply's line and the host's prefix reader drops both. Task context
 * only — no ISR reaches cdc_reply (the TinyUSB RX callback queues to
 * app_queue; nothing else calls in), so the plain mutex is the right one. */
static SemaphoreHandle_t cdc_reply_lock(void)
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

/* The CDC TX FIFO is 256 bytes and the queue call silently drops what does
 * not fit, so a reply longer than that goes out in flushed pieces. */
static std::atomic<TaskHandle_t> companionReplyTask{nullptr};
static char *companionReplyBuffer = nullptr;
static size_t companionReplyCapacity = 0;
void cdc_reply(const char *text)
{
    if (companionReplyTask.load() == xTaskGetCurrentTaskHandle()) {
        const size_t used = strlen(companionReplyBuffer);
        if (used + strlen(text) >= companionReplyCapacity) snprintf(companionReplyBuffer, companionReplyCapacity, "ERR response_too_large\r\n");
        else memcpy(companionReplyBuffer + used, text, strlen(text) + 1);
        return;
    }
    SemaphoreHandle_t lock = cdc_reply_lock();
    /* Bounded, and sent anyway on timeout: a reply the host never sees reads as
     * a dead board, which is worse than one interleaved line. */
    const bool held = lock && xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) == pdTRUE;
    size_t left = strlen(text);
    while (left > 0) {
        const size_t n = left < 128 ? left : 128;
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)text, n);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
        text += n;
        left -= n;
    }
    if (held) xSemaphoreGive(lock);
}

uint32_t s_stack_min = UINT32_MAX;
char     s_stack_verb[20] = "-";

/* FreeRTOS fills a new stack with this byte (tasks.c tskSTACK_FILL_BYTE), and
 * uxTaskGetStackHighWaterMark() counts how many of it are left at the bottom. */
static constexpr uint8_t kStackFillByte = 0xa5;
/* Left alone below the caller's stack pointer: memset's frame and the save
 * area under it are the only live bytes down there. */
static constexpr size_t kStackPaintGap = 256;
/* Left alone at the bottom: the stack-end watchpoint covers 32 bytes there,
 * aligned up to 32 (port.c vPortSetStackWatchpoint), and a store to it panics. */
static constexpr size_t kStackWatchedBottom = 64;

/* The kernel's mark is the deepest this task has ever been, boot included:
 * the free part is filled again before every command, so the mark read after
 * it is that command's alone. */
void hil_stack_mark(void)
{
    uint8_t *const bottom = pxTaskGetStackStart(nullptr) + kStackWatchedBottom;
    uint8_t *const below = static_cast<uint8_t *>(esp_cpu_get_sp()) - kStackPaintGap;
    if (below > bottom) memset(bottom, kStackFillByte, (size_t)(below - bottom));
}

uint32_t hil_stack_note(const char *line, size_t len)
{
    const uint32_t freeNow = uxTaskGetStackHighWaterMark(nullptr);
    if (freeNow >= s_stack_min) return freeNow;
    s_stack_min = freeNow;
    size_t n = 0;
    while (n < len && n < sizeof(s_stack_verb) - 1 && line[n] != ' ') ++n;
    memcpy(s_stack_verb, line, n);
    s_stack_verb[n] = '\0';
    return freeNow;
}

// The kit-selector "Load" path. Loading before APP_START Sampler keeps the
// pre-launch overlay from triggering, so scripts stay non-interactive.
int s_pending_kit = -1;

void async_kit_load(void *)
{
    // On the LVGL thread: hand the whole load to the background task.
    const bool busy = crosspad_sampler::kit_load_task_is_loading();
    crosspad_sampler::kit_load_task_start(false, s_pending_kit);
    // Say which of the two happened — a queued request must not read as a
    // started one.
    ESP_LOGI(TAG, "KIT_LOAD %d %s", s_pending_kit, busy ? "queued" : "started");
}

/* SysEx 0x1D 0x0B: the same request the CDC verb makes, for the USB-audio
 * profile where there is no CDC to make it on. Same refusals, no reply. */
void hil_kit_load(int kitId)
{
    if (!g_kit_scan_done.load() || !crosspad::control::kitIdLoadable(kitId)) {
        ESP_LOGW(TAG, "KIT_LOAD %d refused (scan=%d)", kitId, g_kit_scan_done.load() ? 1 : 0);
        return;
    }
    s_pending_kit = kitId;
    lv_async_call_locked(async_kit_load, nullptr);
}

/* Bench pad hits play from this task, not from the loop that received them.
 * A hit is a synchronous call into the sample engine and waits on the card
 * while a kit loads — CDC_STATS showed maxcmd=PAD_PRESS at 1.14 s — which in
 * the CDC drain loop held every reply behind it and KIT_STATUS timed out on
 * the host. Same priority and stack as the STM link task that plays the
 * physical pads, so a bench hit competes for the card like a real one. */
struct PadInjectMsg { HilPadInject kind; uint8_t pad; uint8_t val; };
QueueHandle_t s_pad_inject_queue;
std::atomic<uint32_t> g_hil_pad_inject_drop{0};

static void pad_inject_task(void *)
{
    PadInjectMsg m;
    for (;;) {
        if (xQueueReceive(s_pad_inject_queue, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.kind == HilPadInject::Press)        twin_mirror_pad(m.pad, m.val);
        else if (m.kind == HilPadInject::Release) twin_mirror_pad(m.pad, 0);
        if (m.kind == HilPadInject::Release) {
            /* Injected presses record like played ones, through the pad
             * manager's input tap (midiCallbacks.cpp) -- otherwise no scenario
             * could ever test the loop recorder -- and their releases end the
             * notes, so a scenario can record a length. */
            getSequencer().noteOut(m.pad);
        }
        crosspad::control::applyPadInput(crosspad::getPadManager(), m.kind, m.pad, m.val);
    }
}

void hil_pad_inject_init(void)
{
    s_pad_inject_queue = xQueueCreateWithCaps(32, sizeof(PadInjectMsg), MALLOC_CAP_SPIRAM);
    assert(s_pad_inject_queue);
    xTaskCreatePinnedToCoreWithCaps(pad_inject_task, "hil_pad", 4000, nullptr, 6, nullptr, 0,
                                    MALLOC_CAP_SPIRAM);
}

bool hil_pad_inject(HilPadInject kind, uint8_t pad, uint8_t val)
{
    const PadInjectMsg m{kind, pad, val};
    if (!s_pad_inject_queue || xQueueSend(s_pad_inject_queue, &m, 0) != pdTRUE) {
        ++g_hil_pad_inject_drop;
        return false;
    }
    if (kind == HilPadInject::Press)        ++g_hil_pad_press_count;
    else if (kind == HilPadInject::Release) ++g_hil_pad_release_count;
    return true;
}

bool verb_fw_updater_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (fw_updater_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_cp_account_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (cp_account_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_cp_remote_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (cp_remote_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_fw_ui_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (fw_ui_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_host_info_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (host_info_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_wifi_verbs_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (wifi_verbs_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_kit_convert_verbs_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (kit_convert_verbs_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_sdr_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (sdr_verbs_handle && sdr_verbs_handle(cmd, cdc_reply)) {
        return true;
    }
    return false;
}

bool verb_sd_bench_verb_verbs(VerbLine &l)
{
    char *cmd = l.cmd;
    if (sd_bench_verb(cmd, cdc_reply)) return true;
    return false;
}

/* Every verb, in the order a line is offered to them. The order is part of
 * the protocol: verbs match by prefix (MEM_BLOCKS before MEM, IMU_REGW before
 * IMU_REG before IMU), so the first one that takes the line wins. */
static bool (*const k_verbs[])(VerbLine &) = {
    verb_pad_notes,
    verb_usb_msc,
    verb_net_mode,
    verb_fw_list,
    verb_fw_slots,
    verb_fw_install,
    verb_fw_switch,
    verb_fw_confirm,
    verb_fw_updater_verbs,
    verb_cp_account_verbs,
    verb_cp_remote_verbs,
    verb_fw_ui_verbs,
    verb_host_info_verbs,
    verb_wifi_verbs_verbs,
    verb_kit_convert_verbs_verbs,
    verb_sdr_verbs,
    verb_tbar,
    verb_ui_state,
    verb_twin,
    verb_motion,
    verb_twin_2,
    verb_twin_3,
    verb_pwr_gesture,
    verb_tasks,
    verb_toast,
    verb_screenshot,
    verb_in_pair,
    verb_codec_reg,
    verb_ingain,
    verb_mem_blocks,
    verb_worker,
    verb_ulp,
    verb_reset_log,
    verb_imu_regw,
    verb_imu_reg,
    verb_stm_regw,
    verb_stm_reg,
    verb_reset_log_2,
    verb_reset_log_3,
    verb_coredump_info,
    verb_imu_midi,
    verb_imu_midi_2,
    verb_heap_trace,
    verb_touch_trace,
    verb_touch_drag,
    verb_touch_tap,
    verb_topbar,
    verb_ui_tree,
    verb_uac_stats,
    verb_usb_host,
    verb_usb_enum,
    verb_uac_late,
    verb_uac_pkt,
    verb_uac_flow,
    verb_clock_status,
    verb_imu_gesture,
    verb_imu_ths,
    verb_imu,
    verb_settings_reset,
    verb_reset_test,
    verb_mem,
    verb_lvgl_prof,
    verb_lvgl_inv,
    verb_splash,
    verb_lvgl_stats,
    verb_cdc_stats,
    verb_ble_status,
    verb_ble_devices,
    verb_ble_stop,
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
    verb_daw_state,
#endif
    verb_sysex_echo,
    verb_audio_rate,
    verb_usb_guard,
    verb_ota_prefs,
    verb_ble_enable,
    verb_ble_start,
    verb_ble_scan,
    verb_ble_scan_2,
    verb_ble_connect,
    verb_ble_disconnect,
    verb_ble_txoff,
    verb_ble_send,
    verb_app_list,
    verb_app_versions,
    verb_app_sha,
    verb_app_start,
    verb_enc_focus,
    verb_enc_group,
    verb_enc_raw,
    verb_enc_state,
    verb_led_state,
    verb_pad_stats_reset,
    verb_midi_trs,
    verb_midi_loop,
    verb_stm_vpad,
    verb_click,
    verb_seq_dump,
    verb_seq,
    verb_song,
    verb_asong_seq,
    verb_asong_trk,
    verb_asong_dbg,
    verb_asong,
    verb_outvol,
    verb_mix_eq,
    verb_clip,
    verb_heap_check,
    verb_stool,
    verb_audio_level,
#ifdef HIL_HAS_MIXER
    verb_mix_lvl,
    verb_mix_preset,
    verb_input_mon,
    verb_mix_route,
    verb_mix_mute,
#endif
    verb_pad_stats,
    verb_enc_rotate,
    verb_enc_press,
    verb_enc_press_2,
    verb_app_self_close,
    verb_app_destroy,
    verb_app_stop,
    verb_pad_press,
    verb_pad_release,
    verb_pad_pressure,
    verb_pad_pan,
    verb_kit_list,
    verb_kit_status,
    verb_settings_export,
    verb_settings_status,
    verb_kit_swap_log,
    verb_wave_status,
    verb_smpl_pace,
    verb_smpl_stall,
    verb_smpl_last,
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
    verb_smpl_pool,
#endif
    verb_kit_load,
    verb_kit_delete,
    verb_pad_info,
    verb_pad_mix,
    verb_kit_save_status,
    verb_kit_save,
    verb_arena,
    verb_inst_load,
    verb_inst_release,
    verb_inst_status,
    verb_pitched_status,
    verb_pitched_cap,
    verb_simd_mode,
    verb_sd_bench_verb_verbs,
    verb_sd_cache,
    verb_sd_trace,
    verb_sd_bounce,
    verb_pitched_bench,
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    verb_smpl_poly,
#endif
    verb_smpl_poly_2,
    verb_audio_buf,
    verb_lat_trace,
    verb_rt_prof,
    verb_smpl_peak,
    verb_smpl_pitch,
    verb_smpl_loops,
    verb_smpl_engine,
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
    verb_smpl_bkg,
#endif
    verb_smpl_preview,
    verb_smpl_preview_stop,
    verb_stm_fw_info,
    verb_stm_fw_update,
    verb_stm_reset,
    verb_stm_dfu,
    verb_audio_tasks,
};

bool hil_control_handle_cdc(const char *buff, size_t len)
{
    VerbLine l;
    /* A CDC line can be longer than this (CDC_LINE_MAX, for FS_* paths): no
     * verb here takes one, and a cut-off argument must not run. */
    if (len >= sizeof(l.cmd)) {
        cdc_reply("ERR too_long\r\n");
        return true;
    }
    memcpy(l.cmd, buff, len);
    l.cmd[len] = '\0';
    l.len = len;

    for (auto verb : k_verbs) {
        if (verb(l)) return true;
    }
    return false;
}

/* "c reg" and nothing after it: CODEC_REG's read form. */
static bool codec_reg_is_read(const char *args)
{
    unsigned c = 0, reg = 0;
    char extra[2] = {0};
    return sscanf(args, "%u %x %1s", &c, &reg, extra) == 2;
}

void hil_control_companion_handle(const char *command, char *reply, size_t capacity) {
    if (strcmp(command, "CP_CAPS") == 0) {
        snprintf(reply, capacity, "CP_CAPS 1 control=1 files=0\r\n"); return;
    }
    /* Remote control and read-only state. Nothing that grants more than the
     * caller already has: USB_GUARD is readable, never settable, from here. */
    const bool allowed = strcmp(command, "STM_FW_INFO") == 0 || strcmp(command, "UI_STATE") == 0 ||
        strcmp(command, "APP_LIST") == 0 || strcmp(command, "LED_STATE") == 0 || strcmp(command, "PAD_NOTES") == 0 ||
        strcmp(command, "ENC_PRESS") == 0 || strncmp(command, "ENC_PRESS ", 10) == 0 ||
        strncmp(command, "ENC_ROTATE ", 11) == 0 ||
        strcmp(command, "APP_STOP") == 0 || strncmp(command, "APP_START ", 10) == 0 ||
        strncmp(command, "PAD_PRESS ", 10) == 0 || strncmp(command, "PAD_RELEASE ", 12) == 0 ||
        strncmp(command, "KIT_LOAD ", 9) == 0 || strcmp(command, "KIT_STATUS") == 0 ||
        strcmp(command, "SETTINGS_STATUS") == 0 || strcmp(command, "USB_GUARD") == 0 ||
        /* The card's transactions by task, read and cleared (sd_sector_io.cpp). */
        strcmp(command, "SD_TRACE") == 0 ||
        /* Read-only diagnostics. The MIDI+UAC2 profile has no CDC, so without
         * these the one profile where the render is under the most pressure is
         * also the one that cannot be measured -- which is how a render peaking
         * at 7.7 ms against a 1.45 ms block went unseen. They only report, and
         * SMPL_PEAK's reset-on-read of its own maxima is what a diagnostic is
         * for. Nothing here grants the caller anything it did not have. */
        strncmp(command, "SMPL_PEAK", 9) == 0 || strcmp(command, "MEM") == 0 ||
        strcmp(command, "PAD_STATS") == 0 || strcmp(command, "CLOCK_STATUS") == 0 ||
        strcmp(command, "AUDIO_STATUS") == 0 || strcmp(command, "UAC_STATS") == 0 || strcmp(command, "UAC_FLOW") == 0 || strcmp(command, "UAC_PKT") == 0 || strcmp(command, "UAC_LATE") == 0 ||
        /* The host USB audio is tuned for: a board in this profile is the one
         * that needs it, and it has no CDC to set it from. */
        strcmp(command, "USB_HOST") == 0 || strcmp(command, "USB_HOST AUTO") == 0 ||
        strcmp(command, "USB_HOST MAC") == 0 || strcmp(command, "USB_HOST LINUX") == 0 ||
        strncmp(command, "USB_ENUM", 8) == 0 ||
        /* Per-channel peaks, routes and mutes: which channel carries what is
         * on an output, in the USB audio profile (over the WiFi remote or
         * BLE -- the listing is longer than a SysEx reply). Read-only. */
        strcmp(command, "MIX_LVL") == 0 ||
        /* The same argument, for the three the first pass left out. A report
         * of "everything stutters in the audio profile" cannot be chased with
         * SMPL_PEAK alone: which engine holds the kit and what it dropped
         * (SMPL_ENGINE), how long a frame of UI takes (LVGL_STATS) and where
         * the CPU went (TASKS, whose table goes to the STM console -- a
         * separate USB device that this profile does not take away) are the
         * three that say whether the render, the UI or the scheduler is at
         * fault. All read-only; TASKS and LVGL_STATS reset their own maxima,
         * which is what a diagnostic is for. */
        strcmp(command, "SMPL_ENGINE") == 0 || strcmp(command, "LVGL_STATS") == 0 ||
        strcmp(command, "TASKS") == 0 || strcmp(command, "CDC_STATS") == 0 ||
        /* A song played and stopped -- nothing about it changes: what a
         * performance run needs in the one profile that records the board
         * (crosspad-hil play_perf). Matched whole. */
        strcmp(command, "ASONG") == 0 || strcmp(command, "ASONG PLAY") == 0 ||
        strcmp(command, "ASONG STOP") == 0 ||
        /* The Recorder's state (the report: input, peaks, take, error), and
         * a take from the USB input: that input exists only in this profile,
         * so a take from it can be neither made nor read anywhere else. Only
         * these -- the input, start, stop, let go; the rest of STOOL (files,
         * slices, exports) stays CDC-only. */
        strcmp(command, "STOOL") == 0 || strcmp(command, "STOOL IN USB") == 0 ||
        strcmp(command, "STOOL REC") == 0 || strcmp(command, "STOOL STOP") == 0 ||
        strcmp(command, "STOOL RELEASE") == 0 ||
        /* Which image is running. A board that cannot be switched out of this
         * profile without someone pressing Allow on it can still be reflashed
         * over UART through the STM bridge -- and then there is no way to
         * prove what landed, because every verb that says so needed CDC. */
        strcmp(command, "APP_SHA") == 0 || strcmp(command, "APP_VERSIONS") == 0 ||
        /* The read-only forms only, matched whole: `RT_PROF 1` starts the
         * render profiler and `SMPL_POLY n` changes the engine's voice caps,
         * and a prefix match let both through from BLE, SysEx and the WiFi
         * remote. */
        strcmp(command, "RT_PROF") == 0 || strcmp(command, "SMPL_POLY") == 0 ||
        /* Settings -> Audio -> Buffer, read or chosen: the latency a DAW on
         * the USB-audio profile hears is the one thing it would want to set,
         * and that profile has no CDC. The `Q` form, unsaved and for a bench,
         * stays CDC-only. */
        strcmp(command, "AUDIO_BUF") == 0 ||
        (strncmp(command, "AUDIO_BUF ", 10) == 0 && command[10] >= '0' && command[10] <= '9') ||
        strcmp(command, "RESET_LOG") == 0 || strcmp(command, "COREDUMP_INFO") == 0 ||
        strncmp(command, "TWIN ", 5) == 0 || strcmp(command, "TWIN_STATE") == 0 || strcmp(command, "TWIN_HELLO") == 0 ||
        strncmp(command, "MOTION ", 7) == 0 ||
        strncmp(command, "TWIN_SETTINGS ", 14) == 0 || strncmp(command, "TWIN_INFO ", 10) == 0 ||
        strncmp(command, "TWIN_FW ", 8) == 0 ||
        strncmp(command, "HOST_INFO", 9) == 0 || strncmp(command, "FW_LATEST", 9) == 0 ||
        strcmp(command, "FW_UPD_STATUS") == 0 || strcmp(command, "FW_CATALOG") == 0 ||
        /* Start a catalog check: answers OK at once and leases WiFi on the job
         * task, so it is safe from SysEx/BLE/the WiFi remote. Exact
         * match: it takes no arguments. FW_DOWNLOAD stays CDC-only. */
        strcmp(command, "FW_CHECK") == 0 ||
        strncmp(command, "FW_CATALOG ", 11) == 0 || strcmp(command, "ACCT_STATUS") == 0 ||
        strcmp(command, "REMOTE_STATUS") == 0 || strncmp(command, "WIFI_", 5) == 0 ||
        /* A codec register, read only (two arguments; the write form has a
         * third): the MIDI+UAC2 profile is where the codecs are set up for
         * the capture loop, and without CDC nothing could read them there. */
        (strncmp(command, "CODEC_REG ", 10) == 0 && codec_reg_is_read(command + 10));
    if (!allowed) { snprintf(reply, capacity, "ERR unsupported_companion_command\r\n"); return; }
    /* WiFi verbs CDC serves alone. SCAN and PING answer from a task of their
     * own, and the companion mailbox is filled before this handler returns.
     * UP and DOWN are the host's lease: over BLE the radio stops BLE right
     * after the reply, so the peer could never send the DOWN, and the radio
     * would stay up with BLE off until a reboot. Matched by verb name, the
     * way wifi_verbs_handle() reads it, so arguments do not slip past. */
    static const char *const kCdcOnlyWifi[] = {"WIFI_SCAN", "WIFI_PING", "WIFI_UP", "WIFI_DOWN"};
    for (const char *verb : kCdcOnlyWifi) {
        const size_t n = strlen(verb);
        if (strncmp(command, verb, n) == 0 && (command[n] == '\0' || command[n] == ' ')) {
            snprintf(reply, capacity, "ERR use_cdc\r\n"); return;
        }
    }
    /* The guard prompt is answered by a hand on the encoder, never by the
     * host that raised it. */
    if (strncmp(command, "ENC_", 4) == 0 && usb_serial_prompt_active()) {
        snprintf(reply, capacity, "ERR guard_prompt_active\r\n"); return;
    }
    companionReplyBuffer = reply; companionReplyCapacity = capacity;
    companionReplyTask.store(xTaskGetCurrentTaskHandle());
    hil_stack_mark();
    const bool handled = hil_control_handle_cdc(command, strlen(command));
    hil_stack_note(command, strlen(command));
    companionReplyTask.store(nullptr);
    companionReplyBuffer = nullptr;
    if (!handled || !reply[0]) snprintf(reply, capacity, "ERR unsupported_companion_command\r\n");
}

/* The browser twin mirrors touch and pads to whoever asked, so it is a USB
 * host's to start: BLE peers get no TWIN verbs. The motion stream goes out on
 * USB MIDI only, so a BLE peer has nothing to keep alive. */
static void ble_companion_handle(const char *command, char *reply, size_t capacity) {
    if (strncmp(command, "TWIN", 4) == 0 || strncmp(command, "MOTION ", 7) == 0) {
        snprintf(reply, capacity, "ERR twin_usb_only\r\n"); return;
    }
    hil_control_companion_handle(command, reply, capacity);
}

void hil_control_ble_poll(void) {
    crosspad::bleCompanionPoll(ble_companion_handle);
}
