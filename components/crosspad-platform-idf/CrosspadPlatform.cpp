/**
 * @file CrosspadPlatform.cpp
 * @brief ESP-IDF platform implementation — all singleton services
 */

#include <cassert>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "CrosspadPlatform.hpp"
#include "EspEventBus.hpp"
#include "crosspad/platform/CrosspadPlatformInit.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "EspLedStrip.hpp"
#include "EspClock.hpp"
#include "EspImu.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/settings/VibrationSettings.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "EspStm32Transport.hpp"
#include "EspStm32I2cBus.hpp"
#include "EspI2cDevice.hpp"
#include "Drv2605Driver.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/protocol/SysExStm32CommandSender.hpp"
#include "crosspad/protocol/I2cStm32CommandSender.hpp"
#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_system.h"
/* Defined (0 or 1) by every CrossPad pinout: undefined here means the header
 * is missing, and each test below would quietly take the rev1 path. */
#if defined(BSP_BOARD_CROSSPAD_S3) && !defined(BSP_HAS_STM32_I2C)
#error "BSP_HAS_STM32_I2C is undefined: include bsp/esp-bsp.h before testing it"
#endif
#ifdef BSP_BOARD_CROSSPAD_S3
#include "soc/rtc_cntl_reg.h"
#include "bsp_es8388.h"
#include "IdfAudioModule.hpp"
#include "IdfI2sInput.hpp"
#include "UsbAudioPort.hpp"
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "MarcelSampleNode.hpp"
#endif
#include "SamplerEnginePort.hpp"
#include "PitchedSampleNode.hpp"
#include "EspDspFft.hpp"
#include "EspDspKernels.hpp"
#endif
#if CROSSPAD_HAS_SONG_ENGINE
#include "SongEnginePort.hpp"
#include "SampleToolsPort.hpp"
#include "InputGainPort.hpp"
#include "OutputLevelsPort.hpp"
#include "AudioBufferPort.hpp"
#include "crosspad/audio/IOutputLevels.hpp"
#include "InputPairPort.hpp"
#endif
#include "InputMonitorPort.hpp"
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include <crosspad-mixer/MixerPresets.hpp>
#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "crosspad/audio/AudioInputNode.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/transport/SequencerTransport.hpp"
#include "crosspad/transport/Transport.hpp"
#include "NvsSettingsStore.hpp"
#include "crosspad/song/SeqTrackSound.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#define IDF_HAS_MIXER 1
#endif
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include <string>

/* Boot-time and settings-save code, built for size: this file is -O2 for
 * the nodes it constructs (their inline mix() is compiled here), not for
 * CrosspadPlatform_Init(), which runs once. GCC's `cold` optimises a
 * function for size and changes nothing about what it does; the image is
 * PSRAM too (CLAUDE.md). */
#define CP_COLD __attribute__((cold))

static const char* TAG = "CrosspadPlatform";

// Pad index → physical LED index remap table
static const uint8_t PAD_TO_LED_MAP[16] = {
    12, 11, 4, 3, 13, 10, 5, 2, 14, 9, 6, 1, 15, 8, 7, 0
};

/* main/sample_player_port.cpp: the file cache's handles under a folder closed. */
extern void sample_player_port_forget_dir(const char* dir);
/* main/lv_async_safe.cpp: lv_async_call under the display lock, never waits. */
extern "C" void lv_async_call_locked(lv_async_cb_t cb, void *user_data);

