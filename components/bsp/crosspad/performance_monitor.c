/**
 * @file performance_monitor.c
 * @brief Performance monitoring utilities for CrossPad
 *
 * Provides functions to monitor FreeRTOS task CPU usage, stack usage,
 * heap statistics, and system performance metrics.
 */

#include "performance_monitor.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps.h"
#include "bsp_worker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static const char* TAG = "PerfMon";

void perf_print_task_stats(void)
{
#if configGENERATE_RUN_TIME_STATS
    char* buffer = (char*)malloc(2048);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for task stats");
        return;
    }

    ESP_LOGI(TAG, "=== FreeRTOS Task Runtime Statistics ===");
    vTaskGetRunTimeStats(buffer);
    ESP_LOGI(TAG, "\n%s", buffer);

    free(buffer);
#else
    ESP_LOGW(TAG, "Task runtime stats not enabled (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=n)");
#endif
}

void perf_print_task_list(void)
{
#if configUSE_TRACE_FACILITY
    char* buffer = (char*)malloc(2048);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for task list");
        return;
    }

    ESP_LOGI(TAG, "=== FreeRTOS Task List ===");
    vTaskList(buffer);
    ESP_LOGI(TAG, "\n%s", buffer);

    free(buffer);
#else
    ESP_LOGW(TAG, "Task list not enabled (CONFIG_FREERTOS_USE_TRACE_FACILITY=n)");
#endif
}

void perf_print_stack_usage(void)
{
    ESP_LOGI(TAG, "\n=== Task Stack Usage (sorted by free, ascending) ===");
    ESP_LOGI(TAG, "  %-20s  %6s  %3s  %s", "Task", "Free", "Pri", "State");
    ESP_LOGI(TAG, "  %-20s  %6s  %3s  %s", "----", "----", "---", "-----");

    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    TaskStatus_t* tasks = (TaskStatus_t*)malloc(taskCount * sizeof(TaskStatus_t));
    if (tasks == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for task status array");
        return;
    }

    UBaseType_t actualCount = uxTaskGetSystemState(tasks, taskCount, NULL);

    // Bubble sort by stack free ascending (most critical first)
    for (UBaseType_t i = 0; i < actualCount; i++) {
        for (UBaseType_t j = i + 1; j < actualCount; j++) {
            if (tasks[j].usStackHighWaterMark < tasks[i].usStackHighWaterMark) {
                TaskStatus_t tmp = tasks[i];
                tasks[i] = tasks[j];
                tasks[j] = tmp;
            }
        }
    }

    for (UBaseType_t i = 0; i < actualCount; i++) {

    	uint32_t stackFree = (uint32_t)tasks[i].usStackHighWaterMark;
    	uint8_t coreID = xTaskGetCoreID(tasks[i].xHandle);

        const char* stateStr;
        switch (tasks[i].eCurrentState) {
            case eRunning:   stateStr = "RUN"; break;
            case eReady:     stateStr = "RDY"; break;
            case eBlocked:   stateStr = "BLK"; break;
            case eSuspended: stateStr = "SUS"; break;
            case eDeleted:   stateStr = "DEL"; break;
            default:         stateStr = "???"; break;
        }

        // Mark tasks with dangerously low stack
        const char* warn = (stackFree < 256) ? " !!" : (stackFree < 512) ? " !" : "";

        ESP_LOGI(TAG, "  %-20s  %5" PRIu32 "b  %3" PRIu32 "  %u  %s%s",
                 tasks[i].pcTaskName,
                 stackFree,
                 (uint32_t)tasks[i].uxCurrentPriority,
		 coreID&7,
                 stateStr, warn);
    }

    ESP_LOGI(TAG, "  Total tasks: %lu", (unsigned long)actualCount);
    free(tasks);
}

