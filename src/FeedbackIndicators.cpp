#include "FeedbackIndicators.h"
#include <Arduino.h>
#include "config.h"

void FeedbackIndicators::begin() {
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
}

void FeedbackIndicators::success() {
  digitalWrite(PIN_LED_GREEN, HIGH);
  // tone(..., duration) is already non-blocking on the ESP32 Arduino
  // core - it schedules the buzzer to stop itself via a timer, it
  // doesn't sleep here. Only the LED hold needed the delay() removed.
  tone(PIN_BUZZER, TONE_SUCCESS_HZ, TONE_SUCCESS_MS);
  _state = HoldState::SUCCESS_HOLD;
  _holdStartMs = millis();
}

void FeedbackIndicators::error() {
  digitalWrite(PIN_LED_RED, HIGH);
  tone(PIN_BUZZER, TONE_ERROR_HZ, TONE_ERROR_MS);
  _state = HoldState::ERROR_HOLD;
  _holdStartMs = millis();
}

void FeedbackIndicators::wifiConnected() {
  tone(PIN_BUZZER, TONE_WIFI_OK_HZ, TONE_WIFI_OK_MS);
}

void FeedbackIndicators::networkBusy(bool on) {
  digitalWrite(PIN_LED_BLUE, on ? HIGH : LOW);
}

void FeedbackIndicators::selfTest() {
  const int leds[] = {PIN_LED_GREEN, PIN_LED_RED, PIN_LED_BLUE};
  for (int pin : leds) {
    digitalWrite(pin, HIGH);
    delay(350);
    digitalWrite(pin, LOW);
  }
  tone(PIN_BUZZER, TONE_SUCCESS_HZ, 200);
  delay(300);
  tone(PIN_BUZZER, TONE_ERROR_HZ, 300);
  delay(400);
  _state = HoldState::IDLE;
}

void FeedbackIndicators::update() {
  if (_state == HoldState::IDLE) return;

  unsigned long holdMs =
      (_state == HoldState::SUCCESS_HOLD) ? SUCCESS_BEEP_HOLD_MS : ERROR_BEEP_HOLD_MS;

  if (millis() - _holdStartMs >= holdMs) {
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, LOW);
    _state = HoldState::IDLE;
  }
}
