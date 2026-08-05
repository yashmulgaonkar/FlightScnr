#pragma once

#include <cstdint>

#include "hardware/pin_config.h"

namespace hardware {

/** Configure buzzer / haptic (call once from setup). */
void buzzerInit();

/** Load beep on/off and tone from flash. */
void buzzerBootLoad();

bool buzzerEnabled();
/** Current tone step A (quietest) through E (loudest). Piezo boards only. */
char buzzerToneLetter();

void buzzerSetEnabled(bool enabled);
/** Step beep tone A–E (no-op on haptic / Waveshare boards). */
void buzzerToneStep(int8_t delta);

/** Start a short non-blocking click if beeps/vibration are enabled. */
void buzzerClick();

/** Play alert feedback (piezo boards only — haptic boards use screen flash). */
void buzzerAlert();

/** Stop an in-progress beep; call from loop. */
void buzzerPoll();

void saveBeepEnabledFromForm(const char* checkbox_value);
void saveBeepToneFromForm(const char* value);

}  // namespace hardware
