#include "BackendResponse.h"

#include <string.h>

namespace {
bool startsWith(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}
}  // namespace

BackendAction classifyBackendResponse(int httpCode, const char* body) {
  if (httpCode < 200 || httpCode >= 300) return BackendAction::RETRY;
  if (body == nullptr) return BackendAction::RETRY;

  while (*body == ' ' || *body == '\r' || *body == '\n') body++;

  if (startsWith(body, "Success") || startsWith(body, "Duplicate")) {
    return BackendAction::DELIVERED;
  }
  // The request itself was malformed - the same record would fail forever
  // and block everything queued behind it.
  if (startsWith(body, "Error: Missing uid or name")) return BackendAction::DROP;

  // Everything else - "Error: Invalid or missing API key" (fix the key,
  // records are kept), "Error: Server busy", Google's HTML error page,
  // an empty body - is not proof the row was written.
  return BackendAction::RETRY;
}
