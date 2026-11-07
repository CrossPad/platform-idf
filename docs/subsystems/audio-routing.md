# Audio Routing (USB Audio capture path)

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

`main/audio_route_control.{h,cpp}` owns which codec input the USB host records
and where each codec sends its output. Three callers, one implementation — a
GUI change and a HIL script land in exactly the same state:

```c
bool audio_route_set_capture_codec(uint8_t codec);          /* 0|1 */
bool audio_route_set_adc_input(uint8_t codec, uint8_t in);  /* 0=DIFF 1=LINE1 2=LINE2 */
bool audio_route_set_dac_output(uint8_t codec, uint8_t out);/* 1=LINE1 2=LINE2 3=ALL */
bool audio_route_set_volume(uint8_t codec, uint8_t vol);    /* 0..100 */
bool audio_route_set_mute(uint8_t codec, uint8_t mute);
void audio_route_get_state(audio_route_state_t *out);
bool audio_route_apply_preset(audio_route_preset_t preset);
audio_route_preset_t audio_route_get_preset(void);
```

Presets name signal paths this PCB actually has (measured — the same map is
`PRESETS` in `crosspad_hil/capture.py`, which is what the HIL side records
through): `MICS` = codec0 LINE2 (built-in mics),
`LINE_IN` = codec1 LINE2 (jack), `SPEAKER_BUS` / `HEADPHONE_BUS` = LINE1 on
codec 0/1 (the DAC→ADC loop; codec1 is unity with the better SNR).
`MIX` = both codecs on LINE2, summed sample by sample in the USB bridge
(`mic_src 2`, `AUDIO_ROUTE_CAPTURE_MIX`) — the stream stays stereo because the
ESP32-S3 USB FIFO cannot carry a four-channel isochronous IN endpoint beside
MIDI and the feedback endpoint (TinyUSB refuses the configuration). `CUSTOM` (0x7F) is reported, never applied.

- **UI:** Settings → USB, its own top-level category (`cat_build_usb` in
  `settings_ui.cpp`), gated on `usbAudioSupported()`. Its first row is the
  profile itself — "MIDI + Serial" / "MIDI + Audio" — so switching to
  UAC2 needs no SysEx. Routing follows underneath;
  `IGuiPlatform::audioRoute*` keeps crosspad-gui portable, and boards that
  cannot switch leave `audioRouteSupported()` false so those rows disappear.
- **What a host records is the Mixer's USB bus** (`SAMPLER`, `mic_src 3`):
  every boot applies it in `AudioControl_Setup()` — the only point where the
  codecs exist, since `AudioTasks_Start()` initializes them from inside the RT
  task — so the Mixer's ROUTE decides what goes to USB, the inputs included
  (an input sent to USB holds its ADC like any send, `InputMonitorPort`).
  The ADC preset is not persisted, so a board never records the mics while
  ROUTE sends the Sampler to USB. The ADC presets stay for the bench — SysEx, CDC and Settings → USB's
  advanced rows ("Recording Source") — and last until the next boot.
- **Off-device:** SysEx `F0 7D 1D …` works in both USB profiles; CDC `AUDIO_*`
  only in the CDC one (USB-audio mode, which is the default profile, has no
  CDC). Switching profiles persists, whichever way it is done — Settings
  dropdown, CDC `USB_DEFAULT`/`USB_AUDIO`, or the SysEx mode byte.

**Adding a preset** means: enum entry before `_COUNT`, a row in `k_presets`,
and one more option in `kRoutePresetOptions` — the UI's "Custom" index is
derived from the option count, so the three must stay in step.

## The USB audio port: one I2S writer, one I2S reader

The RT task is the only writer of both I2S TX channels and the only reader of
I2S RX, in every profile. Two writers on one DMA ring interleave their blocks
(full-scale garbage on the DAC→ADC bus), and a UAC task deleted
mid-`i2s_channel_write` on a profile switch leaves the channel mutex taken, so
`usb_audio_bridge_deinit()` hangs in `i2s_channel_disable()`.

