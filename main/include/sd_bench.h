#pragma once

/// SD_BENCH over CDC (main/sd_bench.cpp): N files read in turn a piece at a
/// time, as a streamer refills N voices -- MB/s and each read's time. True
/// when @p cmd was the verb; rows go out through @p reply from its own task.
bool sd_bench_verb(const char *cmd, void (*reply)(const char *));
