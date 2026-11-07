#include "EspStm32MidiTransport.hpp"
#include "stm32_uart.h"
#include <atomic>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "CrosspadPlatform.hpp"

static const char* TAG = "STM32_MIDI_TRANSPORT";
/* Bytes from the STM that did not fit the parser stream buffer (PAD_STATS sbuf_drop). */
std::atomic<uint32_t> g_stm_sbuf_drop{0};
/* The parser's task (uartLoop), notified per stored chunk. */
static std::atomic<TaskHandle_t> s_rx_waiter{nullptr};

BEGIN_MIDI_NAMESPACE

// Static member initialization
StreamBufferHandle_t EspStm32MidiTransport::midi_rx_stream = NULL;
bool EspStm32MidiTransport::stream_initialized = false;
bool EspStm32MidiTransport::thruActivated = false;

// Callback function for received MIDI data
static void midi_data_callback(const uint8_t* data, size_t length, void* user_data)
{
    EspStm32MidiTransport::queueMidiData(data, length);
}

EspStm32MidiTransport::EspStm32MidiTransport()
    : initialized(false)
{
}

EspStm32MidiTransport::~EspStm32MidiTransport()
{
    if (initialized) {
        end();
    }
}

esp_err_t EspStm32MidiTransport::initMidiStream()
{
    if (!stream_initialized) {
        midi_rx_stream = xStreamBufferCreate(MIDI_STREAM_BUF_SIZE, 1);
        if (midi_rx_stream == NULL) {
            ESP_LOGE(TAG, "Failed to create MIDI receive stream buffer");
            return ESP_FAIL;
        }
        stream_initialized = true;
        ESP_LOGI(TAG, "MIDI receive stream buffer initialized (%d bytes)", MIDI_STREAM_BUF_SIZE);
    }
    return ESP_OK;
}

void EspStm32MidiTransport::deinitMidiStream()
{
    if (stream_initialized && midi_rx_stream != NULL) {
        vStreamBufferDelete(midi_rx_stream);
        midi_rx_stream = NULL;
        stream_initialized = false;
        ESP_LOGI(TAG, "MIDI receive stream buffer deinitialized");
    }
}

void EspStm32MidiTransport::begin()
{
    if (!initialized) {
        ESP_LOGI(TAG, "Initializing STM32 MIDI transport layer");

        // Initialize UART communication (will not fail if already initialized)
        ESP_RETURN_VOID_ON_ERROR(stm32_uart_init(), TAG, "Failed to initialize STM32 UART");

        // Initialize MIDI receive stream buffer
        ESP_RETURN_VOID_ON_ERROR(initMidiStream(), TAG, "Failed to initialize MIDI stream buffer");

        // Register callback for received MIDI data
        stm32_register_midi_callback(midi_data_callback, NULL);

        initialized = true;
        ESP_LOGI(TAG, "STM32 MIDI transport initialized successfully");
    }
}

void EspStm32MidiTransport::end()
{
    if (initialized) {
        ESP_LOGI(TAG, "Ending STM32 MIDI transport layer");

        // Unregister MIDI callback
        stm32_register_midi_callback(NULL, NULL);

        // Deinitialize MIDI stream buffer
        deinitMidiStream();

        // Note: We don't deinitialize UART here as it might be used by other components
        // stm32_uart_deinit() should be called by the component that initialized it

        initialized = false;
    }
}

void EspStm32MidiTransport::write(uint8_t data)
{
    if (initialized) {
        crosspad::getStm32Transport().send(&data, 1);
    }
}

void EspStm32MidiTransport::write(const uint8_t* data, size_t length)
{
    if (initialized && data && length > 0) {
        crosspad::getStm32Transport().send(data, length);
    }
}

bool EspStm32MidiTransport::available()
{
    if (!initialized || !stream_initialized || midi_rx_stream == NULL) {
        return false;
    }

    return xStreamBufferBytesAvailable(midi_rx_stream) > 0;
}

uint8_t EspStm32MidiTransport::read()
{
    if (!initialized || !stream_initialized || midi_rx_stream == NULL) {
        return 0;
    }

    uint8_t data = 0;
    if (xStreamBufferReceive(midi_rx_stream, &data, 1, 0) == 1) {
        return data;
    }

    return 0; // Stream empty
}

void EspStm32MidiTransport::flush()
{
    if (initialized && stream_initialized && midi_rx_stream != NULL) {
        xStreamBufferReset(midi_rx_stream);
    }
}

void EspStm32MidiTransport::queueMidiData(const uint8_t* data, size_t length)
{
    if (!stream_initialized || midi_rx_stream == NULL || data == NULL || length == 0) {
        return;
    }

    // Bulk write — StreamBuffer handles this in a single operation
    size_t sent = xStreamBufferSend(midi_rx_stream, data, length, 0);
    if (sent > 0) {
        if (TaskHandle_t t = s_rx_waiter.load(std::memory_order_acquire)) xTaskNotifyGive(t);
    }
    if (sent < length) {
        g_stm_sbuf_drop.fetch_add(length - sent, std::memory_order_relaxed);
        ESP_LOGW(TAG, "MIDI stream buffer full, dropped %d/%d bytes", (int)(length - sent), (int)length);
    }
}

void EspStm32MidiTransport::wakeOnRx(TaskHandle_t task)
{
    s_rx_waiter.store(task, std::memory_order_release);
}

void EspStm32MidiTransport::queueMidiByte(uint8_t data)
{
    if (!stream_initialized || midi_rx_stream == NULL) {
        return;
    }

    if (xStreamBufferSend(midi_rx_stream, &data, 1, 0) == 0) {
        ESP_LOGW(TAG, "MIDI stream buffer full, dropping byte: 0x%02X", data);
    }
}

END_MIDI_NAMESPACE
