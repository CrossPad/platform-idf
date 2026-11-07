# CrossPad Config & Workspace Manager — Design

**Date:** 2026-08-18
**Status:** Proposal
**Scope:** `crosspad-apps` (shared manager core), `platform-idf`, `crosspad-pc`, `ESP32-S3`, `crosspad-core` (flag schema)

## Problem

The app manager treats every submodule as registry-owned. It will happily move a
submodule that carries the user's own branch, uncommitted WIP, or local commits
not yet pushed — there is no notion of *ownership*. Three real cases already
exist on a developer checkout: a stale `crosspad-sampler` clone diverged from the
submodule, `crosspad-mixer` pinned to `crosspad_v20` (a user branch, not the
registry ref), and 37 dirty files in `crosspad-docs`.

Separately, the Marlin-style feature config (`crosspad-core/include/crosspad/
config/`) landed with exactly one flag and no way to drive it other than
hand-editing a header that lives **inside a submodule** — editing it dirties
`crosspad-core` for every consumer repo.

The goal is one tool that prepares a CrossPad on demand: pick board revision,
pick feature flags, pick the app set, build, flash — without ever eating work.

## Design

### 1. Files: intent vs state

| File | Owner | Checked in | Purpose |
|------|-------|-----------|---------|
| `crosspad.config.json` | human / TUI | yes | **Intent**: board, feature flags, app set + track policy |
| `crosspad.local.json` | human / TUI | no (gitignored) | Personal overrides: own branches, serial port, dev flags |
| `apps.json` | manager | yes | **State**: what is actually on disk (existing file, unchanged role) |
| `config/profiles/*.json` | human | yes | Named recipes (`full-v2`, `lite`, `audio-dev`, `pc-sim`) |
| `.crosspad/` | manager | no | Backups, generated build flags, caches |

Merge order: `crosspad.config.json` → active profile → `crosspad.local.json`.
State (`apps.json`) is never hand-edited; a mismatch between intent and state is
what the TUI reports and offers to reconcile.

### 2. App track policy

Each app in `crosspad.config.json`:

```json
"apps": {
  "sampler": { "track": "registry" },
  "mixer":   { "track": "branch", "ref": "crosspad_v20" },
  "piano":   { "track": "pinned", "commit": "7337675" },
  "myapp":   { "track": "local" }
}
```

| mode | update behaviour |
|------|------------------|
| `registry` (default) | follow the registry ref; fast-forward only |
| `branch` | follow the named branch; `pull --ff-only`, never switch branch |
| `pinned` | never moves; update reports "pinned, newer available" |
| `local` | manager never touches the worktree; shown as ✋ in the TUI |

Declared mode is **intent**. Independently, every scan computes observed
**status flags** from git: `dirty`, `ahead` (local commits not on upstream),
`detached`, `branch-mismatch` (checked-out branch ≠ declared ref),
`fork` (origin outside the official org). Status flags are what block, not the
mode — a `registry` app that is dirty is still protected.

### 3. Guard: nothing destructive without a backup

Every mutating operation (`update`, `install --force`, `remove`, profile apply)
runs a preflight per app and yields a verdict:

- **CLEAN** → proceed.
- **DIRTY / AHEAD / BRANCH-MISMATCH / FORK** → blocked. The TUI offers, in order:
  1. **Backup & proceed** — snapshot to `.crosspad/backups/<app>/<ts>/`
     (`tracked.patch`, `untracked.tgz`, `local-commits.bundle`, `stashes/`, plus
     a `meta.json` with branch/HEAD/upstream), then proceed.
  2. **Park on a WIP branch** — commit the worktree onto `wip/<date>-<app>` and
     leave it pushed nowhere; the app then reads as CLEAN.
  3. **Skip** (default for `update --all`).
  4. **Force** — only with an explicit flag, and still takes a backup first.
- `remove` of an app with `ahead` or a non-empty backupable state always takes a
  backup before deinit, no prompt.

`update --all` never stops on a blocked app: it processes the clean ones and
prints a skip table with the reason per app. Restore is a first-class command
(`restore <app> [--list]`) that replays a backup dir onto the current worktree.

### 4. Feature flags — schema-driven, project-level output

`Configuration.h` stays the documented, hand-editable default source (Marlin
idiom preserved). Beside it, `crosspad-core` gains a machine-readable catalog:

