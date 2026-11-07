// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
