# Power Button

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

Timing lives in `main/power_button.cpp` (reads `REG_IN_BUTTONS` alongside the
STM power state, one 2-byte I2C transfer, 40 ms cadence); resolution lives in
`components/crosspad-gui/src/components/power_gesture.cpp` (`powerBack()`,
`powerToggleQuickSettings()`) so it's portable to the PC simulator too.

| Gesture | Threshold | Effect |
|---------|-----------|--------|
| Click | release before 0.5 s | Contextual back (ladder below) |
| Hold | 0.5 s | Open drawer: close it. Transport bar on screen: jump the encoder to its keys and back. Status icons on screen: toggle the quick-settings drawer |
| Hold | 2.4 s | Power off (STM, `REG_CTL_LONG_PRESS`): ship mode on battery, soft-off on a charger |
| Hold | 4.2 s | Ship mode at once (battery only) |
| Hold at boot | 1.5 s | Qualified wake |

`powerBack()` walks one context at a time and stops at the first that
handles it: open dropdown → modal/overlay (newest first) → encoder edit mode
→ quick-settings drawer → the encoder's focus on the top bar's keys (back to
the screen it left, `transportbar_toggle_focus()`) → running app
(`handleBack()`) → the app it was opened from → home (whichever home screen
Settings → Display chose) → nothing at home.

