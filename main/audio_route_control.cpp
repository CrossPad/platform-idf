#include "audio_route_control.h"

#include "bsp_es8388.h"
#include "crosspad_s3.h"
#include "usb_audio_bridge.h"
#include "audio/audio_task.h"
#include "crosspad/protocol/SysExProtocol.h"
#include "CrosspadPlatform.hpp"
#include "IdfI2sInput.hpp"
#include "hil_control.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "NvsSettingsStore.hpp"
/* Same detection CrosspadPlatform.cpp uses -- the mixer is an optional
 * component, and IDF_HAS_MIXER is a file-local macro there, not a build-wide
 * define, so it has to be re-derived here. */
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "IdfAudioModule.hpp"
#define CROSSPAD_ROUTE_HAS_MIXER 1
#endif

#include "esp_log.h"
#include "tusb_cdc_acm.h"
#include "freertos/FreeRTOS.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>

static const char *TAG = "audio_route";

/* Subcommands of CROSSPAD_CMD_AUDIO_ROUTE */
#define AUDIO_ROUTE_SUB_ADC_INPUT   0x01
#define AUDIO_ROUTE_SUB_MIC_SRC     0x02
#define AUDIO_ROUTE_SUB_DAC_OUTPUT  0x03
#define AUDIO_ROUTE_SUB_VOLUME      0x04
#define AUDIO_ROUTE_SUB_MUTE        0x05
#define AUDIO_ROUTE_SUB_TASKS       0x06
/* HIL pad stimulation — same path as a physical pad (pad logic + LEDs +
 * sample playback). Lives under 0x1D because SysEx is the only control
 * channel in USB audio mode (no CDC) and this avoids a new core command. */
#define AUDIO_ROUTE_SUB_PAD_PRESS   0x07
#define AUDIO_ROUTE_SUB_PAD_RELEASE 0x08
/* HIL echo — reply mirrors the 4x7-bit payload (sequence number). Lets a
 * benchmark attribute replies to the injection path (ESP native USB vs STM
 * bridge) and measure per-message latency. Reply goes out the ESP native
 * MIDI TX like every audio-route reply. */
#define AUDIO_ROUTE_SUB_ECHO        0x09
#define AUDIO_ROUTE_SUB_PREVIEW     0x0A   /* pad: audition its first sample (kit-browser preview path) */
#define AUDIO_ROUTE_SUB_KIT_LOAD    0x0B   /* kit id: what CDC KIT_LOAD does, for the profile without CDC */
#define AUDIO_ROUTE_SUB_BOUNCE      0x0C   /* scene, track: CDC ASONG BOUNCE, so a bounce can run beside the
                                            * phone's stream; ASONG (back in CDC) reads how it went */
#define AUDIO_ROUTE_SUB_QUERY       0x10

static void (*s_reply_sender)(const uint8_t *frame, size_t len) = nullptr;

/* Wire-format indices <-> BSP enums. State is tracked here because the codec
 * registers are write-mostly and the BSP exposes no readback for mute/input. */
/* Seeded to what bsp_i2s_es8388_init() configures, so a query before anything
 * was routed reports the truth rather than the enum's zero value. */
static uint8_t s_adc_input[2] = {1, 1};   /* 0=DIFF 1=LINE1 2=LINE2 */
/* s_adc_input is what the routing chose -- a preset, Settings, SysEx, CDC. A
 * claim holds the codec's ADC on its own input (LINE2: the mics, the jack)
 * over that choice for as long as something listens to the input itself: the
 * Recorder, or a mixer send of the IN channel. A DAC loop there would put the
 * outputs back into what is heard or recorded. */
static uint8_t s_adc_claims[2] = {0, 0};
/* Each codec's own input -- what its IN channel carries -- as the input pair
 * (InputPairPort) chose. On BSP_HAS_SHARED_LINE_IN boards Line 3/4 is LINE1
 * of both codecs, the net an output pair of BSP_CODEC_LOOP_CODEC also drives
 * (the loop the bus presets record): a pair with Line 3/4 powers that output
 * pair down and puts it in hi-Z. */
static uint8_t s_own_input[2] = {AUDIO_ROUTE_ADC_LINE2, AUDIO_ROUTE_ADC_LINE2};
static bool s_loop_on = true;
static std::mutex s_adc_mutex;       // Settings, CDC, the Recorder, the monitor job and the input pair
static uint8_t s_dac_output[2] = {3, 3};  /* 1=LINE1 2=LINE2 3=ALL */
static uint8_t s_mute[2] = {0, 0};

