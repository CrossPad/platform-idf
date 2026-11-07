// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// The board's inputs, as mixer channels IN1..IN3: the mics, Line 1/2 and,
/// where the board has it (BSP_HAS_SHARED_LINE_IN), Line 3/4.
enum InputSource : uint8_t { kInputMic = 0, kInputLine12, kInputLine34, kInputSources };

/// Which inputs the two ADCs hear (crosspad::IInputPairs): registers
/// PlatformServices::inputPairs on a board with Line 3/4 and connects the
/// pair saved in NVS. Before input_gain_init(), whose gains follow the pair.
void input_pairs_init();

/// RT: the codec input @p src is on now, or -1 while the pair leaves it out.
int input_pairs_codec_of(int src);

/// RT: the input on codec @p codec now.
int input_pairs_source_on(int codec);

} // namespace crosspad
