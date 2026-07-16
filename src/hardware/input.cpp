#include "hardware/input.h"

#include <Wire.h>

#include <cmath>
#include <memory>

#include "TouchDrvCHSC5816.hpp"
#include "Arduino_DriveBus_Library.h"
#include "config.h"
#include "hardware/buzzer.h"
#include "hardware/panel.h"
#include "hardware/pin_config.h"
#include "services/wifi_setup.h"
#include "touch_chip/Arduino_CST816x.h"
#include "ui/boot_screens.h"

namespace {

portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_encoder_step_pending = false;
volatile int8_t s_encoder_pending_delta = 0;
int8_t s_encoder_accum = 0;

/** Quadrature transition: -1, 0, or +1 half-step. */
constexpr int8_t kEncoderQuad[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0,
};
constexpr int8_t kEncoderDetentThreshold = 2;
volatile bool s_knob_tap_pending = false;
volatile bool s_knob_press_pending = false;
volatile int16_t s_tap_x = -1;
volatile int16_t s_tap_y = -1;
volatile SwipeGesture s_swipe_pending = SwipeNone;
volatile bool s_knob_is_down = false;
volatile unsigned long s_knob_down_ms = 0;
volatile bool s_wifi_reset_ui_shown = false;
bool s_long_press_handled = false;
bool s_wifi_reset_ui_cancelled_pending = false;
bool s_knob_interrupt_attached = false;

uint8_t s_knob_previous = 0;
unsigned long s_knob_scan_ms = 0;

TouchDrvCHSC5816 s_touch;
std::shared_ptr<Arduino_IIC_DriveBus> s_cst816_bus;
std::unique_ptr<Arduino_IIC> s_cst816;
bool s_touch_ready = false;
bool s_touch_was_down = false;
bool s_touch_tracking = false;
int16_t s_touch_start_x = 0;
int16_t s_touch_start_y = 0;
int16_t s_touch_last_x = 0;
int16_t s_touch_last_y = 0;

constexpr int kSwipeMinPx = 70;
constexpr int kTapMaxPx = 25;

void IRAM_ATTR onKnobButtonIsr() {
  const bool down = digitalRead(config::kKnobKeyPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_input_mux);
  if (down) {
    if (!s_knob_is_down) {
      s_knob_press_pending = true;
    }
    s_knob_is_down = true;
    s_knob_down_ms = now;
  } else if (s_knob_is_down) {
    const unsigned long held = now - s_knob_down_ms;
    // Suppress tap if the Wi-Fi reset countdown UI was shown (user aborting wipe).
    if (held >= config::kKnobTapMinMs && held < config::kKnobResetHoldMs &&
        !s_wifi_reset_ui_shown) {
      s_knob_tap_pending = true;
    }
    s_knob_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_input_mux);
}

void initKnobButton() {
  pinMode(config::kKnobKeyPin, INPUT_PULLUP);
  if (s_knob_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kKnobKeyPin)),
                  onKnobButtonIsr, CHANGE);
  s_knob_interrupt_attached = true;
}

void initEncoder() {
  pinMode(config::kKnobPinA, INPUT_PULLUP);
  pinMode(config::kKnobPinB, INPUT_PULLUP);
  s_knob_previous = 0;
  if (digitalRead(config::kKnobPinA)) {
    s_knob_previous |= 0x02;
  }
  if (digitalRead(config::kKnobPinB)) {
    s_knob_previous |= 0x01;
  }
}

void pollEncoder() {
  if (millis() < s_knob_scan_ms) {
    return;
  }
  s_knob_scan_ms = millis() + 20;

  uint8_t state = 0;
  if (digitalRead(config::kKnobPinA)) {
    state |= 0x02;
  }
  if (digitalRead(config::kKnobPinB)) {
    state |= 0x01;
  }

  if (state == s_knob_previous) {
    return;
  }

  const int8_t movement = kEncoderQuad[(s_knob_previous << 2) | state];
  s_knob_previous = state;
  if (movement == 0) {
    return;
  }

  s_encoder_accum += movement;
  portENTER_CRITICAL(&s_input_mux);
  if (s_encoder_accum >= kEncoderDetentThreshold) {
    s_encoder_step_pending = true;
    s_encoder_pending_delta = 1;
    s_encoder_accum -= kEncoderDetentThreshold;
  } else if (s_encoder_accum <= -kEncoderDetentThreshold) {
    s_encoder_step_pending = true;
    s_encoder_pending_delta = -1;
    s_encoder_accum += kEncoderDetentThreshold;
  }
  portEXIT_CRITICAL(&s_input_mux);
}