namespace crosspad {

// ── Singletons ──────────────────────────────────────────────────────────

#ifdef IDF_HAS_MIXER
static AudioMixerEngine* s_mixerEngine = nullptr;
static bool s_mixerSaveDeferred = false;   /* boot, then the LVGL thread */
static MetronomeNode* s_metronome = nullptr;
static PatternSequencer* s_sequencer = nullptr;
#ifdef IDF_HAS_MIXER
void sequencer_poll_start();
void sequence_store_start();

/* The mixer's desk as one JSON blob under NVS "mixer"/"state". A blob, not a
 * string: nvs_set_str stops at 4000 bytes and a full desk is past that. */
static constexpr const char* kMixerNvsNs  = "mixer";
static constexpr const char* kMixerNvsKey = "state";

CP_COLD static std::string mixer_state_read(nvs_handle_t h)
{
    size_t len = 0;
    if (nvs_get_blob(h, kMixerNvsKey, nullptr, &len) != ESP_OK || !len) return {};
    std::string json(len, '\0');
    if (nvs_get_blob(h, kMixerNvsKey, json.data(), &len) != ESP_OK) return {};
    json.resize(len);
    return json;
}

CP_COLD static void mixer_state_load(AudioMixerEngine& mixer)
{
    nvs_handle_t h;
    if (nvs_open(kMixerNvsNs, NVS_READONLY, &h) != ESP_OK) return;
    const std::string json = mixer_state_read(h);
    nvs_close(h);
    if (!json.empty() && !mixer.applyStateJson(json)) {
        ESP_LOGW(TAG, "mixer state in NVS does not parse; defaults kept");
    }
}

/* On the Mixer's close, on the UI thread. Only when the desk changed: an NVS
 * write is a flash write, and closing the Mixer without touching it should
 * not cost one. */
CP_COLD static void mixer_state_save(AudioMixerEngine& mixer)
{
    const std::string json = mixer.stateJson();
    nvs_handle_t h;
    if (nvs_open(kMixerNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (mixer_state_read(h) != json) {
        const esp_err_t err = nvs_set_blob(h, kMixerNvsKey, json.data(), json.size());
        if (err == ESP_OK) nvs_commit(h);
        else ESP_LOGW(TAG, "mixer state not saved: %s (%u bytes)", esp_err_to_name(err), (unsigned)json.size());
    }
    nvs_close(h);
}
#endif
#endif

static EspEventBus*     s_eventBus = nullptr;
static EspClock*        s_clock = nullptr;
static EspLedStrip*     s_ledStrip = nullptr;
static PadLedController* s_padLedController = nullptr;
static PadAnimator*     s_padAnimator = nullptr;
static PadManager*      s_padManager = nullptr;
static EspStm32Transport* s_stm32Transport = nullptr;
static Stm32CommandSender* s_stm32CommandSender = nullptr;
static Stm32MessageHandler* s_stm32MsgHandler = nullptr;
static Stm32Manager*    s_stm32Manager = nullptr;
static TaskHandle_t     s_stm32MaintTask = nullptr;
static MidiInputHandler* s_midiInputHandler = nullptr;
#if defined(BSP_BOARD_CROSSPAD_S3) && CONFIG_CROSSPAD_SAMPLER_MARCEL
static MarcelSampleNode* s_marcelSampleNode = nullptr;
#endif

/* A kit's folder goes only once no file in it is open -- FATFS refuses an open
 * one -- and the sample port caches the handles of a kit that has just stopped
 * being current. They are closed first, for the kit browser's delete and
 * KIT_DELETE's alike; a kit removeKit() refuses (current, retired) keeps its. */
class IdfKitLoader : public PortableKitLoader {
public:
    using PortableKitLoader::PortableKitLoader;

    bool removeKit(int id) override {
        const KitInfo* kit = getKit(id);
        if (kit && !kit->path.empty() && id != currentKitId_ && id != retiredKitId_) {
            const std::string& path = kit->path;
            ::sample_player_port_forget_dir(toFsPath(path.substr(0, path.rfind('/'))).c_str());
        }
        return PortableKitLoader::removeKit(id);
    }
};

// ── Singleton accessors (ESP-IDF specific) ─────────────────────────────
// The core getters live in crosspad-core's PlatformServices.cpp.

IStm32Transport& getStm32Transport() {
    return *s_stm32Transport;
}

Stm32CommandSender& getStm32CommandSender() {
    return *s_stm32CommandSender;
}

Stm32MessageHandler& getStm32MessageHandler() {
    return *s_stm32MsgHandler;
}

Stm32Manager& getStm32Manager() {
    return *s_stm32Manager;
}

MidiInputHandler& getMidiInputHandler() {
    return *s_midiInputHandler;
}

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
MarcelSampleNode* getMarcelSampleNode() {
#ifdef BSP_BOARD_CROSSPAD_S3
    return s_marcelSampleNode;
#else
    return nullptr;
#endif
}
#endif

// ── Initialization ──────────────────────────────────────────────────────

CP_COLD void CrosspadPlatform_Init(CrosspadSettings* settings, CrosspadStatus* status) {
    if (s_eventBus) return; // already initialized

    static EspEventBus eventBus({
        .queueSize = 64,
        .taskName = "crosspad_events",
        .taskPriority = 4,
        // 4 kB, not 2: the SamplePlayback handler now reaches
        // PitchedInstrument::noteOn() (pow/cos/sin from libm) for pitched
        // kits, and at 2 kB the first KIT_LOAD of a pitched kit after boot
        // died with the register window spilled over the stack end (PC=1,
        // SP=0x8039677c, coredump seq 423, 2026-09-03).
        .taskStackSize = 4096,
        .taskCoreId = 1
    });
    static EspClock clock;
    static MidiInputHandler midiInputHandler;
#ifdef BSP_BOARD_CROSSPAD_S3
    static EspLedStrip ledStrip({
        .gpioPin = BSP_LED_STRIP_GPIO,
        .ledCount = BSP_LED_NUM,
        .rmtResolutionHz = 10 * 1000 * 1000,
        .useDma = true,
        .padToLedMap = PAD_TO_LED_MAP,
        .padCount = 16
    });
    ledStrip.begin();
#else
    static EspLedStrip ledStrip({
        .gpioPin = GPIO_NUM_NC,
        .ledCount = 0,
        .rmtResolutionHz = 10 * 1000 * 1000,
        .useDma = false,
        .padToLedMap = PAD_TO_LED_MAP,
        .padCount = 16
    });
#endif
    static PadLedController padLedController;
    static PadAnimator padAnimator;
    static PadManager padManager;

    s_eventBus         = &eventBus;
    s_clock            = &clock;
    s_midiInputHandler = &midiInputHandler;
    s_ledStrip         = &ledStrip;
    s_padLedController = &padLedController;
    s_padAnimator      = &padAnimator;
    s_padManager       = &padManager;

    // Common core init, and the PlatformServices registry behind the core
    // getters. kvStore is null: ESP-IDF loads settings in main before this.
    CrosspadPlatformConfig config;
    config.eventBus         = &eventBus;
    config.clock            = &clock;
    config.ledStrip         = &ledStrip;
    config.midiOutput       = &midiInputHandler;
    config.padManager       = &padManager;
    config.padLedController = &padLedController;
    config.padAnimator      = &padAnimator;
    config.settings         = settings;
    config.status           = status;

    /* Dark through the core's begin(): its first paint is every pad in the
     * controller's default green, ~55 ms on the strip before main blacks it
     * out. main brings the brightness back once the boot splash covers the
     * pads. */
    padLedController.setBrightness(0);
    crosspad_platform_init(config);
    /* Not given above -- main loaded the settings already, and the init would
     * load and save them again -- but published, for what keeps a value of
     * its own there (the metronome, transportRestoreClick()). */
    getPlatformServices().kvStore = &NvsSettingsStore::getInstance();

#ifdef BSP_BOARD_CROSSPAD_S3
    // esp-dsp's FFT ahead of core's portable one, before any kit load asks;
    // the S3's render kernels, before the audio task starts.
    setFftBackend(espDspFftBackend);
    espDspKernelsRegister();
#endif

    // Register optional services through PlatformServices
    static EspImu imu;
    getPlatformServices().setImu(&imu);

    // Kit manager, over the SD card VFS mount. listKits() must wait for the
    // mount (main.cpp).
    static IdfKitLoader kitLoader("/sdcard");
    auto& svc = getPlatformServices();
    svc.kitManager = &kitLoader;

#ifdef BSP_BOARD_CROSSPAD_S3
    // Audio pipeline: IdfAudioModule wraps the dual ES8388 I2S streams. With
    // the optional crosspad-mixer present, apps register as mixer channels and
    // AudioMixerEngine renders a bus per codec; without it, the path falls
    // back to AbstractAudioModule's single-bus node chain.
    static IdfAudioModule audioModule;
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    static MarcelSampleNode marcelSampleNode;
    s_marcelSampleNode = &marcelSampleNode;
    IAudioNode* samplerNode = &marcelSampleNode;
#else
    /* The native engine is the sampler: its channel is "Sampler", in the slot
     * the prebuilt one had, so a desk saved by firmware before it loads onto
     * the same channels (crosspad-mixer applyStateJson). */
    IAudioNode* samplerNode = native_sampler_node();
#endif
    AudioModuleConfig audioCfg;
    audioCfg.sampleRate   = settings ? settings->audioEngine.sampleRate : 44100;
    audioCfg.frameCount   = settings ? settings->audioEngine.frameCount : 64;
    audioCfg.channelCount = 2;
    audioCfg.streamCount  = 2;
    audioModule.setup(audioCfg);

    // The inputs as mixer channels -- the mics, Line 1/2 and Line 3/4, each
    // read from the codec the input pair puts it on. Routing is opt-in:
    // auto-routing IN→OUT on the same codec feeds back.
    static IdfI2sInput i2sInput0(kInputMic, audioCfg.sampleRate, audioCfg.frameCount);
    static IdfI2sInput i2sInput1(kInputLine12, audioCfg.sampleRate, audioCfg.frameCount);
    static IdfI2sInput i2sInput2(kInputLine34, audioCfg.sampleRate, audioCfg.frameCount);
    static AudioInputNode in1Node(&i2sInput0, "IN1");
    static AudioInputNode in2Node(&i2sInput1, "IN2");
    static AudioInputNode in3Node(&i2sInput2, "IN3");
    getPlatformServices().setAudioInput(&i2sInput0);
#ifdef IDF_HAS_MIXER
    static AudioMixerEngine mixerEngine;
    s_mixerEngine = &mixerEngine;
    mixerEngine.setup(audioCfg.frameCount, audioCfg.sampleRate, IdfAudioModule::NUM_OUTPUTS);
    mixerEngine.setDefaults();
    {
        IdfI2sInput* const inputs[] = {&i2sInput0, &i2sInput1, &i2sInput2};
        audio_buffer_init(audioModule, mixerEngine, inputs, 3);
    }
    /* Outputs 0 and 1 are codecs 0 and 1. At the top of a jack's volume the
     * codec's analog stage cannot carry a full-scale peak, so the output that
     * feeds it is limited to what it can: kept in step from wherever the
     * volume is set, and taken once here for a codec that is already up. The
     * limiter runs before the board's fix on an output (codec 1's shelf), so
     * the ceiling leaves room for that fix's boost. */
    bsp_es8388_set_volume_callback([](bsp_i2s_interface_t codec, int volume) {
        if (s_mixerEngine) s_mixerEngine->setOutputCeiling((uint8_t)codec,
            bsp_es8388_out_ceiling(volume) * audioModule.boardFixHeadroom((uint8_t)codec));
    });
    for (int c = 0; c < 2; ++c) {
        int volume = 0;
        if (bsp_es8388_get_volume((bsp_i2s_interface_t)c, &volume) == ESP_OK) {
            mixerEngine.setOutputCeiling((uint8_t)c,
                bsp_es8388_out_ceiling(volume) * audioModule.boardFixHeadroom((uint8_t)c));
        }
    }
    // Sampler → both codecs, as the legacy single-bus path did (headphones
    // and speaker share the mix).
    auto chSampler = mixerEngine.addChannel(samplerNode, "Sampler");
    if (chSampler != kMixerInvalidChannelId) {
        for (uint8_t o = 0; o < IdfAudioModule::NUM_OUTPUTS; ++o) {
            mixerEngine.setRouteEnabled(chSampler, o, true);
        }
    }
    // Pitched instrument beside the sampler, same routing. Only one of the two
    // ever holds voices (the kit says which engine plays), so the bus headroom
    // is unchanged.
    auto chPitched = mixerEngine.addChannel(&getPitchedSampleNode(), "Pitched");
    if (chPitched != kMixerInvalidChannelId) {
        for (uint8_t o = 0; o < IdfAudioModule::NUM_OUTPUTS; ++o) {
            mixerEngine.setRouteEnabled(chPitched, o, true);
        }
    }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    /* The native streaming engine, beside the other two. Its channel exists
     * from boot and renders nothing until a kit is taken into it, so the two
     * sample engines can be swapped at runtime without rebuilding the graph.
     * It moves every channel after it one slot on; a desk saved by firmware
     * without it is put back by channel name (crosspad-mixer, desk v4). */
    auto chSampler2 = mixerEngine.addChannel(native_sampler_node(), "Sampler2");
    if (chSampler2 != kMixerInvalidChannelId) {
        for (uint8_t o = 0; o < IdfAudioModule::NUM_OUTPUTS; ++o) {
            mixerEngine.setRouteEnabled(chSampler2, o, true);
        }
    }
#else
    const uint8_t chSampler2 = 0xFF;   /* one sampler channel, chSampler */
#endif
    PitchedInstrument& pitched = getPitchedSampleNode().instrument();
    pitched.setVoiceCap(PitchedSampleNode::kVoiceCap);
    crosspad::getPlatformServices().setPitchedInstrument(&pitched);
    // Host playback over USB audio enters here, not I2S: the UAC task fills
    // a ring and this channel drains it in the render, so the bus is the one
    // place the host's audio and the pads meet. Both codecs, like the sampler.
    static AudioInputNode usbNode(&usbPlaybackInput(), "USB");
    auto chUsb = mixerEngine.addChannel(&usbNode, "USB");
    if (chUsb != kMixerInvalidChannelId) {
        /* Physical outputs only, deliberately not the capture bus: that is what
         * lets the USB mic record the pads alone. Routing it here too would put
         * the host's own playback back into the recording -- a DAW monitoring a
         * metronome would hear it in the take. */
        for (uint8_t o = 0; o < IdfAudioModule::NUM_STREAM_OUTPUTS; ++o) {
            mixerEngine.setRouteEnabled(chUsb, o, true);
        }
    }
    /* The click is a reference for playing, not part of the take: physical
     * outputs only, never the capture bus, so a recording comes back without
     * the metronome on it. Same reasoning as the USB channel above. */
    static crosspad::MetronomeNode metronome;
    crosspad::s_metronome = &metronome;
    auto chClick = mixerEngine.addChannel(&metronome, "Click");
    if (chClick != kMixerInvalidChannelId) {
        for (uint8_t o = 0; o < IdfAudioModule::NUM_STREAM_OUTPUTS; ++o) {
            mixerEngine.setRouteEnabled(chClick, o, true);
        }
    }
    /* The loop recorder rides the same clock the click is counted from, and
     * plays back through handlePadPress() -- the entry point a physical pad
     * uses -- so a recorded take sounds exactly like the performance did,
     * through whichever app owns pad logic. It says it is the sequencer, so
     * pad logic that must tell a finger from a take (held erase, Pad Mixer's
     * selection) can. */
    /* In PSRAM, not a static: 12.6 kB of pattern in internal .data is what
     * took BLE's start margin. Only tasks touch it, never an ISR. */
    static crosspad::PatternSequencer& sequencer = []() -> crosspad::PatternSequencer& {
        void* mem = heap_caps_malloc(sizeof(crosspad::PatternSequencer), MALLOC_CAP_SPIRAM);
        assert(mem);
        return *new (mem) crosspad::PatternSequencer;
    }();
    crosspad::s_sequencer = &sequencer;
    sequencer.attach(&metronome);
    sequencer.setSink([](void*, uint8_t route, uint8_t pad, uint8_t vel, bool on, crosspad::PadPerformance perf) {
        auto& pads = crosspad::getPadManager();
        /* A Pitch seq track's notes, beside the kits': the instrument, whatever
         * screen is up (SeqTrackSound.hpp). */
        if (route == 2) {
            crosspad::seqTrackSound().playPitch(pads, pad, vel, on);
            return;
        }
        /* With what the hit sounded as when it was recorded: a part taken in
         * 16 levels plays its source pad at its levels, mode on or off now. */
        if (on) pads.handlePadPress(pad, vel, crosspad::EventSource::SequencerCallback, perf);
        else    pads.handlePadRelease(pad, crosspad::EventSource::SequencerCallback);
    }, nullptr);
    /* And at every hit, before the pad logic plays it: what the pads make of
     * it right now is what the take keeps. */
    sequencer.setPerformanceSource([](void*, uint8_t pad, uint8_t& vel) {
        return crosspad::getPadManager().performanceFor(pad, vel);
    }, nullptr);
    /* The controller is the portable face of the sequencer: apps and the UI
     * drive it through PlatformServices and never see this platform's engine.
     * Four bars to start with, which is a kit loop rather than a one-bar riff. */
    static crosspad::SequencerController seqController;
    seqController.setSeqLength(crosspad::SequencerLimits::MAX_SEQ_LENGTH);
    seqController.init(sequencer);
    sequencer.setController(&seqController);
    crosspad::getPlatformServices().sequencer       = &seqController;
    crosspad::getPlatformServices().sequencerEngine = &sequencer;
    /* The pattern is the transport's home: it holds it until an app takes it,
     * and gets it back when that app is left stopped -- a board at the home
     * screen or in the Sampler still has PLAY and REC mean something, which
     * is the whole point of one transport in the top bar. */
    crosspad::transportSetHome(crosspad::sequencerTransport());
    crosspad::transportRestoreClick();
    crosspad::sequencer_poll_start();
    crosspad::sequence_store_start();

    uint8_t chIn[kInputSources] = {
        mixerEngine.addChannel(&in1Node, "IN1"),
        mixerEngine.addChannel(&in2Node, "IN2"),
        kMixerInvalidChannelId,
    };
    /* The engine knows buses by number; where each one goes is this board's
     * business, and it is what the Mixer shows: the codecs as the output
     * pairs of a four-channel interface -- the phones (and the speakers
     * beside them) on 1/2, the line out on 3/4 -- and the recording bus a
     * host reads as "USB". */
    mixerEngine.setOutputName(0, "Out 1/2");
    mixerEngine.setOutputName(1, "Out 3/4");
    mixerEngine.setOutputName(IdfAudioModule::CAPTURE_BUS, "USB");
    mixerEngine.setOutputHeard(IdfAudioModule::CAPTURE_BUS, false);
    /* It has no fader in front of it (Settings -> USB gives gain, never
     * less), so this is where the limiter works hardest -- and only a host
     * reads it, where a block of latency is nothing and a click is in the
     * take. */
    mixerEngine.setOutputLookahead(IdfAudioModule::CAPTURE_BUS, true);
    /* Presets on the card, not in NVS: a desk is ~2 kB and NVS is 24 kB. */
    crosspad::getMixerPresets().setDir("/sdcard/crosspad/mixer");

#if CROSSPAD_HAS_SONG_ENGINE
    /* Before the saved desk is applied, so the "Song" and "Preview" strips
     * get their levels back like every other. */
    song_engine_init(mixerEngine, chSampler, chSampler2, chPitched);
    sample_tools_init(mixerEngine);
    input_pairs_init();
    input_gain_init();
    output_levels_init();
    /* A bus whose speakers and jacks have levels of their own in the hardware
     * plays at unity: its level is theirs. Before the saved desk, which
     * would otherwise put an old fader back on it. */
    if (IOutputLevels* ol = getPlatformServices().outputLevels) {
        for (int i = 0; i < ol->outputs(); ++i) {
            for (uint8_t o = 0; o < mixerEngine.numOutputs(); ++o) {
                if (std::strcmp(mixerEngine.getOutputName(o), ol->mixerOutput(i)) == 0) {
                    mixerEngine.setOutputUnity(o, true);
                }
            }
        }
    }
#endif
    /* Last of all: a saved desk finds its channels by slot and name, so Line
     * 3/4 must not move the channels registered before it existed. */
    if (BSP_HAS_SHARED_LINE_IN) chIn[kInputLine34] = mixerEngine.addChannel(&in3Node, "IN3");
    // IN channels off by default: with nothing plugged in, the passthrough put
    // raw ADC noise on the bus. Sent from the Mixer (MONITOR), each holds its
    // codec's ADC on its input while it is sent anywhere.
    input_monitor_start(mixerEngine, chIn);

    /* The desk as it was left, over the defaults above: a board that never
     * saved one, or a blob that does not parse, keeps them. */
    mixer_state_load(mixerEngine);
    /* A desk older than the mixer's format was migrated on the way in (and
     * may have had its only sample engine rescued): saved back once, so the
     * rescue is not repeated over what the player does next. Not here -- the
     * save is ArduinoJson on a 4 kB main task -- but on the LVGL thread once
     * the first screen is up (mixer_state_save_deferred()). */
    s_mixerSaveDeferred = mixerEngine.deskMigrated();
    if (s_mixerSaveDeferred) ESP_LOGI(TAG, "mixer desk migrated: saved back once the UI is up");
    /* Never on the caller's stack. Serialising the desk is ArduinoJson plus a
     * std::string, which is kilobytes, and a setter called from the CDC
     * dispatcher runs on the main task's four -- it overflowed it, inside
     * ArduinoJson's allocator, the first time an EQ was set from a host. The
     * save goes to the LVGL thread, which has the stack for it and is where
     * the Mixer's own close already saved from. Posted with the lock: MIX_EQ
     * (main task), INPUT_MON and MIX_PRESET (their own tasks) get here off the
     * LVGL thread, and a bare lv_async_call() there edits LVGL's timer list
     * under a running lv_timer_handler (LV_OS_NONE: LVGL has no lock of its own). */
    mixerEngine.setStateChangedCallback([]() {
        if (!s_mixerEngine || !lv_is_initialized()) return;
        lv_async_call_locked([](void*) { if (s_mixerEngine) mixer_state_save(*s_mixerEngine); }, nullptr);
    });

    /* The sampler engines and the pitched one: whichever holds the kit is
     * what a bounce and a PADS take have to hear. */
    audioModule.setPadChannels(chSampler, chPitched, chSampler2);
    song_engine_set_pads_tap(audioModule, chSampler, chPitched, chSampler2);
    audioModule.setClockChannel(chClick);
    audioModule.setMixerEngine(&mixerEngine);
    ESP_LOGI(TAG, "Audio pipeline: mixer mode (Sampler+Pitched→all outs, IN1/IN2 muted)");
#else
    audioModule.addNode(samplerNode);
    audioModule.addNode(&getPitchedSampleNode());
    ESP_LOGI(TAG, "Audio pipeline: node-chain mode (no mixer component)");
#endif
    svc.setAudioModule(&audioModule);
#endif

    // ESP-IDF specific: animation state callback
    padAnimator.setAnimationStateCallback([](bool active) {
        if (active) {
            s_padLedController->setAnimating(true);
        } else {
            s_padLedController->stopAnimation();
        }
    });

    // ESP-IDF specific: MidiInputHandler needs PadManager reference
    midiInputHandler.init(padManager, eventBus, settings);

    // STM32 transport (UART_NUM_1)
    static EspStm32Transport stm32Transport(UART_NUM_1);
    s_stm32Transport = &stm32Transport;

    // STM32 message handler (RX dispatch)
    static Stm32MessageHandler stm32MsgHandler;
    stm32MsgHandler.init(padManager, *status);
    stm32MsgHandler.setDfuRequestCallback([](bool stm32Dfu) {
        if (stm32Dfu) {
            ESP_LOGI("CrosspadPlatform", "STM32 entering DFU mode");
            while (true) { vTaskDelay(pdMS_TO_TICKS(100)); }
        } else {
            ESP_LOGI("CrosspadPlatform", "ESP entering DFU mode");
#ifdef BSP_BOARD_CROSSPAD_S3
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
#endif
            esp_restart();
        }
    });
    s_stm32MsgHandler = &stm32MsgHandler;
    getPlatformServices().stm32MessageHandler = &stm32MsgHandler;

    // STM32 command sender (TX). On rev2 the control plane is the STM32 I2C
    // slave (reg_map.h) and the UART stays the pads→ESP MIDI pipe; rev1 keeps
    // the legacy SysEx-over-UART path.
#if defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C
    static EspStm32I2cBus       stm32I2cBus;
    static I2cStm32CommandSender stm32CmdSender(stm32I2cBus);
    static Stm32ConfigSync       stm32ConfigSync(stm32I2cBus);   // NEW
    ESP_LOGI(TAG, "STM32 control plane: I2C @0x%02X (rev2)", STM32_I2C_ADDR_7B);
#else
    static SysExStm32CommandSender stm32CmdSender(stm32Transport);
    ESP_LOGI(TAG, "STM32 control plane: SysEx over UART (rev1)");
#endif
    s_stm32CommandSender = &stm32CmdSender;

    // STM32 manager (portable maintenance logic)
    static Stm32Manager stm32Manager;
    stm32Manager.init(stm32CmdSender, settings, clock);
#if defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C
    stm32Manager.setConfigSync(&stm32ConfigSync);                // NEW
#endif
    // start() is called later by CrosspadPlatform_StartStm32() after UART HAL init
    s_stm32Manager = &stm32Manager;

    ESP_LOGI(TAG, "Platform fully initialized");
}

// Stm32Manager's maintenance task: the stack and priority it had when core
// created it, one tick() per Stm32Manager::kTickPeriodMs.
constexpr uint32_t    kStm32MaintStack    = 3072;
constexpr UBaseType_t kStm32MaintPriority = 3;

static void stm32_maint_task(void*) {
    for (;;) {
        s_stm32Manager->tick();
        vTaskDelay(pdMS_TO_TICKS(Stm32Manager::kTickPeriodMs));
    }
}

CP_COLD void CrosspadPlatform_StartStm32() {
    if (!s_stm32Manager) {
        ESP_LOGE(TAG, "CrosspadPlatform_StartStm32 called before CrosspadPlatform_Init!");
        return;
    }

    if (!s_stm32Manager->isRunning()) {
        // Initial config push, routing + keep-alive, then the maintenance task
        s_stm32Manager->start();
        if (xTaskCreate(stm32_maint_task, "stm32_maint", kStm32MaintStack, nullptr,
                        kStm32MaintPriority, &s_stm32MaintTask) != pdPASS) {
            s_stm32Manager->stop();
        }
    }

    ESP_LOGI(TAG, "STM32 framework wired and started");
}

CP_COLD void CrosspadPlatform_StopStm32() {
    if (s_stm32MaintTask) {
        vTaskDelete(s_stm32MaintTask);
        s_stm32MaintTask = nullptr;
    }
    if (s_stm32Manager) s_stm32Manager->stop();
}

CP_COLD static void vibration_start_task(void*) {
    static EspI2cDevice drvI2c;
    if (!drvI2c.init(bsp_i2c_get_handle(), 0x5A)) {
        ESP_LOGW(TAG, "DRV2605: i2c device add failed");
        vTaskDelete(nullptr);
        return;
    }
    static Drv2605Driver driver(drvI2c, getClock());
    if (driver.initHardware() && driver.startTask()) {
        if (auto* s = CrosspadSettings::getInstance()) driver.applySettings(s->vibration);
        getPlatformServices().setVibration(&driver);
        ESP_LOGI(TAG, "DRV2605: haptics online");
    } else {
        ESP_LOGW(TAG, "DRV2605: init failed, haptics disabled");
    }
    vTaskDelete(nullptr);
}

#ifdef IDF_HAS_MIXER
/* The pattern keeps playing when the sampler panel is closed, so the poll
 * cannot live on an LVGL timer. 2 ms is the jitter a fired hit inherits --
 * against a 170 ms sixteenth at 88 BPM that is inaudible, and the poll itself
 * is a scan of a few hundred integers. Priority 5: above LVGL (4), so a busy
 * screen cannot drag the beat, below the STM's pad driver (6), so playing is
 * still what the board answers first. PSRAM stack, like the HIL pad task that
 * calls the same handlePadPress(). */
/* sequencer_poll_from_render(): who polls, and the task inside a poll. */
static std::atomic<bool> s_pollInRender{false};
static std::atomic<bool> s_pollBusy{false};

static void sequencer_poll_task(void*)
{
    for (;;) {
        s_pollBusy.store(true, std::memory_order_seq_cst);
        if (s_sequencer && !s_pollInRender.load(std::memory_order_seq_cst)) s_sequencer->poll();
        s_pollBusy.store(false, std::memory_order_seq_cst);
        /* 2 ms is the firing jitter, and it only buys anything while a pattern
         * is actually playing. Idle, it is just 500 wake-ups a second on the
         * core the UI renders on, to look at a clock that is not running --
         * and idle is what the sequencer is nearly all the time. Backing off
         * to 20 ms there costs only how long a transport start waits to be
         * noticed, which is a button press either way. */
        /* A count-in is live too: the take starts on the sample after it,
         * and a hit made just before beat one has to be noticed then. */
        const auto st = s_sequencer ? s_sequencer->state() : crosspad::SequencerState::Idle;
        const bool live = st == crosspad::SequencerState::Running ||
                          st == crosspad::SequencerState::PreBeat;
        vTaskDelay(pdMS_TO_TICKS(live ? 2 : 20));
    }
}

void sequencer_poll_from_render(bool on)
{
    s_pollInRender.store(on, std::memory_order_seq_cst);
    for (int i = 0; i < 50 && s_pollBusy.load(std::memory_order_seq_cst); ++i) vTaskDelay(1);
}

void sequencer_poll_now()
{
    if (s_sequencer && s_pollInRender.load(std::memory_order_acquire)) s_sequencer->poll();
}

void sequencer_poll_start()
{
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreatePinnedToCoreWithCaps(sequencer_poll_task, "seq_poll", 4000, nullptr, 5,
                                    nullptr, 0, MALLOC_CAP_SPIRAM);
}
#endif

CP_COLD void CrosspadPlatform_StartVibration() {
    xTaskCreate(vibration_start_task, "vib_init", 4096, nullptr, 3, nullptr);
}

} // namespace crosspad

#ifdef IDF_HAS_MIXER
/* LVGL thread, once the first screen is up (gui.cpp LoadMainScreen): the
 * desk a boot migrated, saved back once. */
extern "C" CP_COLD void mixer_state_save_deferred(void)
{
    if (!crosspad::s_mixerSaveDeferred || !crosspad::s_mixerEngine) return;
    crosspad::s_mixerSaveDeferred = false;
    crosspad::mixer_state_save(*crosspad::s_mixerEngine);
}

// Declared in <crosspad-mixer/AudioMixerEngine.hpp>. Apps add their
// IAudioNodes with getMixerEngine().addChannel().
AudioMixerEngine& getMixerEngine() {
    return *crosspad::s_mixerEngine;
}

/// The metronome doubles as the sequencer's clock; the UI and the recorder both
/// need to reach it. Declared in <crosspad/sequencer/MetronomeNode.hpp>.
crosspad::MetronomeNode& getMetronome() {
    return *crosspad::s_metronome;
}

/// The loop recorder, on the metronome's clock. Declared in
/// <crosspad/sequencer/PatternSequencer.hpp>.
crosspad::PatternSequencer& getSequencer() {
    return *crosspad::s_sequencer;
}
#endif

// Refresh failures of the one strip this platform owns, for HIL LED_STATE.
uint32_t esp_led_strip_refresh_failures(uint32_t* refreshes)
{
    if (refreshes) *refreshes = crosspad::s_ledStrip ? crosspad::s_ledStrip->getRefreshes() : 0;
    return crosspad::s_ledStrip ? crosspad::s_ledStrip->getRefreshFailures() : 0;
}
