// Host-side unit tests: `pio test -e native`
//
// Covers the logic that has no hardware dependency - roster/attendance
// persistence, the upload outbox, backend reply classification and the
// day-rollover helpers. Arduino's String and the NVS Preferences library
// are replaced by test/mocks; "rebooting" is simulated by building fresh
// objects over the same mock flash.
#include <stdlib.h>
#include <unity.h>

#include "BackendResponse.h"
#include "Outbox.h"
#include "StudentDatabase.h"
#include "TimeUtil.h"

void setUp() { MockNvs::wipe(); }
void tearDown() {}

// ----------------------------------------------------------------------
// BackendResponse
// ----------------------------------------------------------------------
void test_response_success_and_duplicate_are_delivered() {
  TEST_ASSERT_EQUAL(BackendAction::DELIVERED,
                    classifyBackendResponse(200, "Success: Attendance logged for Aisyah"));
  TEST_ASSERT_EQUAL(BackendAction::DELIVERED,
                    classifyBackendResponse(200, "Duplicate: Aisyah already logged today, no new row added"));
  TEST_ASSERT_EQUAL(BackendAction::DELIVERED, classifyBackendResponse(200, "\r\n  Success: x"));
}

// The bug this guards: Apps Script answers HTTP 200 for failures too, and
// the old firmware treated every 2xx as "synced".
void test_response_http200_error_text_is_not_delivered() {
  TEST_ASSERT_EQUAL(BackendAction::RETRY,
                    classifyBackendResponse(200, "Error: Invalid or missing API key"));
  TEST_ASSERT_EQUAL(BackendAction::RETRY, classifyBackendResponse(200, "Error: Server busy, try again"));
  TEST_ASSERT_EQUAL(BackendAction::RETRY,
                    classifyBackendResponse(200, "<!DOCTYPE html><html>Sign in</html>"));
  TEST_ASSERT_EQUAL(BackendAction::RETRY, classifyBackendResponse(200, ""));
  TEST_ASSERT_EQUAL(BackendAction::RETRY, classifyBackendResponse(200, nullptr));
}

void test_response_bad_request_is_dropped_not_retried_forever() {
  TEST_ASSERT_EQUAL(BackendAction::DROP,
                    classifyBackendResponse(200, "Error: Missing uid or name parameter"));
}

void test_response_http_failures_retry() {
  TEST_ASSERT_EQUAL(BackendAction::RETRY, classifyBackendResponse(500, "Success: x"));
  TEST_ASSERT_EQUAL(BackendAction::RETRY, classifyBackendResponse(404, "Not found"));
  TEST_ASSERT_EQUAL(BackendAction::RETRY, classifyBackendResponse(-29312, "Connection failed"));
  TEST_ASSERT_EQUAL(BackendAction::RETRY, classifyBackendResponse(0, "Connection failed"));
}

// ----------------------------------------------------------------------
// TimeUtil
// ----------------------------------------------------------------------
void test_clock_validity() {
  TEST_ASSERT_FALSE(isClockValid(0));
  TEST_ASSERT_FALSE(isClockValid(12345));  // ESP32 boots near 1970 until NTP answers
  TEST_ASSERT_TRUE(isClockValid(1800000000));
}

void test_day_key_follows_timezone() {
  // 2023-11-14 22:13:20 UTC == 2023-11-15 03:43:20 in India (UTC+5:30).
  setenv("TZ", "UTC0", 1);
  tzset();
  TEST_ASSERT_EQUAL(20231114, dayKey(1700000000));
  setenv("TZ", "IST-5:30", 1);
  tzset();
  TEST_ASSERT_EQUAL(20231115, dayKey(1700000000));
}

