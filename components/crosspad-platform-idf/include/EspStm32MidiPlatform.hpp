#pragma once

#include <stdint.h>
#include "esp_timer.h"
#include "midi_Namespace.h"

BEGIN_MIDI_NAMESPACE

// ESP-IDF Platform for Arduino MIDI Library
struct EspStm32MidiPlatform
{
    static unsigned long now() {
        return esp_timer_get_time() / 1000; // Convert microseconds to milliseconds
    }
};

END_MIDI_NAMESPACE
