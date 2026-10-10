#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "BackendResponse.h"

// Result of one background upload, delivered back to loop() through a
// FreeRTOS queue once the HTTPS request finishes.
//
// IMPORTANT: this struct is copied byte-for-byte across the queue
// (xQueueSend/xQueueReceive do a raw memcpy). It must stay pure POD - no
// Arduino `String` members. A String is really a pointer to a heap buffer
// plus a destructor; memcpy-ing one leaves two Strings sharing a buffer
// that the sender frees right after xQueueSend() returns.
struct SyncOutcome {
  BackendAction action;  // DELIVERED / DROP / RETRY - what main should do with the record
  int httpCode;          // HTTPClient code; negative = connection/TLS failure
  uint32_t ts;           // echoes the record's timestamp (with uid, identifies the record)
  char uid[24];
  char message[96];      // best-effort copy of the backend's reply text
};

// Owns Wi-Fi, the clock, and the HTTPS upload to the Google Apps Script
// backend. main.cpp never touches WiFi/HTTPClient directly.
//
// Uploads run on their OWN FreeRTOS task pinned to core 0, so a slow or
// hung network call never stalls loop() (RFID polling on core 1). Only
// ONE upload is ever in flight: two concurrent TLS sessions would need
// roughly double the heap, and the outbox already serializes the work.
class AttendanceLogger {
 public:
  // Joins Wi-Fi, giving up after WIFI_CONNECT_TIMEOUT_MS (boot only, so
  // blocking is fine). Even on failure the Wi-Fi stack keeps retrying in
  // the background, and maintain() nudges it too.
  bool connect();

  // Live link state (not a boot-time snapshot).
  bool isConnected() const;

  // The network it joins (WIFI_SSID from secrets.h).
  const char* ssid() const;

  // Creates the result queue. Call once in setup().
  void beginAsync();

  // Call every loop(): re-kicks a dropped Wi-Fi link and starts NTP the
  // first time the link is up. Never blocks.
  void maintain();

  // True while an upload task is running OR its result hasn't been
  // collected via pollResult() yet.
  bool busy() const { return _busy; }

  // Starts uploading one record on the background task. Returns false
  // (nothing started) if busy or not connected.
  bool startUpload(const String& uid, const String& name, uint32_t ts);

  // Returns true and fills `out` if an upload finished since the last
  // call. Never blocks.
  bool pollResult(SyncOutcome& out);

 private:
  struct SyncJob {
    String uid;
    String name;
    uint32_t ts;
    AttendanceLogger* self;
  };

  static void syncTaskEntry(void* param);
  SyncOutcome doSync(const String& uid, const String& name, uint32_t ts);

  QueueHandle_t _resultQueue = nullptr;
  volatile bool _busy = false;
  bool _timeStarted = false;
  unsigned long _lastWifiNudgeMs = 0;
};
