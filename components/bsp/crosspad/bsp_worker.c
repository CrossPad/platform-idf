#include "bsp_worker.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "worker";

/* Stack in PSRAM: the jobs talk I2C, RMT and the SD card, never flash. 6 kB
 * is the largest of the stacks the merged tasks had (4 kB) plus room for a
 * job that logs. Priority 5: above LVGL (4) so the LED animation is not
 * starved by a redraw, below the STM UART loop (6). */
#define WORKER_STACK_BYTES   6144
#define WORKER_PRIORITY      5
#define WORKER_CORE          0
/* A body this long is a task in disguise, not a job; say so once. */
#define WORKER_SLOW_JOB_US   5000

struct bsp_worker_job {
    const char   *name;
    bsp_worker_fn fn;
    void         *ctx;
    uint32_t      period_ms;
    int64_t       due_us;
    uint32_t      runs;
    uint32_t      last_us;
    uint32_t      max_us;
    uint32_t      overruns;
    bool          active;
    bool          running;
    bool          slow_logged;
};

static struct bsp_worker_job s_jobs[BSP_WORKER_MAX_JOBS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task = NULL;

static void wake(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

bsp_worker_job_t *bsp_worker_add(const char *name, uint32_t period_ms, bsp_worker_fn fn, void *ctx)
{
    if (!fn || period_ms == 0) return NULL;
    bsp_worker_job_t *job = NULL;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < BSP_WORKER_MAX_JOBS; i++) {
        if (!s_jobs[i].active && !s_jobs[i].running) {
            job = &s_jobs[i];
            memset(job, 0, sizeof(*job));
            job->name = name;
            job->fn = fn;
            job->ctx = ctx;
            job->period_ms = period_ms;
            job->due_us = esp_timer_get_time();
            job->active = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (!job) {
        ESP_LOGE(TAG, "no slot for job %s (%d in use)", name, BSP_WORKER_MAX_JOBS);
        return NULL;
    }
    wake();
    return job;
}

void bsp_worker_set_period(bsp_worker_job_t *job, uint32_t period_ms)
{
    if (!job || period_ms == 0) return;
    portENTER_CRITICAL(&s_lock);
    job->period_ms = period_ms;
    job->due_us = esp_timer_get_time() + (int64_t)period_ms * 1000;
    portEXIT_CRITICAL(&s_lock);
    wake();
}

void bsp_worker_remove(bsp_worker_job_t *job)
{
    if (!job) return;
    portENTER_CRITICAL(&s_lock);
    job->active = false;
    portEXIT_CRITICAL(&s_lock);
    /* The caller may free the job's state next, so wait out an execution in
     * progress — unless the caller is the job itself, which is by definition
     * done with that state when it returns. */
    if (xTaskGetCurrentTaskHandle() == s_task) return;
    while (job->running) vTaskDelay(1);
}

size_t bsp_worker_stats(bsp_worker_stat_t *out, size_t max)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < BSP_WORKER_MAX_JOBS && n < max; i++) {
        if (!s_jobs[i].active) continue;
        out[n].name = s_jobs[i].name;
        out[n].period_ms = s_jobs[i].period_ms;
        out[n].runs = s_jobs[i].runs;
        out[n].last_us = s_jobs[i].last_us;
        out[n].max_us = s_jobs[i].max_us;
        out[n].overruns = s_jobs[i].overruns;
        n++;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Pass 1: run everything that is due. The job's fn and ctx are read
         * under the lock; the call itself is not, so a job may add, retime or
         * remove jobs, itself included. */
        int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < BSP_WORKER_MAX_JOBS; i++) {
            bsp_worker_job_t *job = &s_jobs[i];
            bsp_worker_fn fn = NULL;
            void *ctx = NULL;
            portENTER_CRITICAL(&s_lock);
            if (job->active && job->due_us <= now) {
                fn = job->fn;
                ctx = job->ctx;
                job->running = true;
            }
            portEXIT_CRITICAL(&s_lock);
            if (!fn) continue;

            const int64_t t0 = esp_timer_get_time();
            const uint32_t next = fn(ctx);
            const int64_t t1 = esp_timer_get_time();
            const uint32_t took = (uint32_t)(t1 - t0);

            portENTER_CRITICAL(&s_lock);
            job->running = false;
            job->runs++;
            job->last_us = took;
            if (took > job->max_us) job->max_us = took;
            if (job->active) {
                if (next) job->period_ms = next;
                const int64_t period_us = (int64_t)job->period_ms * 1000;
                job->due_us += period_us;
                if (job->due_us + period_us < t1) {
                    /* More than a period late: rebase rather than run a
                     * burst of catch-up iterations nobody asked for. */
                    job->due_us = t1 + period_us;
                    job->overruns++;
                }
            }
            const bool warn = took > WORKER_SLOW_JOB_US && !job->slow_logged;
            if (warn) job->slow_logged = true;
            portEXIT_CRITICAL(&s_lock);
            if (warn) {
                ESP_LOGW(TAG, "job %s took %lu us (period %lu ms) — too long for the worker",
                         job->name, (unsigned long)took, (unsigned long)job->period_ms);
            }
            now = t1;
        }

        /* Pass 2: sleep until the earliest due date, or until a job is added
         * or retimed. */
        int64_t earliest = INT64_MAX;
        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0; i < BSP_WORKER_MAX_JOBS; i++) {
            if (s_jobs[i].active && s_jobs[i].due_us < earliest) earliest = s_jobs[i].due_us;
        }
        portEXIT_CRITICAL(&s_lock);
        const int64_t wait_us = earliest - esp_timer_get_time();
        if (wait_us > 0) {
            TickType_t ticks = (TickType_t)((wait_us + 999) / 1000 / portTICK_PERIOD_MS);
            if (ticks == 0) ticks = 1;
            ulTaskNotifyTake(pdTRUE, earliest == INT64_MAX ? portMAX_DELAY : ticks);
        }
    }
}

esp_err_t bsp_worker_start(void)
{
    if (s_task) return ESP_OK;
    TaskHandle_t h = NULL;
    const BaseType_t r = xTaskCreatePinnedToCoreWithCaps(worker_task, "worker", WORKER_STACK_BYTES,
                                                         NULL, WORKER_PRIORITY, &h, WORKER_CORE,
                                                         MALLOC_CAP_SPIRAM);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_task = h;
    wake();   /* jobs registered before the task existed run now */
    return ESP_OK;
}
