#include "EspTinyUsbMidiTransport.h"
#include <atomic>
#include "tusb.h"
#include "device/usbd.h"
#include "class/midi/midi_device.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char* TAG = "ESP_TINYUSB_MIDI";
/* USB MIDI packets that reached the TinyUSB FIFO whole vs. were dropped
 * (PAD_STATS usb_tx / usb_drop). */
std::atomic<uint32_t> g_usb_midi_tx_ok{0};
std::atomic<uint32_t> g_usb_midi_tx_fail{0};
/* Set when a send gave up on a full FIFO, cleared by the next one that fits:
 * the host is not reading MIDI IN, so do not wait for it. */
static std::atomic<bool> s_host_deaf{false};

BEGIN_MIDI_NAMESPACE

// Static member definitions
QueueHandle_t EspTinyUsbMidiTransport::usb_midi_rx_queue = NULL;
bool EspTinyUsbMidiTransport::queue_initialized = false;
bool EspTinyUsbMidiTransport::usb_midi_connected = false;

// Static member initialization
bool EspTinyUsbMidiTransport::thruActivated = false;
volatile bool EspTinyUsbMidiTransport::tx_blocked = false;

void EspTinyUsbMidiTransport::setTxBlocked(bool blocked)
{
    tx_blocked = blocked;
}

bool EspTinyUsbMidiTransport::txBlocked()
{
    return tx_blocked;
}

EspTinyUsbMidiTransport::EspTinyUsbMidiTransport()
    : initialized(false), midi_out_index(0), midi_out_length(0)
{
    memset(midi_out_buffer, 0, sizeof(midi_out_buffer));
    ESP_LOGI(TAG, "EspTinyUsbMidiTransport constructor called");
}

EspTinyUsbMidiTransport::~EspTinyUsbMidiTransport()
{
    if (initialized) {
        end();
    }
}

esp_err_t EspTinyUsbMidiTransport::initMidiQueue()
{
    if (!queue_initialized) {
        usb_midi_rx_queue = xQueueCreate(USB_MIDI_QUEUE_SIZE, sizeof(uint8_t));
        if (usb_midi_rx_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create USB MIDI receive queue");
            return ESP_FAIL;
        }
        queue_initialized = true;
        ESP_LOGI(TAG, "USB MIDI receive queue initialized");
    }
    return ESP_OK;
}

void EspTinyUsbMidiTransport::deinitMidiQueue()
{
    if (queue_initialized && usb_midi_rx_queue != NULL) {
        vQueueDelete(usb_midi_rx_queue);
        usb_midi_rx_queue = NULL;
        queue_initialized = false;
        ESP_LOGI(TAG, "USB MIDI receive queue deinitialized");
    }
}

// Helper method to determine MIDI message length from status byte
uint8_t EspTinyUsbMidiTransport::getMidiMessageLength(uint8_t status)
{
    if ((status & 0xF0) == 0xF0) {  // System messages
        switch (status) {
            case 0xF0: return 0;  // SysEx start - variable length, handle differently
            case 0xF1: case 0xF3: return 2;
            case 0xF2: return 3;
            case 0xF7: return 1;  // SysEx end
            default: return 1;
        }
    }
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;  // Note off/on, aftertouch, control change, pitch bend
        case 0xC0: case 0xD0: return 2;  // Program change, channel aftertouch
        default: return 1;
    }
}

