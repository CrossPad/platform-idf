#pragma once

#include "crosspad/protocol/IStm32Transport.hpp"
#include "driver/uart.h"
#include "stm32_uart.h"

namespace crosspad {

/**
 * @brief ESP-IDF UART transport for STM32 communication
 */
class EspStm32Transport : public IStm32Transport {
public:
    explicit EspStm32Transport(uart_port_t port) : port_(port) {}

    /// Through stm32_uart_send(), not uart_write_bytes(): while the STM
    /// updater owns the link (raw mode) every MIDI writer, the clock included,
    /// must stay off the wire — a stray byte between a page and its CRC fails
    /// the page and can leave the STM in ROM DFU.
    bool send(const uint8_t* data, size_t len) override {
        if (!data || len == 0) return false;
        return stm32_uart_send(data, len) == static_cast<int>(len);
    }

private:
    uart_port_t port_;
};

} // namespace crosspad