static const bsp_es8388_adc_input_t k_adc_map[3] = {
    BSP_ES8388_ADC_INPUT_DIFFERENCE,
    BSP_ES8388_ADC_INPUT_LINE1,
    BSP_ES8388_ADC_INPUT_LINE2,
};

static const bsp_es8388_dac_output_t k_dac_map[4] = {
    BSP_ES8388_DAC_OUTPUT_ALL,   /* index 0 unused; keeps 1-based codes */
    BSP_ES8388_DAC_OUTPUT_LINE1,
    BSP_ES8388_DAC_OUTPUT_LINE2,
    BSP_ES8388_DAC_OUTPUT_ALL,
};

static uint8_t adc_effective(uint8_t codec)
{
    return s_adc_claims[codec] ? s_own_input[codec] : s_adc_input[codec];
}

/* Caller holds s_adc_mutex. The IN channel hears its codec only while the
 * ADC is on the channel's own input -- not the loop, not the other jack. */
static bool adc_apply(uint8_t codec, uint8_t input)
{
    if (bsp_es8388_config_adc_input((bsp_i2s_interface_t)codec, k_adc_map[input]) != ESP_OK) return false;
    crosspad::IdfI2sInput::setLive(codec, input == s_own_input[codec]);
    return true;
}

/* Caller holds s_adc_mutex. The output drivers asked for, less the loop pair
 * while it is off -- then in hi-Z too: at 1.5 kOhm through its 1 uF it is a
 * low-pass for the source on the jack (~3 kHz from a 50 Ohm output). */
static bool dac_apply(uint8_t codec)
{
    uint8_t bits = (uint8_t)k_dac_map[s_dac_output[codec]];
    const bool loop_codec = codec == BSP_CODEC_LOOP_CODEC && BSP_CODEC_LOOP_DAC_OUTPUT;
    if (loop_codec && !s_loop_on) bits &= (uint8_t)~BSP_CODEC_LOOP_DAC_OUTPUT;
    if (bsp_es8388_config_dac_output((bsp_i2s_interface_t)codec, (bsp_es8388_dac_output_t)bits) != ESP_OK) return false;
    return !loop_codec || bsp_es8388_set_off_outputs_hiz((bsp_i2s_interface_t)codec, !s_loop_on) == ESP_OK;
}

bool audio_route_set_adc_input(uint8_t codec, uint8_t input)
{
    if (codec > 1 || input > 2) return false;
    std::lock_guard<std::mutex> l(s_adc_mutex);
    const uint8_t was = s_adc_input[codec];
    s_adc_input[codec] = input;
    if (!adc_apply(codec, adc_effective(codec))) {
        s_adc_input[codec] = was;
        return false;
    }
    ESP_LOGI(TAG, "codec%d ADC input -> %d%s", codec, input, s_adc_claims[codec] ? " (held on LINE2 while claimed)" : "");
    return true;
}

void audio_route_codecs_started(void)
{
    std::lock_guard<std::mutex> l(s_adc_mutex);
    for (uint8_t c = 0; c < 2; ++c) {
        s_adc_input[c] = AUDIO_ROUTE_ADC_LINE1;     // where the init left it
        dac_apply(c);
        adc_apply(c, adc_effective(c));
    }
}

/* crosspad-platform-idf's InputPairPort: the pair it connects puts each
 * codec's own input here. */
extern "C" bool input_pair_route(uint8_t in0, uint8_t in1)
{
    if (in0 > AUDIO_ROUTE_ADC_LINE2 || in1 > AUDIO_ROUTE_ADC_LINE2) return false;
    std::lock_guard<std::mutex> l(s_adc_mutex);
    const bool loop = !BSP_HAS_SHARED_LINE_IN || (in0 != AUDIO_ROUTE_ADC_LINE1 && in1 != AUDIO_ROUTE_ADC_LINE1);
    /* The loop lets go of LINE1 before an ADC listens to the jack there, and
     * drives it again only once none does. Before the codecs are up the
     * writes fail; audio_route_codecs_started() applies all of it. */
    bool ok = true;
    if (!loop && s_loop_on) {
        s_loop_on = false;
        ok = dac_apply(0) && ok;
        ok = dac_apply(1) && ok;
    }
    s_own_input[0] = in0;
    s_own_input[1] = in1;
    for (uint8_t c = 0; c < 2; ++c) ok = adc_apply(c, adc_effective(c)) && ok;
    if (loop && !s_loop_on) {
        s_loop_on = true;
        ok = dac_apply(0) && ok;
        ok = dac_apply(1) && ok;
    }
    ESP_LOGI(TAG, "own inputs [%d,%d], DAC loop %s", in0, in1, s_loop_on ? "on" : "off");
    return ok;
}

