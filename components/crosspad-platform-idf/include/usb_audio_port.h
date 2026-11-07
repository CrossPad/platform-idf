// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file usb_audio_port.h
 * @brief C side of the USB audio port: what the UAC tasks call.
 *
 * The UAC speaker/mic tasks never touch I2S. They copy between TinyUSB's
 * FIFOs and two lock-free rings here; the audio RT task is the only I2S
 * writer and reader on the board. That is what keeps host playback and the
 * pads from interleaving blocks on one DMA ring, and what lets the UAC tasks
 * be deleted at any instant without holding an I2S channel mutex.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_AUDIO_PORT_CAPTURE_MIX 2
/* The mixer bus carrying only the pads (sampler + pitched); no ADC involved. */
#define USB_AUDIO_PORT_CAPTURE_SAMPLER 3

/** Bridge init/deinit: rings are (re)sized for the engine rate and reset. */
void usb_audio_port_start(uint32_t sample_rate, uint32_t engine_frames);
void usb_audio_port_stop(void);
/* The render's block changed (Settings -> Audio -> Buffer). */
void usb_audio_port_set_engine_frames(uint32_t engine_frames);
/* The block the render makes now (IdfAudioModule.cpp); 0 before it started. */
uint32_t audio_rt_block_frames(void);
bool usb_audio_port_active(void);

/** Host → board. Returns frames accepted; the rest stays in the USB FIFO so
 *  the feedback endpoint slows the host down instead of us dropping audio. */
size_t usb_audio_port_playback_write(const int16_t *stereo, size_t frames);
size_t usb_audio_port_playback_space(void);
size_t usb_audio_port_playback_capacity(void);

/** Board → host. Returns frames delivered; the caller zero-fills the rest. */
size_t usb_audio_port_capture_read(int16_t *stereo, size_t frames);
/** The host opened (alt != 0) or closed the mic stream: the render writes the
 *  capture ring only while it is open, the mic task starts it over on open. */
void usb_audio_port_capture_open(bool open);
/** Mic reads the ring could not fill (zero-filled, then the stream re-primes). */
uint32_t usb_audio_port_capture_underruns(void);

/** 0 = codec 0 (mics), 1 = codec 1 (line in), USB_AUDIO_PORT_CAPTURE_MIX = both,
 *  USB_AUDIO_PORT_CAPTURE_SAMPLER = the pads-only mixer bus. */
void usb_audio_port_set_capture_source(int source);
int  usb_audio_port_get_capture_source(void);

/** Counters for the bench: playback underruns (ring empty at render),
 *  playback overruns (host faster than the engine, frames refused), capture
 *  overruns (frames the render could not write while the mic was open: the
 *  mic task slower than the engine). */
void usb_audio_port_stats(uint32_t *play_underrun, uint32_t *play_refused,
                          uint32_t *cap_dropped, size_t *play_level, size_t *cap_level);

/** Fewest frames the render found in the playback ring since the previous
 *  call (UINT32_MAX: no render read it while streaming), then reset. The
 *  margin the stream had left before an underrun. */
uint32_t usb_audio_port_play_min(void);

/** A host is playing into the board (the stream is up and past its prefill). */
bool usb_audio_port_playback_streaming(void);

/** Frames the host has delivered and the render has taken from the playback
 *  ring since boot (wrapping counters): their rates, read twice. */
void usb_audio_port_flow(uint32_t *host, uint32_t *taken);
/** Runs of 32+ silent frames in the host's stream as received and as the render played it. */
void usb_audio_port_zero_runs(uint32_t *host, uint32_t *played);

/** The playback resampler's range and gain (ppm off the host's rate, ppm per
 *  frame the ring is off its target); per host, usb_host_profile.cpp. */
void usb_audio_port_set_asrc(float max_ppm, float ppm_per_frame);
/** The playback resampler's last ratio, ppm off the host's rate (render). */
int32_t usb_audio_port_asrc_ppm(void);
uint32_t usb_audio_port_asrc_cyc_max(void);
uint32_t usb_audio_port_asrc_avg(int which);

#ifdef __cplusplus
}
#endif
