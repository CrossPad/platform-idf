#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runtime audio routing control, reachable over both transports:
 *
 * SysEx (works in CDC+MIDI and MIDI+UAC2 mode):
 *   F0 7D 1D 01 <codec> <input> F7   ADC input: 0=DIFF 1=LINE1 2=LINE2
 *   F0 7D 1D 02 <codec> F7           USB mic capture source (0|1)
 *   F0 7D 1D 03 <codec> <out> F7     DAC output: 1=LINE1 2=LINE2 3=ALL
 *   F0 7D 1D 04 <codec> <vol> F7     volume 0..100
 *   F0 7D 1D 05 <codec> <mute> F7    mute 0|1
 *   F0 7D 1D 10 F7                   query -> reply on the same MIDI:
 *   F0 7D 1D 10 <micSrc> <adc0> <adc1> <out0> <out1> <vol0> <vol1> <mute0> <mute1> F7
 *
 * CDC text (default mode only), mirrors the SysEx set:
 *   AUDIO_ADC <0|1> <DIFF|LINE1|LINE2>
 *   AUDIO_MIC_SRC <0|1>
 *   AUDIO_OUT <0|1> <LINE1|LINE2|ALL>
 *   AUDIO_VOL <0|1> <0-100>
 *   AUDIO_MUTE <0|1> <0|1>
 *   AUDIO_STATUS
 */

/* Wire-format codes, shared by SysEx, CDC and the settings UI. */
#define AUDIO_ROUTE_ADC_DIFF    0
#define AUDIO_ROUTE_ADC_LINE1   1
#define AUDIO_ROUTE_ADC_LINE2   2
#define AUDIO_ROUTE_DAC_LINE1   1
#define AUDIO_ROUTE_DAC_LINE2   2
#define AUDIO_ROUTE_DAC_ALL     3

/**
 * What the USB host records. The presets name signal paths this PCB actually
 * has (see tools/hil_speaker_acoustic.py for the measured map): codec0 LINE2
 * carries the built-in mics, LINE1 on both codecs is the DAC->ADC loop (codec1
 * at unity with the better SNR, codec0 at -24 dB), and codec1 LINE2 is the
 * external line-in jack.
 * CUSTOM is not applyable — it is what the state reports once the advanced
 * rows put the codecs somewhere no preset describes.
 */
typedef enum {
    AUDIO_ROUTE_PRESET_MICS = 0,      /* codec0 ADC LINE2 -> USB capture */
    AUDIO_ROUTE_PRESET_LINE_IN,       /* codec1 ADC LINE2 (line-in jack) */
    AUDIO_ROUTE_PRESET_SPEAKER_BUS,   /* codec0 ADC LINE1 (speaker DAC loop) */
    AUDIO_ROUTE_PRESET_HEADPHONE_BUS, /* codec1 ADC LINE1 (headphone DAC loop) */
    AUDIO_ROUTE_PRESET_MIX,           /* codec0 + codec1 LINE2 summed in the USB bridge (mics + line-in) */
    AUDIO_ROUTE_PRESET_SAMPLER,       /* what the pads play: speaker DAC loop, with the RT mixer resumed */
    AUDIO_ROUTE_PRESET_COUNT,
    AUDIO_ROUTE_PRESET_CUSTOM = 0x7F,
} audio_route_preset_t;

/* Capture source value meaning "both codecs, summed" — the USB stream stays
 * stereo (the ESP32-S3 USB FIFO cannot carry a 4-channel isochronous IN
 * endpoint next to MIDI and feedback), so the mix is made on the board. */
#define AUDIO_ROUTE_CAPTURE_MIX 2
/* The pads-only mixer bus: what the sampler plays, without the host's playback.
 * Digital, so nothing of a DAW's monitoring returns into the recording. */
#define AUDIO_ROUTE_CAPTURE_SAMPLER 3

typedef struct {
    uint8_t captureCodec;   /* codec feeding the UAC2 mic endpoint (0|1) */
    uint8_t adcInput[2];    /* AUDIO_ROUTE_ADC_* per codec */
    uint8_t dacOutput[2];   /* AUDIO_ROUTE_DAC_* per codec */
    uint8_t volume[2];      /* 0..100 per codec */
    uint8_t mute[2];        /* 0|1 per codec */
} audio_route_state_t;

/* Direct setters. The SysEx and CDC handlers are thin wrappers over these, so
 * a GUI calling them lands in exactly the same state a HIL script would.
 * All return false on an out-of-range argument or a codec write failure. */
bool audio_route_set_capture_codec(uint8_t codec);
bool audio_route_set_adc_input(uint8_t codec, uint8_t input);

/* Something listening to a codec's own input -- the one its IN channel
 * carries in the input pair (InputPairPort): the mics, a line jack -- holds
 * its ADC there whatever the routing chose; the choice comes back when the
 * last claim goes. The state query reports the input the codec is on. */
#define AUDIO_ADC_CLAIM_TOOLS    0x01   /* the Recorder */
#define AUDIO_ADC_CLAIM_MONITOR  0x02   /* a mixer send of the IN channel */
bool audio_route_claim_adc(uint8_t codec, uint8_t who, bool on);
/* The codecs were (re)initialised, which leaves both ADCs on LINE1: the
 * shadow says so again and the claims made meanwhile are applied. */
void audio_route_codecs_started(void);
bool audio_route_set_dac_output(uint8_t codec, uint8_t out);
bool audio_route_set_volume(uint8_t codec, uint8_t volume);
bool audio_route_set_mute(uint8_t codec, uint8_t mute);

/** @brief Snapshot the current routing. Never fails; volumes are read back
 *         from the codec, the write-mostly fields from the shadow state. */
void audio_route_get_state(audio_route_state_t *out);

/** @brief Apply one preset (capture codec + its ADC input). Returns false for
 *         AUDIO_ROUTE_PRESET_CUSTOM or an unknown value, and for a bus preset
 *         while the input pair holds the DAC loop off (Line 3/4 is on LINE1). */
bool audio_route_apply_preset(audio_route_preset_t preset);

/** Gain on the pads-only capture bus, in dB above unity (0 = what the speakers
 *  get). The digital tap has none of the codec ADC's input gain, so a take is
 *  quiet next to the old analogue capture; recording gets its own knob because
 *  raising the channel would raise the monitor path with it. The mixer clamps
 *  its output, so overdriving distorts rather than wrapping. */
bool audio_route_set_daw_gain(uint8_t db);

/** @brief Which preset the current state corresponds to, or
 *         AUDIO_ROUTE_PRESET_CUSTOM when it matches none. */
audio_route_preset_t audio_route_get_preset(void);

/**
 * @brief Register the CROSSPAD_CMD_AUDIO_ROUTE SysEx handler with the
 *        Stm32MessageHandler. Call once after CrosspadPlatform_Init.
 */
void audio_route_control_init(void);

/**
 * @brief Set the function used to send a SysEx reply (query response).
 * @param fn Sender taking a full frame (F0..F7) and its length.
 */
void audio_route_control_set_reply_sender(void (*fn)(const uint8_t *frame, size_t len));

/**
 * @brief Handle a CDC text command. Returns true when the buffer matched an
 *        AUDIO_* command (reply, if any, is written back over CDC).
 */
bool audio_route_control_handle_cdc(const char *buff, size_t len);

#ifdef __cplusplus
}
#endif