// Send complete MIDI message as USB packet. Returns true if bytes were accepted by the FIFO.
bool EspTinyUsbMidiTransport::sendMidiPacket(uint8_t* message, uint8_t length)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Cannot send MIDI packet: transport not initialized");
        return false;
    }
    if (length == 0) {
        ESP_LOGE(TAG, "Cannot send MIDI packet: zero length");
        return false;
    }
    // The USB stack is being torn down or brought up (see setTxBlocked): the
    // MIDI class driver's endpoint mutex may already be gone while the device
    // still reports itself mounted, and writing then asserts inside TinyUSB.
    if (tx_blocked) {
        return false;
    }
    // The TX FIFO is only 64 B (CFG_TUD_MIDI_TX_BUFSIZE), so reply/forward
    // bursts overflow it instantly and a partial stream_write corrupts the
    // frame. Retry while the host makes progress draining the FIFO; when no
    // host is listening (USB enumerated, FIFO stays full) give up after a few
    // stalled ticks and drop silently.
    //
    // A host that enumerated MIDI but never reads it (a phone without a MIDI
    // app, a PC that only takes the audio) keeps the FIFO full for good, and
    // those stalled ticks were 2-3 ms per message in front of the next pad's
    // note in uartLoop. So once a send has given up, a channel message tries
    // once and is dropped until one fits again: it is one whole USB packet,
    // which tud_midi_stream_write takes entirely or not at all. SysEx keeps
    // the wait -- it comes here a byte at a time, and one byte lost would
    // corrupt the frame.
    uint32_t written = tud_midi_stream_write(0, message, length);
    const bool channel_msg = message[0] >= 0x80 && message[0] < 0xF0;
    uint8_t stalled_ticks =
        (written == 0 && channel_msg && s_host_deaf.load(std::memory_order_relaxed)) ? 3 : 0;
    while (written < length && stalled_ticks < 3) {
        if (!tud_midi_mounted()) {
            return false;
        }
        vTaskDelay(1);
        uint32_t n = tud_midi_stream_write(0, message + written, length - written);
        if (n == 0) {
            stalled_ticks++;
        } else {
            stalled_ticks = 0;
            written += n;
        }
    }
    if (written == length) {
        s_host_deaf.store(false, std::memory_order_relaxed);
        g_usb_midi_tx_ok.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    s_host_deaf.store(true, std::memory_order_relaxed);
    g_usb_midi_tx_fail.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void EspTinyUsbMidiTransport::begin()
{
    if (initialized) {
        ESP_LOGI(TAG, "ESP TinyUSB MIDI transport already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing ESP TinyUSB MIDI transport");

    // Initialize MIDI receive queue
    ESP_RETURN_VOID_ON_ERROR(initMidiQueue(), TAG, "Failed to initialize MIDI queue");

    // Check if TinyUSB is initialized and MIDI is available
    if (!tusb_inited()) {
        ESP_LOGE(TAG, "TinyUSB not initialized - cannot initialize MIDI transport");
        return;
    }

    initialized = true;
    usb_midi_connected = false;
    ESP_LOGI(TAG, "ESP TinyUSB MIDI transport initialized successfully");
}

bool EspTinyUsbMidiTransport::beginTransmission(uint8_t address)
{
    // Not used in this transport
    ESP_LOGV(TAG, "beginTransmission called with address %d", address);
    return true;
}

void EspTinyUsbMidiTransport::endTransmission()
{
    // Not used in this transport
    ESP_LOGV(TAG, "endTransmission called");
}

void EspTinyUsbMidiTransport::end()
{
    if (!initialized) {
        return;
    }

    ESP_LOGI(TAG, "Ending ESP TinyUSB MIDI transport");

    // Deinitialize MIDI queue
    deinitMidiQueue();

    initialized = false;
    usb_midi_connected = false;
    ESP_LOGI(TAG, "ESP TinyUSB MIDI transport ended");
}

void EspTinyUsbMidiTransport::write(uint8_t data)
{
    if (!initialized) {
        ESP_LOGE(TAG, "ESP TinyUSB MIDI transport not initialized");
        return;
    }

    if (!tud_midi_mounted()) {
        midi_out_index = 0;
        midi_out_length = 0;
        return;
    }

    ESP_LOGD(TAG, "USB MIDI TX: Arduino MIDI library called write(0x%02X)", data);

    // Handle MIDI message buffering
    if (midi_out_index == 0) {
        // First byte - determine message length
        midi_out_length = getMidiMessageLength(data);
        ESP_LOGV(TAG, "USB MIDI TX: Message length for 0x%02X is %d", data, midi_out_length);
        if (midi_out_length == 0) {
            // Variable length message (SysEx) - handle differently
            midi_out_buffer[midi_out_index++] = data;
            // For now, just send single byte messages
            sendMidiPacket(&data, 1);
            midi_out_index = 0;
            return;
        }
    }

    // Add byte to buffer
    midi_out_buffer[midi_out_index++] = data;

    // Check if we have a complete message
    if (midi_out_index >= midi_out_length) {
        // Keep TX logging off the hot path — per-packet INFO logs saturate
        // the console during SysEx bursts (same failure mode as RX, fdec270).
        if (sendMidiPacket(midi_out_buffer, midi_out_length)) {
            ESP_LOGD(TAG, "TX [%02X %02X %02X]",
                     midi_out_buffer[0], midi_out_buffer[1], midi_out_buffer[2]);
        }
        midi_out_index = 0;  // Reset for next message
        midi_out_length = 0;
    }
}

void EspTinyUsbMidiTransport::write(const uint8_t* data, size_t length)
{
    if (!initialized) {
        ESP_LOGE(TAG, "ESP TinyUSB MIDI transport not initialized - cannot write buffer");
        return;
    }

    if (data == NULL || length == 0) {
        ESP_LOGE(TAG, "Invalid parameters: data=%p, length=%d", data, length);
        return;
    }

    if (!tud_midi_mounted()) {
        return;
    }

    ESP_LOGV(TAG, "USB MIDI TX: Arduino MIDI library called write(buffer, %d)", length);    // If this is a complete MIDI message, send it directly
    if (length >= 2 && length <= 3) {
        ESP_LOGV(TAG, "USB MIDI TX: Sending complete message directly");
        sendMidiPacket((uint8_t*)data, length);
        return;
    }

    // Otherwise, process data byte by byte using the single byte write method
    for (size_t i = 0; i < length; i++) {
        write(data[i]);
    }
}

bool EspTinyUsbMidiTransport::available()
{
    if (!initialized || !queue_initialized || usb_midi_rx_queue == NULL) {
        ESP_LOGW(TAG, "ESP TinyUSB MIDI transport not available");
        return false;
    }

    return uxQueueMessagesWaiting(usb_midi_rx_queue) > 0;
}

uint8_t EspTinyUsbMidiTransport::read()
{
    if (!initialized || !queue_initialized || usb_midi_rx_queue == NULL) {
        ESP_LOGE(TAG, "ESP TinyUSB MIDI transport not initialized or queue not available");
        return 0;
    }

    uint8_t data = 0;
    if (xQueueReceive(usb_midi_rx_queue, &data, 0) == pdTRUE) {
        return data;
    }

    return 0;
}

void EspTinyUsbMidiTransport::flush()
{
    if (!initialized || !queue_initialized || usb_midi_rx_queue == NULL) {
        return;
    }

    // Clear the queue
    uint8_t dummy;
    while (xQueueReceive(usb_midi_rx_queue, &dummy, 0) == pdTRUE) {
        // Keep clearing until empty
    }

    // Also reset outgoing message buffer
    midi_out_index = 0;
    midi_out_length = 0;
    memset(midi_out_buffer, 0, sizeof(midi_out_buffer));
}

void EspTinyUsbMidiTransport::poll()
{
    if (!initialized) return;

    if (!queue_initialized || usb_midi_rx_queue == NULL) {
        ESP_LOGE(TAG, "ESP TinyUSB MIDI transport queue not initialized");
        return;
    }

    // Update connection status
    usb_midi_connected = tud_midi_mounted();

    if (!usb_midi_connected) {
        return;
    }

    // Poll for available MIDI data
    while (tud_midi_available()) {
        // Backpressure: a packet carries at most 3 data bytes. If the queue
        // can't take a whole packet, leave it in the TinyUSB FIFO — once the
        // FIFO fills the endpoint NAKs and the host retries, so a burst is
        // throttled instead of dropped. The parser drains the queue right
        // after poll() and the next iteration picks the FIFO back up.
        if (uxQueueSpacesAvailable(usb_midi_rx_queue) < 3) {
            break;
        }
        uint8_t packet[4];
        if (tud_midi_packet_read(packet)) {
            // CIN (Code Index Number) determines how many data bytes are valid.
            // Using packet[i]!=0 is WRONG — 0x00 is a valid MIDI data byte.
            static const uint8_t cin_to_len[] = {
                0, 0, 2, 3,  // 0x0-0x3: reserved / cable / 2-byte / 3-byte system common
                3, 1, 2, 3,  // 0x4-0x7: SysEx start+cont(3) / SysEx end(1,2,3 bytes)
                3, 3, 3, 3,  // 0x8-0xB: NoteOff / NoteOn / PolyAT / CC
                2, 2, 3, 1,  // 0xC-0xF: ProgramChg / ChanPressure / PitchBend / SingleByte
            };
            uint8_t cin = packet[0] & 0x0F;
            uint8_t data_len = cin_to_len[cin];

            // Per-packet logging must stay off the INFO path: at MIDI-storm
            // rates the UART driver monopolizes CPU0 and trips the task
            // watchdog (IDLE0 starvation observed at ~100 pkt/s).
            ESP_LOGD(TAG, "Received USB MIDI packet: [%02X %02X %02X %02X] CIN=%X len=%d",
                     packet[0], packet[1], packet[2], packet[3], cin, data_len);

            // Overflow warn is time-limited: under sustained overload a
            // per-drop (or per-burst) warn still saturates the UART.
            static TickType_t last_overflow_warn = 0;
            for (int i = 1; i <= data_len; i++) {
                if (xQueueSend(usb_midi_rx_queue, &packet[i], 0) != pdTRUE) {
                    TickType_t now = xTaskGetTickCount();
                    if (now - last_overflow_warn >= pdMS_TO_TICKS(1000)) {
                        last_overflow_warn = now;
                        ESP_LOGW(TAG, "USB MIDI receive queue full, dropping data");
                    }
                    break;
                }
            }
        }
    }
}


END_MIDI_NAMESPACE