void perf_print_heap_stats(void)
{
    multi_heap_info_t info;

    ESP_LOGI(TAG, "=== Heap Statistics ===");

    // Internal RAM (SRAM)
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t sram_used = sram_total - info.total_free_bytes;

    ESP_LOGI(TAG, "Internal SRAM:");
    ESP_LOGI(TAG, "  Total:              %7u bytes", (unsigned)sram_total);
    ESP_LOGI(TAG, "  Used:               %7u bytes (%u%%)", (unsigned)sram_used,
             sram_total ? (unsigned)(sram_used * 100 / sram_total) : 0);
    ESP_LOGI(TAG, "  Free:               %7u bytes", (unsigned)info.total_free_bytes);
    ESP_LOGI(TAG, "  Largest free block: %7u bytes", (unsigned)info.largest_free_block);
    ESP_LOGI(TAG, "  Min free ever:      %7u bytes", (unsigned)info.minimum_free_bytes);
    ESP_LOGI(TAG, "  Allocated blocks:   %7u", (unsigned)info.allocated_blocks);
    ESP_LOGI(TAG, "  Free blocks:        %7u", (unsigned)info.free_blocks);
    ESP_LOGI(TAG, "  Total blocks:       %7u", (unsigned)(info.allocated_blocks + info.free_blocks));

    // DMA-capable subset
    multi_heap_info_t dma_info;
    heap_caps_get_info(&dma_info, MALLOC_CAP_DMA);
    size_t dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "DMA-capable:");
    ESP_LOGI(TAG, "  Total:              %7u bytes", (unsigned)dma_total);
    ESP_LOGI(TAG, "  Free:               %7u bytes", (unsigned)dma_info.total_free_bytes);
    ESP_LOGI(TAG, "  Largest free block: %7u bytes", (unsigned)dma_info.largest_free_block);

    // SPIRAM / PSRAM
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
        size_t psram_used = psram_total - info.total_free_bytes;

        ESP_LOGI(TAG, "PSRAM:");
        ESP_LOGI(TAG, "  Total:              %7u bytes (%.1f MB)", (unsigned)psram_total, psram_total / 1048576.0f);
        ESP_LOGI(TAG, "  Used:               %7u bytes (%u%%)", (unsigned)psram_used,
                 psram_total ? (unsigned)(psram_used * 100 / psram_total) : 0);
        ESP_LOGI(TAG, "  Free:               %7u bytes", (unsigned)info.total_free_bytes);
        ESP_LOGI(TAG, "  Largest free block: %7u bytes", (unsigned)info.largest_free_block);
        ESP_LOGI(TAG, "  Min free ever:      %7u bytes", (unsigned)info.minimum_free_bytes);
        ESP_LOGI(TAG, "  Allocated blocks:   %7u", (unsigned)info.allocated_blocks);
    }
}

void perf_print_all_stats(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========== CrossPad Performance Report ==========");

    perf_print_heap_stats();
    ESP_LOGI(TAG, "");

    perf_print_stack_usage();
    ESP_LOGI(TAG, "");

    perf_print_task_stats();

    ESP_LOGI(TAG, "=================================================");
}

static uint32_t perf_monitor_job(void *ctx)
{
    const perf_monitor_config_t *config = (const perf_monitor_config_t *)ctx;
    if (config->print_heap_stats) {
        perf_print_heap_stats();
    }
    if (config->print_stack_usage) {
        perf_print_stack_usage();
    }
    if (config->print_task_stats) {
        perf_print_task_stats();
    }
    return 0;
}

bool perf_start_monitor(perf_monitor_config_t* config)
{
    static perf_monitor_config_t defaultConfig = {
        .interval_ms = 10000,
        .print_task_stats = true,
        .print_stack_usage = true,
        .print_heap_stats = true
    };
    if (config == NULL) {
        ESP_LOGW(TAG, "NULL config, using defaults");
        config = &defaultConfig;
    }
    /* A worker job, not a task: it only reads counters and logs. The config
     * must outlive the job — callers pass a static or the default above. */
    if (bsp_worker_add("perfmon", config->interval_ms, perf_monitor_job, config) == NULL) {
        ESP_LOGE(TAG, "Failed to register performance monitor job");
        return false;
    }
    ESP_LOGI(TAG, "Performance monitor started (interval: %lu ms)", (unsigned long)config->interval_ms);
    return true;
}
