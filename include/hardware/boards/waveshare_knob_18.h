#pragma once

/**
 * Waveshare ESP32-S3-Knob-Touch-LCD-1.8 — 360×360 IPS QSPI (ST77916 + CST816).
 * https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8
 *
 * Note: the rotary encoder has no push button (GPIO 0 enables the PCM5100A DAC).
 * Feedback uses the DRV2605 haptic driver (Waveshare demo: ERM lib 1) when
 * available; there is no piezo buzzer.
 */

#define FLIGHTSCNR_BOARD_NAME "waveshare-knob-1.8"
#define FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18 1

#define FLIGHTSCNR_HAS_KNOB_BUTTON 0
#define FLIGHTSCNR_HAS_BUZZER 0
#define FLIGHTSCNR_HAS_HAPTIC 1
#define FLIGHTSCNR_HAS_LCD_VCI_EN 0
#define FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM 1
#define FLIGHTSCNR_PANEL_AUTODETECT 0

#ifndef CST816_SLAVE_ADDRESS
#define CST816_SLAVE_ADDRESS 0x15
#endif

#define IIC_SDA 11
#define IIC_SCL 12

#define TOUCH_INT 9
#define TOUCH_RST 10

#define LCD_SDIO0 15
#define LCD_SDIO1 16
#define LCD_SDIO2 17
#define LCD_SDIO3 18
#define LCD_SCLK 13
#define LCD_CS 14
#define LCD_RST 21
#define LCD_WIDTH 360
#define LCD_HEIGHT 360
#define LCD_BL 47

/** Arduino_GFX / ST77916 rotation (0–3). Flip relative to USB/cable as needed. */
#define FLIGHTSCNR_DISPLAY_ROTATION 0

#define KNOB_DATA_A 8
#define KNOB_DATA_B 7
