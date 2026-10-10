#pragma once
// Minimal host-side stand-in for <Arduino.h>: just the slice of Arduino's
// `String` that the unit-tested modules use. Lets StudentDatabase and
// Outbox compile and run under plain g++ (pio test -e native).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#define HEX 16
#define DEC 10

class String {
 public:
  String() {}
  String(const char* s) : _s(s ? s : "") {}
  String(const std::string& s) : _s(s) {}
  String(char c) : _s(1, c) {}
  String(int v, int base = DEC) { fromNumber((unsigned long)v, base, v < 0 && base == DEC); }
  String(unsigned v, int base = DEC) { fromNumber(v, base, false); }
  String(unsigned long v, int base = DEC) { fromNumber(v, base, false); }

  unsigned length() const { return (unsigned)_s.size(); }
  const char* c_str() const { return _s.c_str(); }

  char operator[](unsigned i) const { return _s[i]; }
  String& operator+=(const String& o) { _s += o._s; return *this; }
  String& operator+=(char c) { _s += c; return *this; }
  String& operator+=(const char* o) { _s += o; return *this; }
  String operator+(const String& o) const { return String(_s + o._s); }
  String operator+(const char* o) const { return String(_s + o); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b._s); }
  bool operator==(const String& o) const { return _s == o._s; }
  bool operator==(const char* o) const { return _s == o; }
  bool operator!=(const String& o) const { return _s != o._s; }

  void trim() {
    size_t b = _s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) { _s.clear(); return; }
    size_t e = _s.find_last_not_of(" \t\r\n");
    _s = _s.substr(b, e - b + 1);
  }
  void toUpperCase() {
    for (auto& c : _s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  }
  String substring(unsigned from) const { return String(_s.substr(from)); }
  String substring(unsigned from, unsigned to) const { return String(_s.substr(from, to - from)); }
  int indexOf(char c) const {
    size_t p = _s.find(c);
    return p == std::string::npos ? -1 : (int)p;
  }
  bool startsWith(const String& p) const { return _s.compare(0, p._s.size(), p._s) == 0; }

 private:
  void fromNumber(unsigned long v, int base, bool negative) {
    char buf[40];
    if (base == HEX) snprintf(buf, sizeof(buf), "%lx", v);
    else if (negative) snprintf(buf, sizeof(buf), "%d", (int)v);
    else snprintf(buf, sizeof(buf), "%lu", v);
    _s = buf;
  }
  std::string _s;
};