The port (`components/crosspad-platform-idf/UsbAudioPort.{hpp,cpp}`,
`usb_audio_port.h`): the UAC tasks only copy between TinyUSB's FIFOs and two
lock-free rings. **Host → board** is `UsbPlaybackInput`, an `IAudioInput`
behind the mixer channel **"USB"** (routed to both codecs like the sampler),
so host audio and the pads meet on the bus and the RT task is the only TX
writer. **Board → host** is a capture tap at the end of
`IdfAudioModule::processMixer()`: the RT task reads the codec(s) the preset
needs, one block per render, and feeds the capture ring; the UAC mic task
takes one interval from it and zero-fills a cold start. The mixer is not
parked in the audio profile, nor during a profile switch: the I2S channels and
both codecs run 16-bit stereo at MCLK x256 in every profile, so the bridge
starts and stops only the port's rings, and pads, sequencer and line-in keep
playing while USB re-enumerates.
`IdfI2sInput` is suppressed exactly while the port is active, so the tap is
the sole RX reader.

**The speaker task takes only what the ring can hold.**
`uac_device_config_t::output_space_cb` (in the local `usb_device_uac`
component) tells the speaker task how much it may take;
`usb_audio_output_space_cb()` answers with what would bring the ring to half
full, and the rest stays in the FIFO (`refused=` counts what was not taken).
Draining the FIFO unconditionally overflows the ring (a 440 Hz tone came back
at 449 Hz); with the limit, `refused=0` and the returned tone at 440.0 Hz to
0.1 Hz, no clipping, no holes. The task reads again while chunks are waiting,
so a late wake-up does not leave the second chunk in the FIFO. Rings are
64 ms; playback starts after 16 ms of prefill and re-primes after an underrun
instead of chasing the host block by block, and what the host sent during
that silence is dropped as it arrives, so an underrun does not leave its
length behind as latency. The render writes the capture ring only while the
host has the mic stream open (`uac_device_config_t::input_open_cb`); the mic
task is the ring's only reader and starts it over on open, after a short read
(`cap_under`) and after an overflow (`cap_drop`, only real losses with the mic
open): one interval plus three blocks are kept, so a take is 4.4-9.4 ms behind
the render in every session instead of whatever the ring held when the host
opened the stream (1.5-58 ms).

**The feedback is CPU cycles between host SOFs and nothing else**
(`tud_audio_feedback_interval_isr`, `usb_device_uac.c`). The host runs OUT and
IN on one clock, so the feedback must never leave the board's clock -- not
even to refill the ring after a stall (a level trim towards the prefill, a PI
loop, leaves the two streams apart for good). macOS logs `Streams wrapping on
different frames (off by more than 2)` about once a second with this device
whether or not anything is wrong -- it is not the measure of this.

How the cycles become a feedback value depends on the host (table below):

- **Linux takes the last value it polled as it is.** A single interval
  carries the SOF interrupt's own entry latency, a few us against a 1 ms
  frame: one read +5800 ppm (44357 Hz) while the I2S ran at 44100.00 Hz
  against the crystal (`SD_BENCH I2S`), Linux played at 44079 Hz
  (`Momentary freq` in `/proc/asound/card*/stream0`) and the ring drained into
  an underrun -- 16 ms of silence on the USB channel -- every 4-7 s. Counted
  over the last 128 SOF interrupts (`kFbWindow`) the feedback reads
  44104.5 Hz, the host at 44101.
