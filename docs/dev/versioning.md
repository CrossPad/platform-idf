# Submodules and versions

Contributor notes: the core and gui submodules, version floors, bump rules. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Submodule Workflow

Two git submodules:

| Submodule | Location in repo | Purpose |
|-----------|-----------------|---------|
| **crosspad-core** | `components/crosspad-core/` | Portable C++ business logic (shared with Arduino + PC) |
| **crosspad-gui** | `components/crosspad-gui/` | LVGL UI framework (shared with Arduino) |

**Updating submodules after upstream changes:**
```bash
cd components/crosspad-core
git fetch origin && git checkout origin/main
cd ../..
git add components/crosspad-core
```

**Arduino counterpart repo** (`ESP32-S3`) shares crosspad-core and uses Arduino adapters in `lib/crosspad_lib/src/platform/`.

### Submodule version gate

Both submodules carry a semver triple (`crosspad/CrosspadCoreVersion.hpp`,
`crosspad-gui/CrosspadGuiVersion.hpp`). Exact pairing -- a consumer declares the
triple it was written against and includes `crosspad/RequireVersion.hpp`
(major mismatch `#error`, minor/patch drift `#warning`; update the triple only
after building *and running* against the new version, never to silence the
warning) -- is how crosspad-gui pairs with core. **This firmware does not pair
exactly: it declares floors** (next bullet), so never re-add exact pins here.

- This firmware declares **floors** in `main/include/crosspad_deps.h`: another
  MAJOR or anything below is an `#error`, anything above builds quietly (the
  bump bot moves core/gui to what the apps' `requires` need and cannot edit
  code). The ranges themselves are checked by `tools/deps_resolve.py check` in
  every official build cell. crosspad-gui declares the core it needs in
  `CrosspadGuiVersion.hpp` (`CROSSPAD_GUI_REQUIRES_CORE_*`, exact pairing) and
  in `library.json` `dependencies` (the range the resolver reads).
- Bump rules: MAJOR when an interface a consumer implements or calls changes
  shape, MINOR when something is added, PATCH for fixes.
- Runtime: `crosspad_deps_check_runtime()` (after `bsp_stm32_init()`) logs the
  *linked* core/gui versions against the headers — stale objects the
  preprocessor cannot see — and the STM32 protocol against
  `STM32_PROTO_VERSION`. `APP_VERSIONS` over CDC ends with a
  `LIBVER: core=… gui=… stm_proto=have/want` line, `!` after anything that
  does not match; Settings → Info shows the same.
- The STM32 register map has one copy, `crosspad/stm32/reg_map.h` in core;
  `stm32_hal/include/stm32_reg_map.h` only includes it.
