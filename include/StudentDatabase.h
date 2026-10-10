#pragma once
#include <Arduino.h>
#include <Preferences.h>

// A single enrolled student and their attendance state for today's session.
struct Student {
  // NOTE: this UID doubles as the NVS (flash) storage key for the
  // student's attendance flag. ESP32 NVS keys are capped at 15
  // characters, which enroll() enforces (see MAX_UID_LENGTH).
  String uid;     // RFID card UID, uppercase hex, no separators (e.g. "01020304")
  String name;
  bool logged;    // true once they've been marked present today
};

// Result of looking a card up against the roster.
enum class LookupResult {
  UNKNOWN_CARD,     // UID not in the roster
  ALREADY_LOGGED,   // valid student, already marked present
  NEW_ENTRY         // valid student, first tap today
};

// "Database" of enrolled students, backed by the ESP32's NVS flash (via
// the Preferences library) so attendance survives a reset or power loss
// mid-class — not just an in-memory RAM array.
//
// Two independent things are persisted, in two separate NVS namespaces:
//   "attendance" - who has already tapped in today, plus "_day" (which
//                  day those flags belong to). Wiped by RESET, the boot
//                  hold of the reset button, and the day rollover.
//   "roster"     - students enrolled at runtime (ENROLL / ADD). Stored
//                  as numbered slots
//                  u<n>/m<n> (uid/name) plus "cnt"; a removed student's
//                  slot is simply erased. NOT touched by RESET.
//
// Present-flags saved in flash are only trusted once the clock confirms
// they are from TODAY: the ESP32 has no battery-backed clock, so straight
// after power-on it cannot tell yesterday's flags from today's. Until
// rolloverIfNewDay() is first called, only taps made since power-on count
// as "present" - so a stale flag can never turn a real tap away.
//
// The roster array itself lives in main.cpp (sized MAX_STUDENTS). The
// constructor can take pre-filled "built-in" entries; main.cpp passes none.
class StudentDatabase {
 public:
  // `roster` must point to an array of at least `capacity` Students.
  // The first `initialCount` entries are assumed already filled in
  // (built into main.cpp); the rest are free slots that enroll() and
  // begin() (restoring saved enrollments) can grow into.
  StudentDatabase(Student* roster, int initialCount, int capacity);

  // Restores enrolled students from flash. Saved present-flags stay in
  // flash until the day is confirmed (see the class comment). Call once
  // in setup().
  void begin();

  // Wipes all saved ATTENDANCE (flash + in-memory) for a new session.
  // Does not touch enrolled students - see the class comment above.
  void resetAttendance();

  // Call with today's yyyymmdd whenever the clock is valid (see
  // TimeUtil.h) - every second is fine, it only touches flash when
  // something changes. The first call after power-on settles what the
  // saved flags mean: same day -> they are restored; another or unknown
  // day -> they are discarded (taps made since power-on are kept). Later
  // calls clear attendance when the date changes (midnight). Returns true
  // if any attendance was cleared. Matches the backend, which
  // de-duplicates per calendar day.
  bool rolloverIfNewDay(int todayKey);

  // False from power-on until the first rolloverIfNewDay() call.
  bool dayConfirmed() const { return _dayConfirmed; }

  // Looks up `uid`. Returns UNKNOWN_CARD / ALREADY_LOGGED / NEW_ENTRY.
  // On ALREADY_LOGGED or NEW_ENTRY, `outIndex` is set to the matching
  // roster index so the caller can read the student's name. On
  // UNKNOWN_CARD, outIndex is -1 (no match exists).
  LookupResult lookup(const String& uid, int& outIndex) const;

  // Marks the student at `index` as logged (present) for this session,
  // both in RAM and persisted to flash.
  void markLogged(int index);

  // Adds a brand-new student and persists them so they survive a reboot.
  // The UID is upper-cased and may contain ':' or '-' separators
  // ("3f:2a:9b:10" -> "3F2A9B10"); it must then be 8 or 14 hex digits (a
  // 4- or 7-byte card). Fails (returns false, fills outError) if the
  // roster is full, the UID is malformed or already enrolled, or the
  // name is blank.
  bool enroll(const String& uid, const String& name, String& outError);

  // Un-enrolls a student added with enroll() (built-in students come back
  // from main.cpp on every boot, so they can't be removed here). Fills
  // outName with who was removed, or outError.
  bool remove(const String& uid, String& outName, String& outError);

  const Student& at(int index) const;
  int count() const;
  int capacity() const;
  bool isFull() const;

  // How many of the current roster are marked present right now — used
  // by the reset button's short-press "summary" screen.
  int countPresent() const;

 private:
  void saveDay(int dayKey);

  Student* _roster;
  int _count;
  int _initialCount;  // how many of _roster were built in, not enrolled
  int _capacity;
  int _rosterSlots = 0;  // "cnt": roster slots used in flash (incl. removed ones)
  int _savedDay = 0;     // "_day": which day the flags in flash belong to (0 = unknown)
  bool _dayConfirmed = false;
  Preferences _attendancePrefs;
  Preferences _rosterPrefs;
};