bool audio_route_claim_adc(uint8_t codec, uint8_t who, bool on)
{
    if (codec > 1) return false;
    std::lock_guard<std::mutex> l(s_adc_mutex);
    const uint8_t before = adc_effective(codec);
    s_adc_claims[codec] = on ? (uint8_t)(s_adc_claims[codec] | who) : (uint8_t)(s_adc_claims[codec] & ~who);
    const uint8_t after = adc_effective(codec);
    if (after == before) return true;
    ESP_LOGI(TAG, "codec%d ADC -> %d (claims 0x%x)", codec, after, s_adc_claims[codec]);
    /* Before the codecs are up this fails; audio_route_codecs_started()
     * applies the claim once they are. */
    return adc_apply(codec, after);
}

bool audio_route_set_capture_codec(uint8_t codec)
{
    /* 0 / 1 pick a codec; AUDIO_ROUTE_CAPTURE_MIX sums both in the bridge;
     * AUDIO_ROUTE_CAPTURE_SAMPLER taps the pads-only mixer bus instead. */
    if (codec > AUDIO_ROUTE_CAPTURE_SAMPLER) return false;
    return usb_audio_bridge_set_capture_codec(codec) == ESP_OK;
}

bool audio_route_set_dac_output(uint8_t codec, uint8_t out)
{
    if (codec > 1 || out < 1 || out > 3) return false;
    std::lock_guard<std::mutex> l(s_adc_mutex);
    const uint8_t was = s_dac_output[codec];
    s_dac_output[codec] = out;
    if (!dac_apply(codec)) {
        s_dac_output[codec] = was;
        return false;
    }
    ESP_LOGI(TAG, "codec%d DAC output -> %d%s", codec, out, s_loop_on ? "" : " (loop pair held off)");
    return true;
}

bool audio_route_set_volume(uint8_t codec, uint8_t volume)
{
    if (codec > 1 || volume > 100) return false;
    if (bsp_es8388_set_volume((bsp_i2s_interface_t)codec, volume) != ESP_OK) return false;

    /* The same field AudioControl_Setup() restores at boot, and the same one
     * Settings -> Audio writes: a volume set from anywhere else used to live
     * only in the codec register and was back at its old value after a reset.
     * The store write is left to the caller — a SysEx fader sweep must not
     * write NVS per message. */
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    if (cfg) cfg->masterFX.outVolumeCodec[codec][0] = volume;
    return true;
}

bool audio_route_set_mute(uint8_t codec, uint8_t mute)
{
    if (codec > 1 || mute > 1) return false;
    if (bsp_es8388_set_mute((bsp_i2s_interface_t)codec, mute != 0) != ESP_OK) {
        return false;
    }
    s_mute[codec] = mute;
    return true;
}

static void get_state(uint8_t out[9])
{
    int vol0 = 0, vol1 = 0;
    bsp_es8388_get_volume((bsp_i2s_interface_t)0, &vol0);
    bsp_es8388_get_volume((bsp_i2s_interface_t)1, &vol1);
    out[0] = (uint8_t)usb_audio_bridge_get_capture_codec();
    {
        std::lock_guard<std::mutex> l(s_adc_mutex);
        out[1] = adc_effective(0);        // what the codec is on, claims included
        out[2] = adc_effective(1);
    }
    out[3] = s_dac_output[0];
    out[4] = s_dac_output[1];
    out[5] = (uint8_t)vol0;
    out[6] = (uint8_t)vol1;
    out[7] = s_mute[0];
    out[8] = s_mute[1];
}

void audio_route_get_state(audio_route_state_t *out)
{
    if (!out) return;
    uint8_t raw[9];
    get_state(raw);
    out->captureCodec = raw[0];
    out->adcInput[0]  = raw[1];
    out->adcInput[1]  = raw[2];
    out->dacOutput[0] = raw[3];
    out->dacOutput[1] = raw[4];
    out->volume[0]    = raw[5];
    out->volume[1]    = raw[6];
    out->mute[0]      = raw[7];
    out->mute[1]      = raw[8];
}

