#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>

// Thin wrapper around the SSD1306 OLED. Every screen in this project is
// a 4-line layout: a title, an underline, and three content lines — so
// we expose exactly that instead of raw Adafruit_GFX calls everywhere.
class DisplayUI {
 public:
  DisplayUI();

  // Initializes I2C and the display, trying OLED_I2C_ADDRESS then
  // OLED_I2C_ADDRESS_ALT. Returns false if nothing answers on the bus
  // (missing/miswired OLED). The caller carries on without a screen
  // rather than bricking the whole reader over a display fault.
  bool begin();

  void showMessage(const String& title, const String& line1,
                    const String& line2, const String& line3);

  // Idle/home screen. Line 3 shows a problem if there is one: a dead RFID
  // reader first, otherwise how many taps are still waiting to upload.
  void showReady(bool wifiConnected, int pendingUploads, bool readerOk);

  // False if begin() failed. Drawing calls are then silently skipped.
  bool available() const { return _ok; }

  // The I2C address the OLED answered on (0 if none).
  uint8_t address() const { return _address; }

 private:
  Adafruit_SSD1306 _display;
  bool _ok = false;
  uint8_t _address = 0;
};
