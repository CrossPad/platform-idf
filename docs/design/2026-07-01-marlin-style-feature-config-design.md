# CrossPad Feature-Config + Per-Revision Board Profile — Design

**Date:** 2026-07-01
**Status:** Design — awaiting review
**Scope:** `crosspad-core`, `crosspad-gui`, `platform-idf` (Arduino backport deferred)

## Problem

Firmware ships every feature unconditionally. A build for a stripped-down unit
still carries the full per-pad STM tuning editor, per-pad audio FX chains, and
per-pad data model when only a single **global** config would do. There is no
compile-time way to drop features to shrink flash/RAM.

Separately, per-PCB-revision "optimal initial values" (pad pressure thresholds,
velocity floor, LED brightness, codec gains, audio latency, vibration ranges)
are hardcoded identically for all revisions in struct constructors. Each
revision should carry its own tuned defaults, sourced from a per-revision header.

## Goals

1. **Compile-time feature toggles** — drop subsystems/UI to produce a lighter FW.
   Notably: disable the per-pad editor and collapse the per-pad data model to a
   single global value.
2. **Per-revision default values** — each PCB revision provides its own optimal
   initial values via a header; these feed the portable `CrosspadSettings`.
3. **Marlin-inspired ergonomics** — a hand-edited, heavily-commented
   `Configuration.h` (+ `Configuration_adv.h`) as the single source of feature
   truth, `#define`/`//#define` to enable/disable, a `boards.h` enum +
   `#define CROSSPAD_BOARD`, and a `SanityCheck.h` that `#error`s on bad combos.
4. **Portability preserved** — `crosspad-core` stays platform-agnostic. It sees
   only `CROSSPAD_*` macros, never `sdkconfig.h`. Config compiles on ESP-IDF,
   Arduino, and PC.

## Non-Goals

- Arduino backport (deferred, follows the same pattern).
- Runtime feature toggling (these are compile-time only).
- Migrating existing runtime settings toggles (`enableWiFi`, etc.) out of
  `CrosspadSettings` — they stay; compile-time gates are a separate layer.

## Design Decisions (locked)

| Decision | Choice |
|----------|--------|
| Toggle depth | **Layered**: `PAD_CONFIG_PER_PAD` (full arrays + editors) vs GLOBAL (one value). Core compiles both via `#if`. |
| Flag source of truth | **Portable header + `-D` override.** `#ifndef` around every define; any platform overrides via `-D`. |
| Per-rev defaults delivery | **Compile-time board profile header** in `crosspad-core`, selected by `CROSSPAD_BOARD`. |
| Config ergonomics | **Marlin-style**: hand-edited `Configuration.h`/`Configuration_adv.h`, `boards.h`, `SanityCheck.h`. |
| IDF feature flags | **Pure header.** No feature menu in menuconfig. Kconfig stays only for board revision. |
| Board rev source | **Single source.** On IDF, `CONFIG_BSP_BOARD_REV` drives BOTH the BSP pinout AND `CROSSPAD_BOARD` (via a CMake `-D` bridge). |
| Initial flag catalog | **Broad**: pad editor, per-pad data level, per-pad audio FX, sampler pad editor, wireless (WiFi/BLE/OSC), vibration, audio engine, perf stats. |

## Architecture

### New files (in `components/crosspad-core/include/crosspad/config/`)

```
config/
├── Configuration.h        ← common, hand-edited, heavily commented feature toggles
├── Configuration_adv.h    ← advanced/rarely-touched (latency frames, gains, ranges)
├── boards.h               ← enum: BOARD_CROSSPAD_V1/V2/PC + #define CROSSPAD_BOARD default
├── SanityCheck.h          ← #error on incompatible combos
└── board/
    ├── board_profile.h    ← selector: includes board_v{1,2}.h / board_pc.h by CROSSPAD_BOARD
    ├── board_v1.h         ← BOARD_* tuning constants for rev 1
    ├── board_v2.h         ← BOARD_* tuning constants for rev 2
    └── board_pc.h         ← generic/safe defaults for PC sim (fallback)
```

An umbrella `crosspad/config/crosspad_config.h` includes, in order:
`boards.h` → `Configuration.h` → `Configuration_adv.h` → `board/board_profile.h`
→ `SanityCheck.h`. Everything that consumes config includes only this umbrella.

