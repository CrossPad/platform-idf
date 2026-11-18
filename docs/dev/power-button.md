# The power button and the top bar

Contributor notes: the power button gestures, back, the top bar. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Power Button

Timing lives in `main/power_button.cpp` (the qualified wake at boot and the soft-off gate in `main/boot_power.cpp`); resolution in crosspad-gui
`components/power_gesture.cpp` (`powerBack()`, `powerToggleQuickSettings()`),
portable to the PC simulator.

| Gesture | Threshold | Effect |
|---------|-----------|--------|
| Click | release before 0.5 s | Contextual back, one step: dropdown → modal/overlay → encoder edit → drawer → top-bar focus → running app (its own levels first; Arrange: window → region → MOVE → keys) → the app it was opened from (`AppOrchestrator::historyBack()`) → home → nothing |
| Hold | 0.5 s | Open drawer: close it. Transport bar on screen: jump the encoder to its keys and back. Status icons on screen: toggle the quick-settings drawer |
| Hold | 2.4 s | Power off (STM, `REG_CTL_LONG_PRESS`, pushed from `Stm32Settings::longPressMs`): ship mode on battery, soft-off on a charger |
| Hold | 4.2 s | Ship mode at once (battery only) |
| Hold at boot | 1.5 s | Qualified wake |

**The top bar** (crosspad-gui `transport_bar.cpp`) is one row of two pages,
the transport bar and the status bar: a finger drags it sideways (LVGL scroll,
snapped a page at a time, and it vetoes the screen-wide back/forward swipe
over it); the encoder reaches the battery as the last key and its click swaps
to the icons, where the status bar's arrow takes the keys' place and swaps
back. A downward swipe from the top edge opens the quick-settings drawer
(Settings -> Display) under either page.

**App authors:** an app with internal levels calls `a->setOnBack(fn)` and pops one
level, returning `true`; a dynamically created overlay pushes its own handler with
`ui_back_push()` / `ui_back_remove()`. **Never restore a saved encoder group
unguarded** — use `lv_indev_restore_group()` (a freed group left on the indev
panics). HIL: `PWR_GESTURE CLICK|HOLD`.
Details and the STM 1.4 changes: [docs/subsystems/power-button.md](../subsystems/power-button.md).
