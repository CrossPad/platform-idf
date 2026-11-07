# Performance Monitoring (PERFMON) Guide

## Overview

ESP32-S3's Xtensa cores include hardware performance counters that can measure CPU cycles, instruction counts, cache hits/misses, and other low-level metrics. This is useful for profiling critical audio processing loops like I2S read/write operations.

## Configuration

Performance monitoring is available by default in ESP-IDF for Xtensa-based chips (ESP32, ESP32-S3). No additional configuration is required.

## API Reference

### Include Headers

```c
#include "xtensa/hal.h"
#include "xtensa_perfmon_access.h"
#include "xtensa_perfmon_apis.h"
```

### Initialize Performance Counters

```c
xtensa_perfmon_init(
    0,                       // CPU ID (0 or 1 on dual-core ESP32-S3)
    0,                       // Trace level (reserved, use 0)
    XTPERF_CNT_CYCLES,       // Counter 0: CPU cycles
    XTPERF_CNT_INSN,         // Counter 1: Instructions executed
    XTPERF_CNT_D_LOAD_U1,    // Counter 2: D-cache load misses
    XTPERF_CNT_D_STORE_U1,   // Counter 3: D-cache store misses
    XTPERF_CNT_I_MISS,       // Counter 4: I-cache misses
    XTPERF_CNT_I_HIT         // Counter 5: I-cache hits
);
```

**Available counter types** (see `xtensa_perfmon_masks.h`):

- `XTPERF_CNT_CYCLES` — Total CPU cycles
- `XTPERF_CNT_INSN` — Instructions completed
- `XTPERF_CNT_D_LOAD_U1` — D-cache load misses
- `XTPERF_CNT_D_STORE_U1` — D-cache store misses
- `XTPERF_CNT_I_MISS` — I-cache misses
- `XTPERF_CNT_I_HIT` — I-cache hits
- `XTPERF_CNT_D_HIT` — D-cache hits
- `XTPERF_CNT_BUBBLES` — Pipeline bubbles (stalls)
- `XTPERF_CNT_BRANCH_TAKEN` — Branches taken
- `XTPERF_CNT_BRANCH_MISS` — Branch mispredictions
- Many more (check ESP-IDF `xtensa_perfmon_masks.h`)

### Measure Code Section

```c
// Reset counters to zero
xtensa_perfmon_reset(0);

// Start counting
xtensa_perfmon_start();

/* === YOUR CODE TO MEASURE === */
i2s_channel_read(...);
process_audio_data(...);
i2s_channel_write(...);
/* ============================= */

// Stop counting
xtensa_perfmon_stop();

// Read counter values
uint32_t cycles = xtensa_perfmon_value(0);  // Counter 0
uint32_t insn = xtensa_perfmon_value(1);    // Counter 1
uint32_t d_load_miss = xtensa_perfmon_value(2);
uint32_t d_store_miss = xtensa_perfmon_value(3);
uint32_t i_miss = xtensa_perfmon_value(4);
uint32_t i_hit = xtensa_perfmon_value(5);

// Calculate metrics
float cpi = (float)cycles / (float)insn;  // Cycles per instruction
float i_hit_rate = 100.0f * (float)i_hit / (float)(i_hit + i_miss);

ESP_LOGI(TAG, "Cycles=%u Insn=%u CPI=%.2f I-Hit-Rate=%.1f%%",
         cycles, insn, cpi, i_hit_rate);
```

### Cleanup

```c
xtensa_perfmon_dump();  // Optional: dump all counter values to log
```

## Example: I2S Loopback with Perfmon

See [`examples/es8388_example.c`](examples/es8388_example.c) → `audio_loopback_perfmon_example()` for a complete working example.

The example:
1. Initializes 6 performance counters
2. Measures each iteration of an I2S read → process → write loop
3. Logs statistics every 10 iterations
4. Calculates average metrics over 100 iterations

**Example output:**

