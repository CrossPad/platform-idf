/**
 * @file audio_rt.h
 * @brief The RT audio task's controls and counters that cross between main/
 *        (the task, audio/audio_task.cpp) and this component (the I2S module,
 *        IdfAudioModule.cpp). What only main/ uses is in main/audio/audio_task.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── IdfAudioModule.cpp ─────────────────────────────────────────────────── */

void     audio_rt_set_queue_frames(uint32_t frames);
uint32_t audio_rt_queue_frames(void);
bool     audio_rt_queue_supported(uint32_t block, uint32_t queue);
/* The block the render makes now; 0 before it started. */
uint32_t audio_rt_block_frames(void);
/* Whether the pass that just ran handed its block to an output that will
 * make the next one wait. */
bool     audio_rt_pass_paced(void);
uint32_t audio_rt_unpaced_passes(void);
void     audio_rt_last_write(uint32_t *write_done_us, uint32_t *dac_us);
uint32_t audio_rt_pacer_wait_us(void);
uint32_t audio_rt_pacer_resyncs(void);
void     audio_rt_pacer_restart(void);
uint32_t audio_rt_tx_underruns(void);
uint32_t audio_rx_skips(void);
void     audio_rt_fade_next(int dir);
void     audio_rt_prof_enable(bool on);
void     audio_rt_prof_read(char *out, uint32_t cap);
void     audio_mixer_prof_enable(bool on, void *mixer);
void     audio_mixer_prof_read(void *mixer, char *out, uint32_t cap);

/* ── main/audio/audio_task.cpp ──────────────────────────────────────────── */

bool audio_rt_park(bool park, uint32_t timeout_ms);
void audio_rt_note_live(void);
bool audio_rt_live_recently(void);
bool audio_rt_pressed_recently(void);

/** Take the render off the outputs (true) -- a pass faded to silence, then
 *  passes of the clock and the pads tap only, as fast as the capture's writer
 *  and the streamer keep up (a freeze off the clock) -- or put it back
 *  (false): the clock on the DMA again, faded in. Waits up to @p timeout_ms
 *  for the render to have switched; false when it did not. */
bool audio_rt_offline(bool on, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */

#include <atomic>

extern std::atomic<uint32_t> g_rt_gap_max_us;    /* audio_task.cpp; read-and-clear by the swap log */
extern std::atomic<uint32_t> g_rt_overruns;      /* audio_task.cpp; passes busy for longer than a block, since boot */
extern std::atomic<uint32_t> g_bg_pass_max_us;   /* audio_module.cpp; read-and-clear by the swap log */
extern std::atomic<uint32_t> g_bg_pass_last_us;  /* audio_module.cpp; the loader paces itself on it */
extern std::atomic<uint32_t> g_bg_pass_mark_us;  /* audio_module.cpp; where the streamer's pass is */
#endif