// ----------------------------------------------------------------------
// Outbox
// ----------------------------------------------------------------------
void test_outbox_is_fifo_and_persists_across_reboot() {
  {
    Outbox ob;
    ob.begin();
    TEST_ASSERT_TRUE(ob.empty());
    ob.push("AAAA0001", 1800000001);
    ob.push("AAAA0002", 1800000002);
    TEST_ASSERT_EQUAL(2, ob.size());
    TEST_ASSERT_EQUAL_STRING("AAAA0001", ob.front().uid);
  }
  Outbox rebooted;  // power cycle: fresh object, same flash
  rebooted.begin();
  TEST_ASSERT_EQUAL(2, rebooted.size());
  TEST_ASSERT_EQUAL_STRING("AAAA0001", rebooted.front().uid);
  TEST_ASSERT_EQUAL_UINT32(1800000001, rebooted.front().ts);

  TEST_ASSERT_TRUE(rebooted.popIfFront("AAAA0001", 1800000001));
  TEST_ASSERT_EQUAL_STRING("AAAA0002", rebooted.front().uid);

  Outbox again;
  again.begin();
  TEST_ASSERT_EQUAL(1, again.size());  // the pop was persisted too
}

void test_outbox_pop_only_removes_the_matching_front() {
  Outbox ob;
  ob.begin();
  ob.push("AAAA0001", 100);
  ob.push("AAAA0002", 200);
  TEST_ASSERT_FALSE(ob.popIfFront("AAAA0002", 200));  // not at the front
  TEST_ASSERT_FALSE(ob.popIfFront("AAAA0001", 999));  // same uid, different tap
  TEST_ASSERT_EQUAL(2, ob.size());
  TEST_ASSERT_TRUE(ob.popIfFront("AAAA0001", 100));
  TEST_ASSERT_TRUE(ob.popIfFront("AAAA0002", 200));
  TEST_ASSERT_TRUE(ob.empty());
  TEST_ASSERT_FALSE(ob.popIfFront("AAAA0002", 200));  // popping an empty queue is safe
}

void test_outbox_full_drops_oldest_and_stays_bounded() {
  Outbox ob;
  ob.begin();
  for (int i = 0; i < OUTBOX_CAPACITY; i++) {
    TEST_ASSERT_TRUE(ob.push(String(i, DEC), (uint32_t)(1000 + i)));
  }
  TEST_ASSERT_EQUAL(OUTBOX_CAPACITY, ob.size());
  TEST_ASSERT_FALSE(ob.push("NEWEST", 5000));  // signals that one was discarded
  TEST_ASSERT_EQUAL(OUTBOX_CAPACITY, ob.size());
  TEST_ASSERT_EQUAL_STRING("1", ob.front().uid);  // "0" was dropped

  Outbox rebooted;
  rebooted.begin();
  TEST_ASSERT_EQUAL(OUTBOX_CAPACITY, rebooted.size());
}

void test_outbox_ignores_a_corrupt_flash_blob() {
  Preferences p;
  p.begin("outbox", false);
  uint8_t junk[7] = {1, 2, 3, 4, 5, 6, 7};  // not a multiple of the entry size
  p.putBytes("q", junk, sizeof(junk));

  Outbox ob;
  ob.begin();
  TEST_ASSERT_TRUE(ob.empty());
  ob.push("AAAA0001", 1);  // and it still works afterwards
  TEST_ASSERT_EQUAL(1, ob.size());
}

// ----------------------------------------------------------------------
// StudentDatabase
// ----------------------------------------------------------------------
const int DAY1 = 20261001;
const int DAY2 = 20261002;

void test_lookup_new_duplicate_unknown() {
  Student roster[5] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 5);
  db.begin();
  int idx = -2;
  TEST_ASSERT_EQUAL(LookupResult::NEW_ENTRY, db.lookup("01020304", idx));
  TEST_ASSERT_EQUAL(0, idx);
  db.markLogged(0);
  TEST_ASSERT_EQUAL(LookupResult::ALREADY_LOGGED, db.lookup("01020304", idx));
  TEST_ASSERT_EQUAL(LookupResult::UNKNOWN_CARD, db.lookup("DEADBEEF", idx));
  TEST_ASSERT_EQUAL(-1, idx);
}

