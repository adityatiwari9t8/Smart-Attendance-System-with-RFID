#include "StudentDatabase.h"
#include "config.h"  // for MAX_UID_LENGTH

namespace {

// NVS key of roster slot `slot`: 'u' -> "u3" (uid), 'm' -> "m3" (name).
String slotKey(char kind, int slot) {
  return String(kind) + String(slot);
}

// Upper-cases `in` and drops ':' / '-' separators, so a UID copied from
// another tool ("3f:2a:9b:10") matches what the reader produces.
String normalizeUid(const String& in) {
  String out;
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ':' || c == '-' || c == ' ' || c == '\t') continue;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    out += c;
  }
  return out;
}

bool isHexString(const String& s) {
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

}  // namespace

StudentDatabase::StudentDatabase(Student* roster, int initialCount, int capacity)
    : _roster(roster),
      _count(initialCount),
      _initialCount(initialCount),
      _capacity(capacity) {}

void StudentDatabase::begin() {
  // --- Restore enrolled students -------------------------------------
  _rosterPrefs.begin("roster", false);
  _rosterSlots = _rosterPrefs.getInt("cnt", 0);
  for (int i = 0; i < _rosterSlots && _count < _capacity; i++) {
    String uid = _rosterPrefs.getString(slotKey('u', i).c_str(), "");
    String name = _rosterPrefs.getString(slotKey('m', i).c_str(), "");
    if (uid.length() == 0 || name.length() == 0) continue;  // removed (or corrupt) slot
    int existing;
    if (lookup(uid, existing) != LookupResult::UNKNOWN_CARD) continue;  // already a built-in student
    _roster[_count] = {uid, name, false};
    _count++;
  }

  // --- Attendance ----------------------------------------------------
  // The present-flags stay in flash for now: whether they still apply is
  // only known once the clock says what today is (rolloverIfNewDay()).
  _attendancePrefs.begin("attendance", false);
  _savedDay = _attendancePrefs.getInt("_day", 0);
  _dayConfirmed = false;
}

void StudentDatabase::saveDay(int dayKey) {
  _savedDay = dayKey;
  _attendancePrefs.putInt("_day", dayKey);
}

void StudentDatabase::resetAttendance() {
  _attendancePrefs.clear();  // wipes every key in the "attendance" namespace...
  if (_savedDay != 0) _attendancePrefs.putInt("_day", _savedDay);  // ...but it's still the same day
  for (int i = 0; i < _count; i++) {
    _roster[i].logged = false;
  }
}

bool StudentDatabase::rolloverIfNewDay(int todayKey) {
  if (_dayConfirmed) {
    if (_savedDay == todayKey) return false;
    // Midnight passed while running.
    resetAttendance();
    saveDay(todayKey);
    return true;
  }

  // First valid clock reading since power-on.
  _dayConfirmed = true;
  if (_savedDay == todayKey) {
    // The saved flags are today's (e.g. a reboot mid-class): bring them back.
    for (int i = 0; i < _count; i++) {
      if (_attendancePrefs.getBool(_roster[i].uid.c_str(), false)) _roster[i].logged = true;
    }
    return false;
  }

  // The saved flags are from an earlier day - or from an unknown one ("_day"
  // was never written because the clock was never set). Discard them, but
  // keep anyone who tapped since power-on: that was today.
  bool clearedAny = false;
  for (int i = 0; i < _count; i++) {
    if (!_roster[i].logged && _attendancePrefs.getBool(_roster[i].uid.c_str(), false)) {
      clearedAny = true;
    }
  }
  _attendancePrefs.clear();
  for (int i = 0; i < _count; i++) {
    if (_roster[i].logged) _attendancePrefs.putBool(_roster[i].uid.c_str(), true);
  }
  saveDay(todayKey);
  return clearedAny;
}

LookupResult StudentDatabase::lookup(const String& uid, int& outIndex) const {
  for (int i = 0; i < _count; i++) {
    if (_roster[i].uid == uid) {
      outIndex = i;  // valid either way — caller needs the name for both
      return _roster[i].logged ? LookupResult::ALREADY_LOGGED
                                : LookupResult::NEW_ENTRY;
    }
  }
  outIndex = -1;
  return LookupResult::UNKNOWN_CARD;
}

