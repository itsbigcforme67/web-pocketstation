#pragma once
#include <Arduino.h>
class File { public: operator bool() const; size_t size(); bool seek(size_t); size_t read(uint8_t*, size_t); size_t write(const uint8_t*, size_t); void close();
  const char* name(); bool isDirectory(); File openNextFile(); };
struct FS { bool begin(bool); File open(const String&, const char* m = "r"); bool exists(const char*); bool mkdir(const char*); bool remove(const String&); bool rename(const String&, const String&); };
extern FS LittleFS;
