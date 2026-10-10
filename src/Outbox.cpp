#include "Outbox.h"

#include <string.h>

void Outbox::begin() {
  _count = 0;
  _prefs.begin("outbox", false);
  size_t bytes = _prefs.getBytesLength("q");
  if (bytes > 0 && bytes % sizeof(OutboxEntry) == 0 &&
      bytes <= sizeof(_items)) {
    _prefs.getBytes("q", _items, bytes);
    _count = (int)(bytes / sizeof(OutboxEntry));
    // A flash blob from a build with different layout/limits can't be
    // trusted - make sure every uid is terminated.
    for (int i = 0; i < _count; i++) _items[i].uid[MAX_UID_LENGTH] = '\0';
  }
  // Wrong-sized blob (layout changed between firmware versions): start
  // empty rather than interpret garbage as records.
}

void Outbox::save() {
  if (_count == 0) {
    _prefs.remove("q");
  } else {
    _prefs.putBytes("q", _items, (size_t)_count * sizeof(OutboxEntry));
  }
}

bool Outbox::push(const String& uid, uint32_t ts) {
  bool dropped = false;
  if (_count >= OUTBOX_CAPACITY) {
    memmove(&_items[0], &_items[1], (size_t)(OUTBOX_CAPACITY - 1) * sizeof(OutboxEntry));
    _count = OUTBOX_CAPACITY - 1;
    dropped = true;
  }
  OutboxEntry& e = _items[_count++];
  memset(&e, 0, sizeof(e));
  strncpy(e.uid, uid.c_str(), MAX_UID_LENGTH);
  e.ts = ts;
  save();
  return !dropped;
}

bool Outbox::popIfFront(const char* uid, uint32_t ts) {
  if (_count == 0) return false;
  if (strcmp(_items[0].uid, uid) != 0 || _items[0].ts != ts) return false;
  memmove(&_items[0], &_items[1], (size_t)(_count - 1) * sizeof(OutboxEntry));
  _count--;
  save();
  return true;
}
