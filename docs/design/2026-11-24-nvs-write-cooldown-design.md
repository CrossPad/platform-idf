# Settings writes are queued, not paid for per step

Date: 2026-11-24. Decision: put a write queue between the settings setters and
NVS, with a cooldown, so one gesture costs one flash write; and flush before
anything leaves the board.

## Problem

Every way of moving a level in this firmware ends in a flash write, at the rate
of the thing driving it:

- `crosspad-gui/src/components/status_bar.cpp:1461` — the quick-settings drawer's
  output sliders, per LVGL value change (a drag is ~30 changes a second).
- `crosspad-gui/src/components/settings_ui_audio.cpp:109` — Settings → Audio's
  per-output rows, same shape.
- `main/host_midi.cpp` (`write_level_of`, the CC and SysEx address paths on
  PR #34) — one write per MIDI message from a controller's fader. A hardware
  controller sends CC at up to ~100/s.
- All of them then go through `OutputLevelsPort::setLevel()`
  (`components/crosspad-platform-idf/OutputLevelsPort.cpp:115`) →
  `NvsSettingsStore::saveU8()` → `nvs_set_u8` + `nvs_commit`.
- The Settings screen, the volume overlay and the route presets call
  `CrosspadSettings::saveTo()` (`components/crosspad-core/src/settings/CrosspadSettings.cpp:192`),
  which saves **every** key it owns — ~90 of them, `saveU8`/`saveBool`/`saveI32`
  each with its own `nvs_open`/`set`/`commit` — whether or not anything changed.
  A single toggle in the Settings app is therefore ~90 flash writes.

NVS is append-within-a-page and compacts when a page runs out, so this is not
one 4-byte write per step: it is a page of entries per gesture and, when a page
fills, a read-erase-write of the whole 24 kB sector's worth of pages. It also
happens on whoever was moving the slider — the LVGL thread for a finger, the
MIDI task for a host. `CrosspadPlatform.cpp:140` already noticed this for the
Mixer's desk ("an NVS write is a flash write, and closing the Mixer without
touching it should not cost one") and guarded that one path with a compare; the
setters themselves were never guarded.

## Design

One queue, in the platform store, between every settings setter and NVS.

    finger / host / verb / app
        -> CrosspadSettings::saveTo(), IOutputLevels::setLevel(), IInputGain::setGainDb()
        -> NvsSettingsStore::saveU8|Bool|I32|I16      <- the queue sits here
        -> NvsWriteQueue (policy, pure C++)
        -> sink: nvs_open + set* + commit, batched per namespace, on a task that
           has an internal RAM stack

**Why the store and not a caller.** The behaviour belongs to the thing that
touches flash, which is `NvsSettingsStore` — the board's `IKeyValueStore`. Doing
it inside `OutputLevelsPort` fixes the levels and leaves the Settings screen
writing 90 keys per toggle; doing it inside `host_midi.cpp` fixes the host and
leaves the finger. Every settings writer already passes through these six
functions, so there they all get it at once and #34 needs no change of its own.

**Why the policy is a separate, pure class.** `NvsWriteQueue`
(`components/crosspad-platform-idf/NvsWriteQueue.{hpp,cpp}`) contains no
ESP-IDF and no FreeRTOS: a caller-supplied microsecond clock and a
caller-supplied sink. So the rules — the interesting part, and the part a board
run cannot name — are pinned by `main/tests/test_nvs_write_queue.cpp` against a
fake clock and an NVS that only records, in the same Catch2 suite as the CDC
text and the display-lock tests. What stays in `NvsSettingsStore.cpp` is exactly
the platform's share: NVS handles as the sink, the task allowed to write flash,
the worker job, the restart hook.

The rules, as the tests state them:

| Rule | Why |
|---|---|
| A value equal to what the queue holds is not a change and costs nothing. | That is an untouched `saveTo()`: ~90 keys, zero writes. |
| A changed value waits `kQuietUs` (5 s) after the **last** change to that key. | A whole drag — 40 steps, say — is one write, of the value it ended on. |
| …but never longer than `kMaxAgeUs` (120 s) after it first went pending. | A fader that never stops is still answered; a value is not held hostage by its own cooldown. |
| A read answers the queued value, not what was last on flash. | A host that moves a fader and reads its own map back must see what it asked for, or a control loop doubles the traffic. An i16 key read as u8 does *not* get answered from the queue, because that pair is a type mismatch on flash too. |
| A flush is one `nvs_open`/`commit` per namespace, not per key. | The settings blob spans ~15 namespaces; this is what turns 90 commits into a handful. |
| A key edited during its own write stays pending and is written again. | The 5 s spell is not a promise that no one touches it again. |
| A refused write (open failed, set failed) stays pending, is retried, and is counted. | Silence is how a settings store loses a value. |
| A full table drops the oldest entry **already on flash**, never one still owed; if all are owed the put is refused and counted. | Losing a change is worse than losing a cache entry. 128 entries is more keys than the whole settings blob, so this is a bound, not a plan. |
| An erase of a namespace also forgets it in the queue. | Otherwise the queue's "already on flash" claim makes the next write of that value a no-op, and an erased board answers its own old settings back. `SETTINGS_RESET` and the store's `eraseAll()` both do this now. |
| A restart, an OTA or a soft-off flushes first. | The cooldown is only safe if leaving is a flush point. |

## Who writes, and from which stack

A flash write needs a task whose stack is in internal RAM — the driver runs with
the cache disabled and asserts on a PSRAM stack
(`esp_task_stack_is_sane_cache_disabled`). This firmware already knows that
(`main.cpp`, over `app_arm_softoff_boot_gate`: the soft-off gate write runs on a
short-lived internal-stack task because the worker's stack is in PSRAM). So:

- the `nvsq` worker job (1 s) checks `hasDue()` and asks; it does not write;
- the write runs on a one-shot `nvsq_flush` task (6 kB internal stack, priority
  5) and deletes itself when done: the body is NVS's page machinery plus a log
  line, and **every** flush goes through that task, including a restart's,
  because the caller's stack is not ours to choose — the soft-off comes from the
  worker (a PSRAM stack), a `RESTART`-ish path from the CDC dispatcher (the main
  task's 4 kB), and the first build of this feature proved the point by
  overflowing a 4 kB stack ("Stack canary watchpoint triggered (nvsq_flush)",
  found on the bench ladder, not in review). The queue's own flush working set
  (the batch of entries it is writing) is therefore members of the queue object,
  not locals on that stack;
- `esp_restart()` runs the shutdown handlers first, with the scheduler still up,
  and every way out of this firmware — OTA, `FW_SWITCH`, transfer mode, network
  mode, `SETTINGS_RESET`, the soft-off — calls `esp_restart()`, so one registered
  handler covers the lot; it asks for the one-shot task and waits (bounded,
  500 ms);
- the soft-off path also flushes explicitly, in `app_arm_softoff_boot_gate()`
  before the gate marker: the STM cuts the rail a few hundred ms after it asks,
  and the gate write is the one that must not be cut short.

`esp_register_shutdown_handler()` has five slots and ESP-IDF claims some of them
itself (timekeeping always, WiFi and Bluetooth when they ran), so the
registration can fail. It logs when it does, and then the periodic flush and the
soft-off hook are all that is left — a smaller guarantee, but a stated one.

## What it costs, and what can be lost

- Internal RAM: ~110 B for the queue object, 4 kB only while a write is running
  (the task), and the table in PSRAM (`high` in the stats says how many keys it
  really held). No permanent task, no permanent stack.
- A **panic** does not run the shutdown handlers, and neither does a light or
  deep sleep entry: a crash or a battery pull costs at most the quiet spell of
  edits — 5 s of a gesture, in practice one gesture in progress. NVS's own page
  format is power-fail safe, so a commit cut short costs that key, not the store.
  This is the trade being made, on purpose: not writing per step, in exchange
  for a bounded window.
- If the worker has no free job slot (its table holds 12 and every component
  adds to it), the due check moves into the setters (`lazy_check`), which is
  still correct — a change reaches flash once something asks again — and slower.

## Not done here

- The Mixer's desk blob (`CrosspadPlatform.cpp`, NVS `mixer`/`state`, ~2 kB) is
  still written per `notifyStateChanged()`, guarded only by its own compare with
  what is stored. Coalescing a 2 kB blob through a 128-entry key table is a
  different mechanism, and that path already fires on app close rather than per
  step. It is the next thing of this kind, and a host driving the Mixer from
  `MIX_*` verbs is how to see it.
- `InputPairPort`, `fw_updater`, `fw_slots`, `wifi_creds`, `cp_account`,
  `ota_prefs` and the soft-off marker keep their own NVS access: those are
  one-shot choices (which input pair, which network, which slot), not gestures.
  Only `InputGainPort` moved, because an input gain is a slider.
- Two different factory-reset lists exist (`NvsSettingsStore::eraseAll()` and
  `hil_control.cpp`'s `kSettingsNamespaces`): they do not agree on which
  namespaces a reset erases. That is a separate finding — noted in the PR, not
  fixed here, because fixing it is a behaviour change under a bench.

## Tested

Numbers from the bench are in the PR body; this page states the mechanism, not
results.