void test_attendance_survives_same_day_reboot_and_reset_clears_it() {
  {
    Student roster[5] = {{"01020304", "Aisyah", false}, {"11223344", "Balqis", false}};
    StudentDatabase db(roster, 2, 5);
    db.begin();
    db.rolloverIfNewDay(DAY1);
    db.markLogged(1);
  }
  Student roster[5] = {{"01020304", "Aisyah", false}, {"11223344", "Balqis", false}};
  StudentDatabase db(roster, 2, 5);
  db.begin();
  TEST_ASSERT_FALSE(db.rolloverIfNewDay(DAY1));  // clock says: still the same day
  TEST_ASSERT_FALSE(db.at(0).logged);
  TEST_ASSERT_TRUE(db.at(1).logged);
  TEST_ASSERT_EQUAL(1, db.countPresent());

  db.resetAttendance();
  TEST_ASSERT_EQUAL(0, db.countPresent());
  Student roster2[5] = {{"01020304", "Aisyah", false}, {"11223344", "Balqis", false}};
  StudentDatabase db2(roster2, 2, 5);
  db2.begin();
  db2.rolloverIfNewDay(DAY1);
  TEST_ASSERT_EQUAL(0, db2.countPresent());  // the wipe reached flash
}

// The bug this guards: yesterday's flags were loaded at power-on and the
// date was only checked 30 s later, so the first students to tap the next
// morning got "ALREADY LOGGED" and their tap was never recorded. With no
// Wi-Fi (no clock) that lasted all day.
void test_stale_flags_never_turn_a_tap_away_before_the_clock_is_set() {
  {
    Student roster[3] = {{"01020304", "Aisyah", false}};
    StudentDatabase db(roster, 1, 3);
    db.begin();
    db.rolloverIfNewDay(DAY1);
    db.markLogged(0);  // present on day 1
  }
  // Next morning, power on, no clock yet.
  Student roster[3] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 3);
  db.begin();
  TEST_ASSERT_FALSE(db.dayConfirmed());
  int idx;
  TEST_ASSERT_EQUAL(LookupResult::NEW_ENTRY, db.lookup("01020304", idx));
}

void test_new_day_discards_old_flags_but_keeps_taps_made_since_power_on() {
  {
    Student roster[3] = {{"01020304", "Aisyah", false}, {"11223344", "Balqis", false}};
    StudentDatabase db(roster, 2, 3);
    db.begin();
    db.rolloverIfNewDay(DAY1);
    db.markLogged(0);
    db.markLogged(1);
  }
  Student roster[3] = {{"01020304", "Aisyah", false}, {"11223344", "Balqis", false}};
  StudentDatabase db(roster, 2, 3);
  db.begin();
  db.markLogged(1);  // Balqis taps on day 2, before NTP answers

  TEST_ASSERT_TRUE(db.rolloverIfNewDay(DAY2));  // Aisyah's day-1 flag was cleared
  TEST_ASSERT_FALSE(db.at(0).logged);
  TEST_ASSERT_TRUE(db.at(1).logged);
  TEST_ASSERT_FALSE(db.rolloverIfNewDay(DAY2));  // only once

  // ...and that is what flash now says too.
  Student roster2[3] = {{"01020304", "Aisyah", false}, {"11223344", "Balqis", false}};
  StudentDatabase db2(roster2, 2, 3);
  db2.begin();
  TEST_ASSERT_FALSE(db2.rolloverIfNewDay(DAY2));
  TEST_ASSERT_FALSE(db2.at(0).logged);
  TEST_ASSERT_TRUE(db2.at(1).logged);
}

// Flags saved while the clock was never set have no known day; they must
// not be trusted either.
void test_flags_from_an_unknown_day_are_discarded() {
  {
    Student roster[3] = {{"01020304", "Aisyah", false}};
    StudentDatabase db(roster, 1, 3);
    db.begin();
    db.markLogged(0);  // offline the whole time: "_day" never written
  }
  Student roster[3] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 3);
  db.begin();
  TEST_ASSERT_TRUE(db.rolloverIfNewDay(DAY2));
  TEST_ASSERT_EQUAL(0, db.countPresent());
}

