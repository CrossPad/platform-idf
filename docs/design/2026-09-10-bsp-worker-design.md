# bsp_worker — one task for the periodic housekeeping loops

Date: 2026-09-10. Decision: merge several tasks into one worker super loop,
modular and easy to extend.

## Problem

Six FreeRTOS tasks exist only to run a short body every N ms: SoftOffMon
(40 ms, STM power state over I2C), PadAnim (10 ms, LED animation + battery
gauge at 1 Hz), imu_task (10 ms, I2C read + Kalman; 1 s backoff when the part
is gone), imu_midi (20 ms, feed the mapper; only while enabled), sd_hotplug
(1 s, 1–30 s backoff without a card), perfmon (10 s, never started today).
Each carries a TCB in internal RAM, a stack sized for its own worst case, and
its own copy of the sleep/backoff idiom. Nothing about them needs a task of
its own: none blocks on a queue or a driver event, and each body is a few ms.

## Design

`components/bsp/crosspad/bsp_worker.c` + `include/bsp_worker.h`, C API so C
(perfmon) and C++ (everything else) register the same way. BSP is the lowest
layer, so any component may depend on it.

```c
typedef uint32_t (*bsp_worker_fn)(void *ctx);   /* returns 0 = keep period, else new period_ms */
typedef struct bsp_worker_job bsp_worker_job_t;
bsp_worker_job_t *bsp_worker_add(const char *name, uint32_t period_ms, bsp_worker_fn fn, void *ctx);
void bsp_worker_set_period(bsp_worker_job_t *job, uint32_t period_ms);
void bsp_worker_remove(bsp_worker_job_t *job);   /* returns after the job is no longer running */
esp_err_t bsp_worker_start(void);                /* once; jobs may be added before or after */
size_t bsp_worker_stats(bsp_worker_stat_t *out, size_t max);  /* name, period, runs, last_us, max_us, overruns */
```

One task `worker`: stack 6 kB in PSRAM (`xTaskCreatePinnedToCoreWithCaps`),
priority 5, core 0 (above LVGL at 4 so animation is not starved by rendering,
below UART drv at 6). Fixed table of 12 jobs behind a `portMUX` spinlock.
Loop: take the earliest `due`; sleep with `ulTaskNotifyTake` until it (an
`add`/`set_period` notifies, so a new job runs without waiting for the old
sleep); run every job whose `due` has passed, in table order; `due += period`
(a job late by more than one period is re-based to now and counts an
`overrun`). Per job `runs`, `last_us`, `max_us`, `overruns`; a body over
5 ms logs once (`W worker: <name> took N us`). A job's return value replaces
its period, which is how sd_hotplug and the IMU express their backoff.

A job is a plain function with static or `ctx` state; it must not block for
more than a few ms and must not call flash write APIs (PSRAM stack). Long
work (the kit rescan after an SD insert) stays a one-shot task started from
the job.

Conversions: the six task bodies become job functions in place;
`bsp_imu_start_task`/`bsp_imu_stop_task` add/remove the IMU job (the
park-and-`vTaskDeleteWithCaps` dance goes away);
`imu_midi_apply_settings` adds/removes the feed job; `perf_start_monitor`
adds a job and returns `bool`.

Visibility: CDC verb `WORKER` prints one `WORKER: name=… period=… runs=…
last_us=… max_us=… overruns=…` row per job and a `WORKER: end n=…` line;
`crosspad-hil` gets it in `knowledge/cdc.yaml` + `verbs.py` + the
completeness test, per the tooling rule.

## Not in scope

UART drv (blocks on UART events, latency-critical), lv_async_fwd (queue),
fx_worker (writes flash), audio_rt/bg, kit_load, WFLoader, ble_midi_tx, the
one-shot tasks. A later step may multiplex the queue-driven ones with a
timeout; not now.

## Verification

Build v2; `bench.py flash` (smoke); `MEM` before/after at the launcher (the
expected gain is the five TCBs, ~2 kB internal — the point is one loop, not
RAM); `WORKER` shows six jobs with `max_us` and zero overruns after a minute;
`app_churn --rounds 3`, `back_churn --rounds 2` (power gestures go through
SoftOffMon's `power_button_feed`); `IMU_STATUS` still updates; `led state`
still animates a pad press.
