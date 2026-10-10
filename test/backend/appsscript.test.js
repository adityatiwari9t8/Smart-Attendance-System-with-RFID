// Tests for backend/AppsScript.gs - run with:  node --test test/backend
//
// Loads the REAL script file into a sandbox with fake SpreadsheetApp /
// LockService / Utilities / ContentService objects, so doGet() runs its
// actual code against an in-memory sheet.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const SOURCE = fs.readFileSync(path.join(__dirname, '../../backend/AppsScript.gs'), 'utf8');
const KEY = 'test-key-123';

// Formats a Date as yyyy-MM-dd in a fixed-offset zone ('UTC' or e.g.
// 'Asia/Kolkata' = +5:30) - enough of Utilities.formatDate for these tests.
const OFFSET_MIN = { UTC: 0, 'Asia/Kolkata': 330, 'America/New_York': -240 };
function ymd(d, tz = 'UTC') {
  return new Date(d.getTime() + OFFSET_MIN[tz] * 60000).toISOString().slice(0, 10);
}

function makeBackend({
  rows = [], key = KEY, lockAvailable = true, throwOnAppend = false,
  sheetTz = 'UTC', scriptTz = 'UTC',
} = {}) {
  const sheet = {
    rows: [['Timestamp', 'Student Name', 'Card UID'], ...rows], // rows[0] = header
    formats: 0,
    getLastRow() { return this.rows.length; },
    getMaxRows() { return 1000; },
    appendRow(r) {
      if (throwOnAppend) throw new Error('Service unavailable');
      this.rows.push(r);
    },
    getRange(row, col, numRows, numCols) {
      const self = this;
      return {
        getValues: () =>
          self.rows.slice(row - 1, row - 1 + numRows).map((r) => r.slice(col - 1, col - 1 + numCols)),
        setNumberFormat: () => { self.formats++; return this; },
        setFontWeight: () => this,
      };
    },
  };

  const context = {
    SpreadsheetApp: {
      getActiveSpreadsheet: () => ({ getActiveSheet: () => sheet, getSpreadsheetTimeZone: () => sheetTz }),
    },
    LockService: {
      getScriptLock: () => ({
        tryLock: () => lockAvailable,
        releaseLock: () => {},
      }),
    },
    Session: { getScriptTimeZone: () => scriptTz },
    Utilities: { formatDate: (d, tz) => ymd(d, tz) },
    ContentService: { createTextOutput: (text) => ({ getContent: () => text }) },
    Date, Math, Number, String, isFinite, isNaN,
  };
  vm.createContext(context);
  vm.runInContext(SOURCE, context);
  // `var API_KEY` becomes a context property - configure it like a user would.
  context.API_KEY = key;

  return {
    sheet,
    get: (params) => context.doGet({ parameter: params }).getContent(),
  };
}

const req = (extra = {}) => ({ key: KEY, uid: '01020304', name: 'Aisyah', ...extra });

test('ships with a placeholder key, not a real secret', () => {
  assert.match(SOURCE, /var API_KEY = 'CHANGE_ME_TO_A_LONG_RANDOM_STRING';/);
});

test('refuses to run while the placeholder key is still in place', () => {
  const b = makeBackend({ key: 'CHANGE_ME_TO_A_LONG_RANDOM_STRING' });
  assert.match(b.get({ key: 'CHANGE_ME_TO_A_LONG_RANDOM_STRING', uid: '1', name: 'x' }), /^Error: Server API key not configured/);
  assert.equal(b.sheet.rows.length, 1);
});

test('rejects a wrong or missing key and writes nothing', () => {
  const b = makeBackend();
  assert.match(b.get(req({ key: 'nope' })), /^Error: Invalid or missing API key/);
  assert.match(b.get({ uid: '1', name: 'x' }), /^Error: Invalid or missing API key/);
  assert.equal(b.sheet.rows.length, 1);
});

test('rejects missing uid or name with the DROP-class error', () => {
  const b = makeBackend();
  assert.match(b.get({ key: KEY, name: 'x' }), /^Error: Missing uid or name/);
  assert.match(b.get({ key: KEY, uid: '1' }), /^Error: Missing uid or name/);
});

