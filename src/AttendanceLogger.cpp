#include "AttendanceLogger.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "freertos/task.h"
#include "config.h"
#include "secrets.h"

namespace {

// Percent-encodes a string for safe use in a URL query parameter.
String urlEncode(const String& value) {
  String encoded;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++) {
    unsigned char c = (unsigned char)value[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0xF];
      encoded += hex[c & 0xF];
    }
  }
  return encoded;
}

// Copies a String into a fixed char buffer, always null-terminated.
void copyTruncated(const String& src, char* dest, size_t destSize) {
  size_t n = src.length();
  if (n >= destSize) n = destSize - 1;
  memcpy(dest, src.c_str(), n);
  dest[n] = '\0';
}

// One HTTPS GET attempt. Returns the HTTP status (or a negative
// HTTPClient/mbedTLS code such as -29312 "SSL - The connection indicated
// an EOF") and fills `outBody` with the response text.
int attemptSync(const String& uid, const String& name, uint32_t ts, String& outBody) {
  // setInsecure() skips certificate validation - the usual trade-off for
  // ESP32 + Apps Script (see docs/ARCHITECTURE.md). A fresh client pair
  // per attempt avoids reusing a TLS session that failed mid-handshake.
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_REQUEST_TIMEOUT_MS / 1000);
  client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_MS / 1000);  // default is 120 s

  HTTPClient http;
  http.setConnectTimeout(HTTP_REQUEST_TIMEOUT_MS);
  http.setTimeout(HTTP_REQUEST_TIMEOUT_MS);

  String url = String(GOOGLE_SCRIPT_URL) + "?uid=" + urlEncode(uid) +
               "&name=" + urlEncode(name) + "&key=" + urlEncode(API_KEY);
  if (ts > 0) url += "&ts=" + String(ts);

  if (!http.begin(client, url)) {
    outBody = "Could not open connection";
    return 0;
  }
  // Apps Script's /exec replies with a 302 to a one-time
  // script.googleusercontent.com URL; STRICT_FOLLOW makes HTTPClient
  // re-issue the GET there.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  outBody = (code > 0) ? http.getString() : String("Connection failed");
  http.end();
  return code;
}

}  // namespace

bool AttendanceLogger::connect() {
  WiFi.persistent(false);        // don't wear flash re-saving credentials every boot
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(400);
    Serial.print(".");
  }
  maintain();  // start NTP right away if we made it
  return isConnected();
}

const char* AttendanceLogger::ssid() const {
  return WIFI_SSID;
}

bool AttendanceLogger::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

void AttendanceLogger::maintain() {
  unsigned long now = millis();
  if (!isConnected()) {
    if (now - _lastWifiNudgeMs >= WIFI_RECHECK_MS) {
      _lastWifiNudgeMs = now;
      WiFi.reconnect();
    }
    return;
  }
  if (!_timeStarted) {
    // Returns immediately; SNTP fills the clock in within a second or two.
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    _timeStarted = true;
  }
}

void AttendanceLogger::beginAsync() {
  _resultQueue = xQueueCreate(SYNC_QUEUE_LENGTH, sizeof(SyncOutcome));
  if (_resultQueue == nullptr) {
    Serial.println("[SYNC] WARNING: failed to create result queue - uploads disabled.");
  }
}

bool AttendanceLogger::startUpload(const String& uid, const String& name, uint32_t ts) {
  if (_busy || _resultQueue == nullptr || !isConnected()) return false;

  // Heap-allocated so it outlives this call; the task frees it.
  SyncJob* job = new SyncJob{uid, name, ts, this};
  _busy = true;

  // Stack is in BYTES on the ESP32 Arduino core. 20KB gives the mbedTLS
  // handshake comfortable headroom. Pinned to core 0 because Arduino's
  // loop() runs on core 1 - this keeps RFID polling free of network work.
  BaseType_t created =
      xTaskCreatePinnedToCore(syncTaskEntry, "cloudSync", 20480, job, 1, nullptr, 0);

  if (created != pdPASS) {
    Serial.println("[SYNC] Failed to start upload task (out of memory?).");
    delete job;
    _busy = false;
    return false;
  }
  return true;
}

void AttendanceLogger::syncTaskEntry(void* param) {
  SyncJob* job = static_cast<SyncJob*>(param);
  SyncOutcome outcome = job->self->doSync(job->uid, job->name, job->ts);
  xQueueSend(job->self->_resultQueue, &outcome, pdMS_TO_TICKS(200));
  delete job;
  // A FreeRTOS task must never just return - it has to delete itself.
  vTaskDelete(nullptr);
}

SyncOutcome AttendanceLogger::doSync(const String& uid, const String& name, uint32_t ts) {
  SyncOutcome outcome{};
  outcome.ts = ts;
  copyTruncated(uid, outcome.uid, sizeof(outcome.uid));

  // Retry connection-level failures (negative codes: DNS, refused, TLS
  // dropped mid-handshake) a couple of times with a short backoff. Any
  // real HTTP answer is final here - the outbox decides whether to try
  // again later - so one tap is never sent more than ~3 times per round.
  const int kMaxAttempts = 3;
  String body;
  int code = 0;
  for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
    body = "";
    code = attemptSync(uid, name, ts, body);
    if (code > 0) break;
    if (attempt < kMaxAttempts) {
      Serial.printf("[SYNC] Attempt %d failed (code %d), retrying...\n", attempt, code);
      vTaskDelay(pdMS_TO_TICKS(400 * attempt));  // fine: this is the background task
    }
  }

  outcome.httpCode = code;
  copyTruncated(body, outcome.message, sizeof(outcome.message));
  // The reply TEXT decides success: Apps Script returns HTTP 200 even for
  // "Error: Invalid or missing API key".
  outcome.action = classifyBackendResponse(code, body.c_str());
  return outcome;
}

bool AttendanceLogger::pollResult(SyncOutcome& out) {
  if (_resultQueue == nullptr) return false;
  if (xQueueReceive(_resultQueue, &out, 0) != pdTRUE) return false;
  _busy = false;  // only now may the next upload start
  return true;
}
