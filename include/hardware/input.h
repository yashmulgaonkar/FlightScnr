#pragma once

#include <cstdint>

/** Rotary encoder, knob button, and CHSC5816 touch gestures. */
void inputInit();

/** Poll encoder rotation, knob press, and touch. Call frequently from loop. */
void inputPoll();

/**
 * Rotary encoder detent since last call.
 * @return +1 clockwise, -1 counter-clockwise, 0 if none.
 */
int8_t inputConsumeEncoderDelta();

/** True after knob short-press (no touch coordinates). */
bool inputConsumeKnobTap();

/** True once when the knob button is first pressed down. */
bool inputConsumeKnobPress();

/**
 * True after a screen tap (not a swipe). Optional touch position in display coords.
 */
bool inputConsumeScreenTap(int16_t* x, int16_t* y);

enum SwipeGesture : uint8_t {
  SwipeNone = 0,
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
};

/** Swipe detected since last poll; SwipeNone if no swipe pending. */
SwipeGesture inputConsumeSwipe();

/** Poll for knob long-press (Wi-Fi reset). May reboot the device. */
void inputPollLongPress();

/**
 * True once when the user released the knob after the Wi-Fi reset countdown UI
 * was shown (abort). Caller should redraw the active screen.
 */
bool inputConsumeWifiResetUiCancelled();

/** Drop queued knob/encoder/touch events (e.g. after idle timeout to radar). */
void inputDiscardPendingInteractions();
