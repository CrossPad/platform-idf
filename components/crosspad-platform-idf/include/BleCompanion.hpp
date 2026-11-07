#pragma once
#include <cstddef>
namespace crosspad {
// Execute on the main command task, never in NimBLE's GATT callback.
void bleCompanionPoll(void (*handler)(const char *, char *, size_t));
// Runs on the NimBLE host task the moment a complete command is waiting, so
// the main loop can wake for it instead of noticing on its next periodic pass.
void bleCompanionSetWake(void (*wake)());
}
