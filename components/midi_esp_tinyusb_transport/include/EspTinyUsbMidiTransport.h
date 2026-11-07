#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "midi_Namespace.h"
#include "EspTinyUsbMidiPlatform.h"
#include "freertos/FreeRTOS.h"

BEGIN_MIDI_NAMESPACE

#define USB_MIDI_QUEUE_SIZE 512

class EspTinyUsbMidiTransport
{
public:
    EspTinyUsbMidiTransport();
    ~EspTinyUsbMidiTransport();

    // Arduino MIDI Transport interface
    void begin();
    void end();

    // Send data via USB MIDI
    void write(uint8_t data);
    void write(const uint8_t* data, size_t length);

    bool beginTransmission(uint8_t address);

    void endTransmission();

    // Check if data is available (for receiving)
    bool available();

    // Read data from USB MIDI
    uint8_t read();

    // Flush any pending data
    void flush();

    // Poll for new MIDI data (should be called regularly)
    void poll();

    // Get the transport name
    const char* getName() const { return "ESP TinyUSB MIDI"; }

    // Thru functionality
    static bool thruActivated;

    /**
     * @brief Block/unblock USB MIDI transmission around a USB stack teardown.
     *
     * tud_midi_mounted() is not enough on its own: tud_deinit() runs the class
     * drivers' deinit — which destroys the MIDI endpoint-stream mutex — before
     * it clears _usbd_dev.cfg_num, so there is a window where the device still
     * reports itself mounted and a write asserts inside osal_mutex_lock on a
     * deleted semaphore. Anything that emits MIDI in that window (a pad hit
     * during a USB profile switch) takes the board down, so the switch closes
     * this gate for its duration instead.
     */
    static void setTxBlocked(bool blocked);
    static bool txBlocked();

private:
    bool initialized;

    // MIDI message buffering for outgoing data
    uint8_t midi_out_buffer[4];  // Buffer for outgoing MIDI message
    uint8_t midi_out_index;      // Current position in buffer
    uint8_t midi_out_length;     // Expected length of current message

    // Static members for shared queue
    static QueueHandle_t usb_midi_rx_queue;
    static bool queue_initialized;
    static bool usb_midi_connected;
    static volatile bool tx_blocked;

    // Initialize the shared MIDI receive queue
    static esp_err_t initMidiQueue();
    static void deinitMidiQueue();

    // Helper methods
    uint8_t getMidiMessageLength(uint8_t status);
    bool sendMidiPacket(uint8_t* message, uint8_t length);
};

END_MIDI_NAMESPACE

void periodic_midi_write_example_cb(void *arg);
void midi_task_read_example(void *arg);