### Configuration.h — shape (Marlin-inspired, not verbatim)

```c
#pragma once
#include "boards.h"

//===========================================================================
//============================ Board selection ==============================
//===========================================================================
// Which PCB revision this firmware targets. On ESP-IDF this is overridden by
// the Kconfig board-revision choice (-D from CMake), so editing here only
// affects PC / Arduino builds.
#ifndef CROSSPAD_BOARD
#  define CROSSPAD_BOARD BOARD_CROSSPAD_V2
#endif

//===========================================================================
//============================ Pad configuration ============================
//===========================================================================
// Per-pad tuning depth. Comment out to collapse to a single GLOBAL value
// applied to all 16 pads (lighter FW: fewer NVS keys, less RAM).
#ifndef PAD_CONFIG_PER_PAD
#  define PAD_CONFIG_PER_PAD
#endif

// Full STM per-pad tuning editor (LVGL screen). Requires PAD_CONFIG_PER_PAD.
#ifndef FEAT_PAD_EDITOR
#  define FEAT_PAD_EDITOR
#endif

//#define ...   // disabled feature example (Marlin convention)
```

Advanced knobs (`FEAT_PER_PAD_AUDIO_FX`, `FEAT_SAMPLER_PAD_EDITOR`,
`FEAT_WIRELESS_WIFI/BLE/OSC`, `FEAT_VIBRATION`, `FEAT_AUDIO_ENGINE`,
`FEAT_PERF_STATS`) live in `Configuration.h` or `Configuration_adv.h` grouped
under banner comments, each `#ifndef`-guarded so `-D` overrides.

> **Note on flag style:** Marlin uses bare `#define X` / `//#define X` (presence
> = enabled). To keep `-D` override working across platforms, CrossPad wraps each
> in `#ifndef X / #define X / #endif`. Consumers test presence with
> `#if defined(FEAT_X)`. This preserves the Marlin uncomment-to-enable feel while
> allowing a platform to force a flag on/off from the build system.

### boards.h

```c
#pragma once
#define BOARD_CROSSPAD_V1 1
#define BOARD_CROSSPAD_V2 2
#define BOARD_CROSSPAD_PC 3
```

`board_profile.h` maps `CROSSPAD_BOARD` → the right `board_v*.h`; unknown → `#error`.

### board_vN.h — constants only

```c
// board_v2.h  — optimal initial values for rev 2
#define BOARD_PAD_SENS_DEF          1
#define BOARD_PAD_LR_RATIO_DEF      50
#define BOARD_PAD_ANALOG_MODE_DEF   2      // AFTERTOUCH
#define BOARD_VEL_CURVE_DEF         0      // LINEAR
#define BOARD_VEL_FLOOR_DEF         1
#define BOARD_LCD_BRIGHTNESS_DEF    100
#define BOARD_RGB_BRIGHTNESS_DEF    100
#define BOARD_LED_DEFAULT_COLOR     {0,25,0}
#define BOARD_AUDIO_SAMPLE_RATE_DEF 48000
#define BOARD_AUDIO_FRAMES_DEF      64     // latency budget
#define BOARD_CODEC_MIC_GAIN_DEF    /* rev-tuned */
#define BOARD_VIB_IN_MIN            10
#define BOARD_VIB_IN_MAX           127
#define BOARD_VIB_OUT_MIN            5
#define BOARD_VIB_OUT_MAX          200
```

### SanityCheck.h

```c
#pragma once
#if defined(FEAT_PAD_EDITOR) && !defined(PAD_CONFIG_PER_PAD)
#  error "FEAT_PAD_EDITOR requires PAD_CONFIG_PER_PAD"
#endif
#if defined(FEAT_PER_PAD_AUDIO_FX) && !defined(FEAT_AUDIO_ENGINE)
#  error "FEAT_PER_PAD_AUDIO_FX requires FEAT_AUDIO_ENGINE"
#endif
// ... further coherence checks as flags are added
```

## Data-Model Collapse (core compiles both ways)

`Stm32Settings.hpp` hides the per-pad arrays behind an accessor so consumers
never branch on the level:

