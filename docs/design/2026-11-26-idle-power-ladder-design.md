# Idle power: what the dim state leaves running, and in what order to stop it

Status: **proposal, measured-by-planned-instrument, nothing implemented.** Written
26.11.2026 after the sleep/wake analysis that produced
`docs/subsystems/power-button.md` ("Dim, sleep, soft-off…") and
`tools/keepawake.py meter`. Every number this document asks for is one the bench can
get in one evening now; nothing here should be implemented ahead of its own number.

## What the ladder already does (so nobody re-does it)

`PWR_STATE_LIGHT` (the dim state, 600 s of no activity by default) is entered by the
STM and handled in `main/main.cpp:app_pwr_state_cb()`. What it already saves:

| already off in LIGHT | where |
|---|---|
| the power amplifier | the STM cuts it behind the ESP's back; `invalidateAmpState()` on the way out, because the cached `REG_CTL_AUDIO_EN` is stale |
| the gyro | `bsp_imu_set_inactivity(true)`; motion wakes the board (`REG_CMD_USER_ACTIVITY`) |
| the screen | `sendBrightness(min(8, LCDbrightness))` — a user level below 8 % never gets brighter on dim |
| the pad LEDs, mostly | `RGBbrightness / 5` |
| the STM itself | it is the one that went idle: it scans pads, buttons and the encoder in its own low-power state, and the ESP stays awake only because it is the one with the screen and the audio |

And the next rung down is already the floor: soft-off on a charger, ship mode on
battery (`REG_CMD_CHG_SHIP`, BATFET off). **The battery-drain question is not the off
state, it is the dim state** — that is where a board sits for an hour on a table
between songs, and what the ESP keeps running there is what this document is about.

## The instrument, before the changes

`tools/keepawake.py meter` logs `REG_PWR_IBAT_*` (signed mA, positive = the battery
giving) against `REG_IN_PWR_STATE`, and prints mean/min/max **per state**. Nothing in
the ESP firmware reads IBAT at all, which is why nobody has ever put a number on this.
`PWR_GESTURE CLICK|HOLD` over CDC injects a power-button gesture, so the descent —
awake → dim → soft-off — is scriptable end to end; only the wake back out of soft-off
needs a hand at the button.

Scale, to decide what is worth taking: 4000 mAh, so **100 mA ≈ 40 h** and 10 mA ≈ 400 h
of shelf time on a charged battery. A step that cannot beat ~10 mA on this board is not
worth its complexity, and the meter says so in one run.

## The steps, in the order that buys the most for the least risk

**1. Pad LED strip to zero in LIGHT, not one fifth.**
`force_pad_strip_dark()` already exists in `main.cpp` for the boot/charge path, so the
primitive is proven; the dim branch just does not use it. Twenty WS2812Bs at `/5` of
the user's level are still lit, and a dark device with glowing pads is also the UX
complaint ("which of these two states am I in?"). Risk: an app that repaints pad LEDs
from a timer will fight it — so this is a *resting* level that `PadLedController`
applies, not a one-shot write. Measure: LIGHT before and after, one run.

**2. Stop redrawing the screen nobody can read.**
LVGL keeps ticking and animating in LIGHT: an app's animation timer, the top bar, the
battery row. That is SPI traffic to a panel at ≤ 8 % brightness plus PSRAM
bandwidth. The shape: the LVGL port task holds its `lv_timer_handler()` loop to a crawl
(or stops posting invalidations) while `g_hil_last_pwr_state == PWR_STATE_LIGHT`, and
the `AWAKE`/`WOKE` branches restore it — those branches already exist and already
restore brightness, amp state and pad level. Risk, and it is real: touch stays live (the
controller is not stopped), so a finger on a dim screen must still register and wake the
board — the first tap has to be delivered *and* relight, not swallowed. Anything an app
drives from an LVGL timer needs to survive a paused timer without a stale model. Measure:
LIGHT with animations running versus paused, on a Sampler screen and on the launcher.

