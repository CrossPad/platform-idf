/**
 * @file bsp_worker.h
 * @brief One task for the periodic housekeeping loops.
 *
 * A job is a short, non-blocking function run every period_ms from a single
 * "worker" task whose stack lives in PSRAM. It replaces a FreeRTOS task that
 * would only ever do "body; vTaskDelay(period)": one TCB instead of one per
 * loop, one place that knows every period, and per-job timing that a CDC
 * verb can print. A job must not block for more than a few milliseconds and
 * must not call flash write APIs (the stack is external memory); long work
 * belongs in a one-shot task started from the job.
 *
 * Design: docs/design/2026-09-10-bsp-worker-design.md
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Runs once per period. Returns 0 to keep the period, or a new period in ms
 *  (how a job backs off when its device is gone). */
typedef uint32_t (*bsp_worker_fn)(void *ctx);

typedef struct bsp_worker_job bsp_worker_job_t;

typedef struct {
    const char *name;
    uint32_t period_ms;
    uint32_t runs;
    uint32_t last_us;
    uint32_t max_us;
    uint32_t overruns;   /**< due dates missed by more than one period */
} bsp_worker_stat_t;

#define BSP_WORKER_MAX_JOBS 12

/** Register a job. May be called before or after bsp_worker_start(); a job
 *  added while the worker sleeps runs without waiting for that sleep to end.
 *  NULL when the table is full. */
bsp_worker_job_t *bsp_worker_add(const char *name, uint32_t period_ms, bsp_worker_fn fn, void *ctx);

/** Change a job's period; the next run is rescheduled from now. */
void bsp_worker_set_period(bsp_worker_job_t *job, uint32_t period_ms);

/** Unregister a job. Returns once the job is no longer executing, so its
 *  state may be torn down afterwards. Safe to call from the job itself. */
void bsp_worker_remove(bsp_worker_job_t *job);

/** Create the worker task. Idempotent. */
esp_err_t bsp_worker_start(void);

/** Copy up to max job statistics into out; returns the number written. */
size_t bsp_worker_stats(bsp_worker_stat_t *out, size_t max);

#ifdef __cplusplus
}
#endif
