#include "hardware/display.h"

#include "hardware/display_brightness.h"
#include "hardware/display_font.h"
#include "hardware/panel.h"
#include "hardware/pin_config.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
#include "display/Arduino_ST77916.h"
#include "boards/waveshare_st77916_init.h"
#else
#include "display/Arduino_CO5300.h"
#include "display/Arduino_SH8601.h"
#endif

namespace {

Arduino_DataBus* s_bus = nullptr;
Arduino_GFX* s_panel = nullptr;

}  // namespace

PlaneGfx tft;

void displayInit() {
#if FLIGHTSCNR_HAS_LCD_VCI_EN
  pinMode(LCD_VCI_EN, OUTPUT);
  digitalWrite(LCD_VCI_EN, HIGH);
#endif

#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
#endif

  s_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2,
                                  LCD_SDIO3);

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
  // Knob 1.8 needs st77916_150-class init. Rotation comes from the board header
  // (USB/cable orientation); touch coordinates are remapped to match.
  s_panel =
      new Arduino_ST77916(s_bus, LCD_RST, FLIGHTSCNR_DISPLAY_ROTATION, true, LCD_WIDTH,
                          LCD_HEIGHT, 0, 0, 0, 0, kWaveshareSt77916Init,
                          sizeof(kWaveshareSt77916Init));
  Arduino_TFT::setPixelAlign2(false);
  Serial.printf("Display: ST77916 (150 init, rot=%u)\n",
                static_cast<unsigned>(FLIGHTSCNR_DISPLAY_ROTATION));
#else
  if (hardware::panelUsesCo5300()) {
    s_panel = new Arduino_CO5300(s_bus, LCD_RST, 0, false, LCD_WIDTH, LCD_HEIGHT,
                                 0, 0, 0, 0);
    Arduino_TFT::setPixelAlign2(true);
    Serial.println("Display: CO5300");
  } else {
    s_panel = new Arduino_SH8601(s_bus, LCD_RST, 0, false, LCD_WIDTH, LCD_HEIGHT);
    Arduino_TFT::setPixelAlign2(false);
    Serial.println("Display: SH8601");
  }
#endif

  if (!s_panel->begin(40000000)) {
    Serial.println("Display init failed");
  }

  tft.attach(s_panel, true);
  planeGfxPanelLockInit();
  tft.fillScreen(BLACK);

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
  // ST77916 brightness is PWM backlight (see display_brightness).
#else
  for (uint8_t brightness = 0; brightness < 255; ++brightness) {
    s_panel->Display_Brightness(brightness);
    delay(2);
  }

  if (hardware::panelUsesCo5300()) {
    s_panel->SetContrast(CO5300_ContrastOff);
  } else {
    s_panel->SetContrast(SH8601_ContrastOff);
  }
#endif

  hardware::displayBrightnessBootLoad();
  hardware::displayApplyBrightness();

  tft.setTextWrap(false);
}

void displaySleep() {
  if (s_panel != nullptr) {
    s_panel->displayOff();
  }
#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  digitalWrite(LCD_BL, LOW);
#endif
}

void displayWake() {
#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  hardware::displayApplyBrightness();
#endif
  if (s_panel != nullptr) {
    s_panel->displayOn();
  }
}