void StudentDatabase::markLogged(int index) {
  if (index >= 0 && index < _count) {
    _roster[index].logged = true;
    // Write through to flash immediately using the UID as the key.
    _attendancePrefs.putBool(_roster[index].uid.c_str(), true);
  }
}

bool StudentDatabase::enroll(const String& uidIn, const String& nameIn, String& outError) {
  String uid = normalizeUid(uidIn);
  String name = nameIn;
  name.trim();

  if (uid.length() == 0) {
    outError = "UID cannot be blank";
    return false;
  }
  if (uid.length() > MAX_UID_LENGTH) {
    // The UID is used directly as an NVS key elsewhere (see markLogged()),
    // and ESP32 NVS keys are capped at 15 characters. A longer UID here
    // would silently fail to persist later instead of failing loudly now.
    outError = "UID too long (max " + String(MAX_UID_LENGTH) + " chars)";
    return false;
  }
  if (!isHexString(uid) || (uid.length() != 8 && uid.length() != 14)) {
    // Catches "ADD 3F 2A 9B 10 Name" (which would otherwise enroll "3F")
    // and typos like an O for a 0.
    outError = "UID must be 8 or 14 hex digits";
    return false;
  }
  if (name.length() == 0) {
    outError = "Name cannot be blank";
    return false;
  }
  if (isFull()) {
    outError = "Roster full (" + String(_capacity) + " max)";
    return false;
  }
  int existing;
  if (lookup(uid, existing) != LookupResult::UNKNOWN_CARD) {
    outError = "UID already enrolled as " + _roster[existing].name;
    return false;
  }

  // Always a fresh slot at the end - never one derived from the roster
  // index, which would overwrite a saved student whenever an earlier slot
  // was skipped on restore. "cnt" is written last, so a power cut halfway
  // leaves an unused slot rather than a half-written student.
  int slot = _rosterSlots;
  _rosterPrefs.putString(slotKey('u', slot).c_str(), uid);
  _rosterPrefs.putString(slotKey('m', slot).c_str(), name);
  _rosterPrefs.putInt("cnt", slot + 1);
  _rosterSlots = slot + 1;

  _roster[_count] = {uid, name, false};
  _count++;
  return true;
}

bool StudentDatabase::remove(const String& uidIn, String& outName, String& outError) {
  String uid = normalizeUid(uidIn);
  int index;
  if (lookup(uid, index) == LookupResult::UNKNOWN_CARD) {
    outError = "No student with UID " + uid;
    return false;
  }
  if (index < _initialCount) {
    outError = "Built-in student (edit main.cpp)";
    return false;
  }

  // Erase the uid first: from that moment the slot is dead on restore.
  for (int slot = 0; slot < _rosterSlots; slot++) {
    if (_rosterPrefs.getString(slotKey('u', slot).c_str(), "") == uid) {
      _rosterPrefs.remove(slotKey('u', slot).c_str());
      _rosterPrefs.remove(slotKey('m', slot).c_str());
    }
  }
  _attendancePrefs.remove(uid.c_str());

  outName = _roster[index].name;
  for (int i = index; i < _count - 1; i++) _roster[i] = _roster[i + 1];
  _count--;
  _roster[_count] = Student();

  // Nobody enrolled any more: start the slot numbering over.
  if (_count == _initialCount) {
    _rosterPrefs.clear();
    _rosterSlots = 0;
  }
  return true;
}

const Student& StudentDatabase::at(int index) const {
  return _roster[index];
}

int StudentDatabase::count() const {
  return _count;
}

int StudentDatabase::capacity() const {
  return _capacity;
}

bool StudentDatabase::isFull() const {
  return _count >= _capacity;
}

int StudentDatabase::countPresent() const {
  int present = 0;
  for (int i = 0; i < _count; i++) {
    if (_roster[i].logged) present++;
  }
  return present;
}
