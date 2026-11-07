# App Manager TUI redesign — tasks, not objects

**Date:** 2026-09-15
**Scope:** `tools/crosspad_app_manager.py` (TUI layer) and the parts of the
manager it needs underneath. The `idf.py app-*` CLI keeps its verbs.
**Out of scope:** onboarding (README minimal setup, a script that installs
ESP-IDF / `gh` / MCP / HIL and lands in this TUI) — a separate spec that
follows this one.

## Why

A user who saw that Steve had pushed Sampler commits could not pull them
from the TUI: every app is `track=pinned`, `[U] Update` lists them as
`✋ protected` and offers nothing, and the way out (`[W] Workspace` → `[m]` →
`registry` → back → `[U]` → `[F]`) requires knowing the manager's policy
model, git, and that "update" and "flash" are different screens. That is
friction we lose customers on.

The TUI today is a list of *objects* (Apps / Build / Device / Workspace /
Profiles). The redesign makes it a list of *tasks*, each ending on the
board, with one thing to do at the top. Developer screens stay, one level
down.

## Who

Two groups, one UI:

- **Customer / player** — knows nothing about git, submodules, CMake or
  board revisions. Wants Steve's new Sampler on the pad. Never sees the
  words submodule, track, commit, fullclean, pinned, CDC, SysEx.
- **App author / advanced user** (Steve, contributors) — knows a terminal,
  does not know this manager's policies. Gets the same top level plus
  `Developer tools`, where nothing is hidden.

Both must get everything on a plate: every screen says what to do next and
has the key that does it.

## Principles

1. **One thing to do, first line, with Enter.** Every screen opens with the
   most useful next action. The dashboard computes it; other screens state
   it.
2. **Tasks end on the board.** "Update" is not done until the board answers
   with the new versions. No separate update/build/flash mental model for
   customers.
3. **A version is a version.** No `pinned` mode in the UI. An app either
   follows the latest release, follows the latest development, or stays on
   a concrete version the user picked. The config model below is unchanged;
   the UI stops naming it.
4. **Errors say what to do, not what happened.** The full log always lands
   in `.crosspad/last-update.log`; the screen shows one line and a key.
5. **Windows, Linux, macOS.** Everything below runs on all three. The
   existing key layer (`termios` / `msvcrt`) stays; glyphs get ASCII
   fallbacks when the console cannot draw them (Windows `cmd.exe` without
   a UTF-8 code page: `✓`→`OK`, `✗`→`X`, `▸`→`>`, `█░`→`#-`).
6. **Full-screen console.** The TUI runs in the terminal's alternate screen
   buffer (`ESC[?1049h` / `ESC[?1049l`; on Windows via
   `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)`, which Windows 10+
   supports), so `q` returns a clean shell. Layout reads
   `shutil.get_terminal_size()` on every redraw: wider terminals get the
   "what changed" column and longer descriptions, shorter ones scroll lists
   instead of truncating them. Minimum 80×24; below that the TUI says so and
   still works with wrapping.

## Screens

### 1. Dashboard

```
 CrossPad                              v2 board · fw v2 ✓ · connected

 What's next
 ▸ 3 updates waiting: Sampler, Mixer, Instructions
   [Enter] Update my CrossPad — download, build, flash  (~4 min)

 Apps on your CrossPad
   Sampler        0.2.1 → 0.3.0
   Mixer          0.2.1 → 0.2.2
   Instructions   1.0.0 → 1.0.1
   Fish Tank      0.1.0   up to date
   DAW Control    0.2.0   your own copy

 [1] Update my CrossPad     [2] Add or remove apps
 [3] Something's wrong      [4] Developer tools
 [q] Quit
```

**"What's next" is computed**, in this priority, first match wins:

| Condition | Line | Enter does |
|---|---|---|
| Board connected, firmware revision ≠ board revision | `The board runs v1 firmware on a v2 board` | Update my CrossPad |
| Any app has a newer version per its follow rule, or the installed set changed since the last successful update, or core/gui gitlinks moved | `N updates waiting: …` / `Apps changed — put them on the board` | Update my CrossPad |
| No network (the first unreachable call sets it for the session) | `No connection — can't check for updates` | Something's wrong |
| Tools missing (`idf.py`, `gh` not signed in) | `Set up: gh is not signed in` | Something's wrong |
| Nothing | `Everything is up to date` | nothing (Enter is a no-op) |

