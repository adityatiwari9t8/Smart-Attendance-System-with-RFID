#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>

#include "config.h"

// One attendance record waiting to be uploaded to the sheet.
struct OutboxEntry {
  char uid[MAX_UID_LENGTH + 1];
  uint32_t ts;  // unix time of the tap, or 0 if the clock wasn't set yet
};

// Persistent FIFO of attendance records not yet confirmed by the backend
// (the "outbox" pattern). A tap is appended here the instant it happens,
// and a record leaves only after the Apps Script confirms it. That is
// what makes the system tolerate Wi-Fi drops, a down backend, or power
// loss mid-upload: nothing is ever "synced or lost".
//
// The whole queue is stored as one NVS blob, rewritten on every change
// (at most OUTBOX_CAPACITY * 20 bytes - tiny, and taps are rare).
// When full, the OLDEST entry is dropped so memory stays bounded.
//
// Only ever touched from the main loop task - the upload task never sees
// this object, it just gets a copy of the front entry.
class Outbox {
 public:
  void begin();  // load the saved queue from flash

  // Returns false if the queue was full and the oldest entry had to be
  // discarded to make room (the new entry is still stored).
  bool push(const String& uid, uint32_t ts);

  bool empty() const { return _count == 0; }
  int size() const { return _count; }
  const OutboxEntry& front() const { return _items[0]; }

  // Removes the front entry only if it is still `uid`/`ts` - guards
  // against removing the wrong record if the front changed while an
  // upload for it was in flight.
  bool popIfFront(const char* uid, uint32_t ts);

 private:
  void save();

  OutboxEntry _items[OUTBOX_CAPACITY];
  int _count = 0;
  Preferences _prefs;
};
