#pragma once
#include <Arduino.h>
struct JsonVariant;
struct JsonObject { JsonVariant operator[](const char*); };
struct JsonArray { JsonObject* begin(); JsonObject* end(); template <class T> T add(); };
struct JsonVariant {
  String operator|(const char*) const; String operator|(const String&) const; bool operator|(bool) const; int operator|(int) const;
  template <class T> T as(); template <class T> T to(); template <class T> void operator=(const T&);
};
struct JsonDocument { JsonVariant operator[](const char*); };
struct DeserializationError { operator bool() const; };
template <class S> DeserializationError deserializeJson(JsonDocument&, S&);
template <class S> size_t serializeJson(JsonDocument&, S&);
