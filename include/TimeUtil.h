#pragma once
// Tiny wall-clock helpers. Header-only and Arduino-free so they can be
// unit tested on the host.
#include <stdint.h>
#include <time.h>

// Anything before this (Nov 2023) means the clock was never set - the
// ESP32 boots at 1970 until NTP answers.
#define MIN_VALID_EPOCH 1700000000UL

inline bool isClockValid(time_t t) {
  return t >= (time_t)MIN_VALID_EPOCH;
}

// Local calendar day as yyyymmdd (e.g. 20261001), used to notice that the
// day rolled over. Uses the process timezone (configTzTime() on the ESP32).
inline int dayKey(time_t t) {
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  return (tmLocal.tm_year + 1900) * 10000 + (tmLocal.tm_mon + 1) * 100 + tmLocal.tm_mday;
}
