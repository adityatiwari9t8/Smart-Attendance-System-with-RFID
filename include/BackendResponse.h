#pragma once
// =====================================================================
// BackendResponse — decides what an Apps Script reply MEANS.
//
// Apps Script answers HTTP 200 for everything, including failures
// ("Error: Invalid or missing API key" is still a 200), so the status
// code alone can't say whether the row reached the sheet. This looks at
// the response text instead. No Arduino dependencies, so it is unit
// tested on the host (test/test_native).
// =====================================================================

enum class BackendAction {
  DELIVERED,  // the sheet has this record (new row, or already there today)
  DROP,       // the server will never accept this record - retrying is pointless
  RETRY       // not delivered; keep it queued and try again later
};

// `httpCode` is the HTTPClient return value (negative = connection-level
// failure). `body` may be null/empty.
BackendAction classifyBackendResponse(int httpCode, const char* body);