- **macOS averages the per-interval values itself.** Their intervals
  telescope, so their sum is exact. Fed the windowed value, macOS began after
  75-270 s of playback to fill its own stream with silence: 2-3 ms gaps every
  ~12 ms that grew to the whole signal, seen on the board only as `zh` (the
  zeros came in the host's packets) -- 5 of 6 runs of 3-5 min with the
  window, 0 of 2 runs of 5 min per interval. The feedback also starts from
  the nominal rate at every stream open (`tud_audio_set_itf_cb`) rather than
  from a stamp taken before the pause.

**A resampler in the render takes up the host's rounding.** What is left --
the host's own rounding, 100-700 ppm on Linux and not constant; on macOS
-100 to -1800 ppm, wandering over tens of seconds, while the capture stream
it could clock by ran at 44098-44103 Hz -- no feedback can remove without
leaving the IN stream's clock, so the render takes the host's frames at
1 +- 1000 ppm (Linux) or 1 +- 5000 ppm (macOS) of their rate, steered by the
ring's level (`UsbPlaybackInput::read`: Catmull-Rom, 32.32 phase, a P loop on
the level smoothed over 1 s; ~11 s time constant at 2 ppm a frame, ~3.8 s at
6 ppm a frame). The ring then holds 450-800 frames for minutes (394-868 on
macOS), 0 underruns; at +-1000 ppm on macOS the ratio sat on its limit and the
ring ran dry (9 underruns in two minutes of a 1 kHz tone, each a gap of
silence in a USB take in the Recorder). `UAC_STATS` `asrc_ppm` is the ratio
applied, `asrc_cyc_max`/`asrc_read`/`asrc_loop` its cost (~10 000 cycles,
~45 us a block). `UAC_FLOW` gives the rates themselves: the feedback sent
(`fb`, samples a frame x 1e6), the frames the host delivered (`host`) and the
render took (`taken`), the capture frames handed to USB (`mic`) and capture
intervals overwritten before they were sent (`lost`), the pacer's resyncs and
codec 0's TX underruns (`rs`, `txu`), runs of 32+ silent frames in the host's
stream as received and as played (`zh`, `zp`), and `t` (us); two reads give
each rate.

**The USB interrupt runs on core 1.** The DWC2 re-arms an iso OUT endpoint
for the frame after the one it reads in DSTS, so an interrupt for packet N
served after the next SOF arms the wrong parity and packet N+1 is dropped. On
core 0 (TinyUSB installed from `app_main`) the status bar's `BLK` --
`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)`, every 500 ms on the
LVGL task -- walks the whole PSRAM heap under its lock, interrupts off, for
1.3-1.6 ms; SOFs came up to 2.08 ms apart (`UAC_PKT sof_gap_us`, `UAC_LATE`
names the task), and ~2 speaker packets a second were never received
(`UAC_PKT skip`) in bursts every ~90 s as the board's and the host's 1 kHz
ticks slid past each other. `CONFIG_TINYUSB_INIT_IN_DEFAULT_TASK=y`
(`sdkconfig.v1`/`.v2`) runs `tusb_init()`, and so `esp_intr_alloc()`, in the
TinyUSB task, pinned to core 1: 0 packets skipped in 4 min, SOFs at most
1.03 ms apart, the resampler at -4 ppm instead of on its limit, and a 3 kHz
tone from a Mac clean on Out 2 for 300 s and in a 202 s USB take in the
Recorder (no gap of 32+ silent samples, no phase jump); the pacer's
`rs`/`txu` stay 0. The same holds for every host.

**The board finds out which computer it is plugged into**
(`main/usb_host_profile.cpp`). The two hosts enumerate differently, so in
Auto, the default, the board tells them apart before any audio stream opens:
TinyUSB's `dcd_event_handler` is wrapped (`-Wl,--wrap`, `main/CMakeLists.txt`)
and sees every setup packet from the USB interrupt. The first GET_DESCRIPTOR
for a string other than the language table after a bus reset decides, and
nothing changes it until the next reset. Linux asks for every string at 255
bytes in one request (`usb_string()`, `drivers/usb/core/message.c`). macOS
asks for the 2-byte header and then the exact length; on a Mac
(`USB_ENUM`): SET_ADDRESS first, device descriptor 8 then 18 bytes, string 2
for 2 then 18. Windows asks for 255 too and gets the Linux shape; so
does a host not yet placed. Settings -> USB host -> Computer picks Auto,
macOS or Linux and shows what was detected; `USB_HOST [AUTO|MAC|LINUX]` does
the same over CDC and the companion SysEx. The mode persists in NVS
(`cfg_usb/mode`) and applies at the next SOF, with no restart or
re-enumeration:

| | Linux (default) | macOS |
|---|---|---|
| Feedback (`tud_audio_feedback_interval_isr`) | over the last 128 SOFs | per interval |
| At a speaker stream open | the window carries on | nominal, then counts from there |
| Resampler (`usb_audio_port_set_asrc`) | +-1000 ppm, 2 ppm a frame | +-5000 ppm, 6 ppm a frame |

`UAC_FLOW fb` shows which is in force: steady to a few Hz on Linux, tens to
hundreds of Hz from one reading to the next on macOS.

