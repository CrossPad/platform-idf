// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

/**
 * @file instrument_host.h
 * @brief The board's IInstrumentHost: an app borrows the pitched engine.
 *
 * Registered in PlatformServices::instrumentHost by instrument_host_init().
 * While an app holds the engine, the kits keep their state but not their
 * sound: a kit load only parses (reloadPad leaves the engine alone) and a
 * pad's SamplePlayback request is not the kits' to play. release() reloads
 * the current kit, which brings the kits' engine back.
 */

/// Create the worker and publish the host. Once, after the platform init.
void instrument_host_init();

/// True from an app's first load until its release() has been carried out.
bool instrument_host_owns_engine();

/// An instrument is loaded beside the kits' engines -- a song's Pitch track
/// playing beside its kit tracks -- rather than in their place.
bool instrument_host_beside();

/// An instrument holding the engine in the kits' place for a song that plays
/// it beside them is given back, to be built beside them on its next request.
/// Not while an app that borrows the engine itself (Pitch Play) is in front.
void instrument_host_move_beside();

/// How long the last request took on the worker, ms (INST_STATUS).
uint32_t instrument_host_last_ms();
