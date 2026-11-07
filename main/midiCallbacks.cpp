#include "CrossPad.h"
#include "imu.h"
#include "CrosspadPlatform.hpp"
#include "usb_config_manager.h"
#include "audio_route_control.h"
#include "companion_sysex.h"
#include "touch_trace.h"
#include "power_button.h"
#include "twin_mirror.h"
#include "motion_stream.h"
#include "midi_clock.h"
#include "usb_serial_prompt.h"
#include "net_mode.h"
#include "stm32_hal_init.h"
#include "stm32_uart.h"
#include "midi_loop_test.h"
#include "lat_trace.h"
#include "SamplerEnginePort.hpp"
#include "driver/gpio.h"
#include "crosspad/stm32/Stm32NoteMap.hpp"
#include "crosspad/protocol/SysExProtocol.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/pad/PadManager.hpp"
/* The sequencer lives on the metronome, which is a mixer channel: no mixer,
 * no loop recorder. Same detection audio_route_control.cpp uses. */
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include "crosspad/sequencer/PatternSequencer.hpp"
crosspad::PatternSequencer& getSequencer();
#define CROSSPAD_MIDI_HAS_SEQUENCER 1
#endif

#include <MIDI.h>
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "EspStm32MidiTransport.hpp"
#endif
#include "EspTinyUsbMidiTransport.h"
#include "EspBleMidi.hpp"
#include "crosspad/midi/BleMidiOffsets.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include "crosspad/platform/PlatformServices.hpp"
#include "processMIDI.hpp"
extern "C" void audio_rt_note_live(void);   // main/audio/audio_task.cpp





// Custom settings for ESP-IDF
struct EspMidiSettings : public MIDI_NAMESPACE::DefaultSettings
{
    static const unsigned SysExMaxSize = 256;
    static const long BaudRate = 460800;
};

#ifdef CONFIG_IDF_TARGET_ESP32S3
//////   MIDI UART interface  //////
static MIDI_NAMESPACE::EspStm32MidiTransport STM32MIDI_transport;
MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::EspStm32MidiTransport, EspMidiSettings, MIDI_NAMESPACE::EspStm32MidiPlatform> STM32MIDI(STM32MIDI_transport);
#endif

//////   MIDI USB interface  //////
static MIDI_NAMESPACE::EspTinyUsbMidiTransport USB_MIDI_transport;
MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::EspTinyUsbMidiTransport, EspMidiSettings, MIDI_NAMESPACE::EspTinyUsbMidiPlatform> USB_MIDI(USB_MIDI_transport);
/* Set once uartLoop has begun USB_MIDI. Senders that start earlier in the boot
 * (the IMU mapper, pad logic) would otherwise log a transport error per byte. */
static std::atomic<bool> s_usbMidiBegun{false};


extern CrosspadSettings *settings;
extern CrosspadStatus status;

//////   Transport wrappers (platform-specific, handle channel +1 conversion)   //////

#ifdef CONFIG_IDF_TARGET_ESP32S3
class Stm32MidiOutput : public crosspad::IMidiOutput {
public:
    void sendNoteOn(uint8_t note, uint8_t channel) override {
        STM32MIDI.sendNoteOn(note, 127, static_cast<uint8_t>(channel + 1));
    }
    void sendNoteOff(uint8_t note, uint8_t channel) override {
        STM32MIDI.sendNoteOff(note, 0, static_cast<uint8_t>(channel + 1));
    }
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override {
        STM32MIDI.sendNoteOn(note, velocity, static_cast<uint8_t>(channel + 1));
    }
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override {
        STM32MIDI.sendNoteOff(note, velocity, static_cast<uint8_t>(channel + 1));
    }
    void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) override {
        STM32MIDI.sendControlChange(controller, value, static_cast<uint8_t>(channel + 1));
    }
    void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) override {
        STM32MIDI.sendAfterTouch(note, pressure, static_cast<uint8_t>(channel + 1));
    }
    void sendProgramChange(uint8_t program, uint8_t channel) override {
        STM32MIDI.sendProgramChange(program, static_cast<uint8_t>(channel + 1));
    }
    void sendPitchBend(int16_t value, uint8_t channel) override {
        STM32MIDI.sendPitchBend(value, static_cast<uint8_t>(channel + 1));
    }
};
static Stm32MidiOutput s_stm32Output;
#endif

