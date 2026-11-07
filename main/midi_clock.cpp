#include "midi_clock.h"

#include "crosspad/midi/MidiClock.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/transport/Transport.hpp"
#include "CrosspadPlatform.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "EspBleMidi.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <cmath>
#include <cstdio>
#include <new>

static const char *TAG = "MidiClock";

namespace {

using crosspad::MidiClock;

constexpr uint8_t kClock    = 0xF8;
constexpr uint8_t kStart    = 0xFA;
constexpr uint8_t kContinue = 0xFB;
constexpr uint8_t kStop     = 0xFC;

/* While the host drives, the timer only watches for its silence. */
constexpr int64_t kPollUs = 20000;

alignas(MidiClock) unsigned char s_storage[sizeof(MidiClock)];
MidiClock *s_clock = nullptr;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t s_timer = nullptr;
int64_t s_lastHostReminderUs = 0; // uartLoop only

/* Changes alone would leave a subscriber that arrives while the host already
 * drives (the Sampler opened mid-song) without the state: repeat it once a
 * second while the host is the source. */
constexpr int64_t kHostReminderUs = 1000000;

/* The metronome follows the host tempo when it is this far off. Below the
 * published 0.05 BPM: the click runs on its own sample clock between host
 * ticks, and every 0.01 BPM is ~110 ppm of drift at 90 BPM. */
constexpr float kMetronomeDeltaBpm = 0.02f;

/* Every clock byte leaves here: BLE when a peer is connected and the router
 * forwards MIDI to it, the STM always (its own routing decides USB / TRS).
 * Both calls queue and return. */
void fan_out(uint8_t b, int64_t atUs)
{
    /* Only with a peer: without one every tick would count as a BLE drop and
     * bury the drops that mean something. */
    auto *cfg = crosspad::CrosspadSettings::getInstance();
    auto &ble = crosspad::getEspBleMidi();
    if ((!cfg || cfg->esp32Routing.Midi2BLE) && ble.isConnected()) ble.sendRealTime(b, atUs);
    crosspad::getStm32Transport().send(&b, 1);
}

void publish()
{
    float bpm = 0.0f;
    MidiClock::Source src{};
    bool changed;
    portENTER_CRITICAL(&s_lock);
    changed = s_clock->takeTempoChange(bpm, src);
    portEXIT_CRITICAL(&s_lock);
    if (!changed) return;
    /* The transport bar's BPM refuses an edit while the host sets the tempo. */
    crosspad::transportSetTempoFromHost(src == MidiClock::Source::Host);
    crosspad::getEventBus().postTempoChanged(
        bpm, src == MidiClock::Source::Host ? crosspad::EventSource::MidiInput
                                            : crosspad::EventSource::System);
}

void arm(int64_t dueUs, int64_t nowUs)
{
    int64_t wait = dueUs - nowUs;
    if (wait < 50) wait = 50;
    if (wait > kPollUs) wait = kPollUs;
    esp_timer_start_once(s_timer, (uint64_t)wait);
}

/* esp_timer task: the internal generator and the host timeout. */
void on_timer(void *)
{
    const int64_t now = esp_timer_get_time();
    auto &met = ::getMetronome();
    const float metBpm = met.bpm();
    bool internal, sendTick = false;
    int64_t next;

    portENTER_CRITICAL(&s_lock);
    s_clock->poll(now);
    internal = s_clock->source() == MidiClock::Source::Internal;
    if (internal) {
        s_clock->setBpm(metBpm, now);          // the metronome holds the manual tempo
        if (s_clock->nextTickDue() <= now) {
            s_clock->internalTickSent(now);
            sendTick = true;
        }
        next = s_clock->nextTickDue();
    } else {
        next = now + kPollUs;
    }
    portEXIT_CRITICAL(&s_lock);

    if (sendTick) fan_out(kClock, now);
    publish();
    arm(next, esp_timer_get_time());
}

/* The click started or stopped (the caller's task: the Sampler, the
 * sequencer). On the internal source Start and tick 0 go out now, on the
 * click's first beat, and the generator counts from here; while the host
 * drives, its own FA/FC were already forwarded. */
void on_click_transport(bool running, void *)
{
    const int64_t now = esp_timer_get_time();
    bool internal;
    int64_t next = now;
    portENTER_CRITICAL(&s_lock);
    internal = s_clock->source() == MidiClock::Source::Internal;
    if (internal) {
        if (running) {
            s_clock->internalStart(now);
            s_clock->internalTickSent(now);
        } else {
            s_clock->internalStop(now);
        }
        next = s_clock->nextTickDue();
    }
    portEXIT_CRITICAL(&s_lock);
    if (!internal) return;
    fan_out(running ? kStart : kStop, now);
    if (!running) return;
    fan_out(kClock, now);
    esp_timer_stop(s_timer);
    arm(next, esp_timer_get_time());
}

} // namespace

