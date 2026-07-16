# Testing

Testing the fallback-restore mechanism was performed by running the
`FallbackTestRunner.ino` example on various hardware platforms. The test
results confirmed that the mechanism works properly on all targeted platforms.

## How test runner works?

The test runner walks a 32-case matrix — 2 priority arrangements x 2 addressing
modes x 8 scenarios — running ONE case per boot and restarting itself, so no
reflashing is needed between cases. The only manual part is the physical action:
the runner prompts (asks to disconnect the cable or disable the WiFi) and waits
for ENTER to ensure the asked action was done and starts the timing.

For every transition it checks TWO things:

- event — the expected high-level event (`CONNECTED` / `FALLBACK` / `RESTORED` /
  `DISCONNECTED`) on the expected interface;
- NTP — that a time sync follows within a timeout. This doubles as an end-to-end
  liveness probe: an NTP sync against an FQDN can only succeed if the default
  route AND the DNS resolver survived the handoff. Both of those broke in ways
  that were invisible at the event level, which is exactly why this part
  exists.

It also records timings: from ENTER to event (approximate: includes your
reaction time, so treat it as an upper bound) and from event to NTP sync
(exact).

**Important:** Pressing ENTER does not trigger the wait for the event; that
process runs in the background. Pressing ENTER simply indicates that the event
trigger (e.g., unplugging the cable) has occurred and the timer can start.

Results are persisted across reboots and printed as a summary table when the
matrix completes.

In the event of an accidental error (e.g., enabling Ethernet instead of WiFi),
the current test case can be restarted by rebooting the MCU (e.g., using the
reset button).

## Test environment

### MCU hardware

|Platform|Board|Ethernet|WiFi|
|--|--|--|--|
|ESP32|Waveshare ESP32-S3-POE-ETH|Wiznet W5500 (integrated, SPI)|built in|
|ESP32|Olimex ESP32-POE|Microchip LAN8720A (integrated, RMII)|built in|
|ESP8266|NodeMCU|Wiznet W5500 (external, SPI)|built in|

#### Waveshare ESP32-S3-POE-ETH settings

This board is not included in the ESP32 Arduino core, so it must be configured
manually.

- Board: ESP32S3 Dev Module
- USB CDC On Boot: Enabled
- CPU Frequency: 240MHz (WiFi)
- Flash mode: QIO 80MHz
- Flash size: 16MB
- PSRAM: OPI PSRAM

ETH_PHY_\*:

```cpp
#define ETH_PHY_TYPE         ETH_PHY_W5500
#define ETH_PHY_SPI_HOST     SPI3_HOST
#define ETH_PHY_SPI_FREQ_MHZ 25
#define ETH_PHY_ADDR          1
#define ETH_PHY_CS           14
#define ETH_PHY_IRQ          10
#define ETH_PHY_RST           9
#define ETH_PHY_SPI_SCK      13
#define ETH_PHY_SPI_MISO     12
#define ETH_PHY_SPI_MOSI     11
```

### Network hardware

The tests were conducted using a Mikrotik RouterBoard 2011.

Disabling and enabling an Ethernet port can be easily done through the menu in
the MikroTik WinBox manager software. For WiFi, a WiFi outage and recovery could
be simulated by enabling or disabling a rule created for the MCU's WiFi MAC
address that blocks the MCU's WiFi connection.

### Software

- Arduino IDE 2.3.10
- ESP32 Arduino Core 3.3.10
- ESP8266 Arduino Core 3.1.2

## Test results

### Platform: ESP32

#### Waveshare ESP32-S3-POE-ETH