/* A preset is (capture codec, that codec's ADC input). The other codec's input
 * is left alone — nothing reads it while it is not the capture source, and
 * clobbering it would undo an advanced-row choice for no gain. */
struct RoutePreset { uint8_t codec; uint8_t input; bool needsMixer; };
static const RoutePreset k_presets[AUDIO_ROUTE_PRESET_COUNT] = {
    { 0, AUDIO_ROUTE_ADC_LINE2, false },  /* MICS */
    { 1, AUDIO_ROUTE_ADC_LINE2, false },  /* LINE_IN */
    { 0, AUDIO_ROUTE_ADC_LINE1, false },  /* SPEAKER_BUS */
    { 1, AUDIO_ROUTE_ADC_LINE1, false },  /* HEADPHONE_BUS */
    { AUDIO_ROUTE_CAPTURE_MIX, AUDIO_ROUTE_ADC_LINE2, false },  /* MIX: both jacks, summed */
    /* SAMPLER records the mixer's pads-only bus digitally -- no ADC, no DAC
     * loop -- so the host's playback cannot return into the take. Its ADC input
     * field is unused. It still needs the RT mixer, which the USB-audio profile
     * parks: without a render there is no bus to read. */
    { AUDIO_ROUTE_CAPTURE_SAMPLER, 0, true },  /* SAMPLER */
};

/* SAMPLER and SPEAKER_BUS share a codec/input signature, so the state alone
 * cannot tell them apart; remember what was asked for and only fall back to the
 * scan once the advanced rows move the codecs somewhere else. */
static audio_route_preset_t s_last_preset = AUDIO_ROUTE_PRESET_CUSTOM;

bool audio_route_set_daw_gain(uint8_t db)
{
    if (db > 36) return false;
#ifdef CROSSPAD_ROUTE_HAS_MIXER
    /* Output volume, not channel volume: the capture bus is the only output the
     * codecs do not carry, so this moves what a DAW records and nothing else. */
    const float gain = powf(10.0f, (float)db / 20.0f);
    getMixerEngine().setOutputVolume(crosspad::IdfAudioModule::CAPTURE_BUS, gain);
    ESP_LOGI(TAG, "DAW output gain +%u dB (x%.2f)", (unsigned)db, gain);
    return true;
#else
    return false;
#endif
}

bool audio_route_apply_preset(audio_route_preset_t preset)
{
    if ((unsigned)preset >= AUDIO_ROUTE_PRESET_COUNT) return false;
    const RoutePreset &p = k_presets[preset];
    if (!s_loop_on && p.codec <= 1 && p.input == AUDIO_ROUTE_ADC_LINE1) {
        /* LINE1 is Line 3/4 while the input pair uses it: there is no bus to record. */
        ESP_LOGW(TAG, "preset %d refused: the DAC loop is off for Line 3/4", (int)preset);
        return false;
    }
    /* Input first: switching the capture source before its ADC is routed would
     * stream one buffer of whatever the old input was. */
    if (p.codec == AUDIO_ROUTE_CAPTURE_SAMPLER) {
        /* Digital source: no ADC to route. */
    } else if (p.codec == AUDIO_ROUTE_CAPTURE_MIX) {
        if (!audio_route_set_adc_input(0, p.input) || !audio_route_set_adc_input(1, p.input)) return false;
    } else if (!audio_route_set_adc_input(p.codec, p.input)) {
        return false;
    }
    if (!audio_route_set_capture_codec(p.codec)) return false;
    if (p.needsMixer) AudioTasks_SetSuspended(false);
    s_last_preset = preset;
    ESP_LOGI(TAG, "preset %d applied (codec%d input %d%s)", (int)preset, p.codec, p.input,
             p.needsMixer ? ", RT mixer resumed" : "");
    return true;
}

/* A bus preset names the DAC loop; with the loop off its signature is Line 3/4. */
static bool preset_possible(unsigned i)
{
    return s_loop_on || k_presets[i].codec > 1 || k_presets[i].input != AUDIO_ROUTE_ADC_LINE1;
}

