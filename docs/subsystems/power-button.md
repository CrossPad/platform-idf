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

**A charge-screen boot starts nothing else.** The ESP learns it at the wake
gate (`PWR_STATE_CHARGE`, `charge_boot_active()` in `main.cpp`) and then skips
the launcher and the apps (the screen is black under the charge overlay before
the panel lights), the sample engine and its start-up sound, the kit scan,
the audio tasks and the switch to the USB audio profile, BLE, WiFi (remote
management), the update check and the STM update offer. USB comes up before the
gate, so the host sees the CDC+MIDI profile for those seconds, never USB audio.
The button on the charge screen (`PWR_STATE_WOKE`) restarts the ESP into a
normal boot.

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
