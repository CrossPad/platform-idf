/**
 * @file sample_player_port.hpp
 * @brief The sample player's file port (main/sample_player_port.cpp): what the
 *        engines, the loaders and every other reader of the card call, and the
 *        counters the HIL verbs print.
 */
#pragma once

#include <atomic>
#include <cstdint>

/* ── The card, shared with the streamer ─────────────────────────────────── */

/* Makes a bulk card reader or writer give way to the streamer while pads are
 * sounding; every such reader calls it between pieces of work. weight 1 = the
 * kit loader; a cosmetic reader passes more and waits longer. */
void sample_player_port_yield_to_streamer(uint32_t weight);
void sample_player_port_stream_let_go(void);
void sample_player_port_pass_begin(void);
void sample_player_port_pass_end(uint32_t passUs);

/* The board's one 4 kB DMA-capable stage, a piece at a time (NULL: none);
 * urgent, the streamer steps aside while this waits. */
void     sample_player_port_stage_init(void);
uint8_t *sample_player_port_stage_take(bool urgent);
void     sample_player_port_stage_give(void);
bool     sample_player_port_direct_io(void);

void sample_player_port_flush_files(void);
void sample_player_port_forget_file(const char *path);
void sample_player_port_forget_dir(const char *dir);     /* the file cache's handles under a folder closed */
void sample_player_port_forget_blocks(void);
void sample_player_port_close_library_files(void);
void sample_player_port_report_errors(void);

/* ── The prebuilt engine ────────────────────────────────────────────────── */

void    sample_player_port_engine_started(void);
void    sample_player_port_set_poly_cap(uint8_t cap);
uint8_t sample_player_port_poly(void);

bool sample_player_pad_sounding(uint8_t pad);
void sample_player_pad_starting(uint8_t pad);
void sample_player_pad_forget_voices(uint8_t pad);

/* ── Counters ───────────────────────────────────────────────────────────── */

/* Who reaches into the file port, by task. Every open, seek and read is
 * meant to come from audio_bg (streaming) or a mutator holding the file
 * mutex; a caller outside those two is a race with the stream. */
struct PortCallers { uint32_t audio_bg; uint32_t other; char lastOther[16]; };
extern PortCallers g_port_opens, g_port_seeks, g_port_closes, g_port_reads;

extern uint32_t g_port_defer;        /* handles the RT task handed off for closing */
extern uint32_t g_port_defer_lost;   /* no room to park: descriptor leaked rather than closed on RT */
extern uint32_t g_port_evict;        /* cache slots recycled */
extern uint32_t g_port_staged, g_port_stage_fallback;   /* since boot */
extern uint32_t g_port_stream_staged, g_port_stream_fb, g_port_stream_read_max_us;
extern uint32_t g_port_pf_hits, g_port_pf_misses;       /* since boot */
extern std::atomic<uint32_t> g_port_stream_fopen;    /* real fopen() calls the streamer had to make */
extern std::atomic<uint32_t> g_port_fopen_max_us;    /* slowest fopen() since the swap log last read it */
extern std::atomic<uint32_t> g_port_stream_bytes;    /* bytes the streamer read off the card */
extern std::atomic<uint32_t> g_port_mutator_bytes;   /* bytes the loader (and other mutators) read */
extern std::atomic<uint32_t> g_port_pace_ms;
extern std::atomic<bool>     g_port_loader_hurry;

extern std::atomic<uint32_t> g_engine_noblock;    /* "no free block" reports (one per starved buffer) */
extern std::atomic<uint32_t> g_engine_underrun;   /* "buffer underrun" reports */

/* What the library's memory hooks have moved, so DeInit's semantics can be
 * read off the console rather than assumed. */
extern uint32_t g_marcel_malloc_bytes, g_marcel_malloc_count;
extern uint32_t g_marcel_fast_bytes, g_marcel_fast_internal_bytes;
extern uint32_t g_marcel_free_bytes, g_marcel_free_count;
extern uint32_t g_marcel_free_untracked;   /* freed without a recorded size */
extern uint32_t g_marcel_blocks_lost;      /* table full: size not remembered */

/* Last eight events the engine played, for SMPL_LAST over CDC — listening
 * cannot tell "the pad below sounds" from "the wrong note arrived". */
struct EngineNoteEvent { uint8_t note; uint8_t wavIdx; uint32_t ms; };
extern EngineNoteEvent g_engine_last[8];
extern std::atomic<uint32_t> g_engine_last_count;

/* Pad → engine latency: the request stamps the note on hand-off, NoteOnCb
 * fires when the voice starts. Nothing else on the chain measures it.
 * 32-bit throughout -- the voice start that reads them is in the render, and a
 * 64-bit atomic is not lock-free on this core. The stamp is esp_timer's low
 * 32 bits (0 = none; a stamp that lands on 0 is written as 1), and the
 * difference of two is right across the 71-minute wrap. */
extern std::atomic<uint32_t> g_note_req_us[128];
extern std::atomic<uint32_t> g_note_lat_last_us, g_note_lat_max_us, g_note_lat_count;
extern std::atomic<uint32_t> g_note_lat_sum_us;