audio_route_preset_t audio_route_get_preset(void)
{
    uint8_t codec = (uint8_t)usb_audio_bridge_get_capture_codec();
    if (codec == AUDIO_ROUTE_CAPTURE_SAMPLER) return AUDIO_ROUTE_PRESET_SAMPLER;
    if (codec > AUDIO_ROUTE_CAPTURE_MIX) return AUDIO_ROUTE_PRESET_CUSTOM;
    if (s_last_preset < AUDIO_ROUTE_PRESET_COUNT && preset_possible(s_last_preset)) {
        const RoutePreset &l = k_presets[s_last_preset];
        const bool still = (l.codec == codec) && ((codec == AUDIO_ROUTE_CAPTURE_MIX)
            ? (s_adc_input[0] == l.input && s_adc_input[1] == l.input)
            : (s_adc_input[codec] == l.input));
        if (still) return s_last_preset;
    }
    for (unsigned i = 0; i < AUDIO_ROUTE_PRESET_COUNT; ++i) {
        if (k_presets[i].codec != codec || !preset_possible(i)) continue;
        const bool match = (codec == AUDIO_ROUTE_CAPTURE_MIX)
            ? (s_adc_input[0] == k_presets[i].input && s_adc_input[1] == k_presets[i].input)
            : (s_adc_input[codec] == k_presets[i].input);
        if (match) return (audio_route_preset_t)i;
    }
    return AUDIO_ROUTE_PRESET_CUSTOM;
}

static void send_query_reply(void)
{
    if (!s_reply_sender) return;
    uint8_t frame[4 + 9 + 1];
    frame[0] = CROSSPAD_SYSEX_START;
    frame[1] = CROSSPAD_SYSEX_MANUFACTURER;
    frame[2] = CROSSPAD_CMD_AUDIO_ROUTE;
    frame[3] = AUDIO_ROUTE_SUB_QUERY;
    get_state(&frame[4]);
    frame[13] = CROSSPAD_SYSEX_END;
    s_reply_sender(frame, sizeof(frame));
}

static void handle_sysex(const uint8_t *args, unsigned len)
{
    if (len < 1) return;
    switch (args[0]) {
    case AUDIO_ROUTE_SUB_ADC_INPUT:
        if (len >= 3) audio_route_set_adc_input(args[1], args[2]);
        break;
    case AUDIO_ROUTE_SUB_MIC_SRC:
        if (len >= 2) audio_route_set_capture_codec(args[1]);
        break;
    case AUDIO_ROUTE_SUB_DAC_OUTPUT:
        if (len >= 3) audio_route_set_dac_output(args[1], args[2]);
        break;
    case AUDIO_ROUTE_SUB_VOLUME:
        if (len >= 3) audio_route_set_volume(args[1], args[2]);
        break;
    case AUDIO_ROUTE_SUB_MUTE:
        if (len >= 3) audio_route_set_mute(args[1], args[2]);
        break;
    case AUDIO_ROUTE_SUB_TASKS:
        // HIL hook: override the USB-mode default for the mixer tasks
        // (1 = running, 0 = suspended), e.g. keep the RT mixer alive in USB
        // audio mode for capture-only loopback measurements.
        if (len >= 2 && args[1] <= 1) {
            ESP_LOGW(TAG, "audio tasks override via SysEx: %s",
                     args[1] ? "resume" : "suspend");
            AudioTasks_SetSuspended(args[1] == 0);
        }
        break;
    case AUDIO_ROUTE_SUB_PAD_PRESS:
        if (len >= 3 && args[1] < 16) hil_pad_inject(HilPadInject::Press, args[1], args[2]);
        break;
    case AUDIO_ROUTE_SUB_PAD_RELEASE:
        if (len >= 2 && args[1] < 16) hil_pad_inject(HilPadInject::Release, args[1], 0);
        break;
    case AUDIO_ROUTE_SUB_PREVIEW:
        if (len >= 2 && args[1] < 16) hil_preview_pad(args[1]);
        break;
    case AUDIO_ROUTE_SUB_KIT_LOAD:
        if (len >= 2) hil_kit_load(args[1]);
        break;
    case AUDIO_ROUTE_SUB_BOUNCE:
        if (len >= 3) {
            /* A request: the song engine's worker does the card work. */
            auto *song = crosspad::getPlatformServices().songEngine;
            const bool ok = song && song->bounceScene(args[1], args[2]);
            ESP_LOGI(TAG, "BOUNCE %u %u %s", args[1], args[2], ok ? "started" : "refused");
        }
        break;
    case AUDIO_ROUTE_SUB_ECHO:
        if (len >= 5 && s_reply_sender) {
            uint8_t frame[9];
            frame[0] = CROSSPAD_SYSEX_START;
            frame[1] = CROSSPAD_SYSEX_MANUFACTURER;
            frame[2] = CROSSPAD_CMD_AUDIO_ROUTE;
            frame[3] = AUDIO_ROUTE_SUB_ECHO;
            frame[4] = args[1] & 0x7F;
            frame[5] = args[2] & 0x7F;
            frame[6] = args[3] & 0x7F;
            frame[7] = args[4] & 0x7F;
            frame[8] = CROSSPAD_SYSEX_END;
            s_reply_sender(frame, sizeof(frame));
        }
        break;
    case AUDIO_ROUTE_SUB_QUERY:
        send_query_reply();
        break;
    default:
        ESP_LOGW(TAG, "unknown audio-route subcommand 0x%02X", args[0]);
        break;
    }
}

