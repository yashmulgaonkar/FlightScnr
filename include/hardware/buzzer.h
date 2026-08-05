#pragma once

#include <cstdint>

namespace hardware {

/** Configure buzzer PWM (call once from setup). */
void buzzerInit();

/** Load beep on/off and tone from flash. */
void buzzerBootLoad();

bool buzzerEnabled();
/** Current tone step A (quietest) through E (loudest). Piezo boards. */
char buzzerToneLetter();
/** Intensity 1–5 (same storage as tone A–E). Prefer for haptic boards. */
uint8_t buzzerIntensityLevel();
/** "20%" … "100%" for the current intensity step. */
const char* buzzerIntensityLabel();

void buzzerSetEnabled(bool enabled);
void buzzerToneStep(int8_t delta);

/** Start a short non-blocking click if beeps are enabled. */
void buzzerClick();

/** Play a 3x beep alert pattern (non-blocking, higher frequency). */
void buzzerAlert();

/** Stop an in-progress beep; call from loop. */
void buzzerPoll();

void saveBeepEnabledFromForm(const char* checkbox_value);
void saveBeepToneFromForm(const char* value);

}  // namespace hardware
