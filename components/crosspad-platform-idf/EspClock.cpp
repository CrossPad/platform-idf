#include "EspClock.hpp"
#include "esp_timer.h"

namespace crosspad {

int64_t EspClock::getTimeUs() {
    return esp_timer_get_time();
}

} // namespace crosspad
