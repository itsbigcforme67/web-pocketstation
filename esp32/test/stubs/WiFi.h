#pragma once
#include <Arduino.h>
#define WL_CONNECTED 3
#define WIFI_STA 1
class WiFiClient { public: int available(); size_t readBytes(uint8_t*, size_t); };
struct WiFiT { int status(); void mode(int); void begin(const char*, const char*); };
extern WiFiT WiFi;
#include <time.h>
void configTzTime(const char*, const char*, const char*);
bool getLocalTime(struct tm*, uint32_t);
