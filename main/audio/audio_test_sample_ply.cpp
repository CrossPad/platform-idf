/**
 * @file audio_test_sample_ply.cpp
 * @brief Implementation of a test of the sample stream player
 *
 * @date 25.02.2026
 * @author Marcel Licence
 */


#include "bsp_sdmmc.h"
#include "engine_handoff.h"

#include <ml_smpl_stream_ply.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>


/* The engine is started and stopped at runtime — a pitched kit owns its PSRAM
 * while it is current — so "ready" is the handoff's flag, not a boot-time
 * latch. Before the first start the library's note handlers dereference memory
 * that was never allocated, and the pad path (prio-6 uartLoop) runs long before
 * setup finishes in prio-1 app_main. */
bool smpl_stream_ply_ready(void)
{
    return marcel_engine_running();
}

void smpl_stream_ply_test(void)
{
    printf("\033[32msetup_smpl_stream_ply\033[0m\n");
    fflush(stdout);

    /* app_main runs at priority 1 on core 0, under the LVGL task (4) drawing
     * the animated splash: the ~130 ms of CPU this start needs took 3.3 s at
     * boot (smoke 05.10: 'control block' 2011 -> 'started' 5280), the codecs
     * and the launcher waiting behind it, IDLE0 starved all along. Above
     * LVGL only for the start; the splash stalls for that long. */
    const UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 5);
    const bool started = marcel_engine_start();
    vTaskPrioritySet(NULL, prio);
    if (!started)
    {
        return;
    }

    char startSoundFilename[] = "crosspad/STARTUP.wav";
    SampleStreamPlayer_Preview(startSoundFilename);
}