```cpp
#include "crosspad/config/crosspad_config.h"

#if defined(PAD_CONFIG_PER_PAD)
  uint16_t padSensitivity[kPads];
  uint16_t& sensitivity(int i)       { return padSensitivity[i]; }
#else
  uint16_t padSensitivityGlobal;
  uint16_t& sensitivity(int /*i*/)   { return padSensitivityGlobal; }
#endif
```

- Same pattern for `padLrRatio`, `padAnalogMode`, `padVelCurve`, `padVelFloor`.
- GLOBAL mode: `load()`/`save()` write **one** NVS key per field instead of 16.
- Same accessor pattern for per-pad audio (`audio[16]`) gated by
  `FEAT_PER_PAD_AUDIO_FX`.

## Board Profile Feeds Defaults

Struct constructors and `CrosspadSettings::loadFrom` reference `BOARD_*` macros
instead of literals. The board profile only changes the **fallback default**
passed to the KVS reader, so it applies on first boot / factory reset while
saved user values still win:

```cpp
// Stm32Settings ctor
velCurveDef = BOARD_VEL_CURVE_DEF;
velFloorDef = BOARD_VEL_FLOOR_DEF;

// CrosspadSettings::loadFrom
RGBbrightness = store.readU8("cfg_display", "rgb_brightness", BOARD_RGB_BRIGHTNESS_DEF);
```

## ESP-IDF Wiring (single board-rev source)

- **No feature menu in menuconfig.** Feature flags are edited in `Configuration.h`.
- Existing `CONFIG_BSP_BOARD_REV_V1/V2` remains the revision selector and keeps
  driving the BSP pinout (`crosspad_s3.h`, unchanged).
- A CMake bridge maps the Kconfig revision to `CROSSPAD_BOARD` (numeric, matching
  `boards.h`), so `#ifndef CROSSPAD_BOARD` in `Configuration.h` yields to it —
  one revision source drives both pinout and profile:

```cmake
# top-level or crosspad-core CMakeLists
if(CONFIG_BSP_BOARD_REV_V2)
  add_compile_definitions(CROSSPAD_BOARD=2)   # == BOARD_CROSSPAD_V2
elseif(CONFIG_BSP_BOARD_REV_V1)
  add_compile_definitions(CROSSPAD_BOARD=1)
endif()
```

PC sim compiles with the `Configuration.h` default (`BOARD_CROSSPAD_PC`) or its
own `-DCROSSPAD_BOARD=3`.

## UI Gating + Global Fallback Editor

- `crosspad-gui/src/components/stm_pad_editor_screen.cpp`: wrap the file body and
  every call site (settings → pads category) in `#if defined(FEAT_PAD_EDITOR)`.
  With no references, the linker drops the screen → flash saved.
- Sampler `pad_editor` gated by `FEAT_SAMPLER_PAD_EDITOR`.
- **A global editor always remains.** When the per-pad editor is off (or level is
  GLOBAL), settings → pads shows a single set of sliders (sensitivity / L-R /
  curve / floor). The same widget writes via `sensitivity(i)`; the accessor
  resolves the target (loop over 16 in FULL, single value in GLOBAL).

## Testing / Verification

Build three variants to prove the core compiles both levels and stays portable:

1. **v2 FULL** — baseline, all features on: `idf.py -B build_v2 -DSDKCONFIG=sdkconfig.v2 build`.
2. **v2 lite** — `//#define PAD_CONFIG_PER_PAD` + editors off; compare `.bin` size vs baseline.
3. **PC sim** — `crosspad_build platform=pc` with `board_pc`, level FULL — proves portability.

**Success metrics:** all three link; PC sim boots; flash delta FULL→lite is
reported. `SanityCheck.h` is exercised by a deliberately-bad combo to confirm it
`#error`s.

## Risks / Open Points

- **Accessor churn:** every current consumer of `padSensitivity[i]` etc. must
  move to `sensitivity(i)`. Bounded but touches several call sites in core + gui.
- **NVS key divergence:** GLOBAL vs FULL use different key sets; switching build
  variant on a device leaves stale keys. Harmless (ignored on read) but worth a
  note; factory reset clears them.
- **`Configuration.h` is checked in** — a hand-edit is a repo change, unlike a
  menuconfig-local `sdkconfig`. Acceptable and Marlin-idiomatic; per-build
  overrides still possible via `-D`.