test('logs a new tap: Success reply, one row, UID stays a string', () => {
  const b = makeBackend();
  assert.match(b.get(req()), /^Success: Attendance logged for Aisyah/);
  assert.equal(b.sheet.rows.length, 2);
  assert.equal(b.sheet.rows[1][1], 'Aisyah');
  assert.equal(b.sheet.rows[1][2], '01020304');
  assert.ok(b.sheet.formats > 0, 'UID column forced to plain text');
});

test('second tap the same day is a Duplicate and adds no row', () => {
  const b = makeBackend();
  b.get(req());
  assert.match(b.get(req()), /^Duplicate: Aisyah already logged today/);
  assert.equal(b.sheet.rows.length, 2);
});

test('duplicate check survives numeric UID cells (leading zero lost by Sheets)', () => {
  const b = makeBackend({ rows: [[new Date(), 'Aisyah', 1020304]] });
  assert.match(b.get(req({ uid: '1020304' })), /^Duplicate/);
});

test('a different student the same day is logged', () => {
  const b = makeBackend();
  b.get(req());
  assert.match(b.get(req({ uid: '11223344', name: 'Balqis' })), /^Success/);
  assert.equal(b.sheet.rows.length, 3);
});

test('yesterday\'s row is not a duplicate today', () => {
  const yesterday = new Date(Date.now() - 36 * 3600 * 1000);
  const b = makeBackend({ rows: [[yesterday, 'Aisyah', '01020304']] });
  assert.match(b.get(req()), /^Success/);
});

test('ts: a late upload keeps the time the card was TAPPED', () => {
  const b = makeBackend();
  const tapped = Math.floor(Date.now() / 1000) - 3 * 3600; // 3 hours ago
  b.get(req({ ts: String(tapped) }));
  assert.equal(b.sheet.rows[1][0].getTime(), tapped * 1000);
});

test('ts: duplicate detection uses the TAP day, not the upload day', () => {
  const twoDaysAgo = Math.floor(Date.now() / 1000) - 48 * 3600;
  const b = makeBackend();
  assert.match(b.get(req({ ts: String(twoDaysAgo) })), /^Success/);
  // Same student, today: a different day than the queued tap -> new row.
  assert.match(b.get(req()), /^Success/);
  // Re-sending a tap from that same old day IS a duplicate.
  assert.match(b.get(req({ ts: String(twoDaysAgo) })), /^Duplicate/);
  assert.equal(b.sheet.rows.length, 3);
});

test('ts: garbage, zero, ancient and far-future values fall back to now', () => {
  for (const bad of ['abc', '0', '12345', String(Math.floor(Date.now() / 1000) + 10 * 86400)]) {
    const b = makeBackend();
    const before = Date.now();
    b.get(req({ ts: bad }));
    const stamped = b.sheet.rows[1][0].getTime();
    assert.ok(Math.abs(stamped - before) < 5000, `ts=${bad} should fall back to now`);
  }
});

test('a blank or hand-edited timestamp cell does not break later requests', () => {
  const b = makeBackend({ rows: [['not a date', 'Ghost', 'DEADBEEF'], ['', 'Blank', 'CAFEBABE']] });
  assert.match(b.get(req()), /^Success/);
});

test('backend failures come back as readable text, never an exception', () => {
  const b = makeBackend({ throwOnAppend: true });
  assert.match(b.get(req()), /^Error: Service unavailable/);
});

test('busy lock returns a retryable error and writes nothing', () => {
  const b = makeBackend({ lockAvailable: false });
  assert.match(b.get(req()), /^Error: Server busy/);
  assert.equal(b.sheet.rows.length, 1);
});

// 09:00 and 11:00 in India are on the same day, but the default Apps Script
// project zone (US) puts a day boundary between them. The sheet's own zone
// - the one the teacher sees - must decide.
test('"today" follows the spreadsheet time zone, not the script project zone', () => {
  const b = makeBackend({ sheetTz: 'Asia/Kolkata', scriptTz: 'America/New_York' });
  const nineAmIst = Date.UTC(2026, 0, 12, 3, 30) / 1000;     // 2026-01-12 09:00 IST
  const elevenAmIst = Date.UTC(2026, 0, 12, 5, 30) / 1000;   // 2026-01-12 11:00 IST
  assert.match(b.get(req({ ts: String(nineAmIst) })), /^Success/);
  assert.match(b.get(req({ ts: String(elevenAmIst) })), /^Duplicate/);
});