**A DAW that monitors its input closes an analog loop through the bus
presets.** `SPEAKER_BUS` / `HEADPHONE_BUS` are the DAC→ADC path; with FL
Studio's "CrossPad" track armed (input monitoring on) the chain
ADC1 → USB mic → FL → USB playback → mixer → DAC1 → LINE1 → ADC1 has a gain
above one on codec 1 and rails within a second. That is what a full-scale,
71 %-clipped recording of "silence" is. Record the pads through `MICS`,
`LINE_IN` or `MIX` while monitoring, or mute the track; the bus presets are
for hearing what the board plays, not for feeding it back.

To check all of the above on a bench: `crosspad-hil audio route` probes every
10 s while `pw-play` loops a tone into "CrossPad Output" and a MIDI storm
plays the pads.

## What is actually on each ADC input (measured 2026-10-04)

The preset names above are older than this measurement and two of them are
wrong about what they reach. Measured on a v2 board with nothing plugged in,
`crosspad-hil capture` on each tap:

| Codec / input | What it is | Evidence |
|---|---|---|
| codec 0 LINE2 | the built-in **mics** | -65 dBFS rms idle, **-47 dBFS** while the speakers play: it hears the room |
| codec 0 LINE1 | the **DAC->ADC loop** | rails at **-0.0 dBFS peak** with the codec output unmuted and the pads playing |
| codec 1 LINE2 | **LINE IN 1** | never moves -- idle, pads playing, or DAC loop hot |
| codec 1 LINE1 | **LINE IN 2** | likewise; both codec-1 taps sit at -77.5 dBFS to within 0.05 dB, which is two unplugged jacks |

So `HEADPHONE_BUS` (codec1 LINE1) is **not** a DAC->ADC loop -- it is the
second line-in jack.

The board's own jack-detect ladder settles the shape: four sockets,
`enum Jacks { JACK_HP_OUT, JACK_LINE_IN_1, JACK_LINE_OUT, JACK_LINE_IN_2 }`
(the Arduino repo's `PinSetup.cpp`, a resistor ladder decoding 16
combinations of 4). Two line inputs, one headphone out, one line out, plus
the built-in mics with no socket of their own.

**Verified with a cable, 2026-10-04.** Moving one source between the two
sockets and switching `INSRC 1 0|1` after each move, reading the Mixer's
Line channel:

| Socket | `INSRC 1 _` | Codec / ADC input | Line channel peak |
|---|---|---|---|
| **LINE IN 1/2** | 0 | codec 1, LINE2 | 0.0744 with the cable in it, 0.0003 without |
| **LINE IN 3/4** | 1 | codec 1, LINE1 | 0.1245 with the cable in it, 0.0007 without |

Mirror images, both ways round, so the map is settled: codec 1 carries both
line-in jacks and `HEADPHONE_BUS` above names the second one, not a DAC loop.

The preset table and the Recorder's MIC/LINE still use the old names; this
table is the reference.

## Input gain

The inputs' preamp is `crosspad::IInputGain` (`PlatformServices::inputGain`),
implemented in `components/crosspad-platform-idf/InputGainPort.cpp`: 0..+48 dB
in 3 dB steps per codec (0 = the mics, 1 = the line-in jack). The first
+24 dB is the ES8388's MicAmp PGA, set while running with
`bsp_es8388_set_mic_gain()` (a set before codec 1 is up is what its init
applies); anything above it is digital, `input_gain_apply()` on each ADC block
where it is read -- `IdfI2sInput::read` (the IN1/IN2 mixer channels), the USB
bridge's capture read, and the recorder's own read in the USB profile -- so
the mixer, a take and the USB microphone all hear the same gain. Kept in NVS
(`audio_in`/`g0`,`g1`, dB×10); defaults +24 dB for the mics (the MicAmp's top,
what the codec init always used) and 0 dB for the jack. At +24 dB a take of
an ordinary room peaked around -60 dBFS, which is why the range goes on past
the analog stage. The line inputs have a range of their own
(`IInputGain::minDbOf()`/`maxDbOf()`, core 2.34): -24..+24 dB. At 0 dB a
-40 dBFS sine out of an external audio interface came back at about -29 dBFS on Line 1/2
(2026-10-29), so a line needs a cut more than a boost; below 0 dB the cut is
digital in the same `input_gain_apply()`, and a line stops at the MicAmp's
top -- a digital boost on it only raised its noise. In the Mixer an input
row's fader is its channel's level, as a source's is; the gain shows under
the input's name, and the name opens the window where it is set, its slider
spanning the input's own range (Mixer 0.20). `INGAIN [i dB]` over CDC.

