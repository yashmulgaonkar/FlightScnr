#pragma once

#include <cstdint>

/**
 * Monotonic revision for settings portal sync.
 * Bump whenever a mirrored setting is persisted (device UI or web Save).
 */
uint32_t settingsStateRev();
unsigned long settingsStateUpdatedMs();
void settingsStateBump();