void onCst816Interrupt() {
  if (s_cst816 != nullptr) {
    s_cst816->IIC_Interrupt_Flag = true;
  }
}

void initTouchChsc5816() {
  s_touch.setPins(TOUCH_RST, TOUCH_INT);
  if (!s_touch.begin(Wire, CHSC5816_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("CHSC5816 touch init failed — encoder/knob only");
    s_touch_ready = false;
    return;
  }
  s_touch_ready = true;
  Serial.println("CHSC5816 touch ready");
}

void initTouchCst816() {
  s_cst816_bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
  s_cst816 = std::unique_ptr<Arduino_IIC>(new Arduino_CST816x(
      s_cst816_bus, CST816_SLAVE_ADDRESS, TOUCH_RST, TOUCH_INT, onCst816Interrupt));
  if (!s_cst816->begin()) {
    Serial.println("CST816 touch init failed — encoder/knob only");
    s_touch_ready = false;
    return;
  }
  s_cst816->IIC_Write_Device_State(
      s_cst816->Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE,
      s_cst816->Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC);
  s_touch_ready = true;
  Serial.println("CST816 touch ready");
}

void initTouch() {
  if (hardware::panelUsesCo5300()) {
    initTouchCst816();
  } else {
    initTouchChsc5816();
  }
}

void queueSwipe(SwipeGesture gesture) {
  portENTER_CRITICAL(&s_input_mux);
  s_swipe_pending = gesture;
  portEXIT_CRITICAL(&s_input_mux);
}

void queueTap(int16_t x, int16_t y) {
  portENTER_CRITICAL(&s_input_mux);
  s_tap_x = x;
  s_tap_y = y;
  portEXIT_CRITICAL(&s_input_mux);
}

void finishTouchGesture() {
  const int dx = s_touch_last_x - s_touch_start_x;
  const int dy = s_touch_last_y - s_touch_start_y;
  const int adx = std::abs(dx);
  const int ady = std::abs(dy);

  if (dx <= -kSwipeMinPx && ady * 2 < adx) {
    queueSwipe(SwipeLeft);
  } else if (dx >= kSwipeMinPx && ady * 2 < adx) {
    queueSwipe(SwipeRight);
  } else if (dy >= kSwipeMinPx && adx * 2 < ady) {
    queueSwipe(SwipeDown);
  } else if (dy <= -kSwipeMinPx && adx * 2 < ady) {
    queueSwipe(SwipeUp);
  } else if (adx <= kTapMaxPx && ady <= kTapMaxPx) {
    queueTap(s_touch_last_x, s_touch_last_y);
  }
}

void pollTouchChsc5816() {
  int16_t x[2] = {};
  int16_t y[2] = {};
  const uint8_t points = s_touch.getPoint(x, y);
  const bool down = points > 0;

  if (down && !s_touch_was_down) {
    s_touch_start_x = x[0];
    s_touch_start_y = y[0];
    s_touch_last_x = x[0];
    s_touch_last_y = y[0];
    s_touch_tracking = true;
    hardware::buzzerClick();
  } else if (down && s_touch_tracking) {
    s_touch_last_x = x[0];
    s_touch_last_y = y[0];
  } else if (!down && s_touch_was_down && s_touch_tracking) {
    finishTouchGesture();
    s_touch_tracking = false;
  }

  s_touch_was_down = down;
}

void pollTouchCst816() {
  if (s_cst816 == nullptr) {
    return;
  }

  const bool down =
      s_cst816->IIC_Read_Device_Value(
          s_cst816->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER) > 0;
  int16_t x = 0;
  int16_t y = 0;
  if (down) {
    x = static_cast<int16_t>(s_cst816->IIC_Read_Device_Value(
        s_cst816->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X));
    y = static_cast<int16_t>(s_cst816->IIC_Read_Device_Value(
        s_cst816->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y));
  }

  if (down && !s_touch_was_down) {
    s_touch_start_x = x;
    s_touch_start_y = y;
    s_touch_last_x = x;
    s_touch_last_y = y;
    s_touch_tracking = true;
    hardware::buzzerClick();
  } else if (down && s_touch_tracking) {
    s_touch_last_x = x;
    s_touch_last_y = y;
  } else if (!down && s_touch_was_down && s_touch_tracking) {
    finishTouchGesture();
    s_touch_tracking = false;
  }

  s_touch_was_down = down;
}

void pollTouch() {
  if (!s_touch_ready) {
    return;
  }

  if (hardware::panelUsesCo5300()) {
    pollTouchCst816();
  } else {
    pollTouchChsc5816();
  }
}

}  // namespace