## Input pairs (Mic, Line 1/2, Line 3/4 on two ADCs)

The board has three stereo inputs and two ADCs: the mics are codec 0 LINE2,
Line 1/2 is codec 1 LINE2, and Line 3/4 is LINE1 of **both** codecs. Any two
of the three can be heard at once; each pair puts each codec on one input:

| Pair | codec 0 | codec 1 | DAC loop pair |
|---|---|---|---|
| `Mic + 1/2` (default) | Mic, LINE2 | Line 1/2, LINE2 | on |
| `Mic + 3/4` | Mic, LINE2 | Line 3/4, LINE1 | off |
| `1/2 + 3/4` | Line 3/4, LINE1 | Line 1/2, LINE2 | off |

The Line 3/4 jack (J7, "UNIVERSAL" on the schematic) is also driven by
**codec 1's LOUT1/ROUT1** through 1 uF, L and R crossed -- the DAC→ADC loop
the `SPEAKER_BUS` / `HEADPHONE_BUS` presets record (both read that net, so
both record codec 1's DAC). Codec 0 has no driver on it; Out 3/4 is codec 1's
LOUT2/ROUT2. A pair with Line 3/4 powers the loop pair down
(`BSP_CODEC_LOOP_CODEC` / `BSP_CODEC_LOOP_DAC_OUTPUT` in the pinout, DACPOWER
bits) and puts it in hi-Z (`bsp_es8388_set_off_outputs_hiz()`, DACCONTROL23
VROI: 40 kOhm instead of 1.5 kOhm to VREF) before an ADC listens there, and
drives it again only once none does. While it is off the bus presets are
refused (`audio_route_apply_preset()` returns false) and never reported:
their signature would be Line 3/4.

A powered loop driver holds the jack through its 1 uF: a source plugged in
then sees a low-pass of its own output impedance against that capacitor
(~3 kHz from a 50 Ohm interface) and the outputs leak into the input --
monitored, that rings. Off and in hi-Z the input is flat against Line 1/2
within 1 dB to 16 kHz (hardware, 2026-11-03).

- **Core:** `crosspad::IInputPairs` (`PlatformServices::inputPairs`), null on
  a board without the choice (`BSP_HAS_SHARED_LINE_IN` 0, v1, the simulator).
- **Port:** `components/crosspad-platform-idf/InputPairPort.cpp` -- the pair
  table, NVS `audio_in`/`pair`, which codec each input is read from.
  `main/audio_route_control.cpp` `input_pair_route()` moves the ADCs and the
  loop pair; a claim (`audio_route_claim_adc`) holds a codec on the input its
  IN channel carries in the current pair, and an IN channel is live only
  while its codec's ADC is exactly there.
- **Mixer:** a channel per input, not per codec -- IN1 Mic, IN2 Line 1/2, IN3
  Line 3/4 (registered last, so a saved desk keeps its slots). The input a
  pair leaves out reads nothing (each codec keeps one reader), its row is
  dimmed "not in pair" on IN and ROUTE, its gain and sends stay. Gain is per
  input (`INGAIN 2 …`), applied to the codec the input is on.
- **Line 3/4 is not darker on the board.** A ~3 kHz pole on it (white noise
  from a ~50 Ohm interface, -16 dB at 18 kHz) is the source's output against
  the 1 uF of a powered loop driver (above). With the driver off and in hi-Z
  the jack reads flat, so there is no correction.
- **The ES8388's DLL registers stay at their reset values.** esp_codec_dev
  writes the undocumented 0x35=A0, 0x37=D0, 0x39=D0 (0x35 and 0x37 together
  are enough to do the harm): with them, about half the starts left a codec's
  ADC as white noise around -38 dBFS RMS (on codec 1 sometimes full scale on
  the left channel) on every input it read, at any gain, until the next start;
  the ADC's digital mute gave clean zeros, so the noise is in the codec, not
  the I2S read. Re-initialised with them, codec 0 started noisy 12 times in
  16; at their reset values none in 16, and 20 board restarts had both codecs
  clean, through 12 restarts of I2S0's clocks as well (2026-11-03).
  `bsp_es8388_init()` writes them at their reset values, and the warm-boot
  shortcut requires 0x35 at its reset value, so a codec an older firmware set
  up goes through the cold path. The soft reset (CONTROL1 SCPReset) after the
  graceful shutdown stays because the init assumes a power-on chip.
- **Off-device:** `IN_PAIR [n]` over CDC; `CODEC_REG c reg [val]` for raw
  ES8388 registers.
- **`bsp_es8388_config_dac_output()` selects the powered output drivers**,
  kept across `start()`. It leaves the output mixer registers (0x27/0x2A)
  alone: LINE1/LINE2 codes written there cut the DAC off the outputs.

## Monitoring an input (the Mixer's MONITOR)

The mixer's IN1 (codec 0: the built-in mics) and IN2 (codec 1: the line-in
jack) are channels like any other: MONITOR on the Mixer's IN page sends one to
the outputs you hear -- `Out 1/2` (phones and speakers, codec 0), `Out 3/4`
(line out, codec 1), both or neither (`AudioMixerEngine::isOutputHeard`; `USB rec` keeps
its own send). How it works:

