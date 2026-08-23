#include <cstdio>
#include <cstring>
// Host stub of Arduino.h — JUST enough for NetworkManager/NetworkAdapter to
// compile and run a behavioural smoke test on a PC. Not used on device.
#pragma once
#include <cstdint>

// PROGMEM helpers. The shipping code uses these because ESP8266 needs strings
// kept in flash; on a host they are plain memory, so the macros map to the
// ordinary functions. They live here rather than being avoided in the code under
// test, because what this harness exists to prove is that the *real* header
// compiles — stubbing around its dependencies would defeat that.
typedef const char* PGM_P;
#define PSTR(s)       (s)
#define PROGMEM
#define snprintf_P    snprintf
#define vsnprintf_P   vsnprintf
#define strlen_P      strlen
#define strncpy_P     strncpy
#define strncat_P     strncat
#define memcpy_P      memcpy
#define strcmp_P      strcmp

extern uint32_t g_millis;          // controllable mock clock (defined in test)
inline uint32_t millis() { return g_millis; }

// Minimal IPAddress: the glue only default-constructs it and returns it.
class IPAddress {
public:
    IPAddress() : _v(0) {}
    explicit IPAddress(uint32_t v) : _v(v) {}
    bool operator==(const IPAddress& o) const { return _v == o._v; }
    uint8_t operator[](int i) const { return (uint8_t)((_v >> (8*i)) & 0xFF); }
    uint32_t raw() const { return _v; }
private:
    uint32_t _v;
};