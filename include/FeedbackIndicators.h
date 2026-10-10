#pragma once

// Drives the three status LEDs and the piezo buzzer. Keeping this in
// one place means main.cpp never touches digitalWrite()/tone() directly,
// which makes the pin wiring easy to change in one file (config.h).
//
// NON-BLOCKING: success()/error() used to hold the LED on with delay(),
// which froze the whole loop() for 500-600ms every single tap - during
// that window the RFID reader couldn't be polled and Serial commands
// couldn't be read. Now success()/error() just turn the LED on and
// remember *when*; update() (called every loop() iteration) turns it
// back off once the hold time has passed. No delay() anywhere in here.
class FeedbackIndicators {
 public:
  void begin();

  void success();          // starts a green LED + beep (non-blocking)
  void error();             // starts a red LED + beep (non-blocking)
  void wifiConnected();     // short confirmation beep, no LED
  void networkBusy(bool on);  // blue LED on/off while Wi-Fi/HTTP is in flight

  // Lights each LED in turn and sounds both tones, so a freshly wired
  // build can be checked at a glance. BLOCKS for ~2s - this is the one
  // deliberate delay() in the project, only ever run by the SELFTEST
  // Serial command, never during normal scanning.
  void selfTest();

  // Call once per loop() iteration. Turns off whichever LED success()/
  // error() lit up, once its hold time has elapsed. Cheap - just a
  // millis() comparison when idle.
  void update();

 private:
  enum class HoldState { IDLE, SUCCESS_HOLD, ERROR_HOLD };

  HoldState _state = HoldState::IDLE;
  unsigned long _holdStartMs = 0;
};