void inputInit() {
  initKnobButton();
  initEncoder();
  initTouch();
}

void inputPoll() {
  pollEncoder();
  pollTouch();
}

int8_t inputConsumeEncoderDelta() {
  portENTER_CRITICAL(&s_input_mux);
  const int8_t delta = s_encoder_step_pending ? s_encoder_pending_delta : 0;
  if (delta != 0) {
    s_encoder_step_pending = false;
    s_encoder_pending_delta = 0;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return delta;
}

bool inputConsumeKnobTap() {
  portENTER_CRITICAL(&s_input_mux);
  const bool tap = s_knob_tap_pending;
  if (tap) {
    s_knob_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return tap;
}

bool inputConsumeKnobPress() {
  portENTER_CRITICAL(&s_input_mux);
  const bool press = s_knob_press_pending;
  if (press) {
    s_knob_press_pending = false;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return press;
}

bool inputConsumeScreenTap(int16_t* x, int16_t* y) {
  portENTER_CRITICAL(&s_input_mux);
  const bool tap = s_tap_x >= 0 && s_tap_y >= 0;
  if (tap) {
    if (x != nullptr) {
      *x = s_tap_x;
    }
    if (y != nullptr) {
      *y = s_tap_y;
    }
    s_tap_x = -1;
    s_tap_y = -1;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return tap;
}

SwipeGesture inputConsumeSwipe() {
  portENTER_CRITICAL(&s_input_mux);
  const SwipeGesture swipe = s_swipe_pending;
  if (swipe != SwipeNone) {
    s_swipe_pending = SwipeNone;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return swipe;
}

void inputDiscardPendingInteractions() {
  portENTER_CRITICAL(&s_input_mux);
  s_encoder_step_pending = false;
  s_encoder_pending_delta = 0;
  s_encoder_accum = 0;
  s_knob_tap_pending = false;
  s_knob_press_pending = false;
  s_swipe_pending = SwipeNone;
  s_tap_x = -1;
  s_tap_y = -1;
  portEXIT_CRITICAL(&s_input_mux);
}

void inputPollLongPress() {
  if (digitalRead(config::kKnobKeyPin) == LOW) {
    portENTER_CRITICAL(&s_input_mux);
    if (!s_knob_is_down) {
      s_knob_is_down = true;
      s_knob_down_ms = millis();
      s_wifi_reset_ui_shown = false;
    }
    const unsigned long down_ms = s_knob_down_ms;
    portEXIT_CRITICAL(&s_input_mux);

    const unsigned long held = millis() - down_ms;
    if (!s_long_press_handled && held >= config::kKnobResetCountdownStartMs &&
        held < config::kKnobResetHoldMs) {
      bootScreenWifiResetCountdownTick(held, config::kKnobResetHoldMs);
      if (bootScreenWifiResetCountdownActive()) {
        portENTER_CRITICAL(&s_input_mux);
        s_wifi_reset_ui_shown = true;
        portEXIT_CRITICAL(&s_input_mux);
      }
    }

    if (!s_long_press_handled && held >= config::kKnobResetHoldMs) {
      s_long_press_handled = true;
      bootScreenWifiResetCountdownCancel();
      Serial.println("Knob held, resetting Wi-Fi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    const bool was_showing = bootScreenWifiResetCountdownActive() || s_wifi_reset_ui_shown;
    portENTER_CRITICAL(&s_input_mux);
    s_knob_is_down = false;
    s_wifi_reset_ui_shown = false;
    portEXIT_CRITICAL(&s_input_mux);
    s_long_press_handled = false;
    if (was_showing) {
      bootScreenWifiResetCountdownCancel();
      s_wifi_reset_ui_cancelled_pending = true;
    }
  }
}

bool inputConsumeWifiResetUiCancelled() {
  const bool pending = s_wifi_reset_ui_cancelled_pending;
  s_wifi_reset_ui_cancelled_pending = false;
  return pending;
}
