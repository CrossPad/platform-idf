#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_timer.h"
#include "midi_Namespace.h"
#include "stm32_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "EspStm32MidiPlatform.hpp"

BEGIN_MIDI_NAMESPACE

#define MIDI_STREAM_BUF_SIZE 2048

class EspStm32MidiTransport
{
public:
    EspStm32MidiTransport();
    ~EspStm32MidiTransport();

    // Arduino MIDI Transport interface
    void begin();
    void end();

    // Send data to STM32
    void write(uint8_t data);
    void write(const uint8_t* data, size_t length);

    bool beginTransmission(uint8_t address) {
        // Not used in this transport
        return true;
    }

    void endTransmission() {
        // Not used in this transport
    }

    // Check if data is available (for receiving)
    bool available();

    // Read data from STM32
    uint8_t read();

    // Flush any pending data
    void flush();

    // Get the transport name
    const char* getName() const { return "STM32 UART"; }

    // Thru functionality
    static bool thruActivated;

    // MIDI data queue management (called by UART event task)
    static void queueMidiData(const uint8_t* data, size_t length);
    static void queueMidiByte(uint8_t data);

    // Task to wake (xTaskNotifyGive) whenever queueMidiData() stores bytes, so
    // the reader blocks on its notification instead of polling the stream.
    static void wakeOnRx(TaskHandle_t task);

private:
    bool initialized;

    // Static members for shared stream buffer (since MIDI library creates transport instances)
    static StreamBufferHandle_t midi_rx_stream;
    static bool stream_initialized;

    // Initialize the shared MIDI receive stream buffer
    static esp_err_t initMidiStream();
    static void deinitMidiStream();
};

END_MIDI_NAMESPACE
