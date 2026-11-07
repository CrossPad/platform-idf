# Dependency resolver for the firmware's apps, core and gui

Date: 2026-10-11. Status: draft for owner review.
Builds on the release automation (signed kit builds, app bump PRs); this is a
phase that slots in before auto-tag.

## Problem

Apps declare what they need in `crosspad-app.json`:

```json
"requires": {"crosspad-core": ">=1.45.0", "crosspad-gui": ">=1.16.0"}
```

Nothing enforces it. `tools/app_bump.py` pins an app's newest `vX.Y.Z` tag without
reading `requires`; `bump_gate.py` only judges the diff's shape and the build matrix.
Today, an app release that needs core 1.60 while the firmware pins core 1.53 gets
pinned anyway and fails only if the compiler trips on missing API; otherwise it ships
silently on a core it was not written for. Two apps with disjoint core ranges are not
detected at all. The only code that evaluates ranges (`version_satisfies` in
`crosspad_app_manager.py`) runs in the TUI and only warns.

The owner wants the behaviour of the ESP-IDF Component Manager: declared ranges, one
solver, an unsatisfiable set is an error, and the automation **bumps core/gui** when an
app needs a newer one (owner decision).

## Goals

1. Every app, core and gui version the firmware pins satisfies every declared range.
   Checked on every build of `main` and every PR, not only on bot PRs.
2. The hourly bump picks a consistent set: newest app versions, and the **lowest**
   core/gui that satisfies them (a smaller core jump risks fewer other apps).
3. A new app version that needs a newer core produces one bump PR that moves core
   (and gui, if needed) together with the app.
4. What cannot be satisfied is held back with one alert per cause in #ci-alerts, and
   the rest of the bump still goes out.

## Non-goals

- App-to-app dependencies. No app has one (every app's `REQUIRES` is core, gui,
  lvgl and third-party packages). Revisit when one appears.
- Automatic MAJOR bumps of core or gui. A MAJOR is a human decision.
- Downgrades. Candidates are only versions at or above the current pin.
- crosspad-pc and the registry. crosspad-pc tracks master; `registry.json` keeps
  serving the TUI. The solver reads app repos directly (see "Inputs").
- Replacing the git submodules with IDF components. The gitlinks stay the lock.

## Design

### Version sources

| What | Version from | Range declared in | Candidate versions |
|------|--------------|-------------------|--------------------|
| app | `crosspad-app.json` `version` | its `requires` | `vX.Y.Z` tags on the default branch, > pinned |
| core | `include/crosspad/CrosspadCoreVersion.hpp` (= `library.json`) | - | `vX.Y.Z` tags on the default branch, >= pinned, same MAJOR |
| gui | `include/crosspad-gui/CrosspadGuiVersion.hpp` (= `library.json`) | `library.json` `dependencies.crosspad-core` | same as core |