void test_midnight_while_running_clears_attendance_once() {
  Student roster[3] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 3);
  db.begin();
  TEST_ASSERT_FALSE(db.rolloverIfNewDay(DAY1));  // first ever run: nothing to clear
  db.markLogged(0);
  TEST_ASSERT_FALSE(db.rolloverIfNewDay(DAY1));
  TEST_ASSERT_TRUE(db.at(0).logged);

  TEST_ASSERT_TRUE(db.rolloverIfNewDay(DAY2));
  TEST_ASSERT_FALSE(db.at(0).logged);
  TEST_ASSERT_FALSE(db.rolloverIfNewDay(DAY2));
}

// RESET wipes the flags, not the knowledge of what day it is - otherwise
// taps made after a RESET would look like flags of an unknown day.
void test_reset_keeps_the_day() {
  {
    Student roster[3] = {{"01020304", "Aisyah", false}};
    StudentDatabase db(roster, 1, 3);
    db.begin();
    db.rolloverIfNewDay(DAY1);
    db.resetAttendance();
    db.markLogged(0);
  }
  Student roster[3] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 3);
  db.begin();
  TEST_ASSERT_FALSE(db.rolloverIfNewDay(DAY1));
  TEST_ASSERT_TRUE(db.at(0).logged);
}

void test_enroll_validation() {
  Student roster[3] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 3);
  db.begin();
  String err;
  TEST_ASSERT_FALSE(db.enroll("", "X", err));
  TEST_ASSERT_FALSE(db.enroll("AB12CD34", "   ", err));
  TEST_ASSERT_TRUE(err.startsWith("Name cannot be blank"));
  TEST_ASSERT_FALSE(db.enroll("0123456789ABCDEF", "Too Long", err));  // 16 chars > NVS key limit
  TEST_ASSERT_TRUE(err.startsWith("UID too long"));
  TEST_ASSERT_FALSE(db.enroll("01020304", "Clone", err));  // already enrolled
  TEST_ASSERT_TRUE(err.startsWith("UID already enrolled"));

  TEST_ASSERT_TRUE(db.enroll(" ab12cd34 ", " Priya Sharma ", err));  // trims + upper-cases
  TEST_ASSERT_EQUAL_STRING("AB12CD34", db.at(1).uid.c_str());
  TEST_ASSERT_EQUAL_STRING("Priya Sharma", db.at(1).name.c_str());
  TEST_ASSERT_TRUE(db.enroll("FF00FF00", "Third", err));
  TEST_ASSERT_TRUE(db.isFull());
  TEST_ASSERT_FALSE(db.enroll("11111111", "Overflow", err));
  TEST_ASSERT_TRUE(err.startsWith("Roster full"));
}

// "ADD 3F 2A 9B 10 Priya" used to enroll the UID "3F" (named "2A 9B 10
// Priya") without complaint; that card could then never be matched.
void test_enroll_rejects_malformed_uids_and_accepts_separators() {
  Student roster[5] = {};
  StudentDatabase db(roster, 0, 5);
  db.begin();
  String err;
  TEST_ASSERT_FALSE(db.enroll("3F", "2A 9B 10 Priya", err));
  TEST_ASSERT_TRUE(err.startsWith("UID must be 8 or 14 hex digits"));
  TEST_ASSERT_FALSE(db.enroll("3F2A9B1", "Odd Length", err));
  TEST_ASSERT_FALSE(db.enroll("3F2A9BXY", "Not Hex", err));
  TEST_ASSERT_FALSE(db.enroll("3F2A9B10AB", "Five Bytes", err));
  TEST_ASSERT_EQUAL(0, db.count());

  TEST_ASSERT_TRUE(db.enroll("3f:2a:9b:10", "Priya", err));  // copied from another tool
  TEST_ASSERT_EQUAL_STRING("3F2A9B10", db.at(0).uid.c_str());
  TEST_ASSERT_TRUE(db.enroll("04-A1-B2-C3-D4-E5-F6", "Seven Byte", err));
  TEST_ASSERT_EQUAL_STRING("04A1B2C3D4E5F6", db.at(1).uid.c_str());
}