void audio_route_control_init(void)
{
    crosspad::getStm32MessageHandler().setAudioRouteCallback(
        [](const uint8_t *args, unsigned len) { handle_sysex(args, len); });
    ESP_LOGI(TAG, "audio route control registered (SysEx cmd 0x%02X)", CROSSPAD_CMD_AUDIO_ROUTE);
}

void audio_route_control_set_reply_sender(void (*fn)(const uint8_t *frame, size_t len))
{
    s_reply_sender = fn;
}

/* ── CDC text commands ─────────────────────────────────────────── */

static void cdc_reply(const char *text)
{
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)text, strlen(text));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
}

static int parse_adc_input_name(const char *s)
{
    if (strncmp(s, "DIFF", 4) == 0) return 0;
    if (strncmp(s, "LINE1", 5) == 0) return 1;
    if (strncmp(s, "LINE2", 5) == 0) return 2;
    return -1;
}

static int parse_dac_output_name(const char *s)
{
    if (strncmp(s, "LINE1", 5) == 0) return 1;
    if (strncmp(s, "LINE2", 5) == 0) return 2;
    if (strncmp(s, "ALL", 3) == 0) return 3;
    return -1;
}

bool audio_route_control_handle_cdc(const char *buff, size_t len)
{
    /* Work on a bounded, NUL-terminated copy — CDC buffers are not strings. */
    char cmd[64];
    size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
    memcpy(cmd, buff, n);
    cmd[n] = '\0';

    int codec = -1, val = -1;
    char arg[8] = {0};

    if (strncmp(cmd, "AUDIO_STATUS", 12) == 0) {
        uint8_t st[9];
        get_state(st);
        char out[96];
        snprintf(out, sizeof(out),
                 "AUDIO: mic_src=%d adc=[%d,%d] out=[%d,%d] vol=[%d,%d] mute=[%d,%d]\r\n",
                 st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8]);
        cdc_reply(out);
        return true;
    }
    if (sscanf(cmd, "AUDIO_ADC %d %7s", &codec, arg) == 2) {
        int input = parse_adc_input_name(arg);
        cdc_reply(input >= 0 && audio_route_set_adc_input(codec, input) ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    if (sscanf(cmd, "AUDIO_MIC_SRC %d", &codec) == 1) {
        cdc_reply(audio_route_set_capture_codec(codec) ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    if (sscanf(cmd, "AUDIO_OUT %d %7s", &codec, arg) == 2) {
        int out = parse_dac_output_name(arg);
        cdc_reply(out >= 0 && audio_route_set_dac_output(codec, out) ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    if (sscanf(cmd, "AUDIO_VOL %d %d", &codec, &val) == 2) {
        const bool ok = audio_route_set_volume(codec, val);
        if (ok) {
            auto* cfg = crosspad::CrosspadSettings::getInstance();
            if (cfg) cfg->saveTo(NvsSettingsStore::getInstance());
        }
        cdc_reply(ok ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    if (sscanf(cmd, "AUDIO_MUTE %d %d", &codec, &val) == 2) {
        cdc_reply(audio_route_set_mute(codec, val) ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    return false;
}

/* The claimants in crosspad-platform-idf: the sample tools while the Recorder
 * listens to a codec, the input monitor while the mix sends an IN channel
 * anywhere. Not persisted; each claims again when it starts. */
extern "C" bool sample_tools_adc_claim(uint8_t codec, bool on)
{
    return audio_route_claim_adc(codec, AUDIO_ADC_CLAIM_TOOLS, on);
}

extern "C" bool input_monitor_adc_claim(uint8_t codec, bool on)
{
    return audio_route_claim_adc(codec, AUDIO_ADC_CLAIM_MONITOR, on);
}