Range syntax is the one `version_satisfies` already implements and tests
(`>= <= > < == ^ ~`, comma = AND, `*`), ported into `tools/semver_range.py` with the
same test vectors plus the cases below. Unknown or unparsable range: **error**, not pass
(the TUI's lenient "unknown passes" is wrong for a gate).

gui's `library.json` today says `"crosspad-core": "*"`. It becomes `"^1.53.0"`
(MAJOR equal, at least the core it was built against), kept in sync with
`CROSSPAD_GUI_REQUIRES_CORE_*` by gui's own CI.

### Tagging core and gui

core and gui get the same reusable workflow the apps use
(`CrossPad/.github/.github/workflows/app-release.yml@v1`), with a new input
`kind: library`. For a library, `app_release.py`:

- reads the version from `library.json` and checks the version header agrees;
- creates the `vX.Y.Z` tag on the version-bump commit, same rules as apps (newer than
  the last tag, no backfill);
- creates **no GitHub release**: no `release.published`, so no Discord post and no
  relay. The hourly bump picks the tag up. (Owner rule: no spam.)

core and gui already have old release tags (`v1.22.0`, `v1.12.1`), so the no-backfill
rule does not apply to them: the first run (a manual dispatch after rollout) tags the
current version (`v1.53.0`, `v1.23.0`) as the baseline. Tags only, so no post.

The reusable workflow checks its scripts out at a fixed tag. The library mode ships as
tag `v2` (apps stay on `@v1`, unchanged; they can move later, `kind: app` is identical).

### Solver (`tools/deps_resolve.py`)

Inputs, all read with the bot's read-only token or from the checkout:

- the app set: union of apps in `config/profiles/*.json` (one gitlink per app serves
  every profile, so one solution serves them all);
- for each app: pinned version and `requires` (from the submodule at its gitlink),
  and every candidate tag with its `crosspad-app.json` at that tag;
- core and gui: pinned version, candidate tags, and gui's core range at each gui tag.

Algorithm (exhaustive, the space is small: about 13 apps × 1-3 new tags, and
under 10 lib candidates each):

```
for (core, gui) in candidates, ascending:            # lowest first
    if gui's range excludes core: skip
    for each app: pick the newest version (pinned or candidate) whose
                  requires accepts (core, gui); none -> this pair is infeasible
    score = number of apps at their newest tag
best = max score; ties -> lowest (core, gui)
```

An app is **held** when the best solution leaves it below its newest tag. Its cause is
the range that excluded the newer version, e.g. `slicer 0.4.0 needs crosspad-core
>=2.0.0; 2.x is a MAJOR bump`. If no pair is feasible (the current pin is broken), the
resolver fails: that is what `check` (below) exists to prevent.

Outputs a plan: the pins (apps, core, gui) with old→new versions and commits, and the
held list with causes. Deterministic: same inputs, same plan.

### Where it runs

1. **`app_bump.py` plan** calls the solver instead of "newest tag per app". core and
   gui are pinned like apps: gitlink only (they are not in profiles). The PR title
   lists everything, e.g. `chore(deps): core 1.53.0→1.60.0, slicer 0.3.1→0.4.0`;
   the body lists held apps with causes.
2. **Alerts:** each held cause goes to #ci-alerts once per (app, version, cause), on
   the same once-a-day slot the bump already uses (UTC hour 5). No repeat per hour.
3. **`bump-gate`** accepts a gitlink move of `components/crosspad-core` and
   `components/crosspad-gui` under the same rule as an app's (mode stays gitlink, the
   one commit is the bot's, verified). Which commit the bot picked is trusted the same
   way it is for apps today; the version rules are enforced where the content is: the
   build job's `check` (item 4) and the `crosspad_deps.h` floor, and `bump-gate` fails
   whenever a matrix cell is not green.
4. **`deps_resolve.py check --profile <kit>`** (offline, reads only the checkout) runs
   in every official build cell of `main.yml`, after `profile apply`, on every push and PR. A hand-made PR that pins an app beyond what
   core satisfies gets a red build with the exact range that failed.

### Compile-time pin (`main/include/crosspad_deps.h`)

Today it wants exactly core 1.53.0 and gui 1.23.0: a MINOR difference is a
`#warning`, which would fire on every bot bump of core and which the bot cannot fix
(it may not touch code). It becomes a **floor**: same MAJOR, at least the triple.
Below the floor or another MAJOR is an `#error`. The floor is raised by a person when
firmware code starts relying on newer core API. The runtime check
(`crosspad_deps_check_runtime`) is unchanged.

`RequireVersion.hpp` in core stays as it is (gui keeps its exact pairing with core;
its `library.json` range is what the solver reads).

## Error handling

- `APP_BUMP_HOLD` also accepts `core` and `gui`.
- Any API failure while reading candidates: no plan, no PR change, `report-failure`
  path as today. Never a partial plan.
- Unparsable `requires` in an app tag: that tag is not a candidate; cause reported as
  held. In the pinned app (the `check` path): error.
- A tag that is not on the default branch: not a candidate (same as today).

## Testing

- `semver_range`: the TUI's vectors, plus `^0.x` (minor is the breaking part),
  pre-release tags rejected, unknown operator is an error, whitespace, empty string.
- Solver, table-driven: all-newest fits; app needs newer core → core moves to the
  lowest satisfying tag; two apps disjoint → one held with cause; MAJOR needed →
  held; gui's range excludes the core an app needs → gui moves too or app held;
  ties → lowest pair; no candidates → no-op plan.
- `check`: passes on the current tree; fails naming the app and range when a pin is
  out of range.
- `bump-gate`: a core/gui gitlink move by the bot passes; a core gitlink added, deleted
  or retyped fails; another library path still fails.
- `app_release.py --kind library`: tags, creates no release; header/library.json
  mismatch refused.
- End-to-end on hardware (phase 6 style): bump a test app's `requires` above the
  pinned core on a branch, tag, watch the plan move core in one PR.

## Rollout

1. CrossPad/.github: `kind: library` in `app_release.py` and the reusable workflow;
   new tag `v2` after review (owner; tags there are admin-only).
2. crosspad-gui: `library.json` range `^1.53.0`, CI check that it matches the
   `CROSSPAD_GUI_REQUIRES_CORE_*` header; workflow stub. crosspad-core: workflow stub.
3. platform-idf: `semver_range.py`, `deps_resolve.py` (+ `check` in `main.yml`),
   `crosspad_deps.h` floor, `app_bump.py` integration, `bump-gate` for core/gui.
4. Pilot: next core version bump gets tagged; a test app tag that requires it
   produces one combined PR.

## Open risk

A core bump the solver picks can break the API of an app that did not ask for it.
The matrix build catches compile breaks (the PR stays red, alert as today); runtime
breaks are what phase 6's bench check is for.