**The way back is a history, not "close the app".** AppOrchestrator
keeps the registered apps opened since the last one opened from home
(`historyBack()`, at most 8): a start goes on top, a start of one already in it
drops everything above it -- the Recorder handing back to Arrange is a step back,
not a third entry. A launcher row, an app picked on the home map,
`mode_overlay_go_home()` and a rebuilt main screen start it again; hand-built screens (the kit selector's gate) are not in
it. The kit browser's own close (its back handler) takes the same step: Kits
opened from the Sampler goes back to the Sampler, a take opened in the Slicer
from Kits goes back to Kits.

Arrange's own levels, one per click: a window → the loop region being turned
(its handle's edit mode ending ends it too) → MOVE being turned → MOVE / DUP /
DEL → out of the app. A block that is only picked is not a level: the encoder
picks one by walking over it and the pick outlives the screen, so unpicking it
would take a click that shows nothing.

**App authors:** an app with its own internal levels (a settings screen with
sub-pages, a wizard) must call `a->setOnBack(fn)` in `createFn` and pop one
level itself, returning `true` when it handled the click. Returning `false`
(or never setting the hook) falls through to the framework default, which
destroys the app — so a click on an app with unhandled internal levels exits
straight to the launcher instead of stepping back one level.

From bundled STM **1.4** `power_btn.c` does not call `sleep_enter_light()`
on the `BTN_ARMED_SLEEP` release, so the drawer hold only opens the drawer. On
STM ≤1.3 the same hold also dims the screen and pads underneath it, and the
ESP's `REG_CMD_USER_ACTIVITY` kick (`kLightCancelWindowMs`, 2.5 s,
`main/power_button.cpp`) cancels that a beat later; against 1.4 the kick is a
no-op. Auto-dim by idle timeout is a different path (`inactivity.c`).

The soft-off threshold is `REG_CTL_LONG_PRESS`, which the ESP pushes from
`Stm32Settings::longPressMs` on every `pushAll()` — the STM's own compile-time
default never survives an attached ESP. With STM 1.16 (core 2.44.2, gui
2.11.7) it is 2400 ms by default, Settings → Power Long-Press offers 1 / 1.5 /
2 / 2.4 / 3 s, and the STM clamps it to 1000..3700 ms under the 4.2 s ship
hold. The value lives in the NVS key `long_press2`, so a board that saved a
value under the old key starts at 2.4 s. A settings backup from an older
firmware may carry `"longPressMs":4000`: `Stm32Settings::sanitizeLongPressMs`
(core 2.44.3) turns any hold above 3700 ms into 2400 on import and on load
(below 1000 into 1000), and Power Long-Press (gui 2.11.8) snaps a non-preset
hold to the nearest choice and writes it back, so the page never shows one
hold while the STM runs another.

**Off on battery is ship mode** (STM 1.16). Soft-off kept the BQ25895, the STM
and the rails it leaves on powered and emptied a full cell in about a day. Every
power-off -- the hold, Power Off in the UI, the idle timeout, a low cell -- goes
through soft-off as before, and the STM's soft-off loop cuts the BATFET a few
seconds in when no charger feeds the board; on a charger it stays soft-off (it
keeps charging) until the cable is pulled. The button (the BQ's QON, about 1 s)
or a charger on the ESP port brings it back. The STM tells the power-ups apart
at boot: a POR the BQ outlived (it still holds the STM's profile: HVDCP, MaxCharge
and auto D+/D- off, all on in the chip's defaults) with VBUS on the ESP port and
the button up is a charger plugged into a shipped unit -- the charge screen, then
soft-off on the charger; with the button down or no VBUS it is QON; a BQ at its
defaults is a cell connected afresh (or a board with no battery). Both start
normally. A power-up keeps the backlight at 0 until the ESP sets a level, and
the ESP keeps the pads black until the boot splash draws on them.

## Dim, sleep, soft-off: what a host can and cannot do

Measured on a v2 board on 2026-11-26, so these are the board's numbers and not
the ones the headers imply. The STM dims the screen at `REG_CTL_DIM_TO_S`
(**600 s** by default) and takes the whole board into soft-off at
`REG_CTL_SLEEP_TO_S` (**1800 s**); both come from the ESP's
`Stm32Settings::dimTimeoutS` / `sleepTimeoutS` on every settings push, with
`REG_CTL_AUTO_SLEEP` on by default, and Settings → Power edits them.

| the STM calls it | what is alive | what a host can do |
|---|---|---|
| `PWR_STATE_AWAKE` (0x00) | everything | anything |
| `PWR_STATE_LIGHT` (0x04) — the dim stage | the ESP is up: the console answers, the screen is at `min(8, LCDbrightness)` and the pads at a fifth of their level | **write `REG_CMD_USER_ACTIVITY` and it is back**: the STM leaves LIGHT, the ESP's `app_pwr_state_cb` restores the backlight and the pad brightness on the `PWR_STATE_AWAKE` case that follows. Observed 0x03/0x04 → 0x00 within two seconds |
| `PWR_STATE_SLEEP_PENDING` (0x01) → soft-off | the ESP restarts into its gate (`enter_esp_softoff()`, `main.cpp`) and deep-sleeps behind a persistent NVS marker; the STM goes into STOP1 | **nothing.** The STM wakes on its own power button (BTN1 on its ADC ladder) or on a charger event; no register asks it to. `REG_CMD_USER_ACTIVITY` only resets the idle timer, and it only reaches the STM while the ESP is running |
| `PWR_STATE_SHIP_PENDING` (0x02) → ship mode | the BATFET is cut; on a battery everything is dark and draws essentially nothing | a button press (the BQ's QON, ~1 s) or a charger on the ESP port |

So an automated run fails silently in the middle, and the failure has two
shapes. In LIGHT the console keeps answering — a scenario reads replies and
assumes a live device while its screen is dark and `UI_STATE` shows what
a dim board shows. Past the sleep timeout the console is gone altogether, and
only a hand at the button recovers it.

One more shape, worth knowing because it asks for a hand anyway: an OTA over
CDC pops a confirm dialog on the device (`main/gui/ota_overlay.cpp`, with an
"Auto-accept updates" checkbox) and `ota_flash.py` gets
`OTA_ERROR timeout_confirm` until somebody presses it — on a board that may be
sitting at 8 % backlight at that moment. The keep-awake rule below is what makes
that dialog readable: the host request that started the transfer is itself the
activity that relights the panel. `PWR`, `PWR WAKE` and
`PWR KEEPAWAKE <sec>` over CDC (and the companion verbs, which is how a phone
page steers a board in the audio profile) are the answer to both: they are the
same `REG_CMD_USER_ACTIVITY` byte the touch callback and the IMU already write,
kept honest about the bus, and the lease is what carries a run through a silent
gap — a build, a flash, a long analysis. The rule, its periods and the reason
its quiet window is minutes rather than hours are in
`main/include/host_keepawake.hpp`; `main/tests/test_host_keepawake.cpp` pins
them (8 cases / 40 assertions). `tools/keepawake.py` is the host end: it reads
`PWR`, asks for a wake or a lease, and says out loud when the board is already
past reach.

Two traps worth knowing before anyone reaches for this from a script:

- **`STM_REGW` writes the STM's copy and saves nothing.** A settings push (an STM
  restart, or the Settings page) puts the stored value back, and `PWR` prints
  both columns because they disagree after an experiment: a board poked to
  `dim=5 sleep=10` dims and soft-offs every few seconds while Settings says 600
  and 1800, which reads as a firmware fault and is a stale register.
- **Opening the STM bridge VCP resets the ESP.** The bridge wires DTR/RTS to EN
  and GPIO0 (`bench.py recover`'s docstring), so a plain `pyserial` open — which
  asserts both — lands the ESP in its ROM download mode with no CDC afterwards.
  Talk to the board through the hub (`crosspad-hil cdc …`) or set DTR/RTS false
  at the port level before anything else if a raw console is unavoidable.
  A related one: an ESP restarted in the middle of an I2C transaction with the
  STM can lose the *shared bus* for the rest of the session — `STM_REG`,
  `IMU_REG` and the charger registers all answer `ERR i2c` while the ESP itself
  runs fine — and the ESP only probes the STM link once at boot, so it keeps
  reporting `STM_FW running=0.0(down)`. `STM_RESET` over CDC pulses the ESP's
  reset line (GPIO46, `bsp_stm32_reset()`) and is the only reset a host can
  reach; if the bus stays down after that, the board needs a power cycle.

**Verifying it on a board, in three commands** (the numbers are the point — a
run that cannot read them is the one that loses the board halfway):

```text
PWR                       state=0x04 ... host_quiet=... ; 0x04 = dimmed but alive
PWR WAKE                  PWRW: OK  ->  PWR again, state must read 0x00
PWR KEEPAWAKE 900         PWRKA: lease_left=900000ms nudge_every=20000ms
```

With the lease on, leave the console alone for five minutes and ask again: the
state is still 0x00 and `nudges` has grown by roughly the number of periods that
passed. `i2c_fail` growing instead means the STM link is down — that is the bus
hang below, not a keep-awake that does not work.

**What the STM still owes this** (no ESP-side fix exists): a register that
asks it to wake the ESP — a host-side equivalent of BTN1 — so a soft-off board
is not a brick until someone touches it; and keeping its I2C slave answerable
while it is in STOP1, or publishing the reason it slept in a way a host can
read afterwards. The counterpart list is `components/stm32_hal/TODO.md`.

**Four questions the sleep path still owes, and the cheap way to answer each:**

- **Does the STM's idle timer get reset by pad and encoder activity at all?** The
  rule above counts control traffic as activity and deliberately ignores a host's
  notes, on the reading that the STM scans the pads itself, so a performance resets
  its own timer. That is a reading of the register map, not something the ESP side
  can observe: twelve minutes of hitting pads while reading REG_IN_PWR_STATE settles
  it, and it matters more than anything else on this list. If the reading is wrong,
  a performer loses their screen ten minutes into a set and nothing in this
  subsystem is the obvious cause.

- **Does the ESP come back on its own, and into what?** The gate re-checks the STM
  every `STM_GATE_RECHECK_US` (60 s) and leaves after `STM_GATE_MAX_UNREADABLE`
  (3) unreadable rounds -- so an ESP gated against an STM that answers nothing
  boots itself roughly three minutes later. That is not a wake: the STM is still
  asleep, so it boots with no pads, no encoder and no battery gauge, which is
  exactly what `STM_FW running=0.0(down)` on a live-looking board means. Cheap
  test: let a board soft-off, touch nothing, and watch `crosspad-hil devices` for
  five minutes -- then ask whether a degraded boot like that should show a screen
  of its own ("co-processor not answering") instead of a launcher whose pads do
  nothing.
- **Does a VBUS change wake a soft-off board that is on a charger?** The
  ship-mode paragraph above says a charger on the ESP port brings a *shipped*
  unit back; for a soft-off unit on a cable it is untested, and it decides
  whether "unplug it and plug it back in" is a recovery anyone can be told to
  do. The STM publishes the wake reason afterwards
  (`REG_IN_PWR_WAKE_FLAGS`: bit0 active, bit1 qualified; `REG_IN_PWR_HOLD_*` the
  real BTN1 hold time), so one read after the replug answers it.
- **Two different faults look the same to a host.** The bridge USB
  (`0483:5740`, its VCP and its MIDI) can stay enumerated while every STM
  register reads `ERR i2c` -- seen tonight with the shared bus hung, and it would
  also be seen in STOP1. The way to tell them apart from the host side is the
  rest of that bus: `IMU_REG 0x6F` (WHO_AM_I, 0x6A) answers when the bus is
  healthy and fails when it is hung. `tools/keepawake.py` does exactly that
  before it says anything about sleep.

**What `tools/keepawake.py meter` is for.** The STM publishes its battery gauge and
nothing in the ESP firmware reads `REG_PWR_IBAT_HI/LO` at all -- the device has never
looked at its own current -- so the only way to find out what a power state actually
costs is to ask the gauge from the host and log it. The meter does two console reads
per sample (`STM_REG 0x012D` for the state, and `STM_REG 0x0200 43`: one block that
spans `REG_PWR_VBAT_*`, `REG_PWR_CHG_STATUS` and `REG_PWR_IBAT_*` inside `STM_REG`'s
64-byte cap), writes one CSV row each -- `IBAT` is a signed 16-bit milliamp value,
positive is the battery giving and negative is charging -- and finishes with a
mean/min/max per `PWR_STATE`, gaps recorded as gaps and never as zeros. A sample of
a hung or sleeping STM is not a measurement of 0 mA. `--minutes 20 --interval 10` is
the short version; the number worth having is `awake` minus `light`, and `keepawake.py
wake` in a second window is how you force that comparison instead of waiting for the
board to dim on its own. For scale: on a 4000 mAh cell, 100 mA is about 40 hours.

**A host can already press the button logically.** `PWR_GESTURE CLICK|HOLD` is
catalogued in crosspad-hil and injects the gesture into the STM: a `HOLD` (at or above
`REG_CTL_LONG_PRESS`, 2.4 s by default) takes the board into soft-off exactly as a
hand does. So the whole *descent* -- awake, dim, soft-off, and the ESP restarting into
its gate -- is testable end to end from a script, with `meter` logging it. What still
needs a person is the way back up out of soft-off, which is what I13 in
`components/stm32_hal/TODO.md` asks the STM to fix.

**A charge-screen boot starts nothing else.** The ESP learns it at the wake
gate (`PWR_STATE_CHARGE`, `charge_boot_active()` in `main.cpp`) and then skips
the launcher and the apps (the screen is black under the charge overlay before
the panel lights), the sample engine and its start-up sound, the kit scan,
the audio tasks and the switch to the USB audio profile, BLE, WiFi (remote
management), the update check and the STM update offer. USB comes up before the
gate, so the host sees the CDC+MIDI profile for those seconds, never USB audio.
The button on the charge screen (`PWR_STATE_WOKE`) restarts the ESP into a normal boot.

**`PWR_STATE_WOKE` is an awake board, not a dim one.** On the second v2 board on this
bench (COM41, STM 1.16) on 28.11.2026 `REG_IN_PWR_STATE` read 0x03 while the Sampler ran
on the display with all sixteen pads lit, and it held at 0x03 for as long as auto-sleep
was off in Settings -- the state the STM parks in when there is nothing left to time out
of. `tools/keepawake.py` used to group 0x03 with `PWR_STATE_LIGHT` and answer
"dim: the screen is dark, wake it", which is harmless to send and wrong to read; the two
states answer separately now, with the measurement quoted in `decide()` and a test on it.

**Nested layers register, they do not rely on the app hook.** A layer that is
not the app itself — the file explorer over the pad editor over the sampler
panel — pushes its own handler with `crosspad_gui::ui_back_push(fn, user)` in
its create and drops it with `ui_back_remove(token)` from its `LV_EVENT_DELETE`.
That is step 2 of the ladder, so each click peels exactly one layer: inside a
folder the explorer climbs a directory, at its start folder it closes, then the
pad editor closes, then the app. `setOnBack()` is for an app's *own* internal
levels; a dynamically created overlay uses the stack.

**Never restore a saved encoder group unguarded.** The pattern is everywhere
(`s_prevGroup = lv_indev_get_group(enc)` in create, restore on delete) and it
panics on a deep enough screen: an app teardown deletes its
groups synchronously, but the nested layers that saved those pointers are only
reached by the *async* deletion of the app's tree afterwards, and they then put
a freed `lv_group_t*` back on the indev. `lv_group_delete()` detaches the indev
from the group it deletes; nothing stops a later write from re-attaching a dead
one, and the next encoder read dereferences it — `lv_group_get_focused`,
LoadProhibited, taskLVGL. Use `lv_indev_restore_group(indev, saved)`
(crosspad-gui `widgets/gui_helpers.h`), which checks the group against LVGL's
live list (`lv_group_by_index`) and leaves the indev alone when it is gone.
Reproduced as: Sampler → Edit → Browse → into a folder → power click.

**HIL:** `PWR_GESTURE CLICK|HOLD` over CDC drives the whole chain without a
finger. PC sim: Space/Ctrl fire `power_click`/`power_hold`.