**3. Silence the audio path, not only the render.**
`AudioTasks_SetSuspended()` looks like the power switch and is not: it stores a flag
that the render loop checks (`main/audio/audio_task.cpp`, `g_suspended`), which skips
*processing*. The I2S clocks and the two ES8388 codecs are still doing what they were
told. `audio_rt_park(park, timeout_ms)` is the primitive that actually stops the render
between passes. So the step is: park the render, stop the I2S interfaces and take the
codecs down through the BSP's codec power path — while nothing is sounding. Guards
needed: a sounding pad, an armed Recorder, a running Song or an open file transfer each
veto it (`song_io` and the streamer have their own quiesce rules already). Coming back
is exactly what `invalidateAmpState()` on `AWAKE` is for, so the wake path is halfway
there. Measure: LIGHT with the codecs idle versus parked, on battery and on a DAW
session that goes quiet for ten minutes — and listen for the pop, which is a bench
ear, not a number.

**4. Let the card sleep.**
An mounted, powered card draws real milliamps for hours. The shape: unmount and free
the SDMMC pins on entry to LIGHT, remount on wake — nothing subtler, because IDF's
`sdmmc` wrapper has no sleep/retain path. Veto while a bounce, a take, an export or a
kit load is in flight (`song_io` owns the card work; a load is a long task already).
Measure: LIGHT with the card mounted versus unmounted, and — the cost side — the first
kit load after a wake, since a remount is exactly the "slow first read" the kit-load
timeouts already budget for.

**5. The ESP itself, asleep — the prize and the real work.**
`# CONFIG_PM_ENABLE is not set` in **both** `sdkconfig.v1` and `sdkconfig.v2`, while on
v2 every option that only means something *with* power management is switched on:
`CONFIG_PM_SLEEP_FUNC_IN_IRAM=y`, `CONFIG_PM_SLP_IRAM_OPT=y`,
`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y`,
`CONFIG_PM_RESTORE_CACHE_TAGMEM_AFTER_LIGHT_SLEEP=y`. (On v1
`CONFIG_PM_SLEEP_FUNC_IN_IRAM` is left off — so the two revisions were not configured
by the same hand, and the pair is itself a question.) Somebody prepared for light sleep
and never turned it on, and that is the first thing to answer: was it tried and
abandoned (USB? an ISR in flash? a PSRAM refresh cost?), or never tried? The
existing proof that the wake plumbing works is the soft-off gate: `main.cpp` EXT1-wakes
the ESP on the STM's UART line going low, plus a timer re-check
(`STM_GATE_RECHECK_US`), and the STM already announces power states over that UART
(`F0 7D 50 <state> F7`). The same line is the wake source a dim-state ESP sleep would
use.

The rule that must go with it, and it is not negotiable: **no ESP sleep while VBUS is
present.** TinyUSB cannot survive sleep — the host would see the device drop in the
middle of a session — so the decision comes from the charger telemetry the ESP already
reads: `REG_PWR_CHG_STATUS` bit `REG_CHG_STATUS_VBUS_PRESENT`. Battery: sleep is on the
table. USB: the most that is allowed is steps 1-4. A board that vanishes from a DAW
because it took a nap is a defect, not a feature, however good the battery number is.

## Decision table

| | awake | dim (LIGHT) | soft-off (charger) | ship (battery) |
|---|---|---|---|---|
| STM | idle, scanning pads/button/encoder | asleep, wakes on activity | asleep | off (BATFET) |
| ESP today | full | **full: LVGL, I2S+codecs, card, LEDs/5** | restarted into the gate, deep sleep | off |
| ESP proposed | full | 1-4 always; 5 only when VBUS is absent | unchanged | unchanged |

## What not to do

Do not fix idle drain by making the timeouts longer. The dim and sleep timeouts are a
performer's expectation, not a setting to tune around a firmware gap, and a host that
pokes them with `STM_REGW` changes nothing durable: the next settings push restores the
stored value (that is how bench boards end up dimming after five seconds). If the
product ever decides the dim timeout should be longer, that is a Settings decision with
a user-visible reason, and `PWR` prints `stm` and `stored` side by side precisely so a
disagreement between them cannot be hidden.

## Sequencing, and the gate each step has to clear

One step per PR, in the order above, each with its `meter` run attached — a step whose
measured saving does not beat its own risk note comes back out. Step 5 needs a decision
first: find out why `PM_ENABLE` is off, and if the answer is "nobody knows", a branch
with PM enabled and no functional change at all is the experiment, measured with the
audio path running (the RT task and the USB FIFOs are what a DFS change bites, not the
idle current).
