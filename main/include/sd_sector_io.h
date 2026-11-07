#pragma once
/**
 * @file sd_sector_io.h
 * @brief The card's sector layer (sd_sector_io.cpp): PSRAM transfers bounced
 *        a multi-sector command at a time, and every transaction counted.
 */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Take the bounce buffer. Early in app_main, while internal RAM is whole. */
void sd_bounce_init(void);
/** Off: PSRAM transfers go IDF's way again (a sector a command). For an A/B. */
void sd_bounce_enable(bool on);
/** Whether a transfer to or from PSRAM may be handed to the card as it is. */
bool sd_bounce_ready(void);
/** The cache of single sectors on or off (SD_CACHE); either way it is emptied. */
void sd_cache_enable(bool on);
bool sd_cache_on(void);
/** SD_TRACE's line: transactions by task since the last call, then cleared. */
void sd_trace_cmd(char *out, size_t outLen);

#ifdef __cplusplus
}
#endif
