/**
 * Smart Attendance System — date-wise Google Sheets backend.
 *
 * The firmware sends uid, name, key, and the original tap timestamp (ts).
 * Card UIDs are looked up in the Students tab. RFID Attendance is the matrix
 * with one row per student and one column per calendar date; an accepted
 * tap writes only "P" in that student's cell for the tap date.
 *
 * Deploy as a web app: Execute as Me; access Anyone. After editing this file,
 * deploy a new version from Manage deployments.
 */

// Must exactly match API_KEY in your local Arduino upload sketch.
// Keep the real key private; the repository template intentionally uses a
// placeholder. Generate one with: openssl rand -hex 16
var API_KEY = 'YOUR_API_KEY';

var STUDENTS_SHEET_NAME = 'Students';
var ATTENDANCE_SHEET_NAME = 'RFID Attendance';
var STUDENTS_HEADERS = ['URN', 'Student Name', 'Card UID'];
var ATTENDANCE_HEADERS = ['URN', 'Student Name', ''];
var MAX_STUDENTS = 2;
var FIRST_DATE_COLUMN = 4; // Column C is a visual spacer.

function doGet(e) {
  try {
    return handleAttendanceRequest_(e);
  } catch (err) {
    return textResponse_('Error: ' + err.message);
  }
}

function handleAttendanceRequest_(e) {
  var params = (e && e.parameter) || {};
  var key = params.key;
  var uid = normalizeUid_(params.uid || '');
  var requestName = String(params.name || '').trim();

  if (API_KEY === 'YOUR_API_KEY') {
    return textResponse_('Error: Server API key not configured');
  }
  if (key !== API_KEY) {
    return textResponse_('Error: Invalid or missing API key');
  }
  if (!uid || !requestName) {
    return textResponse_('Error: Missing uid or name parameter');
  }

  var lock = LockService.getScriptLock();
  if (!lock.tryLock(10000)) {
    return textResponse_('Error: Server busy, try again');
  }

  try {
    var spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
    var timeZone = spreadsheet.getSpreadsheetTimeZone() || Session.getScriptTimeZone();
    var studentsSheet = getStudentsSheet_(spreadsheet);
    var roster = readStudentRoster_(studentsSheet);
    if (roster.length > MAX_STUDENTS) {
      return textResponse_('Error: Students roster is limited to ' + MAX_STUDENTS + ' entries');
    }

    var student = findStudentByUid_(roster, uid);
    if (!student) {
      return textResponse_('Error: UID not found in Students roster');
    }

    var tapTime = tapTime_(params.ts);
    var dateKey = Utilities.formatDate(tapTime, timeZone, 'yyyy-MM-dd');
    var attendanceSheet = getAttendanceSheet_(spreadsheet);
    var dateColumn = getOrCreateDateColumn_(attendanceSheet, dateKey);
    var studentRow = getOrCreateStudentRow_(attendanceSheet, student);
    var attendanceCell = attendanceSheet.getRange(studentRow, dateColumn);

    if (String(attendanceCell.getValue()).trim().toUpperCase() === 'P') {
      return textResponse_('Duplicate: ' + student.name + ' already present on ' + dateKey);
    }

    attendanceCell.setValue('P');
    attendanceCell.setHorizontalAlignment('center');
    attendanceCell.setFontWeight('bold');
    return textResponse_('Success: Attendance marked P for ' + student.name + ' on ' + dateKey);
  } finally {
    lock.releaseLock();
  }
}

function getStudentsSheet_(spreadsheet) {
  var sheet = spreadsheet.getSheetByName(STUDENTS_SHEET_NAME);
  if (!sheet) sheet = spreadsheet.insertSheet(STUDENTS_SHEET_NAME);

  if (sheet.getLastRow() === 0) {
    sheet.getRange(1, 1, 1, STUDENTS_HEADERS.length).setValues([STUDENTS_HEADERS]);
    sheet.getRange(1, 1, 1, STUDENTS_HEADERS.length).setFontWeight('bold');
    sheet.setFrozenRows(1);
    sheet.setColumnWidth(1, 120);
    sheet.setColumnWidth(2, 180);
    sheet.setColumnWidth(3, 150);
  } else {
    var headers = sheet.getRange(1, 1, 1, STUDENTS_HEADERS.length).getDisplayValues()[0];
    if (headers.join('|') !== STUDENTS_HEADERS.join('|')) {
      throw new Error('Students tab headers must be URN, Student Name, Card UID');
    }
  }

  // Preserve leading zeroes when a numeric-looking UID is typed into the roster.
  sheet.getRange(2, 3, Math.max(sheet.getMaxRows() - 1, 1), 1).setNumberFormat('@');
  return sheet;
}