class UsbMidiOutput : public crosspad::IMidiOutput {
public:
    void sendNoteOn(uint8_t note, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendNoteOn(note, 127, static_cast<uint8_t>(channel + 1));
    }
    void sendNoteOff(uint8_t note, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendNoteOff(note, 0, static_cast<uint8_t>(channel + 1));
    }
    /* Output only. The engine is fed exactly once, by SamplerPadLogic through
     * the SamplePlayback event — a second call from here (a pre-sampler
     * leftover) made every pad also fire pad N-4. */
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendNoteOn(note, velocity, static_cast<uint8_t>(channel + 1));
    }
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendNoteOff(note, velocity, static_cast<uint8_t>(channel + 1));
    }
    void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendControlChange(controller, value, static_cast<uint8_t>(channel + 1));
    }
    void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendAfterTouch(note, pressure, static_cast<uint8_t>(channel + 1));
    }
    void sendProgramChange(uint8_t program, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendProgramChange(program, static_cast<uint8_t>(channel + 1));
    }
    void sendPitchBend(int16_t value, uint8_t channel) override {
        if (!begun()) return;
        USB_MIDI.sendPitchBend(value, static_cast<uint8_t>(channel + 1));
    }
    void sendSysEx(const uint8_t* frame, size_t len) override {
        if (!begun()) return;
        USB_MIDI.sendSysEx((unsigned)len, frame, true);
    }
private:
    static bool begun() { return s_usbMidiBegun.load(std::memory_order_acquire); }
};

static UsbMidiOutput s_usbOutput;

// Thin MIDI input callbacks — delegate to MidiInputHandler

#ifdef CONFIG_IDF_TARGET_ESP32S3
/* The STM encodes the pad index as note = 36 + padIdx + noteOffset.
 * stm32NoteToPad() is the single source of truth: it honours the live
 * noteOffset, unlike getPadForMidiNote(), which a sampler kit redefines. */

#include <atomic>
/* Physical presses, counted separately from CDC-injected ones (PAD_STATS). */
std::atomic<uint32_t> g_hil_stm_press_count{0};
std::atomic<uint32_t> g_hil_stm_at_count{0};      /* poly aftertouch samples decoded from the STM */
std::atomic<uint8_t>  g_hil_stm_last_note{0};
std::atomic<uint8_t>  g_hil_stm_last_pad{0xFF};

/* Long enough for a struck pad to stop ringing, short enough that a knock made
 * right after playing still counts. */
static constexpr uint32_t kPadTapMuteMs = 400;

/* A physical hit, whichever way the STM reports it. */
static void stm_pad_press(uint8_t pad, uint8_t velocity)
{
    ++g_hil_stm_press_count;
    g_hil_stm_last_pad = pad;
    /* A pad struck with a finger shakes the case as hard as a knock on it, and
     * the board's own playing was read as taps. Told here, at the input, the
     * IMU drops them while the hit rings out. */
    bsp_imu_mute_taps_for(kPadTapMuteMs);
    if (pad == 0xFF) return;
    twin_mirror_pad(pad, velocity);
    crosspad::getPadManager().handlePadPress(pad, velocity);
}

static void stm_pad_release(uint8_t pad)
{
    if (pad == 0xFF) return;
    twin_mirror_pad(pad, 0);
#ifdef CROSSPAD_MIDI_HAS_SEQUENCER
    /* How long it was held is part of the note: a chord, a sample on HOLD.
     * Not behind the MODE map's check -- a note recorded before the map came
     * up still ends when the finger lifts. */
    getSequencer().noteOut(pad);
#endif
    crosspad::getPadManager().handlePadRelease(pad);
}

void onNoteOn2Audio(midi::Channel channel, uint8_t note, uint8_t velocity)
{
    if (midi_loop_take(channel, note, velocity)) return;
    int off = crosspad::CrosspadSettings::getInstance()->stm.noteOffset;
    g_hil_stm_last_note = note;
    const uint8_t pad = crosspad::stm32NoteToPad(note, off);
    lat_trace_note_in(pad, stm32_uart_last_rx_us());
    {
        NativeStallClock clock(NativeStallClock::kPadPath);
        stm_pad_press(pad, velocity);
    }
    lat_trace_note_posted(pad);
}

