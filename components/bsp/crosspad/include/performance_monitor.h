/**
 * @file performance_monitor.h
 * @brief Performance monitoring utilities for CrossPad
 *
 * Provides functions to monitor FreeRTOS task CPU usage, stack usage,
 * heap statistics, and system performance metrics.
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for periodic performance monitoring
 */
typedef struct {
    uint32_t interval_ms;       ///< Monitoring interval in milliseconds
    bool print_task_stats;      ///< Enable task runtime statistics
    bool print_stack_usage;     ///< Enable stack high water mark reporting
    bool print_heap_stats;      ///< Enable heap statistics
} perf_monitor_config_t;

/**
 * @brief Print FreeRTOS task runtime statistics
 *
 * Shows CPU usage percentage for each task. Requires:
 * - CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
 * - CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y
 */
void perf_print_task_stats(void);

/**
 * @brief Print FreeRTOS task list
 *
 * Shows task names, states, priorities, and stack usage.
 * Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y
 */
void perf_print_task_list(void);

/**
 * @brief Print stack high water marks for all tasks
 *
 * Shows remaining stack space (in bytes) for each task.
 * Useful for identifying oversized stacks.
 */
void perf_print_stack_usage(void);

/**
 * @brief Print heap statistics
 *
 * Shows free heap, largest free block, minimum free ever,
 * for both internal RAM and SPIRAM (if available).
 */
void perf_print_heap_stats(void);

/**
 * @brief Print comprehensive performance report
 *
 * Combines all statistics into a single formatted report.
 */
void perf_print_all_stats(void);

/**
 * @brief Start periodic performance monitoring task
 *
 * Creates a low-priority FreeRTOS task that periodically prints
 * performance statistics to the console.
 *
 * @param config Monitoring configuration (NULL for defaults)
 * @return Task handle, or NULL on failure
 *
 * @note The task runs at priority 1 (very low) to minimize impact
 */
bool perf_start_monitor(perf_monitor_config_t* config);   /* registers a bsp_worker job */

#ifdef __cplusplus
}
#endif
