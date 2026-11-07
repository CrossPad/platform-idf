#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HIL test hooks over CDC — remote app launch, pad stimulation, audio-task
 * control and telemetry, so a hardware-in-the-loop run needs no hands.
 *
 * Text commands on CDC0, replies terminated with \r\n. Verb groups:
 *   APP_*    launcher and app lifecycle, incl. APP_VERSIONS (baked in at
 *            configure time, so it describes THIS binary, not the worktree)
 *   KIT_*    kit listing, load and load status
 *   PAD_*    press/release/pressure injection, the note map, throughput counters
 *   ENC_*    encoder navigation and the focused group
 *   SMPL_*   sample-engine telemetry (last events, peaks, latency)
 *   STM_*    co-processor firmware info, in-field update, DFU, virtual pads
 *   BLE_*    BLE MIDI role, scan, connect and counters
 *   MEM, TASKS, RESET_LOG, COREDUMP_INFO, CDC_STATS, LED_STATE, UI_STATE
 *
 * hil_control_handle_cdc() below is the authoritative list; the host side
 * speaks it in crosspad_hil/verbs.py. The bulk verbs (FS_*, ASSETS_BEGIN) are
 * not here — they need binary framing, and live in file_transfer.h.
 *
 * Returns true if the buffer was consumed as a HIL command.
 */
bool hil_control_handle_cdc(const char *buff, size_t len);
void hil_control_ble_poll(void);

/* The companion verb set (BLE GATT and SysEx 0x1F fronts): CP_CAPS, the
 * allowlist below, "ERR unsupported_companion_command" for the rest. The
 * CDC reply lands in `reply` (capacity 481, the mailbox's). */
void hil_control_companion_handle(const char *command, char *reply, size_t capacity);

/* SamplerPlatformPort.cpp: audition pad N's first sample through the kit-browser preview path (SysEx 0x1D 0x0A). */
void hil_preview_pad(unsigned char padIdx);

/* hil_control.cpp: KIT_LOAD by SysEx (0x1D 0x0B) — the audio profile has no CDC. */
void hil_kit_load(int kitId);

/* hil_control.cpp: the task that plays bench-injected pad hits (see
 * hil_pad_inject). Call once after CrosspadPlatform_Init(). */
void hil_pad_inject_init(void);

#ifdef __cplusplus
} /* extern "C" */

#include <atomic>
#include <cstdint>

#include "crosspad/control/PadInput.hpp"

/* Trigger-chain counters. Incremented from the pad injection path and from the
 * EventBus -> sample engine bridge; read back over CDC as PAD_STATS. */
extern std::atomic<uint32_t> g_hil_pad_press_count;
extern std::atomic<uint32_t> g_hil_pad_release_count;
extern std::atomic<uint32_t> g_hil_note_on_count;
/* pad<<8 | velocity of the last note-on the sampler asked for -- what a
 * performance mode (full level, 16 levels) actually turned a hit into. */
extern std::atomic<uint16_t> g_hil_last_note_on;
extern std::atomic<uint32_t> g_hil_pad_inject_drop; /* hits refused, queue full */

/* A pad hit from the bench (CDC PAD_*, SysEx 0x1D 0x08/0x09). Queued to the
 * hil_pad task and played there through crosspad::control::applyPadInput(),
 * as the simulator's pad_* verbs are; returns false when the queue is full. */
using HilPadInject = crosspad::control::PadInput;
bool hil_pad_inject(HilPadInject kind, uint8_t pad, uint8_t val);

/* CDC transport counters, read back as CDC_STATS. A command with no reply
 * either never crossed USB, was dropped by a full app_queue, or was declined
 * by a handler — three different fixes, one symptom without these. */
extern std::atomic<uint32_t> g_cdc_rx_chunks;  /* USB reads queued */
extern std::atomic<uint32_t> g_cdc_commands;   /* lines dispatched to handlers */
extern std::atomic<uint32_t> g_cdc_dropped;    /* lost to a full app_queue */
extern std::atomic<uint32_t> g_cdc_max_cmd_us; /* longest single handler, read-and-cleared */
extern std::atomic<uint32_t> g_cdc_max_gap_us; /* longest wait for the drain loop, ditto */
extern char g_cdc_max_cmd_verb[20];            /* the command that took max_cmd_us */
extern std::atomic<uint32_t> g_cdc_last_stack_free; /* main-task stack the last CDC command left, bytes */

/* Around each command on the main task (CDC, BLE, SysEx, the WiFi remote):
 * hil_stack_mark() before it, hil_stack_note() after. The main task's stack
 * is internal RAM (CONFIG_ESP_MAIN_TASK_STACK_SIZE) and every verb runs inside
 * hil_control_handle_cdc()'s frame; CDC_STATS reports the least any command
 * left free and which verb it was -- the warning before the stack-end
 * watchpoint panics the board. */
void hil_stack_mark(void);
/* Returns the stack that command left free, in bytes. */
uint32_t hil_stack_note(const char *line, size_t len);

/* False while listKits() is walking the card (main.cpp: the "kit_scan" task, and
 * the rescan an SD reinsert starts). The scan grows the kit vector in place, so
 * a reader that iterates it mid-scan walks a moved buffer: KIT_LIST and KIT_LOAD
 * answer "ERR scanning" instead of racing it. */
extern std::atomic<bool> g_kit_scan_done;

#endif