void onNoteOff2Audio(midi::Channel /*channel*/, uint8_t note, uint8_t /*velocity*/)
{
    int off = crosspad::CrosspadSettings::getInstance()->stm.noteOffset;
    stm_pad_release(crosspad::stm32NoteToPad(note, off));
}

/* A rev 1.x STM sends no note-on: its pads arrive as edges of
 * CMD_BUTTONS_DIGITAL, which carry no pressure, so every hit is a full one. */
static constexpr uint8_t kDigitalPadVelocity = 127;

static void handleSysEx(byte *array, unsigned size)
{
    crosspad::getStm32MessageHandler().handleMessage(array, size);
}
#endif

// Deferred USB mode switch — can't tear down USB from inside a USB MIDI callback
static volatile int s_pending_usb_mode = -1;  // -1 = no pending switch

/* Foreign-manufacturer SysEx arriving on USB is sent straight back to the
 * host when this is on. A DAW under Wine only accepts MIDI whose ALSA source
 * is the device it opened, so a host-side tool that shares the FL Studio
 * script (the fl-studio-mcp bridge, manufacturer 0x7C) can reach it only by
 * bouncing off the board. Test-only, never persisted: SYSEX_ECHO over CDC. */
static volatile bool s_usbSysExEcho = false;

void usbMidi_setSysExEcho(bool on) { s_usbSysExEcho = on; }
bool usbMidi_getSysExEcho() { return s_usbSysExEcho; }

//////   Wire MIDI output targets to MidiInputHandler   //////

void midiCallbacks_init()
{
    auto& handler = crosspad::getMidiInputHandler();
#ifdef CONFIG_IDF_TARGET_ESP32S3
    handler.setStm32Output(&s_stm32Output);
#endif
    handler.setUsbOutput(&s_usbOutput);
    // bleOutput not available on ESP-IDF yet — stays nullptr

#ifdef CROSSPAD_MIDI_HAS_SEQUENCER
    /* Every press a player makes is a note of the take, stamped once in the
     * pad manager whichever way it came in -- the pads, MIDI, the HIL's hits,
     * the Sampler's touch pads, which no per-input stamp reached. Never the
     * recorder's own playback, nor a finger on the MODE map, whose pads are
     * buttons (the tap skips both, core 2.7.0); a host's note under the map
     * is still one. */
    crosspad::getPadManager().setInputTap([](void*, uint8_t pad, uint8_t velocity) {
        getSequencer().noteIn(pad, velocity);
        /* Someone is playing: a bounce now is heard, so it keeps time. */
        audio_rt_note_live();
    }, nullptr);
#endif

#ifdef CONFIG_IDF_TARGET_ESP32S3
    crosspad::getStm32MessageHandler().setPadEdgeCallback([](uint8_t pad, bool pressed) {
        if (pressed) stm_pad_press(pad, kDigitalPadVelocity);
        else stm_pad_release(pad);
    });
    crosspad::getStm32MessageHandler().setVersionReplyCallback(stm32_hal_set_uart_version);

    // Deferred: USB cannot be torn down from inside a USB callback.
    crosspad::getStm32MessageHandler().setUsbModeCallback([](uint8_t mode) {
        /* The CDC profile is the console, OTA and file plane: with the guard
         * on, a host asking for it is answered by the user on the device
         * (usb_serial_prompt), not here. Going to audio is always honoured. */
        auto *cfg = crosspad::CrosspadSettings::getInstance();
        if (mode != 0x02 && cfg && cfg->usbSerialGuard &&
            usb_config_manager_get_mode() != USB_MODE_DEFAULT) {
            usb_serial_prompt_request();
            return;
        }
        s_pending_usb_mode = (mode == 0x02) ? (int)USB_MODE_AUDIO : (int)USB_MODE_DEFAULT;
    });

    // Companion control over SysEx (0x1F): the same verb set the BLE front
    // answers, for the USB-audio profile which has no CDC. Replies leave from
    // uartLoop (companion_sysex_pump), the only USB MIDI writer.
    crosspad::getStm32MessageHandler().setCompanionCallback(companion_sysex_receive);
    companion_sysex_init([](const uint8_t *frame, size_t len) {
        USB_MIDI.sendSysEx((unsigned)len, frame, true);
    });

    // Audio routing (SysEx 0x1D + CDC AUDIO_*). Replies go over USB MIDI,
    // which exists in both USB modes.
    audio_route_control_init();
    audio_route_control_set_reply_sender([](const uint8_t *frame, size_t len) {
        USB_MIDI.sendSysEx((unsigned)len, frame, true);
    });
#endif
}

