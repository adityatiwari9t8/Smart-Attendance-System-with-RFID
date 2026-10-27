var API_KEY = 'YOUR_API_KEY';

var ATTENDANCE_SHEET_NAME = 'RFID Attendance';
var ATTENDANCE_HEADERS = ['Student Name', 'Card UID', ''];
var FIRST_DATE_COLUMN = 4;

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
    var attendanceSheet = getAttendanceSheet_(spreadsheet);
    var roster = readStudentRoster_(attendanceSheet);
    var student = findStudentByUid_(roster, uid);
    if (!student) {
      return textResponse_('Error: UID not found in RFID Attendance column B');
    }

    var tapTime = tapTime_(params.ts);
    var dateKey = Utilities.formatDate(tapTime, timeZone, 'yyyy-MM-dd');
    var dateColumn = getOrCreateDateColumn_(attendanceSheet, dateKey);
    var studentRow = student.row;
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

function readStudentRoster_(sheet) {
  var lastRow = sheet.getLastRow();
  if (lastRow < 2) return [];

  var rows = sheet.getRange(2, 1, lastRow - 1, 2).getDisplayValues();
  var roster = [];
  for (var i = 0; i < rows.length; i++) {
    var name = String(rows[i][0]).trim();
    var uid = normalizeUid_(rows[i][1]);
    if (!name && !uid) continue;
    if (!name) throw new Error('Enter a Student Name for every UID row');
    for (var j = 0; j < roster.length; j++) {
      if (uid && roster[j].uid === uid) throw new Error('Each student must have a unique Card UID');
    }
    roster.push({ name: name, uid: uid, row: i + 2 });
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
  if (!sheet) throw new Error('Create or rename the existing tab to RFID Attendance');

  if (sheet.getLastRow() === 0) {
    sheet.getRange(1, 1, 1, ATTENDANCE_HEADERS.length).setValues([ATTENDANCE_HEADERS]);
    sheet.getRange(1, 1, 1, ATTENDANCE_HEADERS.length).setFontWeight('bold');
    sheet.setFrozenRows(1);
    sheet.setFrozenColumns(2);
    sheet.setColumnWidth(1, 180);
    sheet.setColumnWidth(2, 150);
    sheet.setColumnWidth(3, 28);
  } else {
    var headers = sheet.getRange(1, 1, 1, ATTENDANCE_HEADERS.length).getDisplayValues()[0];
    if (headers[0] !== 'Student Name' || headers[1] !== 'Card UID') {
      throw new Error('RFID Attendance headers must be Student Name, Card UID, then a blank spacer');
    }
  }
  sheet.getRange(2, 2, Math.max(sheet.getMaxRows() - 1, 1), 1).setNumberFormat('@');
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
