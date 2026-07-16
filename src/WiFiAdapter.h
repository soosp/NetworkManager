#pragma once

#ifdef ARDUINO_ARCH_ESP32
#include "ESP32WiFiAdapter.h"
using WiFiAdapter = ESP32WiFiAdapter;
#elif defined(ARDUINO_ARCH_ESP8266)
#include "ESP8266WiFiAdapter.h"
using WiFiAdapter = ESP8266WiFiAdapter;
#else
#error "WiFiAdapter: WiFi is only supported on ESP32 and ESP8266"
#endif