//////   BLE MIDI   //////

/* The pad a host's note plays: the note the pad sends, else the fixed grid
 * from keyOffset. A kit without per-pad notes numbers its pads 0..15, while a
 * DAW's drum map starts at keyOffset (C2) -- without the second lookup a host
 * reached no pad at all once such a kit was loaded. */
static uint8_t host_note_pad(uint8_t note)
{
    const uint8_t pad = crosspad::getPadManager().getPadForMidiNote(note);
    if (pad < 16) return pad;
    const int grid = static_cast<int>(note) - status.keyOffset;
    return (grid >= 0 && grid < 16) ? static_cast<uint8_t>(grid) : 0xFF;
}

/* A note from a host -- BLE MIDI or the ESP's own USB MIDI -- is treated like
 * any other musical source: a note the grid owns goes through
 * handlePadPress(), the same entry point a physical pad uses, so whichever app
 * owns pad logic sounds it. Anything outside the grid takes
 * MidiInputHandler's external-input path. @p channel is 0-based. */
static void host_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    auto& pads = crosspad::getPadManager();
    uint8_t pad = host_note_pad(note);
    if (pad < 16) {
        /* Played from a host, not a finger: the MODE map chooses only with
         * the pads themselves. A take records it through the pad manager's
         * input tap, like a pad. */
        pads.handlePadPress(pad, velocity, crosspad::EventSource::MidiInput);
    }
    else          crosspad::getMidiInputHandler().handleNoteOn(channel, note, velocity);
}

static void host_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    auto& pads = crosspad::getPadManager();
    uint8_t pad = host_note_pad(note);
    if (pad < 16) {
#ifdef CROSSPAD_MIDI_HAS_SEQUENCER
        getSequencer().noteOut(pad);
#endif
        pads.handlePadRelease(pad, crosspad::EventSource::MidiInput);
    }
    else          crosspad::getMidiInputHandler().handleNoteOff(channel, note, velocity);
}

static void bleMidi_wireCallbacks()
{
    static bool wired = false;
    if (wired) return;
    wired = true;

    auto& ble = crosspad::getEspBleMidi();

    /* The twin gets the pad as the board plays it; a USB host's notes reach it
     * as host messages instead (twin_mirror_host, below). */
    ble.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
        const uint8_t pad = host_note_pad(note);
        if (pad < 16) twin_mirror_pad(pad, velocity);
        host_note_on(channel, note, velocity);
    });
    ble.setHandleNoteOff([](uint8_t channel, uint8_t note, uint8_t velocity) {
        const uint8_t pad = host_note_pad(note);
        if (pad < 16) twin_mirror_pad(pad, 0);
        host_note_off(channel, note, velocity);
    });
    ble.setHandleControlChange([](uint8_t channel, uint8_t cc, uint8_t value) {
        crosspad::getMidiInputHandler().handleControlChange(channel, cc, value);
    });

    /* Feed the status bar's existing Bluetooth glyph rather than adding a
     * second icon of our own. */
    ble.setOnConnectionChanged([](bool connected, const crosspad::BleMidiDevice&) {
        status.bluetoothConnected = connected;
    });

    /* Outbound goes through the same router every other transport uses. */
    crosspad::getMidiInputHandler().setBleOutput(&ble);
    crosspad::getPlatformServices().setBleMidi(&ble);
    bleMidi_applyNoteOffsets();
}

