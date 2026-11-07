#include "tusb.h"
#include "class/midi/midi_device.h"
#include "uac_descriptors.h"
#include "tusb_config_uac.h"
#include "usb_descriptors.h"
#include <string.h>

/*
 * Audio mode: MIDI + UAC2, no CDC — SysEx over MIDI carries control instead.
 *
 * The DWC2 allows 5 IN endpoints including EP0, and this fits exactly:
 * IN  EP0 + 0x81 MIDI + 0x82 feedback + 0x83 mic
 * OUT 0x01 MIDI + 0x02 speaker
 */

// Interface numbering for audio mode (MIDI + UAC2)
enum {
    ITF_NUM_MIDI_A = 0,
    ITF_NUM_MIDI_STREAMING_A,
    ITF_NUM_AUDIO_CONTROL,
    ITF_NUM_AUDIO_STREAMING_SPK,
    ITF_NUM_AUDIO_STREAMING_MIC,
    ITF_NUM_TOTAL_AUDIO
};

// MIDI endpoints
#define EPNUM_MIDI_OUT_A    0x01
#define EPNUM_MIDI_IN_A     0x81

// Audio endpoints (right after MIDI)
#define EPNUM_AUDIO_SPK_OUT  0x02
#define EPNUM_AUDIO_FB_IN    0x82
#define EPNUM_AUDIO_MIC_IN   0x83

#define CONFIG_AUDIO_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN)

/* Own PID, so Windows does not reuse the default mode's cached driver. */
const tusb_desc_device_t desc_device_audio = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x303A,
    .idProduct = 0x3457,  // Different PID for audio composite mode
    .bcdDevice = USB_BCD_DEVICE(USB_BCD_PROFILE_AUDIO),
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 1
};

const uint8_t desc_cfg_audio[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_AUDIO, 0, CONFIG_AUDIO_TOTAL_LEN, 0, 500),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI_A, 4, EPNUM_MIDI_OUT_A, EPNUM_MIDI_IN_A, 64),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 5, EPNUM_AUDIO_SPK_OUT, EPNUM_AUDIO_MIC_IN, EPNUM_AUDIO_FB_IN),
};

const char* s_str_desc_audio[] = {
    (char[]){0x09, 0x04},       // 0: Language ID (English)
    "Espressif",                 // 1: Manufacturer
    "Crosspad Audio",            // 2: Product
    NULL,                        // 3: Serial — usb_serial_string() at install
    "Crosspad MIDI",             // 4: MIDI interface
    "Crosspad Audio",            // 5: Audio Control interface
    "Crosspad Speaker",          // 6: Speaker stream
    "Crosspad Microphone",       // 7: Mic stream
};
