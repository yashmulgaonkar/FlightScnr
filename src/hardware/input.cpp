#include "hardware/input.h"

#include <Wire.h>

#include <cmath>
#include <memory>

#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
#include "TouchDrvCHSC5816.hpp"
#endif
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
volatile int8_t s_encoder_accum = 0;

/** Quadrature transition: -1, 0, or +1 half-step (T-Encoder Pro). */
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

volatile uint8_t s_knob_previous = 0;

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
// Waveshare demo (bidi_switch_knob): 3ms poll, rising A = CW, rising B = CCW.
#include <esp_timer.h>
constexpr uint32_t kWaveshareEncoderPollUs = 3000;
constexpr uint8_t kWaveshareEncoderDebounceTicks = 2;
uint8_t s_enc_a_level = 1;
uint8_t s_enc_b_level = 1;
uint8_t s_enc_a_debounce = 0;
uint8_t s_enc_b_debounce = 0;
esp_timer_handle_t s_enc_timer = nullptr;

void queueEncoderStep(int8_t step) {
  portENTER_CRITICAL(&s_input_mux);
  s_encoder_step_pending = true;
  if (step > 0) {
    if (s_encoder_pending_delta < 20) {
      s_encoder_pending_delta =
          static_cast<int8_t>(s_encoder_pending_delta + 1);
    }
  } else if (step < 0) {
    if (s_encoder_pending_delta > -20) {
      s_encoder_pending_delta =
          static_cast<int8_t>(s_encoder_pending_delta - 1);
    }
  }
  portEXIT_CRITICAL(&s_input_mux);
}

/** Port of Waveshare process_knob_channel — count a rising edge after debounce. */
void waveshareProcessChannel(uint8_t current_level, uint8_t* prev_level,
                             uint8_t* debounce_cnt, int8_t step) {
  if (current_level == 0) {
    if (current_level != *prev_level) {
      *debounce_cnt = 0;
    } else {
      (*debounce_cnt)++;
    }
  } else {
    if (current_level != *prev_level &&
        ++(*debounce_cnt) >= kWaveshareEncoderDebounceTicks) {
      *debounce_cnt = 0;
      queueEncoderStep(step);
    } else {
      *debounce_cnt = 0;
    }
  }
  *prev_level = current_level;
}

void waveshareEncoderTimerCb(void* /*arg*/) {
  const uint8_t a =
      digitalRead(static_cast<uint8_t>(config::kKnobPinA)) ? 1 : 0;
  const uint8_t b =
      digitalRead(static_cast<uint8_t>(config::kKnobPinB)) ? 1 : 0;
  waveshareProcessChannel(a, &s_enc_a_level, &s_enc_a_debounce, +1);
  waveshareProcessChannel(b, &s_enc_b_level, &s_enc_b_debounce, -1);
}
#endif

#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
TouchDrvCHSC5816 s_touch;
#endif
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

void IRAM_ATTR onKnobButtonIsr() {
#if FLIGHTSCNR_HAS_KNOB_BUTTON
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
#endif
}

void initKnobButton() {
#if FLIGHTSCNR_HAS_KNOB_BUTTON
  pinMode(config::kKnobKeyPin, INPUT_PULLUP);
  if (s_knob_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kKnobKeyPin)),
                  onKnobButtonIsr, CHANGE);
  s_knob_interrupt_attached = true;
#else
  (void)s_knob_interrupt_attached;
#endif
}

#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
void IRAM_ATTR onEncoderIsr() {
  uint8_t state = 0;
  if (digitalRead(static_cast<uint8_t>(config::kKnobPinA))) {
    state |= 0x02;
  }
  if (digitalRead(static_cast<uint8_t>(config::kKnobPinB))) {
    state |= 0x01;
  }

  portENTER_CRITICAL_ISR(&s_input_mux);
  if (state != s_knob_previous) {
    const int8_t movement = kEncoderQuad[(s_knob_previous << 2) | state];
    s_knob_previous = state;
    if (movement != 0) {
      s_encoder_accum =
          static_cast<int8_t>(s_encoder_accum + movement);
      if (s_encoder_accum >= kEncoderDetentThreshold) {
        s_encoder_step_pending = true;
        if (s_encoder_pending_delta < 20) {
          s_encoder_pending_delta =
              static_cast<int8_t>(s_encoder_pending_delta + 1);
        }
        s_encoder_accum =
            static_cast<int8_t>(s_encoder_accum - kEncoderDetentThreshold);
      } else if (s_encoder_accum <= -kEncoderDetentThreshold) {
        s_encoder_step_pending = true;
        if (s_encoder_pending_delta > -20) {
          s_encoder_pending_delta =
              static_cast<int8_t>(s_encoder_pending_delta - 1);
        }
        s_encoder_accum =
            static_cast<int8_t>(s_encoder_accum + kEncoderDetentThreshold);
      }
    }
  }
  portEXIT_CRITICAL_ISR(&s_input_mux);
}
#endif

