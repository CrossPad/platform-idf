/*
 * The shared submodules this firmware was written against.
 *
 * Two gates, because they catch different mistakes:
 *
 *  - Compile time (below): the headers in components/crosspad-core and
 *    components/crosspad-gui against the floors declared here. Another MAJOR
 *    or anything below the floor is an #error; anything above it builds
 *    quietly, because the bump bot moves core/gui to what the apps' ranges
 *    need (tools/deps_resolve.py, spec 2026-10-11 deps resolver) and may not
 *    edit this file. Raise a floor when firmware code starts relying on newer
 *    API, not to follow a submodule.
 *
 *  - Runtime (crosspad_deps_check_runtime): what got *linked* versus those same
 *    headers, and the STM32 register-map protocol once the I2C link is up.
 *    Stale objects and a co-processor flashed from another branch are
 *    invisible to the preprocessor.
 */
#pragma once

#include "crosspad/CrosspadCoreVersion.hpp"
#include "crosspad-gui/CrosspadGuiVersion.hpp"

#define CROSSPAD_IDF_REQUIRES_CORE_MAJOR 2
#define CROSSPAD_IDF_REQUIRES_CORE_MINOR 47
#define CROSSPAD_IDF_REQUIRES_CORE_PATCH 0

#define CROSSPAD_IDF_REQUIRES_GUI_MAJOR 2
#define CROSSPAD_IDF_REQUIRES_GUI_MINOR 14
#define CROSSPAD_IDF_REQUIRES_GUI_PATCH 0

#define CROSSPAD_DEPS_BELOW_(H, W) \
    (H##_MINOR < W##_MINOR || (H##_MINOR == W##_MINOR && H##_PATCH < W##_PATCH))

#if CROSSPAD_CORE_VERSION_MAJOR != CROSSPAD_IDF_REQUIRES_CORE_MAJOR
#error "crosspad-core MAJOR differs from the one this firmware was written for (main/include/crosspad_deps.h)"
#elif CROSSPAD_DEPS_BELOW_(CROSSPAD_CORE_VERSION, CROSSPAD_IDF_REQUIRES_CORE)
#error "crosspad-core is older than the floor in main/include/crosspad_deps.h"
#endif

#if CROSSPAD_GUI_VERSION_MAJOR != CROSSPAD_IDF_REQUIRES_GUI_MAJOR
#error "crosspad-gui MAJOR differs from the one this firmware was written for (main/include/crosspad_deps.h)"
#elif CROSSPAD_DEPS_BELOW_(CROSSPAD_GUI_VERSION, CROSSPAD_IDF_REQUIRES_GUI)
#error "crosspad-gui is older than the floor in main/include/crosspad_deps.h"
#endif

#undef CROSSPAD_DEPS_BELOW_

/**
 * Log the linked core/gui versions against the headers, and the STM32
 * protocol version against STM32_PROTO_VERSION. Call once, after
 * bsp_stm32_init() so the I2C probe result is in. Returns false when anything
 * is incompatible (major mismatch, or STM proto differs); the caller decides
 * whether that is fatal.
 */
bool crosspad_deps_check_runtime(void);

/**
 * One line for CDC / Settings: "core=0.2.0 gui=0.2.0 stm_proto=0x000B/0x000B".
 * Appends " !" markers after any component that does not match. Returns @p buf.
 */
const char* crosspad_deps_summary(char* buf, unsigned len);
