/*
 * NetworkManagerNtpSyncHook.h
 *
 * Opt-in ESP8266 SNTP poll-interval hook for NetworkManager.
 * 
 * Must be included after NetworkManager.h for NetworkManager singleton accessor + getNtpSyncInterval()
 * 
 * WHY THIS EXISTS (ESP8266 SDK / lwip2 quirk)
 * -------------------------------------------
 * On ESP8266 the lwip2 SNTP stack has no runtime API to set the poll interval
 * (the old sntp_set_update_delay() was dropped in lwip2). Instead, after every
 * successful sync, lwip2 asks for the next interval by calling a weak function:
 *
 *     #define SNTP_UPDATE_DELAY sntp_update_delay_MS_rfc_not_less_than_15000()
 *
 * To make NetworkManager's setNtpSyncInterval() / NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL
 * actually change the ESP8266 cadence, that weak default must be overridden by a
 * *strong* definition. A strong, free function cannot live in a header-only,
 * multi-translation-unit library without risking an ODR / "multiple definition"
 * clash, and an inline/COMDAT definition does not reliably win over the core's
 * weak default. So the override is provided here, as a header you include
 * yourself, in exactly ONE translation unit (normally your .ino).
 *
 * ESP32 and AVR need none of this — ESP32 has esp_sntp_set_sync_interval(), and
 * AVR runs its own poll loop. The header is therefore a no-op off ESP8266.
 *
 * USAGE
 * -----
 *     #include "NetworkManager.h"
 *     #include "NetworkManagerNtpSyncHook.h"   // in ONE .cpp/.ino only
 *
 * Then setNtpSyncInterval(ms) (and NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL) drive
 * the ESP8266 SNTP poll cadence. Including this header in more than one translation
 * unit produces a "multiple definition of sntp_update_delay_MS_rfc_not_less_than_15000"
 * link error — by design; include it once.
 *
 * After inclusion, the macro NETWORK_MANAGER_NTP_SYNC_INTERVAL_SETTER is defined, so
 * other code can detect (at compile time, within the same TU) that the hook is
 * present.
 */

#pragma once

// NETWORK_PROFILE_NTP_SERVER_COUNT must be defined before including NetworkManager.h
// because NetworkManager.h includes NetworkProfile.h via NetworkAdapter.h, so
// it is safe to check it here.
#if defined(ARDUINO_ARCH_ESP8266) && (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)

/**
 * @brief lwip2 SNTP poll-interval hook (strong override of the core's weak default).
 *
 * Returns NetworkManager's configured interval, clamped to the RFC 4330 floor of
 * 15 s so the function always honours its own "not less than 15000" contract
 * even if NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL was set lower (the runtime setter
 * already clamps, but the compile-time default macro does not).
 *
 * No extern "C": on the ESP8266 core lwip2 references the C++-mangled symbol.
 */
uint32_t sntp_update_delay_MS_rfc_not_less_than_15000() {
    const uint32_t ms = NetworkManager.getNtpSyncInterval();
    return ms < 15000UL ? 15000UL : ms;
}

/** @brief Defined once this hook is installed, so code can check for it. */
#define NETWORK_MANAGER_NTP_SYNC_INTERVAL_SETTER 1

#endif  // ARDUINO_ARCH_ESP8266