void initEncoder() {
  pinMode(config::kKnobPinA, INPUT_PULLUP);
  pinMode(config::kKnobPinB, INPUT_PULLUP);

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
  s_enc_a_level =
      digitalRead(static_cast<uint8_t>(config::kKnobPinA)) ? 1 : 0;
  s_enc_b_level =
      digitalRead(static_cast<uint8_t>(config::kKnobPinB)) ? 1 : 0;
  s_enc_a_debounce = 0;
  s_enc_b_debounce = 0;

  // Match Waveshare 04_Encoder_Test: 3ms software debounce poll, no GPIO ISR.
  const esp_timer_create_args_t args = {
      .callback = &waveshareEncoderTimerCb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "enc",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &s_enc_timer) == ESP_OK) {
    esp_timer_start_periodic(s_enc_timer, kWaveshareEncoderPollUs);
  }
  Serial.printf("Encoder on GPIO %d/%d (Waveshare 3ms poll)\n",
                static_cast<int>(config::kKnobPinA),
                static_cast<int>(config::kKnobPinB));
#else
  s_knob_previous = 0;
  if (digitalRead(static_cast<uint8_t>(config::kKnobPinA))) {
    s_knob_previous |= 0x02;
  }
  if (digitalRead(static_cast<uint8_t>(config::kKnobPinB))) {
    s_knob_previous |= 0x01;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kKnobPinA)),
                  onEncoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kKnobPinB)),
                  onEncoderIsr, CHANGE);
  Serial.printf("Encoder on GPIO %d/%d (ISR)\n",
                static_cast<int>(config::kKnobPinA),
                static_cast<int>(config::kKnobPinB));
#endif
}

void pollEncoder() {
  // Waveshare: esp_timer. T-Encoder Pro: GPIO ISR.
}

void onCst816Interrupt() {
  if (s_cst816 != nullptr) {
    s_cst816->IIC_Interrupt_Flag = true;
  }
}

#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
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
#endif

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
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
  // Keep the controller awake; default auto-sleep makes polling look dead.
  s_cst816_bus->IIC_WriteC8D8(CST816_SLAVE_ADDRESS, CST816x_WR_DEVICE_AUTO_SLEEP_MODE,
                              0x00);
#endif
  s_touch_ready = true;
  Serial.println("CST816 touch ready");
}

void initTouch() {
  if (hardware::panelUsesCst816()) {
    initTouchCst816();
  } else {
#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
    initTouchChsc5816();
#endif
  }
}

void mapTouchToDisplay(int16_t* x, int16_t* y) {
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
  // Keep touch in the same orientation as FLIGHTSCNR_DISPLAY_ROTATION.
  const int16_t raw_x = *x;
  const int16_t raw_y = *y;
  switch (FLIGHTSCNR_DISPLAY_ROTATION & 3) {
    case 1:
      *x = raw_y;
      *y = static_cast<int16_t>(LCD_WIDTH - 1 - raw_x);
      break;
    case 2:
      *x = static_cast<int16_t>(LCD_WIDTH - 1 - raw_x);
      *y = static_cast<int16_t>(LCD_HEIGHT - 1 - raw_y);
      break;
    case 3:
      *x = static_cast<int16_t>(LCD_HEIGHT - 1 - raw_y);
      *y = raw_x;
      break;
    default:
      break;
  }
#else
  (void)x;
  (void)y;
#endif
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
  } else {
    // Anything that is not a clear swipe counts as a tap. CST816 coords often
    // drift 30–60px during a press, which used to fall in a dead zone.
    queueTap(s_touch_start_x, s_touch_start_y);
  }
}