- **ADC claims** (`audio_route_claim_adc(codec, who, on)`). The routing's
  choice for a codec's ADC -- a preset, Settings, SysEx, CDC -- is kept as it
  was, but while anything listens to the input itself the ADC is held on
  LINE2: the Recorder (`AUDIO_ADC_CLAIM_TOOLS`) and an IN channel with a send
  anywhere (`AUDIO_ADC_CLAIM_MONITOR`, `InputMonitorPort.cpp`, a 100 ms
  `bsp_worker` job that watches the routes, so the Mixer, HIL and a desk loaded
  at boot all count). The last claim gone gives the routing's choice back.
  `AUDIO_STATUS adc=` reports the input the codec is on, claims included.
- **An IN channel on the loop is silent** (`IdfI2sInput::setLive`, set with
  every ADC change). An ADC on LINE1, the DAC loop, would make "Mic" carry
  the speaker bus and "Line" sent to the phones rail at full scale -- a
  feedback loop.
- **Codec init leaves both ADCs on LINE1**: `AudioControl_Setup()` calls
  `audio_route_codecs_started()`, which says so in the shadow state and puts
  back every claim made before the codecs were up (the monitor job runs
  first, and its claim's I2C write fails then).
- **USB audio profile**: the IN channels are suppressed there (one reader per
  RX channel); the render hands each the block it read after the mixer for the
  USB capture (`IdfI2sInput::offer`), so an input is heard one block (~1.5 ms)
  later than in the CDC profile, where the channel reads the I2S itself.

A send of the mics to `Out 1/2` is a mic next to its own speakers: that howls,
as it would on any desk. `MIX_ROUTE ch out 0|1` over CDC sets a send.

**From an audio track.** Arrange's audio track has MONITOR in its
input sheet (a long press on its header, crosspad-arrange 0.22.0): the input it
records heard live or not. It goes through core's `IInputMonitor`
(`PlatformServices::inputMonitor`, core 2.20.0), which `InputMonitorPort.cpp`
implements as the Mixer's MONITOR at "all" or "off": MIC is IN1, LINE IN2,
MIC+LINE both, USB the `USB` channel; the pads have none, being heard anyway.
A send switched on that was dragged to nothing comes back at unity, the desk
is saved as the Mixer's close saves it, and the ADC claim above follows from
the route as it does for the Mixer. `INPUT_MON MIC|LINE|MICLINE|USB [0|1]`
over CDC does the same.

## The jacks' volume (the Mixer's OUT)

Three levels sit before anything digital, one per physical output, each the
analog volume of the codec output pairs it is wired to (`IOutputLevels`,
`OutputLevelsPort.cpp`, 0..100): **Speakers** (codec 0 LOUT1/ROUT1,
`masterFX.outVolumeCodec[0][0]`), **Phones** -- the jack marked Out 1/2 --
(codec 0 LOUT2/ROUT2, `[0][1]`) and **Line out** -- the jack marked Out 3/4 --
(codec 1, `[1][0]`). The speakers and the phones hear one mixer output (`Out
1/2`) at levels of their own; the speakers also have the amp's switch
(`masterFX.speakersEnabled`, the STM's `REG_CTL_AUDIO_EN` gate). Every control
shows them by those names: Settings -> Audio, the first rows of the Mixer's OUT
(above the mix's masters, which are digital, after them) and the status bar's
drawer (crosspad-gui 2.11.5). `IGuiPlatform::setSpeakerVolume` /
`setHeadphoneVolume` are the Speakers and Line out levels (IdfGuiPlatform).

The Founders firmware (tag `first-release-2026-10-31`) gave the codec Line
out's level x 56 / 100; from 52177eb it gets it 1:1. On the first boot after
an update from that firmware (the other OTA slot holds its image),
`output_levels_migrate()` stores the level it played (70 -> 39, the default
64 -> 36), so the jack stays as loud and the number changes instead. It runs
once per board (NVS `audio_out/lo_1to1`, which a settings reset keeps); a
board coming from v1.1.1, already 1:1, keeps its number.

`OUTVOL` over CDC is older and works by codec: bare `OUTVOL` reads
`out1=` Line out's and `out2=` the Speakers' saved level, and `OUTVOL <1|2>
<pct>` writes *every* pair of codec 1 or codec 0 at once, so `OUTVOL 2 n`
moves the Phones as well as the speakers, in the hardware only. 0 % is off: the analog stage bottoms out near -45 dB, which the
speaker amp still made audible, so `bsp_es8388_set_volume(0)` also takes the
DAC's digital volume to its -96 dB floor (and back to 0 dB above 0 %). A
codec's DAC feeds both of its output pairs, so the floor waits until every pair
someone hears is at 0; codec 1's LOUT1/ROUT1, the Line 3/4 loop driver
(`BSP_CODEC_LOOP_*`), is not one of them: Out 3/4 at 0 % or muted floors codec
1 whatever the loop pair's level.

**Codec 1's jack is a little darker, and the render evens it out.** Measured
2026-10-29 with both jacks into an external audio interface at once (white noise and sines from
the card): Out 1/2 and Out 3/4 match within 0.1 dB up to 5 kHz, then codec 1
falls behind -- -0.45 dB at 10 kHz, -0.95 at 15, -1.3 at 18 --, which is a
first-order pole at ~30.3 kHz to 0.05 dB, and it followed the jack when the
interface's inputs were swapped. The codecs' registers match bar the volumes and
codec 0's L/R swap, so it is on the board. `BSP_CODEC1_OUT_POLE_HZ`
(`pinout_v2.h`, 0 on v1: not measured) turns on a fixed high shelf on mixer
bus 1 in `IdfAudioModule::processMixer()` (`EqChain`, 13.9 kHz, +1.65 dB,
q 0.46: within 0.01 dB of the inverse from 500 Hz to 20 kHz), after the
mixer's own EQ and limiter, so the user's OUT EQ stays flat for both jacks.
Because it comes after the limiter, output 1's ceiling is taken 1.65 dB lower
(`IdfAudioModule::boardFixHeadroom()`): a peak at the ceiling is still under
full scale once the shelf has lifted the top, instead of clipping in the
int16 conversion.
Distortion is the same on both (THD+N about -61 dB at 997 and 3181 Hz).
The same jack also inverts: a noise burst correlated against its file came
back with the opposite sign from Out 3/4 and the file's own from Out 1/2.
`BSP_CODEC1_OUT_INVERTED` sets codec 1's DACINVL/DACINVR (DACCONTROL6 bits
5:4, `bsp_es8388.c`), which brought both jacks to the file's sign. The two
jacks are not sample-aligned: the offset between the I2S ports changes with
the boot (Out 3/4 0.3 ms ahead on one, 1.1 ms behind on the next) and is not
corrected.

## Nothing leaves the mixer clipped (the limiter)

Loudness belongs to the jacks above; the digital mix keeps no headroom of its
own. The sample engines sum their voices at unity and a fader reaches +6 dB:
sixteen pads at once reach 1.66 on a sampler channel, and a hard clamp at
±1.0 at the end of each output would cut the tops off -- distortion as soon
as the OUT master comes near 0 dB, always on the USB rec bus, and in every
bounce and PADS take, which record the pads tap with no fader at all.

`AudioMixerEngine::limit()` ends every output (after its EQ and master)
and the pads tap: a per-block peak limiter with a 0.98 ceiling, lower on an
output whose codec cannot carry that (below). The gain comes
down by the first frame the present gain would let over the ceiling, so no
sample does, and goes back to unity with a 200 ms time constant -- but only
after 50 ms without a block that needed the gain where it is; a block below
the ceiling with the gain at 1 is only scanned. `MIX_LVL` gives each output's
deepest gain since the last read as `lim=`, and the tap's on a `tap` row --
1.000 means it never acted. Cost: 80 B of internal RAM, a scan per bus a block.

The hold is what keeps the limiter from being a drive. Without it the gain
rose between the crests of a loud note and was cut again at the next one, so
it moved with the waveform. Modelled block for block (10.10.2026), a sine 6 dB
over the ceiling: THD -47 dB at 100 Hz and -36 dB at 40 Hz, 60 Hz + 7 kHz IMD
-40 dB; with the hold -73 / -77 / -57 dB, and -82 / -87 / -59 dB with the
lookahead below.

**The USB recording bus looks a block ahead.** Coming down within the frames
before a peak is a step when the peak sits near the start of a block, and a
step on a sustained sound is a click. The USB rec bus has nothing in front of
it to keep it under full scale -- Settings -> USB only adds gain -- so it is
where that shows: chords in the Sampler's 16 levels of pitch (one sample,
every voice starting on the same frame) took it to `lim=0.367` on every hit,
clicks in the DAW's take and none on the jacks, which sat at 0.900 behind
their faders (10.10.2026). `setOutputLookahead()` holds that bus back one
block (64 frames, 1.45 ms): the incoming block's peak is known before the held
one leaves, so the gain reaches what both need in one ramp across the whole
block. Only there -- the jacks are what a player hears and keep no extra
latency, and the pads tap is what bounces and takes line up against. 512 B.

**The top of a jack's volume is gain, and the codec clips there.** The analog
volume's codes run to 0x21, three steps (+4.5 dB) past the 0 dB at 0x1E. Out 2
into a Fireface UFX II line input (09.10.2026), sines at 1 kHz and 100 Hz: THD
-62…-88 dB at every volume up to 90 % and every level up to -1 dBFS -- and at
100 % clean to -3 dBFS (-85 dB) but clipped at -1 dBFS (-36 dB, odd harmonics).
So `bsp_es8388_out_ceiling(volume)` gives the peak the stage after the codec
can carry (-3.5 dBFS at 0x21, 1.5 dB more a code below it, full scale from
0x1E down), BSP calls a hook on every `bsp_es8388_set_volume()`, and the
platform turns that into the ceiling of the mixer output feeding that codec
(`setOutputCeiling()`, outputs 0/1 = codecs 0/1). Turning a jack to 100 %
still makes everything below the peaks 4.5 dB louder; the peaks are limited
instead of clipped. `MIX_LVL` shows each output's `ceil=`. Measured without a
headphone load; headphones may clip earlier.

## Mixer presets

The Mixer's list tab keeps six desks (`crosspad-mixer` `MixerPresets`): every
channel's level, mute, solo, pan and sends, the outputs' levels and the
inputs' gain, as `/sdcard/crosspad/mixer/preset_N.json` -- `{"name", "desk":
<AudioMixerEngine::stateJson()>, "gains": [...]}`. On the card, not in NVS:
a desk is ~2 kB of JSON and NVS is 24 kB shared with every setting. SAVE asks
for the name (the slot's own filled in); LOAD applies it and keeps the desk it
replaced for UNDO. The `USB rec` level is never loaded -- it is Settings'.
Slots 1-3 start as STREAM, BEATS and SAMPLING, built from a table of sends by
channel name until saved over: STREAM sends the pads, the song and the inputs
to the stream and the click and the computer to the phones only; BEATS puts
everything the board plays on both outputs, inputs off; SAMPLING puts the
computer in the phones and the click off. `MIX_PRESET [LOAD n|SAVE n name|UNDO]`
over CDC.

