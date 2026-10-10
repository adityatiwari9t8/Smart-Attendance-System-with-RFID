#pragma once
// In-memory stand-in for the ESP32 Preferences (NVS) library.
//
// The store is a process-wide map, so data "survives" across Preferences
// objects exactly like flash survives a reboot - tests simulate a reboot
// by constructing fresh StudentDatabase/Outbox objects and calling
// begin() again. MockNvs::wipe() simulates erasing the flash.
//
// Like the real NVS it REJECTS keys longer than 15 characters (the put*
// call returns 0 and stores nothing), so a test fails if production code
// relies on a key that real flash would refuse.
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"

namespace MockNvs {
using Blob = std::vector<uint8_t>;
inline std::map<std::string, Blob>& store() {
  static std::map<std::string, Blob> s;
  return s;
}
inline void wipe() { store().clear(); }
}  // namespace MockNvs

class Preferences {
 public:
  bool begin(const char* name, bool /*readOnly*/ = false) {
    _ns = name;
    _open = true;
    return true;
  }
  void end() { _open = false; }
  bool clear() {
    std::string prefix = _ns + "/";
    auto& s = MockNvs::store();
    for (auto it = s.begin(); it != s.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) it = s.erase(it);
      else ++it;
    }
    return true;
  }
  bool remove(const char* key) { return MockNvs::store().erase(full(key)) > 0; }

  size_t putBytes(const char* key, const void* value, size_t len) {
    if (!validKey(key)) return 0;
    const uint8_t* p = (const uint8_t*)value;
    MockNvs::store()[full(key)] = MockNvs::Blob(p, p + len);
    return len;
  }
  size_t getBytesLength(const char* key) {
    auto it = MockNvs::store().find(full(key));
    return it == MockNvs::store().end() ? 0 : it->second.size();
  }
  size_t getBytes(const char* key, void* buf, size_t maxLen) {
    auto it = MockNvs::store().find(full(key));
    if (it == MockNvs::store().end()) return 0;
    size_t n = it->second.size() < maxLen ? it->second.size() : maxLen;
    memcpy(buf, it->second.data(), n);
    return n;
  }

  size_t putBool(const char* key, bool v) { return putBytes(key, &v, sizeof(v)); }
  bool getBool(const char* key, bool def = false) {
    bool v = def;
    return getBytes(key, &v, sizeof(v)) ? v : def;
  }
  size_t putInt(const char* key, int32_t v) { return putBytes(key, &v, sizeof(v)); }
  int32_t getInt(const char* key, int32_t def = 0) {
    int32_t v = def;
    return getBytes(key, &v, sizeof(v)) ? v : def;
  }
  size_t putString(const char* key, const String& v) {
    return putBytes(key, v.c_str(), v.length() + 1);
  }
  String getString(const char* key, const String& def = String()) {
    auto it = MockNvs::store().find(full(key));
    if (it == MockNvs::store().end()) return def;
    return String((const char*)it->second.data());
  }

 private:
  static bool validKey(const char* key) { return key && strlen(key) > 0 && strlen(key) <= 15; }
  std::string full(const char* key) const { return _ns + "/" + key; }
  std::string _ns;
  bool _open = false;
};