#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
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
    mapTouchToDisplay(&s_touch_start_x, &s_touch_start_y);
    mapTouchToDisplay(&s_touch_last_x, &s_touch_last_y);
    s_touch_tracking = true;
    hardware::buzzerClick();
  } else if (down && s_touch_tracking) {
    s_touch_last_x = x[0];
    s_touch_last_y = y[0];
    mapTouchToDisplay(&s_touch_last_x, &s_touch_last_y);
  } else if (!down && s_touch_was_down && s_touch_tracking) {
    finishTouchGesture();
    s_touch_tracking = false;
  }

  s_touch_was_down = down;
}
#endif

void pollTouchCst816() {
  if (s_cst816 == nullptr || s_cst816_bus == nullptr) {
    return;
  }

  // Repeated-start burst read (Waveshare demos). A STOP between address write
  // and data read yields stale/jumping coordinates on this CST816.
  uint8_t data[7] = {};
  Wire.beginTransmission(CST816_SLAVE_ADDRESS);
  Wire.write(static_cast<uint8_t>(0x00));
  if (Wire.endTransmission(false) != 0) {
    return;
  }
  const size_t got =
      Wire.requestFrom(static_cast<uint8_t>(CST816_SLAVE_ADDRESS),
                       static_cast<uint8_t>(sizeof(data)));
  if (got < sizeof(data)) {
    return;
  }
  for (size_t i = 0; i < sizeof(data); ++i) {
    data[i] = static_cast<uint8_t>(Wire.read());
  }

  const uint8_t fingers = data[2];
  const bool down = fingers > 0 && fingers < 0x80;
  int16_t x = static_cast<int16_t>(((data[3] & 0x0F) << 8) | data[4]);
  int16_t y = static_cast<int16_t>(((data[5] & 0x0F) << 8) | data[6]);

  // Do not use GestureID 0x05 (Single Click): the register stays latched and
  // would re-fire a tap on every poll until the next touch clears it.

  if (down && !s_touch_was_down) {
    s_touch_start_x = x;
    s_touch_start_y = y;
    s_touch_last_x = x;
    s_touch_last_y = y;
    mapTouchToDisplay(&s_touch_start_x, &s_touch_start_y);
    mapTouchToDisplay(&s_touch_last_x, &s_touch_last_y);
    s_touch_tracking = true;
    hardware::buzzerClick();
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
    Serial.printf("[touch] down raw=%d,%d map=%d,%d fingers=%u\n", x, y,
                  s_touch_start_x, s_touch_start_y, fingers);
#endif
  } else if (down && s_touch_tracking) {
    s_touch_last_x = x;
    s_touch_last_y = y;
    mapTouchToDisplay(&s_touch_last_x, &s_touch_last_y);
  } else if (!down && s_touch_was_down && s_touch_tracking) {
    finishTouchGesture();
    s_touch_tracking = false;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
    Serial.printf("[touch] up map=%d,%d\n", s_touch_last_x, s_touch_last_y);
#endif
  }

  s_cst816->IIC_Interrupt_Flag = false;
  s_touch_was_down = down;
}

void pollTouch() {
  if (!s_touch_ready) {
    return;
  }

  if (hardware::panelUsesCst816()) {
    pollTouchCst816();
  } else {
#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
    pollTouchChsc5816();
#endif
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
  int8_t delta = 0;
  if (s_encoder_pending_delta > 0) {
    delta = 1;
    s_encoder_pending_delta =
        static_cast<int8_t>(s_encoder_pending_delta - 1);
  } else if (s_encoder_pending_delta < 0) {
    delta = -1;
    s_encoder_pending_delta =
        static_cast<int8_t>(s_encoder_pending_delta + 1);
  }
  s_encoder_step_pending = (s_encoder_pending_delta != 0);
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
#if FLIGHTSCNR_HAS_KNOB_BUTTON
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
#else
  // Waveshare knob has no push button; Wi-Fi wipe remains available via the web UI.
  (void)s_long_press_handled;
  (void)s_wifi_reset_ui_shown;
  (void)s_wifi_reset_ui_cancelled_pending;
#endif
}

bool inputConsumeWifiResetUiCancelled() {
  const bool pending = s_wifi_reset_ui_cancelled_pending;
  s_wifi_reset_ui_cancelled_pending = false;
  return pending;
}
