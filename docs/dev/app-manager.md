# The app package manager

Contributor notes: idf.py app-*, profiles, tracks. The short version of what a change in this area
must not break; the pages it links to have the detail.

## App Package Manager

Apps are git submodules under `components/crosspad-*/`, managed by `idf.py app-*`
(registry: [CrossPad/crosspad-apps](https://github.com/CrossPad/crosspad-apps),
fetched anonymously over HTTPS with `gh` only as a fallback, cached 1 h).
Using it: [Install and remove apps](https://crosspad.app/docs/how-to/install-apps/);
every command and flag: [CLI reference](https://crosspad.app/docs/reference/cli/);
the one-line installer: [Start here, step 1](https://crosspad.app/docs/start-here/1-toolchain/).
`idf.py app-manage` opens the TUI: **What's next** on the first line,
`[1] Update my CrossPad` (one log in `.crosspad/last-update.log`), `[2] Add or
remove apps`, `[3] Something's wrong`, `[4] Developer tools` (Workspace,
Configure, Profiles, Board, Build & Flash, New app). An app *follows the
latest release* (`track=registry`, newest `v*` tag), *follows development*
(`track=branch`) or *stays on a version* (`track=pinned`); the UI never names
the modes. Spec: `docs/design/2026-09-15-app-manager-tui-redesign-design.md`.
`tools/app_manager.py doctor | support | status --json`; `?` on every screen; a
checkout that equals a published release flashes its image instead of building.
After `app-install`/`app-remove`: `fullclean` + build of the revision's build dir.
VS Code: `.vscode/settings.template.json` puts **CP Tools** and **OTA** buttons
in the status bar (needs the "VsCode Task Buttons" extension; tasks in
`.vscode/tasks.json`).

`crosspad.config.json` = intent (track policy + feature flags), `apps.json` = state,
`crosspad.local.json` = personal overrides (gitignored). Nothing destructive runs
over local work; feature flags never touch submodule headers (deviations go to
`.crosspad/build_flags.cmake`). `APP_VERSIONS` over CDC says what the device
actually runs. Details, policies, flag catalog: [docs/subsystems/app-manager.md](../subsystems/app-manager.md).
