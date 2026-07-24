// Board power control — Waveshare ESP32-S3-Touch-LCD-3.49 (V2).
//
// WHY THIS EXISTS
// On BATTERY the PWR button only *gates* power for as long as it is physically
// held. The firmware must latch it on by driving SYS_EN (TCA9554 EXIO6) HIGH the
// instant I2C is up; otherwise the board loses power the moment you release PWR —
// it never finishes booting, so the AXS15231B panel just shows uninitialised GRAM
// (full-screen random static) and then goes dark. On USB the board is powered from
// VBUS directly, so the bug is invisible there (which is why USB testing never hit
// it). Reference: Waveshare Examples/Arduino/07_BATT_PWR_Test.
//
// The TCA9554 I/O expander is shared with audio.h (speaker-amp enable on EXIO7),
// so this header owns the single expander handle and both features drive it.
//
// Buttons (both active-LOW, internal pull-up; RESET is a hardware reset line):
//   PWR  = GPIO16 : short press  -> toggle screen sleep (backlight off/on)
//                   hold >=1.5 s -> power off (drop SYS_EN, cut the battery rail)
//   BOOT = GPIO0  : handled in the sketch (short = next screen, hold = Wi-Fi portal)
#pragma once
#include <Arduino.h>
#include "i2c_bsp.h"
#include "src/tca9554/esp_io_expander_tca9554.h"   // shared TCA9554 (also used by audio.h)
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"          // setUpduty() for screen sleep

#define PWR_BTN_GPIO     16                       // PWR button (active LOW)
#define PWR_HOLD_EXIO    IO_EXPANDER_PIN_NUM_6     // SYS_EN — HIGH latches battery power on
#define PWR_AMP_EXIO     IO_EXPANDER_PIN_NUM_7     // speaker-amp enable (owned by audio.h)
#define PWR_OFF_HOLD_MS  1500                      // hold PWR at least this long -> power off

// Single shared TCA9554 handle (created lazily; also used by audio.h's amp enable).
static esp_io_expander_handle_t g_io_expander = NULL;

// Lazily create the shared TCA9554 on I2C bus 0. Returns NULL on failure.
static esp_io_expander_handle_t board_expander() {
  if (g_io_expander) return g_io_expander;
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_master_get_bus_handle(0, &bus) != ESP_OK) return NULL;
  if (esp_io_expander_new_i2c_tca9554(bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                      &g_io_expander) != ESP_OK)
    g_io_expander = NULL;
  return g_io_expander;
}

// Latch the battery power rail ON and arm the PWR button. Call as the FIRST thing
// after i2c_master_Init() in setup(), before the slow display/Wi-Fi bring-up, so a
// brief PWR press is enough to boot. No-op-safe on USB. Returns true on success.
static bool power_begin() {
  esp_io_expander_handle_t io = board_expander();
  if (io) {
    esp_io_expander_set_dir(io, PWR_HOLD_EXIO, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, PWR_HOLD_EXIO, 1);   // SYS_EN high == hold power on
  }
  pinMode(PWR_BTN_GPIO, INPUT_PULLUP);
  return io != NULL;
}

// Cut the battery power rail (SYS_EN low). On battery the board dies immediately;
// on USB this is a no-op (VBUS keeps it alive).
static void power_off() {
  esp_io_expander_handle_t io = board_expander();
  if (io) esp_io_expander_set_level(io, PWR_HOLD_EXIO, 0);
}

// Toggle the display backlight (PWR short press). Backlight is active-low, so
// LCD_PWM_MODE_0 == off and LCD_PWM_MODE_255 == full (matches setup()).
static bool g_screen_asleep = false;
static void power_screen_toggle() {
  g_screen_asleep = !g_screen_asleep;
  setUpduty(g_screen_asleep ? LCD_PWM_MODE_0 : LCD_PWM_MODE_255);
}

// Poll the PWR button once per loop(). A short press toggles screen sleep; holding
// >=PWR_OFF_HOLD_MS powers the board off. The `released_once` guard ignores the
// press that turned the board ON (held through boot) so it can't instantly sleep or
// power off the device — only presses made after that first release count.
static void power_poll_pwr() {
  static bool     released_once = false;
  static bool     acted         = false;   // long-press already fired this hold
  static uint32_t down_ms       = 0;
  bool pressed = (digitalRead(PWR_BTN_GPIO) == LOW);
  if (pressed) {
    if (!released_once) return;                        // still the turn-on hold
    if (down_ms == 0) { down_ms = millis(); acted = false; }
    else if (!acted && millis() - down_ms >= PWR_OFF_HOLD_MS) { acted = true; power_off(); }
  } else {
    if (released_once && down_ms != 0 && !acted) power_screen_toggle();   // was a short press
    released_once = true;
    down_ms = 0;
    acted   = false;
  }
}
