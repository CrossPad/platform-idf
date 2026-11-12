# Audio routing and the engine rate

Contributor notes: codecs, inputs and outputs, USB audio, the sample rate. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Audio Routing (USB Audio capture path)

**MIDI+UAC2 is the default USB profile** (`usbAudioEnabled`, fallback `true` in
`CrosspadSettings::load()` — the constructor's value never reaches a boot). It
carries no CDC endpoint, so a board that has never been switched has no console,
no OTA and no `fs_transfer`: control goes over SysEx on USB MIDI
(`crosspad-hil usb-mode set default`, or `bench.py ready`). Every way of
switching now persists the choice — the Settings dropdown, the CDC command and
the SysEx one all write `usbAudioEnabled` — so a development board stays on CDC across
reboots and `factory_provision.py` flips to CDC for its checks and back to audio
before it logs a serial.

`main/audio_route_control.{h,cpp}` is the one implementation behind the GUI
(Settings → USB), SysEx `F0 7D 1D …` and CDC `AUDIO_*`. Presets name real signal
paths: `MICS` = codec0 LINE2, `LINE_IN` = codec1 LINE2, `SPEAKER_BUS` /
`HEADPHONE_BUS` = LINE1 on codec 0/1 (DAC→ADC loop), `MIX` = both codecs'
LINE2 summed in the USB bridge (`mic_src 2`; a 4-channel UAC2 stream does not
fit the S3's USB FIFO beside MIDI — TinyUSB refuses it). **Every boot records
the Mixer's USB bus** (`SAMPLER`): ROUTE decides what a host gets; the ADC
presets are for testing and last until the next boot.
**The RT task is the only I2S writer and reader, also in the USB-audio
profile**: host playback enters the mixer as the "USB" channel and capture is a
tap in the render (`UsbAudioPort`), the UAC tasks only move bytes between
TinyUSB's FIFOs and two rings. The feedback endpoint is CPU cycles between host
SOFs, and the render takes the host's frames through a resampler steered by
the ring's level (`UAC_STATS asrc_ppm`).
**Both are shaped per host computer, found by itself**
(`main/usb_host_profile.cpp`: Auto reads the first string request after a bus
reset through a `--wrap` of TinyUSB's `dcd_event_handler`; Linux asks for 255
bytes, macOS for 2. Settings -> USB host, `USB_HOST AUTO|MAC|LINUX`,
`USB_ENUM`, NVS `cfg_usb/mode`; Linux until placed):
Linux a feedback over 128 SOFs and ±1000 ppm, macOS a feedback per interval
(a steady one made macOS fill its own stream with silence after minutes,
`UAC_FLOW zh`) and ±5000 ppm. **The USB
interrupt runs on core 1** (`CONFIG_TINYUSB_INIT_IN_DEFAULT_TASK=y`): on core
0 a PSRAM heap walk (`heap_caps_get_largest_free_block`, the status bar's
`BLK`, 1.3-1.6 ms with interrupts off) made the DWC2 arm the wrong frame
parity and drop ~2 speaker packets a second (`UAC_PKT skip`, `UAC_LATE`). A DAW
monitoring its input while the preset is a DAC→ADC bus closes an analog loop
that rails on codec 1 — that is a host-side loop, not a firmware fault.
Adding a preset = enum entry + `k_presets` row + `kRoutePresetOptions` entry, in
step. **An input the mixer sends anywhere (the Mixer's MONITOR: `Out 1/2` =
phones and speakers, `Out 3/4` = line out) holds its codec's ADC on its input over the preset**
(`audio_route_claim_adc`, the Recorder claims the same way), and an IN channel
on the DAC loop is silent, so no send can feed back. **Three inputs, two ADCs:**
Mic = codec 0 LINE2, Line 1/2 = codec 1 LINE2, Line 3/4 = LINE1 of both codecs,
the jack codec 1's LOUT1/ROUT1 also drive (the DAC loop); the Mixer's IN pair
(`IInputPairs`, `InputPairPort.cpp`, `IN_PAIR`) picks two, and a pair with Line
3/4 powers that loop pair down **and puts it in hi-Z** (VROI) -- a driver left
on is a ~3 kHz low-pass for the source and a feedback path -- and refuses the
bus presets. **Each physical output has its own
hardware level** (`IOutputLevels`, `OutputLevelsPort.cpp`): codec 0 LOUT1/ROUT1
= the speakers, LOUT2/ROUT2 = the Out 1/2 jack, codec 1 LOUT2/ROUT2 = Out 3/4; a bus with
such outputs plays at unity in the mixer and OUT shows a row per output. The
DAC's digital floor at 0 % waits until both pairs of a codec are at 0 -- the
loop pair (codec 1 LOUT1/ROUT1) does not count, so Out 3/4 at 0 % or muted is
off.
API and rationale: [docs/subsystems/audio-routing.md](../subsystems/audio-routing.md).

## Audio Engine Rate

Kits are 44.1 kHz WAVs, so the engine runs at 44.1 kHz by default. The native
sample engine steps a voice by the file's rate over the engine's (core
`SamplerEngine`, the audition voice and `STARTUP.wav` too) and the pitched
instrument converts at load, so a file at another rate keeps its pitch; only
the optional prebuilt streamer (`CONFIG_CROSSPAD_SAMPLER_MARCEL`) plays files
1:1 and needs the clock at the content rate. `AudioControl_Setup()` pushes
`audioEngine.sampleRate` (44100 default, 48000 allowed) into
`bsp_i2s_set_sample_rate()` before the codec init, and the USB audio bridge and
UAC report the same rate. Boards that saved settings before core 1.0.3 hold
`sr_idx=1` (48 kHz) in NVS: `AUDIO_RATE 44100` over CDC, then reboot.
**I2S0 RX is initialised before TX** — as the full-duplex slave the RX lost
~165 frames/s at 44.1 kHz (clean at 48 kHz); as the master it is clean at both.
**MCLK is bound back to the RX divider after the TX init**: setting the TX
clock binds it to the TX divider, and on about half the boots the codecs'
MCLK and LRCK then drift against each other (a 200 Hz timing smear, THD+N
−28 dB at 3 kHz on every output and on USB).
Measure with `tools/make_tone_kit.py` + `crosspad-hil run sampler_record`;
judge offline. Details and numbers: [docs/subsystems/audio-engine-rate.md](../subsystems/audio-engine-rate.md).