void test_enrolled_students_and_their_attendance_survive_reboot() {
  {
    Student roster[5] = {{"01020304", "Aisyah", false}};
    StudentDatabase db(roster, 1, 5);
    db.begin();
    db.rolloverIfNewDay(DAY1);
    String err;
    TEST_ASSERT_TRUE(db.enroll("3F2A9B10", "Priya", err));
    db.markLogged(1);
  }
  Student roster[5] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 5);
  db.begin();
  TEST_ASSERT_EQUAL(2, db.count());
  db.rolloverIfNewDay(DAY1);
  int idx;
  TEST_ASSERT_EQUAL(LookupResult::ALREADY_LOGGED, db.lookup("3F2A9B10", idx));
  TEST_ASSERT_EQUAL_STRING("Priya", db.at(idx).name.c_str());
}

// Real cards have 4-, 7- and 10-byte UIDs. A 7-byte UID (14 hex chars) is
// the longest that fits an NVS key; it must persist, not silently vanish.
void test_seven_byte_uid_persists() {
  {
    Student roster[3] = {};
    StudentDatabase db(roster, 0, 3);
    db.begin();
    db.rolloverIfNewDay(DAY1);
    String err;
    TEST_ASSERT_TRUE(db.enroll("04A1B2C3D4E5F6", "NTAG Card", err));
    db.markLogged(0);
  }
  Student roster[3] = {};
  StudentDatabase db(roster, 0, 3);
  db.begin();
  TEST_ASSERT_EQUAL(1, db.count());
  db.rolloverIfNewDay(DAY1);
  TEST_ASSERT_TRUE(db.at(0).logged);
}

void test_restored_enrollment_does_not_duplicate_a_builtin_uid() {
  {
    Student roster[5] = {};
    StudentDatabase db(roster, 0, 5);  // first firmware: no built-ins
    db.begin();
    String err;
    TEST_ASSERT_TRUE(db.enroll("01020304", "Aisyah", err));
  }
  // New firmware now ships 01020304 as a built-in student.
  Student roster[5] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 5);
  db.begin();
  TEST_ASSERT_EQUAL(1, db.count());
}

// The bug this guards: the flash slot for a new student was derived from
// its roster position, so after a saved slot had been skipped on restore,
// the next ENROLL overwrote a DIFFERENT saved student.
void test_enroll_after_a_skipped_slot_keeps_every_saved_student() {
  {
    Student roster[5] = {};
    StudentDatabase db(roster, 0, 5);  // e.g. the real-hardware build
    db.begin();
    String err;
    TEST_ASSERT_TRUE(db.enroll("01020304", "Aisyah", err));  // slot 0
    TEST_ASSERT_TRUE(db.enroll("3F2A9B10", "Priya", err));   // slot 1
  }
  {
    // A build where 01020304 is built in: slot 0 is skipped on restore.
    Student roster[5] = {{"01020304", "Aisyah", false}};
    StudentDatabase db(roster, 1, 5);
    db.begin();
    TEST_ASSERT_EQUAL(2, db.count());
    String err;
    TEST_ASSERT_TRUE(db.enroll("AABBCC11", "Rahul", err));
  }
  Student roster[5] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 5);
  db.begin();
  int idx;
  TEST_ASSERT_EQUAL(3, db.count());
  TEST_ASSERT_NOT_EQUAL(LookupResult::UNKNOWN_CARD, db.lookup("3F2A9B10", idx));  // Priya survived
  TEST_ASSERT_NOT_EQUAL(LookupResult::UNKNOWN_CARD, db.lookup("AABBCC11", idx));
}

