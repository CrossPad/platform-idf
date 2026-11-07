#include "EspTinyUsbMidiTransport.h"
#include "tusb.h"
#include "device/usbd.h"
#include "class/midi/midi_device.h"
#include "esp_log.h"

static const char* TAG = "ESP_TINYUSB_MIDI_TASK";

static bool usb_midi_connected = false;
void midi_task_read_example(void *arg)
{
    // Clean USB MIDI input handler
    uint8_t packet[4];
    
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2));
        
        while (tud_midi_available() && usb_midi_connected) {
            if (tud_midi_packet_read(packet)) {
                ESP_LOGI(TAG, "USB MIDI IN: [%02X %02X %02X %02X]", 
                         packet[0], packet[1], packet[2], packet[3]);
                
            }
        }
    }
    vTaskDelete(NULL);
}

void periodic_midi_write_example_cb(void *arg)
{
    // Example melody stored as an array of note values
    uint8_t const note_sequence[] = {
        74, 78, 81, 86, 90, 93, 98, 102, 57, 61, 66, 69, 73, 78, 81, 85, 88, 92, 97, 100, 97, 92, 88, 85, 81, 78,
        74, 69, 66, 62, 57, 62, 66, 69, 74, 78, 81, 86, 90, 93, 97, 102, 97, 93, 90, 85, 81, 78, 73, 68, 64, 61,
        56, 61, 64, 68, 74, 78, 81, 86, 90, 93, 98, 102
    };

    static uint8_t const cable_num = 0; // MIDI jack associated with USB endpoint
    static uint8_t const channel = 0; // 0 for channel 1
    static uint32_t note_pos = 0;

    // Previous positions in the note sequence.
    int previous = note_pos - 1;

    // If we currently are at position 0, set the
    // previous position to the last note in the sequence.
    if (previous < 0) {
        previous = sizeof(note_sequence) - 1;
    }

    // Send Note On for current position at full velocity (127) on channel 1.
    ESP_LOGI(TAG, "Writing MIDI data %d", note_sequence[note_pos]);

    if (tud_midi_mounted()) {
        usb_midi_connected = true;
        uint8_t note_on[3] = {0x90 | channel, note_sequence[note_pos], 127};
        tud_midi_stream_write(cable_num, note_on, 3);

        // Send Note Off for previous note.
        uint8_t note_off[3] = {0x80 | channel, note_sequence[previous], 0};
        tud_midi_stream_write(cable_num, note_off, 3);
    }
    else {
        usb_midi_connected = false;
    }

    // Increment position
    note_pos++;

    // If we are at the end of the sequence, start over.
    if (note_pos >= sizeof(note_sequence)) {
        note_pos = 0;
    }
}

void start_midi_simple_reader_and_sender()
{
    // USB MIDI example tasks (separate from internal MIDI system)
    const esp_timer_create_args_t periodic_midi_args = {
        .callback = &periodic_midi_write_example_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "periodic_midi",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t periodic_midi_timer;

    // uncomment to enable simple melody MIDI output
    ESP_LOGI(TAG, "Starting USB MIDI example tasks");
    ESP_ERROR_CHECK(esp_timer_create(&periodic_midi_args, &periodic_midi_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_midi_timer, 286 * 1000)); // 286ms tempo

    xTaskCreate(midi_task_read_example, "midi_read_example", 2560, NULL, 5, NULL);
}