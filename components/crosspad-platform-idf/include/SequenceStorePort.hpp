// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace crosspad {

/// Starts the task that reads and writes the open song's `sequences.json`.
/// Safe to call twice; the second call does nothing.
void sequence_store_start();

/// A kit became current. The scenes are not the kit's, so this changes
/// nothing about them -- except on a board that never had shared scenes, where
/// the first song opened adopts that kit's own `sequences.json`. Cheap and
/// non-blocking, safe from the LVGL thread. Pass the kit's `kit.json` path.
void sequence_store_kit_changed(const char* kitJsonPath);

/// Ask for a write on the next sweep even if nothing is marked dirty.
void sequence_store_save_soon();

/// The file the scenes are read from and written to -- the open song's --
/// "" until it was read. For diagnostics.
const char* sequence_store_current_file();

} // namespace crosspad
