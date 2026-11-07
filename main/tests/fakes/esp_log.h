// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#define ESP_LOGW(tag, fmt, ...) std::printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) std::printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