**Offline is one line, in one place.** `_load_registry()` is called once per
app by `app_status()`, so a per-call network attempt meant one failure message
per app on screen. The registry is cached per session, the first unreachable
call sets an offline flag nothing retries, and `refresh_available()` stops at
the first network failure rather than paying a DNS timeout per app. Offline
outranks the tool checks above because `gh auth status` fails without a
network too, and "gh is not signed in" is the wrong thing to tell someone
whose wifi is off. What the cache already knew (updates, a firmware mismatch)
still shows — those are local facts.

The app rows show `installed → available` per the app's follow rule (§2),
`up to date`, `your own copy` (an app the manager does not manage: today's
`local`, or a plain directory created by New app), or `your changes`
(uncommitted / unpushed work — the pipeline will ask, §3).

The `~4 min` estimate is the wall time of the last successful run of the
same steps, stored in `.crosspad/last-update.json`; absent → omitted.

"connected" comes from the same device probe `idf.py board` uses. Without a
board the header reads `no board plugged in`, the dashboard still works.

### 2. App screen — versions

Reached from `[2]` (Enter on an installed app) and from Developer tools →
Workspace `[m]`. One implementation, two entrances.

```
 Sampler                          installed 0.2.1 · by CrossPad
 MIDI sampler with kits, scenes and a pad editor

 Version
 > ● Latest release        0.3.0    auto-update      ← recommended
   ○ Latest development    master   4ac6d69, 17 commits newer
   ○ 0.3.0                          2026-09-14
   ○ 0.2.1                          2026-08-18   ← installed
   ○ 0.2.0                          2026-08-01
   ○ Other commit…

 [Enter] use this version   [c] what changed   [x] remove   [q] back
```

The list is one list. The two "Latest …" rows are follow rules; every row
below is a concrete version. The filled dot marks the current rule.

**Mapping to `crosspad.config.json`** (unchanged file format, `TRACK_MODES`
stay):

| Row | Config | `update()` does |
|---|---|---|
| Latest release | `{"track": "registry"}` | checkout the newest release tag reachable on origin (`v<semver>`); the registry's `version` field is what the dashboard compares against |
| Latest development | `{"track": "branch", "ref": "<default branch>"}` | `fetch` + `merge --ff-only origin/<ref>` |
| Concrete tag / commit | `{"track": "pinned", "commit": "<sha>"}` | nothing (and on pick: checkout that sha) |
| Other commit… | text input → same as concrete | |

`local` is not on this list: an app is `your own copy` when it has no
origin in the CrossPad org / registry, when it is a plain directory, or when
the developer set it in Workspace. Customers cannot produce it.

**Change of behaviour underneath:** today `track=registry` checks out
`origin/<manifest ref>` — the branch head, which is "latest development",
not "latest release". `update()` gains tag resolution: `git tag --list
'v*' --sort=-v:refname` after fetch, first tag wins; an app with no tags
falls back to the default branch head and the row says so (`Latest release
— no releases yet, uses master`). The dashboard's "available" for a
registry-tracked app is the registry `version` when a matching tag exists,
else the branch head.

`[c] what changed` = today's `_show_changelog()` between the installed
commit and the highlighted row's target. `[x] remove` = today's
`_remove_flow()`, then the dashboard's "What's next" becomes `Apps changed`.

### 3. Update my CrossPad — the pipeline

One Enter, one progress screen, five steps, always in this order:

```
 Update my CrossPad                              v2 board · connected

 ✓ Download updates        Sampler 0.3.0, Mixer 0.2.2, Instructions 1.0.1
 ✓ Firmware components     crosspad-core 1.15.0, crosspad-gui 1.4.1
 ▸ Build                   ████████████░░░░░░░░  61 %   ~2 min left
   Flash                   over USB, no cable dance needed
   Check                   the board answers with the new versions

 Sampler: 4ac6d69 fix(sampler): opening the panel no longer stamps 120 BPM…
                                                            [q] cancel
```

