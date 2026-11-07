#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace crosspad {
class BleControlMailbox {
    std::mutex mutex_;
    char command_[192]{};
    size_t used_ = 0;
    uint8_t response_[481]{};
    size_t responseSize_ = 1;
    bool pending_ = false;
    bool executing_ = false;
    uint32_t generation_ = 0;
public:
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        used_ = 0; pending_ = executing_ = false;
        response_[0] = 0; responseSize_ = 1; ++generation_;
    }
    bool write(const uint8_t *bytes, size_t length) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ || executing_ || !length) return false;
        for (size_t i = 0; i < length; ++i) {
            const uint8_t c = bytes[i];
            if ((c == '\n' && i != length - 1) || (c != '\n' && (c < 32 || c > 126)) || used_ + 1 >= sizeof(command_)) {
                used_ = 0; response_[0] = 0; responseSize_ = 1; return false;
            }
            if (c == '\n') {
                if (!used_) return false;
                command_[used_] = 0; pending_ = true;
            } else command_[used_++] = static_cast<char>(c);
        }
        response_[0] = 0; responseSize_ = 1;
        return true;
    }
    bool take(char *command, uint32_t &generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_) return false;
        std::memcpy(command, command_, used_ + 1);
        pending_ = false; executing_ = true; generation = generation_;
        return true;
    }
    void finish(const char *reply, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return;
        const size_t size = std::strlen(reply);
        if (size > 480) reply = "ERR response_too_large\r\n";
        response_[0] = 1;
        responseSize_ = std::strlen(reply) + 1;
        std::memcpy(response_ + 1, reply, responseSize_ - 1);
        used_ = 0; executing_ = false;
    }
    bool busy() {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_ || executing_;
    }
    bool ready() {
        std::lock_guard<std::mutex> lock(mutex_);
        return response_[0] == 1 && !pending_ && !executing_;
    }
    size_t read(uint8_t *out) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::memcpy(out, response_, responseSize_);
        return responseSize_;
    }
};
}