```
I (1234) es8388_example: [  0] Cycles=123456 Insn=45678 CPI=2.70 D-L-Miss=234 D-S-Miss=189 I-Hit=98.5%
I (1345) es8388_example: [ 10] Cycles=121234 Insn=44890 CPI=2.70 D-L-Miss=230 D-S-Miss=185 I-Hit=98.7%
...
I (5678) es8388_example: === Performance Statistics (avg over 100 iterations) ===
I (5679) es8388_example:   CPU Cycles:        122345
I (5680) es8388_example:   Instructions:      45234
I (5681) es8388_example:   CPI:               2.703
I (5682) es8388_example:   D-Cache Load Miss: 232
I (5683) es8388_example:   D-Cache Store Miss:187
I (5684) es8388_example:   I-Cache Misses:    12
I (5685) es8388_example:   I-Cache Hits:      789
I (5686) es8388_example:   I-Cache Hit Rate:  98.50%
```

## Interpreting Results

### Cycles Per Instruction (CPI)

- **Lower is better** (more efficient)
- Typical range: 1.0 - 4.0
- High CPI (>3.0) indicates:
  - Cache misses forcing memory stalls
  - Pipeline bubbles
  - Complex instructions (division, FPU ops)

### Cache Hit Rates

- **Higher is better** (fewer expensive memory accesses)
- I-cache hit rate should be >95% for tight loops
- D-cache misses are common when processing large buffers

### Optimization Tips

1. **Reduce D-cache misses:**
   - Use smaller buffers (fit in cache)
   - Align data to cache line boundaries (64 bytes on ESP32-S3)
   - Use `IRAM_ATTR` / `DRAM_ATTR` for critical buffers

2. **Reduce I-cache misses:**
   - Use `IRAM_ATTR` on hot functions
   - Avoid function pointer calls in tight loops
   - Inline small functions

3. **Reduce CPI:**
   - Use integer operations instead of float when possible
   - Avoid divisions (use shifts/multiplies)
   - Enable compiler optimizations (`CONFIG_COMPILER_OPTIMIZATION_PERF=y`)

## Integration into Main Project

To add perfmon to your I2S audio loop in the main application:

1. Include headers:
   ```c
   #include "xtensa/hal.h"
   #include "xtensa_perfmon_access.h"
   #include "xtensa_perfmon_apis.h"
   ```

2. Initialize once at startup (e.g., in audio task init):
   ```c
   xtensa_perfmon_init(0, 0,
                       XTPERF_CNT_CYCLES,
                       XTPERF_CNT_INSN,
                       XTPERF_CNT_D_LOAD_U1,
                       XTPERF_CNT_D_STORE_U1,
                       XTPERF_CNT_I_MISS,
                       XTPERF_CNT_I_HIT);
   ```

3. Wrap critical sections:
   ```c
   xtensa_perfmon_reset(0);
   xtensa_perfmon_start();

   // Your I2S processing code

   xtensa_perfmon_stop();
   uint32_t cycles = xtensa_perfmon_value(0);
   // Log or store for analysis
   ```

4. **Optional:** Conditionally compile perfmon code:
   ```c
   #ifdef CONFIG_XTENSA_PERFMON_ENABLE
       xtensa_perfmon_start();
   #endif

   // Code to measure

   #ifdef CONFIG_XTENSA_PERFMON_ENABLE
       xtensa_perfmon_stop();
       uint32_t cycles = xtensa_perfmon_value(0);
       ESP_LOGI(TAG, "Cycles: %u", cycles);
   #endif
   ```

## References

- [ESP-IDF Performance Monitor API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/perfmon.html)
- Xtensa ISA documentation (Chapter 7: Performance Monitor)
- ESP-IDF headers: `components/xtensa/include/xtensa_perfmon_*.h`

## Notes

- Perfmon has negligible overhead (~10 cycles for start/stop)
- Counters are 32-bit and can overflow on long measurements
- Only one perfmon context can run per CPU core at a time
- Perfmon is **CPU-local** — use core affinity for consistent measurements
- Interrupts can skew results; consider disabling for critical measurements
