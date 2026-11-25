/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/**
 * @file midi_kit_pc.h
 * @brief A host's MIDI Program Change (with Bank Select) selects the kit.
 *
 * Off by default until the human decides the shipping default; switched by
 * `KIT_PC ON|OFF|STATUS|MODE|CH|MAP` over CDC (and refused with a reason on the
 * console, never silently). The decision itself -- what a program number names --
 * is the pure part, in main/include/kit_pc.h, pinned by main/tests.
 *
 * Threading contract, which is the whole point of this file:
 *  - The MIDI callbacks (uartLoop task) may call kit_change_program() and
 *    kit_change_control_change(). They park the newest pair in atomics and post
 *    one async apply; they never read the kit model, never take a lock, never
 *    wait. A board whose pads are running and a host blasting program changes
 *    must not gain a stall from this feature.
 *  - The kit model is the LVGL thread's (kit_load_task.cpp:17-19,
 *    hil_control.cpp:394-396), so the lookup happens on the LVGL thread, in the
 *    same place kit_load_task_start() is allowed to be called from.
 *  - The verb runs on the main task like every other CDC verb.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KIT_PC_OFF = 0,   /* Program Change does what it did before: nothing local */
    KIT_PC_FLAT = 1,  /* program + bank MSB = the position in the kit listing */
    KIT_PC_GROUP = 2, /* bank MSB = the kind, program = the position inside it */
} kit_pc_mode_t;

/* Read the saved setting. Call before midiCallbacks_init(): the MIDI task starts
 * there and a change may arrive at once. */
void kit_change_init(void);

/* Bank Select CC 0 (MSB) and CC 32 (LSB), latched per channel. @p ch is the
 * MIDI library's channel, 1..16; other controllers pass through untouched. */
void kit_change_control_change(uint8_t ch, uint8_t cc, uint8_t val);

/* A Program Change (0xC0-0xCF). Same 1..16 channel as above: this module owns
 * the 0-based conversion the rest of the core API expects, so a call site cannot
 * get it wrong the way the existing handleSample* forwards did. */
void kit_change_program(uint8_t ch, uint8_t program);

kit_pc_mode_t kit_change_mode(void);
void kit_change_set_mode(kit_pc_mode_t mode); /* persisted */
int kit_change_channel(void);                 /* -1 = any channel, else 0..15 */
void kit_change_set_channel(int ch);          /* persisted, -1 accepted */
const char *kit_change_mode_name(kit_pc_mode_t mode);

/* Observability, because "the host sent a program change and nothing happened"
 * is the failure this feature will be blamed for. Every counter is one the
 * board can be asked for; the last decision is kept with its reason. */
uint32_t kit_change_count(int which); /* 0 seen, 1 loaded, 2 refused, 3 ignored */
int kit_change_last_kit(void);        /* the kit the last decision named, or -1 */
const char *kit_change_last_why(void);

/* KIT_PC STATUS into out, NUL-terminated; returns the length written. */
int kit_change_status_line(char *out, size_t len);
/* One row of the effective program -> kit map at `position` of the current
 * mode's ordering, as one line without the end-of-line: 1 when the row exists,
 * 0 when `out` was left empty (past the end, or the line does not fit). */
int kit_change_map_row(int position, char *out, size_t len);
/* How many rows the current mode has at all (KITPCMAP: end's total=). */
int kit_change_map_total(void);

/* The other direction: the board telling a host which kit it is on. A person
 * picking a kit at the panel, the Sampler loading one, a kit load through any
 * door that is not Program Change -- the board answers with the Bank Select MSB
 * and Program Change a host would have sent for that kit, in the current mode's
 * own addressing, so a DAW's kit row follows what the fingers did.
 *
 * `kit_pc_check_announce()` is the cheap half and may run on a periodic worker
 * job: it compares the current kit id with the last announced one and posts the
 * rest to the LVGL thread, because the kit listing is the LVGL thread's to read
 * (the contract at the top of this file). A kit the host reached by Program
 * Change is seeded as announced, so the board does not answer a host with what
 * it was just told.
 *
 * Switchable (`KIT_PC ECHO on|off`, on by default, not persisted -- the same
 * shape as the Control Change map) because a host with its own kit page does not
 * always want the board writing to it. The LSB (CC 32) is not sent: neither mode
 * uses it in its addressing (kit_pc.h). */
/* A Bank Select MSB + Program pair aimed at the kit loader, from a door that is not
 * a Program Change -- the SysEx surface writes both in one frame (`KIT_PC MAP` prints
 * the pair, the surface writes it back). Latched bank values are not consulted: the
 * frame carries its own bank, which is what makes a setlist of SysEx writes
 * self-contained. */
void kit_change_pair(uint8_t ch1, uint8_t msb, uint8_t program);

int kit_pc_check_announce(void);   /* 1 when an announcement was posted */
void kit_pc_set_echo(int on);
int kit_pc_echo(void);             /* 1 = announcing */
int kit_pc_announced(void);        /* the kit id last announced, -1 = none yet */
uint32_t kit_pc_announce_count(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
