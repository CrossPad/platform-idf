#pragma once
#include "crosspad/platform/IClock.hpp"

namespace crosspad {

class EspClock : public IClock {
public:
    int64_t getTimeUs() override;
};

} // namespace crosspad
