#pragma once

#ifdef ARDUINO_ARCH_ESP32
#include "ESP32EthAdapter.h"
using EthAdapter = ESP32EthAdapter;
#elif defined(ARDUINO_ARCH_ESP8266)
#include "ESP8266EthAdapter.h"
using EthAdapter = ESP8266EthAdapter;
#elif defined(ARDUINO_ARCH_AVR)
#include "AVREthernetAdapter.h"
using EthAdapter = AVREthernetAdapter;
#else
#error "EthAdapter: unsupported architecture (need ESP32, ESP8266 or AVR)"
#endif