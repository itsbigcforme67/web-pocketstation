#pragma once
#include <Arduino.h>
#include <LittleFS.h>
enum { top_left, top_right, middle_left, middle_right, middle_center };
namespace fonts { struct F {}; extern F Font0, Font2, FreeSansBold9pt7b; }
struct LGFX_Sprite {
  void fillScreen(uint16_t); void fillRect(int, int, int, int, uint16_t); void fillRoundRect(int, int, int, int, int, uint16_t);
  void drawRoundRect(int, int, int, int, int, uint16_t); void fillCircle(int, int, int, uint16_t);
  void setTextColor(uint16_t); void setTextColor(uint16_t, uint16_t); void setFont(const fonts::F*); void setTextDatum(int); void setTextWrap(bool);
  void drawString(const String&, int, int); void drawString(const char*, int, int);
  uint16_t color565(uint8_t, uint8_t, uint8_t); void setRotation(int); void setBrightness(int); void startWrite(); void endWrite();
  void setColorDepth(int); void setPsram(bool); void createSprite(int, int); void pushSprite(int, int);
};
struct M5Canvas : LGFX_Sprite { M5Canvas(LGFX_Sprite*); };
namespace m5 {
struct rtc_date_t { int16_t year; int8_t month, date, weekDay; };
struct rtc_time_t { int8_t hours, minutes, seconds; };
struct rtc_datetime_t { rtc_date_t date; rtc_time_t time; };
struct touch_detail_t { int16_t x, y; bool isPressed() const; };
}
struct M5Cfg { int serial_baudrate; bool internal_spk, internal_mic; };
struct M5T {
  LGFX_Sprite Display;
  struct { bool readRegister(uint8_t, uint8_t, uint8_t*, size_t, uint32_t) const; } In_I2C;
  struct { int getCount(); m5::touch_detail_t getDetail(int); } Touch;
  struct { m5::rtc_datetime_t getDateTime(); void setDateTime(const m5::rtc_datetime_t&); } Rtc;
  struct { void begin(); void setVolume(int); size_t isPlaying(int); bool playRaw(const int16_t*, size_t, uint32_t, bool, uint32_t, int); void stop(); } Speaker;
  struct { int getBatteryLevel(); } Power;
  M5Cfg config(); void begin(M5Cfg); void update();
};
extern M5T M5;