void bleMidi_applyNoteOffsets()
{
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    if (!cfg) return;
    auto& ble = crosspad::getEspBleMidi();
    ble.setNoteOffset(cfg->wireless.bleMidiNoteOffset);
    /* A picklist index, not a semitone count: the settings dropdown lists the
     * same core table, so a label cannot select a value it does not name. */
    ble.setTxNoteOffset(crosspad::bleMidiTxOffsetValue(cfg->wireless.bleMidiTxOffset));
}

/* NimBLE's controller mallocs its pools inside BLE_INIT, and when they do not
 * fit it does not fail -- it asserts, and the board reboots:
 *
 *   E BLE_INIT: Malloc failed
 *   BLE assert emi.c 331
 *   COREDUMP: task=btController cause=28
 *
 * Reproduced 9.09 from the launcher at internal free 27 927 B / largest 8 692 B.
 * Every caller here is user-reachable -- the Settings > Connectivity toggle,
 * BLE_START over CDC, and bleMidi_boot() once the launcher has painted -- so
 * without a gate a board with BLE enabled reboots where it used to merely run
 * short (gate #3/#4 got BLE up and died later in kit_load at ~8 kB).
 *
 * The bar is the ~40 kB the BT controller takes while running
 * (docs/subsystems/ble-midi.md) plus 8 kB for the rest of the firmware to keep
 * allocating: below that, refusing is the honest answer and bleMidi_boot()'s
 * warning path finally gets to run. It was 60 kB, the top of the documented
 * 30-60 kB range; the launcher sits at ~60 kB after the 10.09 stack moves, so
 * that bar refused a start the board could afford. */
static constexpr size_t kBleMinInternalFree = 48 * 1024;

/* One lock around every start and stop: the Settings switch (LVGL thread), the
 * CDC and companion verbs (main task) and the WiFi control task all come here,
 * and two begin() calls at once are a NimBLE double init -- an assert and a
 * reboot. While WiFi holds the radio (bleMidi_wifi_hold() to
 * bleMidi_wifi_release()) no start is made beside it, since the WiFi memory
 * gates were measured with BLE off: the start is remembered and made when the
 * radio stops. */
static SemaphoreHandle_t ble_lock()
{
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

namespace {
struct BleLock {
    BleLock()  { xSemaphoreTake(ble_lock(), portMAX_DELAY); }
    ~BleLock() { xSemaphoreGive(ble_lock()); }
    BleLock(const BleLock&) = delete;
    BleLock& operator=(const BleLock&) = delete;
};
}

static bool s_wifiHolds;                     // under ble_lock()
/* What was asked while WiFi held the radio: a BleMidiMode index, or one of these. */
static constexpr int kBleNoRequest = -1;
static constexpr int kBleStayOff = -2;
static int s_bleWanted = kBleNoRequest;      // under ble_lock()

static bool ble_start_locked(uint8_t mode)
{
    const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internalFree < kBleMinInternalFree) {
        ESP_LOGW("MIDI", "BLE MIDI not started: %u B internal free, needs %u B — "
                 "starting it here would assert inside the BT controller and reboot",
                 (unsigned)internalFree, (unsigned)kBleMinInternalFree);
        status.bluetoothActive = false;
        return false;
    }
    bleMidi_wireCallbacks();
    bool ok = crosspad::getEspBleMidi().begin(mode == 0 ? crosspad::BleMidiMode::Host
                                                        : crosspad::BleMidiMode::Server);
    status.bluetoothActive = ok;
    return ok;
}

static void ble_stop_locked()
{
    crosspad::getEspBleMidi().end();
    status.bluetoothActive = false;
    status.bluetoothConnected = false;
}

bool bleMidi_start(uint8_t mode)
{
    BleLock lock;
    /* Already up: say so, instead of measuring RAM that the running stack is
     * holding and refusing "for lack of memory" — that answer sent the bench
     * looking for a leak that was not there. */
    if (crosspad::getEspBleMidi().isRunning()) return true;
    /* Network mode starts no BLE: its internal RAM is WiFi's there, and the
     * Settings toggle, BLE_ENABLE and BLE_START all come through here. */
    if (net_mode_active()) {
        ESP_LOGW("MIDI", "BLE MIDI not started: network mode");
        status.bluetoothActive = false;
        return false;
    }
    if (!CROSSPAD_FW_RADIO) {
        ESP_LOGW("MIDI", "BLE MIDI not started: this firmware's app owns the radio");
        status.bluetoothActive = false;
        return false;
    }
    if (s_wifiHolds) {
        s_bleWanted = mode;
        ESP_LOGW("MIDI", "BLE MIDI start deferred: WiFi has the radio, BLE starts when it stops");
        status.bluetoothActive = false;
        return false;
    }
    return ble_start_locked(mode);
}