```txt
================================= SUMMARY =================================
case  prio  addr    scenario                     events  ntp   sw(s) ntp(s)
   0  ETH   dhcp    1  P > S > P > S > P         PASS    PASS    9.7    4.9
   1  ETH   dhcp    2  P > S > disc > S > P      PASS    PASS   31.3    4.7
   2  ETH   dhcp    3  P > S > disc > P > S > P  PASS    PASS   30.7    4.9
   3  ETH   dhcp    4  S > P > S > P             PASS    PASS   16.8    4.7
   4  ETH   dhcp    5  S > disc > S > P > S > P  PASS    PASS   31.0    3.0
   5  ETH   dhcp    6  S > disc > P > S > P      PASS    PASS   30.9    4.4
   6  ETH   dhcp    7  disc > S > P > S > P      PASS    PASS   30.0    2.5
   7  ETH   dhcp    8  disc > P > S > P          PASS    PASS   30.0    1.9
   8  ETH   static  1  P > S > P > S > P         PASS    PASS    5.6    4.8
   9  ETH   static  2  P > S > disc > S > P      PASS    PASS   30.6    4.6
  10  ETH   static  3  P > S > disc > P > S > P  PASS    PASS   30.6    5.2
  11  ETH   static  4  S > P > S > P             PASS    PASS   15.3    3.3
  12  ETH   static  5  S > disc > S > P > S > P  PASS    PASS   31.2    4.8
  13  ETH   static  6  S > disc > P > S > P      PASS    PASS   30.7    3.9
  14  ETH   static  7  disc > S > P > S > P      PASS    PASS   30.0    4.8
  15  ETH   static  8  disc > P > S > P          PASS    PASS   30.0    4.5
  16  WiFi  dhcp    1  P > S > P > S > P         PASS    PASS    7.7    3.9
  17  WiFi  dhcp    2  P > S > disc > S > P      PASS    PASS   31.6    4.1
  18  WiFi  dhcp    3  P > S > disc > P > S > P  PASS    PASS   32.7    3.8
  19  WiFi  dhcp    4  S > P > S > P             PASS    PASS    4.1    3.8
  20  WiFi  dhcp    5  S > disc > S > P > S > P  PASS    PASS   31.2    5.0
  21  WiFi  dhcp    6  S > disc > P > S > P      PASS    PASS   31.6    3.8
  22  WiFi  dhcp    7  disc > S > P > S > P      PASS    PASS   31.8    4.7
  23  WiFi  dhcp    8  disc > P > S > P          PASS    PASS   30.0    4.4
  24  WiFi  static  1  P > S > P > S > P         PASS    PASS   16.0    4.0
  25  WiFi  static  2  P > S > disc > S > P      PASS    PASS   32.2    4.6
  26  WiFi  static  3  P > S > disc > P > S > P  PASS    PASS   31.8    4.6
  27  WiFi  static  4  S > P > S > P             PASS    PASS    6.8    3.6
  28  WiFi  static  5  S > disc > S > P > S > P  PASS    PASS   31.6    6.5
  29  WiFi  static  6  S > disc > P > S > P      PASS    PASS   31.5    5.6
  30  WiFi  static  7  disc > S > P > S > P      PASS    PASS   30.0    4.2
  31  WiFi  static  8  disc > P > S > P          PASS    PASS   30.0    5.2

  32/32 cases fully passed
  (prio = primary interface; sw = worst ENTER->event, approximate;
   ntp = worst event->sync.)
  Type a case number + ENTER to re-run just that case;
  'r' resets and runs the whole matrix again.
===========================================================================
```

#### Olimex ESP32-POE

```txt
================================= SUMMARY =================================
case  prio  addr    scenario                     events  ntp   sw(s) ntp(s)
   0  ETH   dhcp    1  P > S > P > S > P         PASS    PASS    7.7    3.6
   1  ETH   dhcp    2  P > S > disc > S > P      PASS    PASS   30.9    5.0
   2  ETH   dhcp    3  P > S > disc > P > S > P  PASS    PASS   31.3    3.1
   3  ETH   dhcp    4  S > P > S > P             PASS    PASS   12.5    4.6
   4  ETH   dhcp    5  S > disc > S > P > S > P  PASS    PASS   30.8    4.8
   5  ETH   dhcp    6  S > disc > P > S > P      PASS    PASS   30.6    4.0
   6  ETH   dhcp    7  disc > S > P > S > P      PASS    PASS   30.0    3.8
   7  ETH   dhcp    8  disc > P > S > P          PASS    PASS   30.0    4.2
   8  ETH   static  1  P > S > P > S > P         PASS    PASS    7.4    5.2
   9  ETH   static  2  P > S > disc > S > P      PASS    PASS   30.7    4.0
  10  ETH   static  3  P > S > disc > P > S > P  PASS    PASS   31.0    3.9
  11  ETH   static  4  S > P > S > P             PASS    PASS   11.9    3.2
  12  ETH   static  5  S > disc > S > P > S > P  PASS    PASS   30.5    4.7
  13  ETH   static  6  S > disc > P > S > P      PASS    PASS   31.9    4.0
  14  ETH   static  7  disc > S > P > S > P      PASS    PASS   30.0    4.7
  15  ETH   static  8  disc > P > S > P          PASS    PASS   30.0    4.8
  16  WiFi  dhcp    1  P > S > P > S > P         PASS    PASS   17.1    4.8
  17  WiFi  dhcp    2  P > S > disc > S > P      PASS    PASS   24.4    4.7
  18  WiFi  dhcp    3  P > S > disc > P > S > P  PASS    PASS   31.3    4.7
  19  WiFi  dhcp    4  S > P > S > P             PASS    PASS   17.2    4.2
  20  WiFi  dhcp    5  S > disc > S > P > S > P  PASS    PASS   32.5    4.8
  21  WiFi  dhcp    6  S > disc > P > S > P      PASS    PASS   31.7    4.8
  22  WiFi  dhcp    7  disc > S > P > S > P      PASS    PASS   30.0    3.1
  23  WiFi  dhcp    8  disc > P > S > P          PASS    PASS   30.0    2.7
  24  WiFi  static  1  P > S > P > S > P         PASS    PASS    8.4    4.6
  25  WiFi  static  2  P > S > disc > S > P      PASS    PASS   31.3    3.8
  26  WiFi  static  3  P > S > disc > P > S > P  PASS    PASS   30.8    4.6
  27  WiFi  static  4  S > P > S > P             PASS    PASS   15.7    4.1
  28  WiFi  static  5  S > disc > S > P > S > P  PASS    PASS   31.9    4.4
  29  WiFi  static  6  S > disc > P > S > P      PASS    PASS   31.7    2.5
  30  WiFi  static  7  disc > S > P > S > P      PASS    PASS   30.0    4.4
  31  WiFi  static  8  disc > P > S > P          PASS    PASS   30.0    4.5

  32/32 cases fully passed
  (prio = primary interface; sw = worst ENTER->event, approximate;
   ntp = worst event->sync.)
  Type a case number + ENTER to re-run just that case;
  'r' resets and runs the whole matrix again.
===========================================================================
```