`include/crosspad/config/features.schema.json`
```json
{
  "flags": [
    { "name": "FEAT_PAD_EDITOR", "type": "bool", "default": true,
      "group": "Pads", "help": "Full STM per-pad tuning editor (LVGL screen).",
      "requires": ["CROSSPAD_PAD_CONFIG_LEVEL==FULL"],
      "platforms": ["esp-idf", "pc", "arduino"] },
    { "name": "CROSSPAD_PAD_CONFIG_LEVEL", "type": "enum",
      "values": ["FULL", "GLOBAL"], "default": "FULL", "group": "Pads" }
  ]
}
```

- The TUI renders this as a menuconfig tree (groups, help line, `requires`
  validation) and writes chosen values into `crosspad.config.json → features`.
  The submodule header is **never** written to.
- A generator (`tools/gen_build_flags.py`, shared core) emits per platform:
  - ESP-IDF / PC → `.crosspad/build_flags.cmake` (`add_compile_definitions(...)`),
    included from the top-level `CMakeLists.txt` if present.
  - Arduino → `.crosspad/build_flags.ini` consumed by a PlatformIO `extra_script`.
- Only flags that differ from the header default are emitted, so an untouched
  project builds byte-identically to today.
- `SanityCheck.h` remains authoritative — the TUI's `requires` check is an early
  warning, the compiler is the enforcement.
- The generated flag set is hashed; a changed hash marks the build stale and the
  TUI offers `fullclean` (matching the existing "fullclean after new app" rule).

The flag catalog to fill in is already specced in
`2026-07-01-marlin-style-feature-config-design.md` (pad editor, per-pad level,
per-pad audio FX, sampler pad editor, wireless WiFi/BLE/OSC, vibration, audio
engine, perf stats). Only the first landed.

### 5. Profiles — "CrossPad on demand"

A profile is a full recipe: board revision, feature flag overrides, app set with
track modes, and optional build-dir / sdkconfig name.

```json
{ "name": "lite-v2", "board": "V2", "sdkconfig": "sdkconfig.v2",
  "features": { "CROSSPAD_PAD_CONFIG_LEVEL": "GLOBAL", "FEAT_PAD_EDITOR": false },
  "apps": { "sampler": { "track": "registry" } } }
```

`apply` = diff current state vs recipe → show the plan (install X, remove Y,
flag deltas, board change) → confirm → execute through the same guard. Apps that
are `local` or blocked are listed as untouched, never silently removed.
`save-profile` snapshots the current state into a new recipe.

### 6. TUI

```
Dashboard
 ├ b Browse & Install
 ├ u Update All        → guard preflight, skip table with reasons
 ├ c Configure ▸       NEW  menuconfig-style feature tree from features.schema.json
 ├ p Profiles ▸        NEW  apply / save / diff recipes
 ├ w Workspace ▸       NEW  per-app track mode, git status, backup / restore
 ├ f Build & Flash     → header shows active profile + flag hash + board rev
 ├ h Health            → adds "protected apps" and "intent vs state drift"
 └ d Dev tools
```

Workspace row shape:

```
● sampler      master  ↑2 ✎dirty   track=local     [m]ode [b]ackup [r]estore
● mixer        crosspad_v20        track=branch    [m]ode [b]ackup [r]estore
● instructions main    ✓clean      track=registry  [m]ode [b]ackup [r]estore
```

### 7. Cross-platform

All of it lands in `crosspad-apps/crosspad_app_manager.py`, which all three
platform wrappers already download and cache — one implementation, three
platforms. `PlatformConfig` grows: `config_file`, `flags_emitter`
(`cmake` | `platformio`), `build_dir`, `sdkconfig` naming.

## Migration

1. `crosspad.config.json` is generated from the existing `apps.json` on first
   run (`track: registry` for everything, `branch` where the submodule is on a
   non-default branch — that already covers `mixer` on `crosspad_v20`).
2. No file changes required in any app repo.
3. Without `crosspad.config.json`, behaviour is exactly today's (minus the
   destructive paths, which gain the guard unconditionally).

## Risks

- **Two config files can drift.** Mitigated by making `apps.json` machine-only
  and adding an explicit drift report in Health.
- **Generated flags vs a hand-edited `Configuration.h`.** Both are legal; the
  TUI shows "header default / overridden by project" per flag so the source of a
  value is never ambiguous.
- **Backups grow.** `.crosspad/backups` is gitignored and pruned (keep last N
  per app, configurable).