void bleMidi_stop()
{
    BleLock lock;
    if (s_wifiHolds) s_bleWanted = kBleStayOff;
    ble_stop_locked();
}

bool bleMidi_deferred_for_wifi()
{
    BleLock lock;
    return s_wifiHolds && s_bleWanted >= 0;
}

bool bleMidi_wifi_hold(uint32_t replyDelayMs)
{
    bool running;
    {
        BleLock lock;
        s_wifiHolds = true;
        s_bleWanted = kBleNoRequest;
        running = crosspad::getEspBleMidi().isRunning();
    }
    if (!running) return false;
    vTaskDelay(pdMS_TO_TICKS(replyDelayMs));
    BleLock lock;
    ble_stop_locked();
    return true;
}

void bleMidi_wifi_release(bool restore)
{
    BleLock lock;
    s_wifiHolds = false;
    const int wanted = s_bleWanted;
    s_bleWanted = kBleNoRequest;
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    if (wanted >= 0) ble_start_locked((uint8_t)wanted);
    else if (wanted == kBleNoRequest && restore && cfg && cfg->wireless.enableBLE)
        ble_start_locked(cfg->wireless.bleMidiMode);
}

void bleMidi_boot()
{
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    if (!cfg || !cfg->wireless.enableBLE) {
        ESP_LOGI("MIDI", "BLE MIDI disabled in settings");
        return;
    }
    int64_t t0 = esp_timer_get_time();
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (!bleMidi_start(cfg->wireless.bleMidiMode)) {
        ESP_LOGW("MIDI", "BLE MIDI failed to start after %lld ms (internal free %u, largest %u)",
                 (long long)((esp_timer_get_time() - t0) / 1000),
                 (unsigned)before, (unsigned)largest);
        return;
    }
    ESP_LOGI("MIDI", "BLE MIDI up in %lld ms: internal RAM %u -> %u (cost %d B, largest block %u)",
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)(before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}


//////   UART MIDI task   //////

void uartLoop(void *pvParameters)
{
#ifdef CONFIG_IDF_TARGET_ESP32S3
    // Initialize STM32 UART MIDI
    STM32MIDI.begin();
    // Woken by every chunk the UART event task stores (see the wait at the
    // bottom of the loop).
    midi::EspStm32MidiTransport::wakeOnRx(xTaskGetCurrentTaskHandle());
#endif

    // Initialize USB MIDI — listen on all channels
    USB_MIDI.begin(MIDI_CHANNEL_OMNI);
    s_usbMidiBegun.store(true, std::memory_order_release);
    USB_MIDI.setHandleClock(midi_clock_host_tick);
    USB_MIDI.setHandleStart(midi_clock_host_start);
    USB_MIDI.setHandleContinue(midi_clock_host_continue);
    USB_MIDI.setHandleStop(midi_clock_host_stop);

#ifdef CONFIG_IDF_TARGET_ESP32S3
    // Register MIDI callbacks for STM32 interface
    STM32MIDI.setHandleNoteOn(onNoteOn2Audio);
    STM32MIDI.setHandleNoteOff(onNoteOff2Audio);
    STM32MIDI.setHandleSystemExclusive(handleSysEx);
#endif

    // USB MIDI in: a pad's note plays the pad, the same as over BLE
    /* Each host message is also handed to the browser twin as it is
     * dispatched here (twin_mirror.h, kind 4), so its apps see the same. */
    USB_MIDI.setHandleNoteOn([](midi::Channel ch, uint8_t note, uint8_t vel) {
        const uint8_t m[3] = {(uint8_t)(0x90 | ((ch - 1) & 0x0F)), note, vel};
        twin_mirror_host(m, 3);
        host_note_on(static_cast<uint8_t>(ch - 1), note, vel);
    });
    USB_MIDI.setHandleNoteOff([](midi::Channel ch, uint8_t note, uint8_t vel) {
        const uint8_t m[3] = {(uint8_t)(0x80 | ((ch - 1) & 0x0F)), note, vel};
        twin_mirror_host(m, 3);
        host_note_off(static_cast<uint8_t>(ch - 1), note, vel);
    });
    USB_MIDI.setHandleSystemExclusive([](byte* array, unsigned size) {
        if (s_usbSysExEcho && size >= 2 && array[1] != CROSSPAD_SYSEX_MANUFACTURER) {
            // A frame longer than SysExMaxSize arrives as unframed pieces;
            // re-sending those would put bare data bytes on the wire.
            if (array[0] == CROSSPAD_SYSEX_START && array[size - 1] == CROSSPAD_SYSEX_END) {
                USB_MIDI.sendSysEx(size, array, true);
            }
            return;
        }
        /* Not the companion channel (0x1F): that is the twin's own traffic,
         * and what it changes the twin takes from TWIN_STATE. */
        if (!(size > 2 && array[2] == 0x1F)) twin_mirror_host(array, size);
        crosspad::getStm32MessageHandler().handleMessage(array, size);
    });

#ifdef CONFIG_IDF_TARGET_ESP32S3
    // Wire additional MIDI message types for STM32 interface
    auto ccHandler = [](midi::Channel ch, uint8_t ctrl, uint8_t val) {
        // The STM streams each held pad's left/right balance as CC
        // ccPanBase+pad (kPanWireCentre = centred). That is pad state for the
        // active pad logic, not a controller for the sampler.
        const auto& stm = crosspad::CrosspadSettings::getInstance()->stm;
        if (ctrl >= stm.ccPanBase && ctrl < stm.ccPanBase + 16) {
            crosspad::getPadManager().notifyPadPan((uint8_t)(ctrl - stm.ccPanBase),
                                                   crosspad::IPadLogicHandler::panFromWire(val));
            return;
        }
        crosspad::getMidiInputHandler().handleSampleControlChange(ch, ctrl, val);
    };
    auto stm32AtHandler = [](midi::Channel ch, uint8_t note, uint8_t pressure) {
        int off = crosspad::CrosspadSettings::getInstance()->stm.noteOffset;
        uint8_t pad = crosspad::stm32NoteToPad(note, off);
        if (pad != 0xFF) {
            ++g_hil_stm_at_count;
            crosspad::getPadManager().notifyPadPressure(pad, pressure);
        }
        crosspad::getMidiInputHandler().handleSampleAftertouch(ch, note, pressure);
    };
    auto pbHandler = [](midi::Channel ch, int bend) {
        crosspad::getMidiInputHandler().handleSamplePitchBend(ch, static_cast<int16_t>(bend));
    };
    auto pcHandler = [](midi::Channel ch, uint8_t program) {
        crosspad::getMidiInputHandler().handleSampleProgramChange(ch, program);
    };
    STM32MIDI.setHandleControlChange(ccHandler);
    STM32MIDI.setHandleAfterTouchPoly(stm32AtHandler);
    STM32MIDI.setHandlePitchBend(pbHandler);
    STM32MIDI.setHandleProgramChange(pcHandler);
#endif

    auto usbCcHandler = [](midi::Channel ch, uint8_t ctrl, uint8_t val) {
        const uint8_t m[3] = {(uint8_t)(0xB0 | ((ch - 1) & 0x0F)), ctrl, val};
        twin_mirror_host(m, 3);
        crosspad::getMidiInputHandler().handleSampleControlChange(ch, ctrl, val);
    };
    auto usbAtHandler = [](midi::Channel ch, uint8_t note, uint8_t pressure) {
        const uint8_t m[3] = {(uint8_t)(0xA0 | ((ch - 1) & 0x0F)), note, pressure};
        twin_mirror_host(m, 3);
        crosspad::getMidiInputHandler().handleAftertouch(static_cast<uint8_t>(ch - 1), note, pressure);
    };
    auto usbPbHandler = [](midi::Channel ch, int bend) {
        const unsigned v = (unsigned)(bend + 8192);
        const uint8_t m[3] = {(uint8_t)(0xE0 | ((ch - 1) & 0x0F)), (uint8_t)(v & 0x7F), (uint8_t)((v >> 7) & 0x7F)};
        twin_mirror_host(m, 3);
        crosspad::getMidiInputHandler().handleSamplePitchBend(ch, static_cast<int16_t>(bend));
    };
    auto usbPcHandler = [](midi::Channel ch, uint8_t program) {
        const uint8_t m[2] = {(uint8_t)(0xC0 | ((ch - 1) & 0x0F)), program};
        twin_mirror_host(m, 2);
        crosspad::getMidiInputHandler().handleSampleProgramChange(ch, program);
    };
    USB_MIDI.setHandleControlChange(usbCcHandler);
    USB_MIDI.setHandleAfterTouchPoly(usbAtHandler);
    USB_MIDI.setHandlePitchBend(usbPbHandler);
    USB_MIDI.setHandleProgramChange(usbPcHandler);

    ESP_LOGI("MIDI", "USB MIDI callbacks registered");

    while (1)
    {
#ifdef CONFIG_IDF_TARGET_ESP32S3
        // A finished companion reply leaves before more RX can queue the next
        // request over it.
        companion_sysex_pump();
#endif
        // Poll USB MIDI for new data.
        USB_MIDI_transport.poll();
        // read() parses one byte per call and returns false mid-SysEx, so
        // loop on available() to drain the whole RX queue.
        while (USB_MIDI_transport.available()) { USB_MIDI.read(); }

#ifdef CONFIG_IDF_TARGET_ESP32S3
        while (STM32MIDI_transport.available()) { STM32MIDI.read(); }

        // Deferred USB mode switch, outside the USB callback context.
        if (s_pending_usb_mode >= 0) {
            usb_mode_t mode = (usb_mode_t)s_pending_usb_mode;
            s_pending_usb_mode = -1;
            ESP_LOGI("MIDI", "Executing deferred USB mode switch -> %s", usb_config_manager_get_mode_name(mode));
            usb_config_manager_set_mode(mode);
        }

        // Encoder MIDI — poll encoder state, emit CC on change
        {
            static int8_t enc_last  = -1;   // -1 forces first update
            static bool   btn_last  = false;
            auto& handler = crosspad::getStm32MessageHandler();

            int8_t enc = handler.encoderDetent() & 0x0F;   // detents turned, 0..15 -- not the STM's slipping count
            bool   btn = !gpio_get_level(GPIO_NUM_0);       // active-low, GPIO_NUM_0 = BSP_BUTTON_2 (encoder switch)

            if (enc != enc_last) {
                enc_last = enc;
                crosspad::getMidiInputHandler().sendControlChange(1, enc, 0);
            }

            if (btn != btn_last) {
                btn_last = btn;
                // CC 64 (Sustain Pedal): 127 on press, 0 on release
                crosspad::getMidiInputHandler().sendControlChange(64, btn ? 127 : 0, 0);
            }
        }

        // Touch mirror — press / drag / release as SysEx (touch_trace.h). Only for a
        // host that asked (the twin's keepalive, twin_mirror_active()): otherwise every
        // tap, the on-screen keyboard's too (a WiFi password), would go to any program
        // that opens the MIDI port.
        if (twin_mirror_active())
            touch_mirror_pump([](const uint8_t *frame, size_t len) { USB_MIDI.sendSysEx((unsigned)len, frame, true); });
        // Power button mirror — press / release as SysEx (power_button.h)
        power_button_mirror_pump([](const uint8_t *frame, size_t len) { USB_MIDI.sendSysEx((unsigned)len, frame, true); });
        // Browser twin input lockstep (twin_mirror.h)
        twin_mirror_pump([](const uint8_t *frame, size_t len) { USB_MIDI.sendSysEx((unsigned)len, frame, true); });
        // The board's motion for a host page (motion_stream.h)
        motion_stream_pump([](const uint8_t *frame, size_t len) { USB_MIDI.sendSysEx((unsigned)len, frame, true); });
#endif

        // Bytes from the STM wake this at once (EspStm32MidiTransport
        // queueMidiData); polling every 1 ms made a pad note wait 0-1 ms
        // (avg 0.5). The 1 ms bound stays for USB MIDI and the encoder.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}
