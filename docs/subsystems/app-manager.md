# App Package Manager

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

CrossPad apps are managed as git submodules via a central registry at [CrossPad/crosspad-apps](https://github.com/CrossPad/crosspad-apps). The `idf.py` extension provides these commands:

```bash
idf.py app-list                              # List compatible apps (esp-idf)
idf.py app-list --all                        # Include incompatible platform apps
idf.py app-install --app sampler             # Install app as git submodule
idf.py app-install --app sampler --ref v1.0  # Install specific version/branch
idf.py app-install --app sampler --force     # Install despite platform incompatibility
idf.py app-remove --app sampler              # Remove app submodule
idf.py app-update --app sampler              # Update to latest
idf.py app-update --all                      # Update all installed apps
idf.py app-sync                              # Sync manifest with existing submodules
```

**How it works:**
- `idf_ext.py` registers commands with idf.py
- `tools/app_manager.py` is a thin wrapper that auto-downloads the shared core from `CrossPad/crosspad-apps`
- Registry is fetched anonymously over HTTPS (`raw.githubusercontent.com`, `main`) and cached locally (`app-registry.json`, 1h TTL); `gh api` is only the fallback when the plain request fails. The core itself and the `app-new` template (the public crosspad-apps archive) come the same way
- `apps.json` tracks installed apps (version, ref, install date)
- `main/CMakeLists.txt` auto-discovers installed app components via `components/crosspad-*/`
- `main/app/CMakeLists.txt` scans component sources for `REGISTER_APP` macros (generator: crosspad-core `cmake/crosspad_app_registry.cmake`, shared with crosspad-pc)
- After install/remove: **`idf.py -B build_v2 fullclean && idf.py -B build_v2 -DSDKCONFIG=sdkconfig.v2 build`** is required (`idf.py board` names the revision; `v1` boards use `build_v1` / `sdkconfig.v1`)

**Prerequisites:** none beyond ESP-IDF and git for installing, updating and creating apps — no GitHub account. `gh` (signed in with `gh auth login`) is needed only to publish your own app (`app-new --private` or `--public`, Submit an app) and to download release images from a private repo.

### Workspace policy, feature config, profiles

The manager also owns this project's compile-time configuration and knows which
submodules are yours.

```bash
idf.py app-status                                    # policy vs git state per app
idf.py app-track --app sampler --mode local          # registry|branch|pinned|local
idf.py app-config                                    # show Marlin-style flags
idf.py app-config --flag FEAT_PAD_EDITOR --value off # set one
idf.py app-profile --profile-action apply --name lite
idf.py app-backup --app sampler                      # snapshot local work
idf.py app-restore --app sampler --list
idf.py app-device                                    # what the board is running
```

- `crosspad.config.json` = intent (track policy + feature flags), `apps.json` =
  state (what is on disk), `crosspad.local.json` = personal overrides
  (gitignored), `config/profiles/*.json` = named recipes.
- **Nothing destructive runs over local work.** An app that is dirty, ahead of
  origin, on an unexpected branch or on a fork is skipped by `update`; `--force`
  proceeds but snapshots to `.crosspad/backups/<app>/<ts>/` first (patch +
  untracked tarball + git bundle + stashes). `restore` replays it.
- **Feature flags never touch the submodule header.** Chosen values go to
  `crosspad.config.json`; only deviations from `Configuration.h` defaults are
  emitted to `.crosspad/build_flags.cmake`, which the top-level `CMakeLists.txt`
  includes. An untouched project builds exactly as the headers say.
- **The device answers for itself.** `main/generate_app_versions.cmake` bakes the
  installed app submodules (commit + manifest pin + dirty flag) into the binary
  at configure time; `APP_VERSIONS` over CDC reports them and Settings → Info
  lists them. `idf.py app-device` diffs that against the checkout — the manifest
  says what should be installed, the device says what is actually flashed.
- The flag catalog lives in
  `components/crosspad-core/include/crosspad/config/features.schema.json`.
  Adding a flag means: schema entry + `#ifndef` block in `Configuration.h` +
  `#if FEAT_X` at the call sites + a `SanityCheck.h` 0/1 guard.

## Setup

One command per system installs everything and opens CP Tools — git, Python,
build tools, GitHub CLI and sign-in, the project with its components, ESP-IDF v5.5.5, USB access,
crosspad-hil, the MCP server for Claude Code, a `cptools` launcher (desktop
shortcut on Windows), then `doctor`:

```
Windows:      powershell -ExecutionPolicy Bypass -c "irm https://raw.githubusercontent.com/CrossPad/crosspad-apps/main/install/install.ps1 | iex"
macOS/Linux:  curl -fsSL https://raw.githubusercontent.com/CrossPad/crosspad-apps/main/install/install.sh | bash
```

The scripts live in crosspad-apps/install/ (public, next to the manager).
Every step checks first and repairs: a folder that is not a repository is
moved aside, files deleted inside ESP-IDF come back from its git, a lost
ESP-IDF Python environment is rebuilt, missing components are fetched.
Windows installs per user where it can (winget may fall back to a machine-wide
install and ask once; Visual Studio's C++ tools for the PC simulator always
ask) and keeps to short ASCII paths
(`C:\CrossPad`, `C:\esp`, `IDF_TOOLS_PATH=C:\esp\.espressif`) — user
names with spaces or diacritics break ESP-IDF tools. Tested from scratch
and against a broken setup in clean `ubuntu:24.04` and Windows 11
(`dockurr/windows`) containers.

## Updates, recovery, diagnostics

- **No GitHub account to update.** Registry, changelogs and the manager
  itself come over anonymous HTTPS (urllib), gh only as a fallback. Sign-in
  is a note on Something's wrong ("only needed to publish your own apps").
- **Ready-built firmware.** CI's release job writes
  `CrossPad-<tag>-components.json` (the commit of every submodule the default
  kit was built from, recorded by the build job after the kit's profile moved
  any branch-tracked app). When a
  checkout matches the newest published release exactly — same commits,
  nothing uncommitted, no feature flags changed — Build downloads
  `CrossPad-<tag>-<rev>.bin`, checks it against `SHA256SUMS.txt` and the flash
  hook gets `image=`. Anything else compiles. Private repo: through
  `gh release download`.
- **Stop, go back, repair.** `q`/Ctrl+C stops a step (the whole process
  tree) instead of the TUI; a stopped or failed update is the first line
  of the dashboard. Something's wrong → Go back checks out the commits from
  before the last update (and keeps them), or switches the board to its
  previous firmware slot (`FW_SLOTS`/`FW_SWITCH`, seconds). A folder git
  cannot read is "broken — [2] to repair".
- **Failures in words.** Build/flash output is matched against known
  causes (disk full, long Windows path, missing clean build — forces one on
  retry — USB port busy or not permitted, pyserial missing).
- **doctor / support / --json.** `python3 tools/app_manager.py doctor`
  (exit 1 when something is wrong: tools, git, certificates, project path,
  disk, Defender, board, firmware, USB guard, internet, registry, go-back,
  previous slot), `support` (a zip for #support, secrets blanked),
  `--json` on list/status/doctor/device.
- **Screens.** `?` help on every screen from the hints it draws; `/` finds
  any action; mouse (wheel, clicks on `[key]` labels); `NO_COLOR`,
  `CROSSPAD_PLAIN` for screen readers; live resize on Windows; a welcome
  screen once; what each waiting update brings under What's next; the
  update check in the background; a picker when two boards are plugged in
  (`device=` to the flash hook); requirement checks against the core/gui
  version headers; Settings (update check, Early features); Submit an app
  to the catalog (a PR on crosspad-apps through gh).

## Screens

`idf.py app-manage` presents the manager as a list of tasks rather than a
list of objects (Apps / Build / Device / Workspace / Profiles). Full design,
including layout/input rules and the decision-layer test plan: spec
`docs/design/2026-09-15-app-manager-tui-redesign-design.md`.

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

### 2. App screen — versions

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

Mapping to `crosspad.config.json` (unchanged file format, `TRACK_MODES` stay):

| Row | Config | `update()` does |
|---|---|---|
| Latest release | `{"track": "registry"}` | checkout the newest release tag reachable on origin (`v<semver>`); the registry's `version` field is what the dashboard compares against |
| Latest development | `{"track": "branch", "ref": "<default branch>"}` | `fetch` + `merge --ff-only origin/<ref>` |
| Concrete tag / commit | `{"track": "pinned", "commit": "<sha>"}` | nothing (and on pick: checkout that sha) |
| Other commit… | text input → same as concrete | |

### 3. Update my CrossPad — the pipeline

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

Steps always run in this order — Download updates, Firmware components,
Build, Flash, Check — and always land in `.crosspad/last-update.log`. An app
with local work stops Download once, per app, with a safe default (leave it
alone / back it up and update anyway). The board revision is resolved by
`crosspad_board.resolve()` right before Build and only asked for, in place,
when neither a connected board nor a remembered one answers. With no board,
Flash polls the device probe every 2 s.

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

`[x]` removes only while the filter is empty.

### 5. Something's wrong

```
 Something's wrong?                              v2 board · connected

 ✓ Board found              v2, firmware v2, Sampler 0.2.1
 ✓ Tools                    ESP-IDF 5.5.4, gh signed in, Python 3.12
 ✗ USB serial guard         the board asks before switching USB mode
                            [Enter] turn it off on this board (bench use)
 ! Registry                 last fetched 2 days ago     [Enter] refresh
 ✓ Last update              yesterday 21:14, all versions matched

 [l] open last update log   [s] report for support   [f] flash again via cable
```

`[f]` runs the same recovery path as `bench.py recover` with the newest
`build_<rev>` — the one path for "the board is bricked and won't talk".

### 6. Developer tools

Today's Workspace, Configure (feature flags), Profiles, Board, New app,
Tools, unchanged in function, one level down. Workspace rows show the follow
rule instead of `track=`: `follows latest release` / `follows master` /
`stays on 8e21e0f` / `yours`; `[m]` opens the App screen (§2).

### Data files and hooks

- `.crosspad/last-update.json` — `{started, finished, steps: [{name, ok,
  seconds}], installed_set: [...], build_rev}`; drives the time estimate, the
  "apps changed" detection on the dashboard, and whether Build needs
  `fullclean`.
- `.crosspad/last-update.log` — full output of the last pipeline run,
  overwritten per run; what `[l]`/`[c]` page.
- The ESP-IDF-specific hooks the pipeline calls into `tools/app_manager.py`:
  `device_probe`, `flash_ota(rev, on_line, device=None, image=None)`,
  `flash_uart`, `board_revs`, `board_set`. The core passes `device=` and
  `image=` only to a hook whose signature takes them, so an older wrapper
  keeps working.
- `.crosspad/last-update.json` also carries `previous` / `after` (app →
  commit) when an update moved apps: the "Go back" point.
- `.crosspad/official-release.json` — the newest published release, 1 h;
  `.crosspad/firmware/<tag>/` — downloaded release images;
  `.crosspad/support/` — support zips; `crosspad.local.json` → `cp_tools`
  (settings) and `idf_path` (written by the installer).
- The wrapper keeps the downloaded manager's digest in
  `tools/.crosspad_app_manager.downloaded` and never overwrites a copy that
  differs from it (your edit); a download that does not compile is refused,
  and a copy that fails to load falls back to `.prev`.