extern "C" void midi_clock_init(void)
{
    const int64_t now = esp_timer_get_time();
    s_clock = new (s_storage) MidiClock(::getMetronome().bpm(), now);
    const esp_timer_create_args_t args = {
        .callback = on_timer,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "midi_clock",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer not created — no internal clock");
        return;
    }
    ::getMetronome().setTransportHook(on_click_transport, nullptr);
    arm(now + 1000, now);
    ESP_LOGI(TAG, "MIDI clock up at %.1f BPM", ::getMetronome().bpm());
}

extern "C" void midi_clock_host_tick(void)
{
    if (!s_clock) return;
    const int64_t now = esp_timer_get_time();
    float bpm;
    portENTER_CRITICAL(&s_lock);
    s_clock->hostTick(now);
    s_clock->tickForwarded();
    bpm = s_clock->bpm();
    portEXIT_CRITICAL(&s_lock);
    fan_out(kClock, now);
    /* Re-asserted every tick: a manual edit made while the host drives lasts
     * one tick at most. */
    auto &met = ::getMetronome();
    if (std::fabs(met.bpm() - bpm) > kMetronomeDeltaBpm) met.setBpm(bpm);
    publish();
    if (now - s_lastHostReminderUs >= kHostReminderUs) {
        s_lastHostReminderUs = now;
        crosspad::getEventBus().postTempoChanged(bpm, crosspad::EventSource::MidiInput);
    }
}

static void host_transport(uint8_t status, void (MidiClock::*op)(int64_t))
{
    if (!s_clock) return;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    (s_clock->*op)(now);
    portEXIT_CRITICAL(&s_lock);
    fan_out(status, now);
    auto &met = ::getMetronome();
    if (status == kStart)         met.start();
    else if (status == kContinue) met.resume();
    else                          met.stop();
    publish();
}

extern "C" void midi_clock_host_start(void)    { host_transport(kStart, &MidiClock::hostStart); }
extern "C" void midi_clock_host_continue(void) { host_transport(kContinue, &MidiClock::hostContinue); }
extern "C" void midi_clock_host_stop(void)     { host_transport(kStop, &MidiClock::hostStop); }

extern "C" int midi_clock_status(char *buf, size_t len, bool reset_stats)
{
    if (!s_clock) return snprintf(buf, len, "CLOCK: src=- bpm=0 running=0\r\n");
    /* Scalars only: this runs on the 5 kB main task, and the clock object
     * carries a 97-slot tick window. */
    portENTER_CRITICAL(&s_lock);
    const bool     host     = s_clock->source() == MidiClock::Source::Host;
    const float    bpm      = s_clock->bpm();
    const bool     running  = s_clock->running();
    const uint32_t ticksIn  = s_clock->ticksIn();
    const uint32_t ticksOut = s_clock->ticksOut();
    const int32_t  gap      = s_clock->lastGapUs();
    const int32_t  jitter   = s_clock->maxJitterUs();
    const int32_t  late     = s_clock->maxLateUs();
    const uint32_t restarts = s_clock->restarts();
    if (reset_stats) s_clock->resetStats();
    portEXIT_CRITICAL(&s_lock);
    return snprintf(buf, len,
                    "CLOCK: src=%s bpm=%.2f running=%d ticks_in=%u ticks_out=%u "
                    "last_gap_us=%d max_jitter_us=%d max_late_us=%d restarts=%u\r\n",
                    host ? "host" : "int", (double)bpm, running ? 1 : 0,
                    (unsigned)ticksIn, (unsigned)ticksOut, (int)gap, (int)jitter, (int)late,
                    (unsigned)restarts);
}
