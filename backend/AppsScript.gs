/**
 * Smart Attendance System — Google Apps Script backend.
 *
 * Deploy: Extensions > Apps Script (from the target Sheet) > paste this
 * file in as Code.gs > Deploy > New deployment > Web app >
 *   Execute as: Me
 *   Who has access: Anyone
 * Copy the resulting /exec URL into GOOGLE_SCRIPT_URL in your local
 * SmartAttendance/SmartAttendance.ino upload sketch.
 *
 * Whenever you EDIT this file after the first deploy, you must also do
 * Deploy > Manage deployments > (pencil icon) > New version, otherwise
 * the live URL keeps serving the old code.
 */

// Must exactly match API_KEY in your local Arduino sketch.
// This is a simple shared-secret check - anyone who knows this string can
// write attendance rows, so treat it like a password and NEVER commit the
// real value (this file ships with a placeholder; paste your key into the
// copy inside the Apps Script editor only). It stops random bots that find
// the /exec URL from spamming your sheet; it is NOT strong cryptographic
// auth. Generate one with:  openssl rand -hex 16
// (letters and digits only, so it also survives being pasted into a URL
// when testing the deployment from a browser).
var API_KEY = 'CHANGE_ME_TO_A_LONG_RANDOM_STRING';

var SHEET_HEADERS = ['Timestamp', 'Student Name', 'Card UID'];
var UID_COLUMN = 3;  // 1-based column index for "Card UID"

function doGet(e) {
  // Every code path below returns plain text via textResponse_(), and
  // this try/catch is what guarantees that promise even when something
  // unexpected throws (a spreadsheet API hiccup, a locked-out
  // getActiveSpreadsheet(), etc). Without it, an uncaught exception
  // makes Apps Script serve its own HTML error page instead — which
  // still comes back as an HTTP 200 to HTTPClient (Apps Script doesn't
  // set a failing status on an unhandled error), so the ESP32 would
  // read it as a "successful" sync, parse garbled HTML into `message`,
  // and mark the student logged even though nothing was written to the
  // sheet. Catching here turns that into an honest, parseable error the
  // firmware can actually act on.
  try {
    return handleAttendanceRequest_(e);
  } catch (err) {
    return textResponse_('Error: ' + err.message);
  }
}

function handleAttendanceRequest_(e) {
  var params = e.parameter || {};
  var key = params.key;
  var uid = params.uid;
  var name = params.name;

  if (API_KEY === 'CHANGE_ME_TO_A_LONG_RANDOM_STRING') {
    return textResponse_('Error: Server API key not configured');
  }
  if (key !== API_KEY) {
    return textResponse_('Error: Invalid or missing API key');
  }
  if (!uid || !name) {
    return textResponse_('Error: Missing uid or name parameter');
  }

  uid = uid.trim();
  name = name.trim();

  // The firmware stamps each tap with the time it HAPPENED (unix seconds,
  // from NTP) so a record uploaded late - after a Wi-Fi outage - keeps its
  // real time and lands on the right day. Missing or implausible values
  // (clock never set, garbage) fall back to "now".
  var when = tapTime_(params.ts);

  // Two students can tap within the same second (this is even one of
  // the project's own test scenarios — see test/README item 7), and
  // Apps Script can run more than one doGet() concurrently for the
  // same web app. Without a lock, both requests could read
  // isDuplicateToday_() as false before either has appended a row,
  // producing two rows for the same tap. The lock serializes just the
  // check-then-append section below so only one request at a time can
  // be "in the gap" between checking and writing.
  var lock = LockService.getScriptLock();
  var haveLock = lock.tryLock(10000);
  if (!haveLock) {
    return textResponse_('Error: Server busy, try again');
  }

  try {
    var spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
    var sheet = spreadsheet.getActiveSheet();
    // "Today" is decided in the SPREADSHEET's time zone (File > Settings) -
    // the one its Timestamp column is displayed in - not the Apps Script
    // project's, which is hidden away and often defaults to US time.
    var timeZone = spreadsheet.getSpreadsheetTimeZone() || Session.getScriptTimeZone();
    ensureHeaderRow_(sheet);

    if (isDuplicateOnDay_(sheet, uid, when, timeZone)) {
      // The ESP32 already tracks "already logged" in memory (and now in
      // NVS flash too), but this server-side check is a second, fully
      // independent backstop that keeps the sheet clean either way.
      return textResponse_(
        'Duplicate: ' + name + ' already logged today, no new row added'
      );
    }

    sheet.appendRow([when, name, uid]);
    return textResponse_('Success: Attendance logged for ' + name);
  } finally {
    lock.releaseLock();
  }
}

function ensureHeaderRow_(sheet) {
  if (sheet.getLastRow() === 0) {
    sheet.appendRow(SHEET_HEADERS);
    sheet.getRange(1, 1, 1, SHEET_HEADERS.length).setFontWeight('bold');
  }

  // Force the entire Card UID column to Plain Text ("@"). Without this,
  // Sheets auto-detects an all-digit UID (e.g. "01020304") as a number
  // and silently rewrites it as 1020304 — dropping the leading zero and
  // permanently changing the stored value. Re-applying this format is
  // idempotent and cheap, so it's safe to run on every request; it only
  // protects values written AFTER the format is applied, so it must run
  // before appendRow() below, not after.
  var lastPossibleRow = Math.max(sheet.getMaxRows() - 1, 1);
  sheet.getRange(2, UID_COLUMN, lastPossibleRow, 1).setNumberFormat('@');
}

function tapTime_(tsParam) {
  var now = new Date();
  var secs = Number(tsParam);
  if (!tsParam || !isFinite(secs)) return now;
  var t = new Date(secs * 1000);
  var earliest = new Date('2023-01-01T00:00:00Z');
  var latest = new Date(now.getTime() + 24 * 60 * 60 * 1000);
  return (t >= earliest && t <= latest) ? t : now;
}

function dayString_(date, timeZone) {
  return Utilities.formatDate(date, timeZone, 'yyyy-MM-dd');
}

function isDuplicateOnDay_(sheet, uid, when, timeZone) {
  var lastRow = sheet.getLastRow();
  if (lastRow < 2) return false;

  var dayStr = dayString_(when, timeZone);

  // Column A = Timestamp, Column C = Card UID (see SHEET_HEADERS above).
  var data = sheet.getRange(2, 1, lastRow - 1, 3).getValues();
  for (var i = 0; i < data.length; i++) {
    var rowTime = new Date(data[i][0]);
    // A hand-edited or blank timestamp cell must not break every future
    // request - formatDate() throws on an invalid Date.
    if (isNaN(rowTime.getTime())) continue;

    // Explicit String() coercion on both sides: older rows (or hand-edited
    // ones) may hold the UID as a Number, and strict === between a Number
    // and a String is always false.
    if (dayString_(rowTime, timeZone) === dayStr && String(data[i][2]) === String(uid)) {
      return true;
    }
  }
  return false;
}

function textResponse_(message) {
  return ContentService.createTextOutput(message);
}