| Step | Does | Reuses |
|---|---|---|
| Download updates | for every managed app, apply its follow rule (§2). Clean apps: no questions. | `update()` |
| Firmware components | `git submodule update --init` for `crosspad-core`, `crosspad-gui`, `crosspad-platform-idf` to the gitlinks `platform-idf` records | new, small |
| Build | `idf.py -B build_<rev> -DSDKCONFIG=sdkconfig.<rev> build`; `fullclean` first only when the installed app set changed since the last successful build (recorded in `.crosspad/last-update.json`). Progress = compiled units / total from ninja's `[n/m]` lines. | `_build_flash()` |
| Flash | OTA over CDC. Board in the MIDI+UAC2 profile → switch over SysEx first (what `bench.py ready` does). No CDC at all → UART through the STM bridge (what `bench.py recover` does). The screen only ever says `Flash`. | `ota_flash.py`, `bench.py` |
| Check | `APP_VERSIONS` over CDC; compare `LIBVER` and the per-app versions with what was built. | `bench.py flash` already proves the image landed |

**Local work.** An app with uncommitted files, untracked files, or commits
not on origin (today's `BLOCKING_FLAGS`) stops Download once, per app, with
a safe default:

```
 DAW Control has changes you made.
   [Enter] Leave it alone (keep my changes)
   [b]     Back it up to .crosspad/backups and update anyway
```

`[b]` = today's `backup_app()` then update. Enter = skip, row says
`DAW Control — your changes, left alone`. Clean apps never ask.

**Board revision.** Resolved through `crosspad_board.resolve()` right
before Build: a connected board decides (its `bcdDevice`), else the
remembered `"board"` in `crosspad.local.json`. Only when neither answers
does the pipeline stop at the Build row and ask, once, in place:

```
 ▸ Build                   Which CrossPad do you have?
                           [1] v2 (PCB 20, 2026)   [2] v1 (PCB 19)
                           Look at the sticker on the back cover.
```

The answer is remembered in `crosspad.local.json` (what `idf.py board
--set` does), so the question never comes back — and it never comes at all
when a board was plugged in, because the probe already answered. A
remembered revision that contradicts a later plugged-in board loses to the
board (the resolver's order), and the row says `v2 board found — building
v2`.

**No board.** Steps 1–3 run. Flash waits: `Plug in your CrossPad — I'll
flash it when I see it`, polling the device probe every 2 s, `[q]` leaves
the build in place (next run skips to Flash if nothing changed).

**Errors.** Steps stay on screen, the failing one turns red with one line of
what to do, never a stack trace:

| Failure | Line |
|---|---|
| fetch fails (offline) | `Can't reach GitHub — check your connection, then [r] retry` |
| build fails | `Build failed in Sampler → [c] show the error   [3] Something's wrong` (`[c]` pages the last 40 lines of the log) |
| flash: board never answers | `The board isn't answering → [r] retry   [3] Something's wrong` |
| check: versions differ | `The board still runs the old Sampler → [r] flash again` |

Full output of every step: `.crosspad/last-update.log`, overwritten per run.

### 4. Add or remove apps

```
 Add or remove apps                          type to filter · 12 apps

 On your CrossPad
   ✓ Sampler        0.2.1    MIDI sampler with kits, scenes and a pad editor
   ✓ Mixer          0.2.1    Audio mixer/router
   ✓ Instructions   1.0.0    How to play
   ✓ Fish Tank      0.1.0    A fish tank
   ✓ DAW Control    0.2.0    your own copy

 Available
 > Sequencer        0.2.0    MIDI step sequencer      (Arduino only)
   Synthesizer      0.3.0    Subtractive synth
   …

 [Enter] install / open   [x] remove   [q] back
```

Enter on an available app installs it at Latest release (today's
`_install_flow()`), then returns here with it under "On your CrossPad".
Enter on an installed app opens §2. Apps whose registry `platforms` do not
include this platform are dimmed with the reason and Enter says why.
Registry is refreshed when older than 1 h (unchanged), and the header says
`registry 10 s ago` on the right when the terminal is wide enough.

### 5. Something's wrong

Today's `_health()`, re-cut so every row is an action:

```
 Something's wrong?                              v2 board · connected

 ✓ Board found              v2, firmware v2, Sampler 0.2.1
 ✓ Tools                    ESP-IDF 5.5.4, gh signed in, Python 3.12
 ✗ USB serial guard         the board asks before switching USB mode
                            [Enter] turn it off on this board (bench use)
 ! Registry                 last fetched 2 days ago     [Enter] refresh
 ✓ Last update              yesterday 21:14, all versions matched

 [l] open last update log   [r] flash again via cable (board won't answer)
```

Rows and their fixes:

| Row | Check | Fix on Enter |
|---|---|---|
| Board found | device probe | none; without a board: `✗ No CrossPad found — plug it in over USB. Plugged in? On the pad: hold the encoder → Settings → USB → CDC` |
| Firmware vs board | `bcdDevice` revisions | `Update my CrossPad` |
| Tools | `idf.py` on PATH and importable, `gh auth status`, Python ≥ 3.10 | opens the exact command to run (`gh auth login`) — installing tools belongs to the onboarding spec |
| USB serial guard | `USB_GUARD` over CDC | `USB_GUARD 0` |
| Internet | the offline flag — nothing pings anything, it reports that nothing has failed to reach GitHub | refresh (clears the flag and tries again) |
| Registry | cache age | refresh |
| Last update | `.crosspad/last-update.json` | `[l]` pages the log |
| Board revision memory | `crosspad.local.json` vs connected board | `Forget the remembered board` |

`[r]` = `bench.py recover` with the newest `build_<rev>` — the one path for
"the board is bricked and won't talk".

### 6. Developer tools

Today's Workspace, Configure (feature flags), Profiles, Board, New app,
Tools, unchanged in function, one level down. Two edits:

- Workspace rows show the follow rule instead of `track=`: `follows latest
  release` / `follows master` / `stays on 8e21e0f` / `yours`. `[m]` opens §2.
- Backups, restore, park WIP, remove stay here as they are.

## Layout and input

- One `Screen` helper owns the alternate buffer, cursor hide/show, and
  redraw; every screen is a function that returns rows and a key map. Today's
  `_header` / `_section` / `_footer` / `_read_key` / `_menu_select` are kept
  and moved under it.
- Redraw on `KeyboardInterrupt`-safe key loop; `SIGWINCH` where it exists
  (POSIX) triggers a redraw, Windows redraws on the next key.
- Every list longer than the terminal scrolls, with the highlight kept in
  view; no screen truncates a list silently.
- Keys are letters and digits shown in brackets on the screen they act on,
  nothing hidden. `q` / `Esc` always go back; on the dashboard they quit.

## Data and state

- `crosspad.config.json`, `apps.json`, `crosspad.local.json`: unchanged
  formats. `TRACK_MODES` unchanged.
- New: `.crosspad/last-update.json` — `{started, finished, steps: [{name,
  ok, seconds}], installed_set: [...], build_rev}`; drives the estimate,
  the "apps changed" detection and the `fullclean` decision.
- New: `.crosspad/last-update.log`.
- `.crosspad/` is already gitignored.

## Decision layer, testable without a terminal

The "what's next" rule, the version-list rows for an app, the pipeline
step plan (which steps run, whether `fullclean`, which flash path) and the
error-line mapping are pure functions over dicts (`app_status()` output,
device probe, registry, last-update record). They live in the manager, not
the TUI, and get `pytest` cases under `tools/tests/` in the style of
`test_crosspad_board.py`. The TUI only renders their results.

## Testing

1. `pytest tools/tests/` for the decision layer (above).
2. On hardware: `Update my CrossPad` end to end with Sampler on
   "Latest development" — the 17 commits behind `origin/master` today are
   the fixture; `Check` must report the new Sampler version; `crosspad-hil
   run smoke` after.
3. The three OSes: dashboard, §2, §3 (with no board), §5 opened and quit
   on Windows `cmd.exe`, Windows Terminal, macOS Terminal, Linux — the
   alternate buffer restores the shell, glyph fallback engages on `cmd.exe`,
   an 80×24 window scrolls the app list.
4. Local-work prompt: dirty a submodule, run the pipeline, Enter leaves it,
   `[b]` backs it up (dir appears under `.crosspad/backups`) and updates.

## Not in this spec

- Onboarding: README minimal setup, a script that installs ESP-IDF, `gh`,
  the MCP server and the HIL venv, and starts this TUI. Next spec.
- PC-platform variant of the dashboard (`Build & Run` / `Run Sim`): the
  screens keep the platform switch that exists today; the pipeline's Flash
  and Check steps are `Run` on PC. Not designed in detail here.
- Firmware updates without building (prebuilt images from GitHub
  releases). Worth a spec of its own once release images exist.
