#pragma once
#include <WiFi.h>
class HTTPClient { public: bool begin(const String&); void setTimeout(int); void addHeader(const char*, const String&); int GET(); int PUT(uint8_t*, size_t);
  int getSize(); WiFiClient* getStreamPtr(); String getString(); void end(); };
