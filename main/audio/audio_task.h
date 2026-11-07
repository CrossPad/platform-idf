/**
 * @file audio_task.h
 * @brief Declarations of the audio tasks control
 */

#ifndef MAIN_AUDIO_TASK_H_
#define MAIN_AUDIO_TASK_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Who reads the longest render pass. Each has a read-and-clear copy of its
 *  own: one reader clearing the peak must not hide it from another. */
enum {
    AUDIO_RT_BUSY_SPK_LINE,      /* usb_audio_bridge.c, 'SPK #' */
    AUDIO_RT_BUSY_ENGINE_LINE,   /* sample_player_port.cpp, the engine's error line */
    AUDIO_RT_BUSY_SMPL_PEAK,     /* hil_control.cpp, SMPL_PEAK */
    AUDIO_RT_BUSY_PITCHED,       /* hil_control.cpp, PITCHED_STATUS */
    AUDIO_RT_BUSY_GUI,           /* IdfGuiPlatform.hpp, the status bar's DSP readout */
    AUDIO_RT_BUSY_BAR,           /* IdfGuiPlatform.hpp, the transport bar's CPU readout */
    AUDIO_RT_BUSY_READERS
};

/** The longest render pass since @p reader last asked, in µs: the pass less
 *  its I2S waits (room in TX, and in the USB audio profile the codec reads).
 *  With @p idle1Pct, also core 1's idle share since the previous call that
 *  asked for it -- one caller does, the SPK line. */
uint32_t audio_rt_busy_max(int reader, uint32_t *idle1Pct);

/** The average render pass since the previous call, in µs (the same measure
 *  as audio_rt_busy_max()); 0 with no pass since. One caller: the transport
 *  bar's CPU readout. */
uint32_t audio_rt_busy_avg(void);

/** Take the render off the outputs (true) -- a pass faded to silence, then
 *  passes of the clock and the pads tap only, as fast as the capture's writer
 *  and the streamer keep up (a freeze off the clock) -- or put it back
 *  (false): the clock on the DMA again, faded in. Waits up to @p timeout_ms
 *  for the render to have switched; false when it did not. */
bool audio_rt_offline(bool on, uint32_t timeout_ms);

/** One render block in µs (frames over the rate), what a pass has to fit in;
 *  0 until the RT task runs. */
uint32_t audio_rt_block_us(void);

/** Render passes busy for longer than a block, since boot: each one a block
 *  the DMA got late. */
uint32_t audio_rt_overruns(void);

#ifdef __cplusplus
}
#endif


/**
 * @brief Initialize and start all audio-related tasks.
 *
 * Creates both real-time and background audio tasks
 * pinned to dedicated cores.
 */
void AudioTasks_Start(void);

/**
 * @brief True once the RT task has both codecs up and is rendering.
 *
 * The splash holds until this (bounded), so the launcher appears with the
 * pads already audible.
 */
bool AudioTasks_IsReady(void);

/**
 * @brief Request all audio tasks to stop.
 *
 * Tasks will terminate gracefully.
 */
void AudioTasks_Stop(void);

/**
 * @brief Suspend/resume the RT audio pipeline (mixer render + I2S access).
 *
 * While suspended the RT task idles and touches no I2S channel. Required in
 * USB audio mode: the USB audio bridge needs exclusive access to the I2S
 * RX/TX channels — concurrent mixer reads/writes corrupt both directions.
 */
void AudioTasks_SetSuspended(bool suspended);


#endif /* MAIN_AUDIO_TASK_H_ */