void test_remove_unenrolls_for_good() {
  {
    Student roster[5] = {};
    StudentDatabase db(roster, 0, 5);
    db.begin();
    db.rolloverIfNewDay(DAY1);
    String err, name;
    TEST_ASSERT_TRUE(db.enroll("11111111", "Amit", err));
    TEST_ASSERT_TRUE(db.enroll("22222222", "Typo Nmae", err));
    TEST_ASSERT_TRUE(db.enroll("33333333", "Chitra", err));
    db.markLogged(1);

    TEST_ASSERT_FALSE(db.remove("99999999", name, err));
    TEST_ASSERT_TRUE(err.startsWith("No student"));
    TEST_ASSERT_TRUE(db.remove("22222222", name, err));
    TEST_ASSERT_EQUAL_STRING("Typo Nmae", name.c_str());
    TEST_ASSERT_EQUAL(2, db.count());
    TEST_ASSERT_EQUAL_STRING("Chitra", db.at(1).name.c_str());  // later students shift down
    TEST_ASSERT_EQUAL(0, db.countPresent());                    // their flag went with them

    TEST_ASSERT_TRUE(db.enroll("22222222", "Typo Name", err));  // re-enroll, fixed
  }
  Student roster[5] = {};
  StudentDatabase db(roster, 0, 5);
  db.begin();
  db.rolloverIfNewDay(DAY1);
  TEST_ASSERT_EQUAL(3, db.count());
  int idx;
  TEST_ASSERT_EQUAL(LookupResult::NEW_ENTRY, db.lookup("22222222", idx));  // old flag gone
  TEST_ASSERT_EQUAL_STRING("Typo Name", db.at(idx).name.c_str());
}

void test_remove_refuses_builtin_and_frees_slots_when_empty() {
  Student roster[5] = {{"01020304", "Aisyah", false}};
  StudentDatabase db(roster, 1, 5);
  db.begin();
  String err, name;
  TEST_ASSERT_FALSE(db.remove("01020304", name, err));
  TEST_ASSERT_TRUE(err.startsWith("Built-in"));

  TEST_ASSERT_TRUE(db.enroll("11111111", "Amit", err));
  TEST_ASSERT_TRUE(db.remove("11:11:11:11", name, err));  // separators accepted here too
  TEST_ASSERT_EQUAL(1, db.count());

  Preferences p;
  p.begin("roster", false);
  TEST_ASSERT_EQUAL(0, p.getInt("cnt", 0));  // slot numbering restarted
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_response_success_and_duplicate_are_delivered);
  RUN_TEST(test_response_http200_error_text_is_not_delivered);
  RUN_TEST(test_response_bad_request_is_dropped_not_retried_forever);
  RUN_TEST(test_response_http_failures_retry);
  RUN_TEST(test_clock_validity);
  RUN_TEST(test_day_key_follows_timezone);
  RUN_TEST(test_outbox_is_fifo_and_persists_across_reboot);
  RUN_TEST(test_outbox_pop_only_removes_the_matching_front);
  RUN_TEST(test_outbox_full_drops_oldest_and_stays_bounded);
  RUN_TEST(test_outbox_ignores_a_corrupt_flash_blob);
  RUN_TEST(test_lookup_new_duplicate_unknown);
  RUN_TEST(test_attendance_survives_same_day_reboot_and_reset_clears_it);
  RUN_TEST(test_stale_flags_never_turn_a_tap_away_before_the_clock_is_set);
  RUN_TEST(test_new_day_discards_old_flags_but_keeps_taps_made_since_power_on);
  RUN_TEST(test_flags_from_an_unknown_day_are_discarded);
  RUN_TEST(test_midnight_while_running_clears_attendance_once);
  RUN_TEST(test_reset_keeps_the_day);
  RUN_TEST(test_enroll_validation);
  RUN_TEST(test_enroll_rejects_malformed_uids_and_accepts_separators);
  RUN_TEST(test_enrolled_students_and_their_attendance_survive_reboot);
  RUN_TEST(test_seven_byte_uid_persists);
  RUN_TEST(test_restored_enrollment_does_not_duplicate_a_builtin_uid);
  RUN_TEST(test_enroll_after_a_skipped_slot_keeps_every_saved_student);
  RUN_TEST(test_remove_unenrolls_for_good);
  RUN_TEST(test_remove_refuses_builtin_and_frees_slots_when_empty);
  return UNITY_END();
}
