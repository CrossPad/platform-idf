# Audio engine rate: 44.1 kHz

Measured through the board's own USB audio capture (rate and RX order
2026-09-02, MCLK binding 2026-10-26).

## Why the I2S clock follows the content

Kits ship 44.1 kHz WAVs, so the engine runs at 44.1 kHz by default and a
kit's files play one frame per output frame. A file at another rate still
plays at its own pitch and length: the native sample engine (core
`SamplerEngine`, `src/sampler/SamplerEngine.cpp`) steps each voice by the
file's rate over the engine's, times the pad's pitch, in one increment, and
its audition voice -- which also plays `STARTUP.wav` at boot -- does the same.
The pitched instrument converts a zone once at load
([pitched-instrument.md](pitched-instrument.md)).

The optional prebuilt streamer (`CONFIG_CROSSPAD_SAMPLER_MARCEL`, off by
default) has no resampler: under it a file plays at whatever rate the I2S
clock runs. Clocked at 48 kHz, every pad then plays **8.8 % sharp and 8 %
short** — a pad hit recorded over USB, cross-correlated against the source
file stretched by candidate speeds, matches at speed 0.918
(= 44100/48000); with the clock at the content rate it matches at 1.000. With
that engine built, content must match the engine rate.

## How the rate is set

- `bsp_i2s_set_sample_rate()` / `bsp_i2s_get_sample_rate()` in the BSP; the
  default I2S configs use it. `AudioControl_Setup()` sets it from
  `audioEngine.sampleRate` before the codec init (44100 or 48000; anything else
  falls back to 48000 with a warning — the UAC endpoints are sized for 48 kHz).
- The USB audio bridge clocks I2S at the same rate; `usb_device_uac` takes
  `sample_rate` in its config and reports that one rate to the host instead of
  a compile-time table. The mic task sizes its transfer per interval
  (`rate * 10 ms / 1000`: 441 frames, not `44 * 10`), otherwise the I2S RX ring
  overflows once every few seconds.
- Core 1.0.3: defaults 44100 (`AudioEngineSettings`, `IAudioModule`, NVS
  `sr_idx` default 0). **Boards that saved settings before core 1.0.3 hold
  `sr_idx=1` in NVS** (48 kHz — the old default was written on every save).
  `AUDIO_RATE 44100` over CDC persists the new value; the clock changes on the
  next boot.
- HIL: `AUDIO_RATE [hz]` verb (`crosspad-hil` `audio_rate`), reply
  `AUDIO_RATE: running=<I2S> configured=<NVS>`.

## I2S0 RX is initialised before TX

In full-duplex mode the IDF driver makes the *second* initialised channel the
slave, clocked from its own fractional divider (`full_duplex_slave` in
`i2s_std.c`). With RX in that role the ADC stream at 44.1 kHz (divider fraction
0.172) loses frames: **~165 held frames per second** on the USB capture, single
frames (L and R together) repeating their predecessor in a pattern locked to
the 441-frame block — THD −50 dB, SNR 32 dB through the DAC→ADC loop, against
−85 dB / 66 dB at 48 kHz (fraction 0.02), with an identical idle floor. RX as
the master and TX as the slave: 0 held frames at both rates, THD −85.2 dB, SNR
65.7 dB at 44.1 kHz. The order is the order of two calls in
`bsp_i2s_init_interface_0()`.

## MCLK is bound back to the RX divider

`i2s_hal_set_tx_clock()` binds MCLK to the TX divider whenever it sets the TX
clock -- also for TX as the full-duplex slave. Left that way, the codecs take
MCLK from the TX divider and BCLK/LRCK from the RX master's: the same 76/441
fraction, in a phase set by when each divider started. 441 and the 256 MCLKs of
a frame are coprime, so in a bad phase (about half the boots) the MCLK count
per LRCK frame wanders over 441 frames, and the codecs' timing with it. The
register state does not show it (`I2S0.rx_clkm_conf.mclk_sel` reads 0 and the
dividers' registers are identical on a good boot and a bad one).

What a bad phase sounds like: the outputs get "drive", THD+N −38 dB at 1 kHz
and −28 dB at 3 kHz (Out 1/2) against −66 dB, on USB playback too. The error
is not harmonic (THD alone −73 dB) but a timing smear: the output runs
~0.75 µs late and 0.35 % quieter for ~2 ms of every 5 ms, which at 3 kHz is
10 dB worse than at 1 kHz.

`bsp_i2s_init_interface_0()` therefore binds MCLK back to the RX divider after
the TX init (`i2s_ll_mclk_bind_to_rx_clk()`). Measured 2026-10-26, Out 3/4
into a PC line input whose own floor is −47 dB, 3181 Hz at −11 dBFS:

| | MCLK on the TX divider, bad phase | MCLK on the RX divider (12 boots, 2 layouts) |
|---|---|---|
| pad -> Out 3/4 | −27.9 dB, 0.45 µs rms timing error | −47.0..−47.3 dB, 0.03 µs |
| USB playback -> Out 3/4 | −25.3 dB, 0.52 µs | −47.4 dB, 0.035 µs |
| USB -> DAC -> ADC -> USB | −27.9 dB (−37.9 at 997 Hz) | −69.2 dB (−71.6 at 997 Hz) |

TX and RX keep the same `mclk_multiple`: a different one leaves the two
channels simplex, which is not the full-duplex this driver is built on.

## How to measure it again

```bash
python3 tools/make_tone_kit.py                      # HIL TONE kit on the SD card, reboot
crosspad-hil run sampler_record --kit-name "hil tone" --pads 0 --rates 1 --step 2 --gap 2
crosspad-hil run audio_loopback --duration-hours 0 --tone-seconds 8
```

`sampler_record` is the pads→DAC→ADC→USB path with the RT mixer resumed; it is
the normal way to record the board and the only one that hears the sample
engine. `audio_loopback` is host→DAC→ADC→USB and must leave the mixer parked
(two writers on one I2S TX). Judge the takes offline: playback speed by
cross-correlation against the source WAV, held frames by the second-difference
impulse count (isolated impulses > 10 MAD), THD/SNR by FFT around the tones.
The harness records at the card's advertised rate — `plughw:` would otherwise
resample through ALSA's linear converter and the A/B compares the resampler.