function readStudentRoster_(sheet) {
  var lastRow = sheet.getLastRow();
  if (lastRow < 2) return [];

  var rows = sheet.getRange(2, 1, lastRow - 1, 3).getDisplayValues();
  var roster = [];
  for (var i = 0; i < rows.length; i++) {
    var urn = String(rows[i][0]).trim();
    var name = String(rows[i][1]).trim();
    var uid = normalizeUid_(rows[i][2]);
    if (!urn && !name && !uid) continue;
    if (!urn || !name) {
      throw new Error('Complete URN and Student Name for every roster entry');
    }
    for (var j = 0; j < roster.length; j++) {
      if (roster[j].urn === urn) throw new Error('Each student must have a unique URN');
      if (uid && roster[j].uid === uid) throw new Error('Each student must have a unique Card UID');
    }
    roster.push({ urn: urn, name: name, uid: uid });
  }
  return roster;
}

function findStudentByUid_(roster, uid) {
  for (var i = 0; i < roster.length; i++) {
    if (roster[i].uid === uid) return roster[i];
  }
  return null;
}

function getAttendanceSheet_(spreadsheet) {
  var sheet = spreadsheet.getSheetByName(ATTENDANCE_SHEET_NAME);
  if (!sheet) sheet = spreadsheet.insertSheet(ATTENDANCE_SHEET_NAME);

  if (sheet.getLastRow() === 0) {
    sheet.getRange(1, 1, 1, ATTENDANCE_HEADERS.length).setValues([ATTENDANCE_HEADERS]);
    sheet.getRange(1, 1, 1, 3).setFontWeight('bold');
    sheet.setFrozenRows(1);
    sheet.setFrozenColumns(2);
    sheet.setColumnWidth(1, 120);
    sheet.setColumnWidth(2, 180);
    sheet.setColumnWidth(3, 28);
  } else {
    var headers = sheet.getRange(1, 1, 1, 2).getDisplayValues()[0];
    if (headers[0] !== 'URN' || headers[1] !== 'Student Name') {
      throw new Error('Attendance Matrix must start with URN and Student Name columns');
    }
  }
  return sheet;
}

function getOrCreateDateColumn_(sheet, dateKey) {
  var lastColumn = sheet.getLastColumn();
  if (lastColumn >= FIRST_DATE_COLUMN) {
    var dates = sheet.getRange(1, FIRST_DATE_COLUMN, 1,
      lastColumn - FIRST_DATE_COLUMN + 1).getDisplayValues()[0];
    for (var i = 0; i < dates.length; i++) {
      if (String(dates[i]).trim() === dateKey) return FIRST_DATE_COLUMN + i;
    }
  }

  var newColumn = Math.max(lastColumn + 1, FIRST_DATE_COLUMN);
  sheet.getRange(1, newColumn).setValue(dateKey);
  sheet.getRange(1, newColumn).setFontWeight('bold').setHorizontalAlignment('center');
  sheet.setColumnWidth(newColumn, 100);
  return newColumn;
}

function getOrCreateStudentRow_(sheet, student) {
  var lastRow = sheet.getLastRow();
  if (lastRow >= 2) {
    var urns = sheet.getRange(2, 1, lastRow - 1, 1).getDisplayValues();
    for (var i = 0; i < urns.length; i++) {
      if (String(urns[i][0]).trim() === student.urn) {
        sheet.getRange(i + 2, 2).setValue(student.name);
        return i + 2;
      }
    }
  }

  var row = Math.max(lastRow + 1, 2);
  sheet.getRange(row, 1, 1, 2).setValues([[student.urn, student.name]]);
  return row;
}

function normalizeUid_(value) {
  return String(value || '').trim().toUpperCase().replace(/[:\-\s]/g, '');
}

function tapTime_(tsParam) {
  var now = new Date();
  var seconds = Number(tsParam);
  if (!tsParam || !isFinite(seconds)) return now;
  var tap = new Date(seconds * 1000);
  var earliest = new Date('2023-01-01T00:00:00Z');
  var latest = new Date(now.getTime() + 24 * 60 * 60 * 1000);
  return (tap >= earliest && tap <= latest) ? tap : now;
}

function textResponse_(message) {
  return ContentService.createTextOutput(message);
}