### Platform: ESP8266

#### NodeMCU + Wiznet W5500

```txt
================================= SUMMARY =================================
case  prio  addr    scenario                     events  ntp   sw(s) ntp(s)
   0  ETH   dhcp    1  P > S > P > S > P         PASS    PASS    6.5    0.3
   1  ETH   dhcp    2  P > S > disc > S > P      PASS    PASS   20.7   30.2
   2  ETH   dhcp    3  P > S > disc > P > S > P  PASS    PASS   20.7   30.1
   3  ETH   dhcp    4  S > P > S > P             PASS    PASS   20.0    0.4
   4  ETH   dhcp    5  S > disc > S > P > S > P  PASS    PASS   31.3    0.9
   5  ETH   dhcp    6  S > disc > P > S > P      PASS    PASS   20.9    0.0
   6  ETH   dhcp    7  disc > S > P > S > P      PASS    PASS   21.4    0.1
   7  ETH   dhcp    8  disc > P > S > P          PASS    PASS   20.0    0.4
   8  ETH   static  1  P > S > P > S > P         PASS    PASS    6.7    0.1
   9  ETH   static  2  P > S > disc > S > P      PASS    PASS   28.7   30.1
  10  ETH   static  3  P > S > disc > P > S > P  PASS    PASS   20.8   30.0
  11  ETH   static  4  S > P > S > P             PASS    PASS   18.7    0.1
  12  ETH   static  5  S > disc > S > P > S > P  PASS    PASS   20.6    1.0
  13  ETH   static  6  S > disc > P > S > P      PASS    PASS   20.7    0.6
  14  ETH   static  7  disc > S > P > S > P      PASS    PASS   20.0    0.3
  15  ETH   static  8  disc > P > S > P          PASS    PASS   20.0   30.1
  16  WiFi  dhcp    1  P > S > P > S > P         PASS    PASS   14.5    0.4
  17  WiFi  dhcp    2  P > S > disc > S > P      PASS    PASS   20.5    1.0
  18  WiFi  dhcp    3  P > S > disc > P > S > P  PASS    PASS   20.9    1.5
  19  WiFi  dhcp    4  S > P > S > P             PASS    PASS   11.5    0.3
  20  WiFi  dhcp    5  S > disc > S > P > S > P  PASS    PASS   20.5   30.4
  21  WiFi  dhcp    6  S > disc > P > S > P      PASS    PASS   20.6    0.3
  22  WiFi  dhcp    7  disc > S > P > S > P      PASS    PASS   20.0    0.2
  23  WiFi  dhcp    8  disc > P > S > P          PASS    PASS   20.7    0.3
  24  WiFi  static  1  P > S > P > S > P         PASS    PASS   15.0    0.3
  25  WiFi  static  2  P > S > disc > S > P      PASS    PASS   20.4    0.3
  26  WiFi  static  3  P > S > disc > P > S > P  PASS    PASS   20.4    0.2
  27  WiFi  static  4  S > P > S > P             PASS    PASS   14.5    0.0
  28  WiFi  static  5  S > disc > S > P > S > P  PASS    PASS   20.5   30.1
  29  WiFi  static  6  S > disc > P > S > P      PASS    PASS   20.5   30.4
  30  WiFi  static  7  disc > S > P > S > P      PASS    PASS   20.0    0.2
  31  WiFi  static  8  disc > P > S > P          PASS    PASS   20.0    0.2

  32/32 cases fully passed
  (prio = primary interface; sw = worst ENTER->event, approximate;
   ntp = worst event->sync.)
  Type a case number + ENTER to re-run just that case;
  'r' resets and runs the whole matrix again.
===========================================================================
```
