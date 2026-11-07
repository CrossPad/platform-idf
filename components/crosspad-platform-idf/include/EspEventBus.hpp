/**
 * @file EspEventBus.hpp
 * @brief ESP-IDF/Arduino-ESP32 implementation of IEventBus using esp_event system
 *
 * Wraps ESP-IDF's esp_event_loop for thread-safe, queue-based event dispatch.
 * Works on both ESP-IDF (native) and Arduino-ESP32 (which includes esp_event).
 */
#pragma once

#include "crosspad/event/IEventBus.hpp"
#include "crosspad/event/SubscriptionTable.hpp"
#include "esp_event.h"

namespace crosspad {

/// Configuration for the event bus
struct EspEventBusConfig {
    size_t queueSize = 64;
    const char* taskName = "crosspad_events";
    uint8_t taskPriority = 4;
    size_t taskStackSize = 2048;
    int taskCoreId = 1;  ///< Override to 0 for Arduino (loop core)
};

/**
 * @brief ESP event loop implementation of IEventBus
 *
 * Uses esp_event_loop_create() for a dedicated event loop with its own
 * FreeRTOS task. Events are queued via esp_event_post_to() (thread-safe)
 * and dispatched on the event loop task.
 *
 * The bus registers one esp_event handler for all of its events, at init(),
 * and keeps its subscribers in a SubscriptionTable: subscribe() and
 * unsubscribe() never touch the loop's handler lists.
 *
 * Works on both ESP-IDF and Arduino-ESP32 since Arduino-ESP32 IS ESP-IDF.
 */
class EspEventBus : public IEventBus {
public:
    explicit EspEventBus(const EspEventBusConfig& config = {});
    ~EspEventBus() override;

    // Non-copyable
    EspEventBus(const EspEventBus&) = delete;
    EspEventBus& operator=(const EspEventBus&) = delete;

    bool init() override;

    SubscriptionHandle subscribe(EventType type, EventHandler handler, void* user_ctx = nullptr) override;
    void unsubscribe(SubscriptionHandle handle) override;

    bool postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source) override;
    bool postPadReleased(uint8_t padIdx, EventSource source) override;
    bool postAppChanged(const char* appName, EventSource source) override;
    bool postTempoChanged(float bpm, EventSource source) override;
    bool postLiveModeChanged(bool enabled, EventSource source) override;
    bool postGuiUpdateRequired(EventSource source) override;
    bool postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source, uint8_t velocity,
                            int8_t transpose, uint8_t key) override;
    bool postMotionGesture(MotionGesture gesture, uint8_t orientation, EventSource source) override;

    bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool sendNoteOff(uint8_t channel, uint8_t note, EventSource source) override;

private:
    static void espEventTrampoline(void* handler_arg, esp_event_base_t base,
                                   int32_t id, void* event_data);

    bool postEvent(EventType type, const void* data, size_t dataSize);
    bool sendEvent(EventType type, const void* data, size_t dataSize);

    esp_event_loop_handle_t eventLoop_ = nullptr;
    SubscriptionTable subs_;
    bool initialized_ = false;
    EspEventBusConfig config_;
};

/// Events posted to the bus and not yet through their handlers -- on the
/// bus's own task, at its priority. An offline render (audio_task) waits on
/// it between passes: the notes a pass's sequencer posts reach the engine
/// before the next pass, as the event task makes it in real time.
uint32_t esp_event_bus_pending();

} // namespace crosspad
