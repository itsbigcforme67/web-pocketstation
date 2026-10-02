#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <algorithm>
using std::min; using std::max;
class String {
 public:
  std::string s;
  String() {} String(const char* c) : s(c ? c : "") {} String(const std::string& x) : s(x) {}
  String(int v) : s(std::to_string(v)) {} String(unsigned v) : s(std::to_string(v)) {} String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(uint32_t v, int base) { char b[16]; snprintf(b, 16, "%x", v); s = b; (void)base; }
  size_t length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
  char operator[](size_t i) const { return s[i]; }
  String operator+(const String& o) const { return String(s + o.s); }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
  bool operator<(const String& o) const { return s < o.s; }
  int indexOf(char c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(char c, int from) const { auto p = s.rfind(c, from); return p == std::string::npos ? -1 : (int)p; }
  String substring(int a) const { return String(s.substr(a)); }
  String substring(int a, int b) const { return String(s.substr(a, b - a)); }
  bool endsWith(const char* e) const { size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }
  void remove(int i) { s.erase(i); }
  void trim() {} void toLowerCase() {}
};
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
struct SerialT { void printf(const char*, ...) {} void println(const String&) {} void println(const char*) {} };
extern SerialT Serial;
uint32_t millis(); uint32_t micros(); void delay(uint32_t);
void* ps_malloc(size_t);
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
void* heap_caps_malloc(size_t, int);
struct EspT { uint32_t getFreeHeap(); uint32_t getFreePsram(); };
extern EspT ESP;
#define HEX 16
