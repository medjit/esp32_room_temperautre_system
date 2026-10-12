/**
 * @file config.h
 * @brief Compile-time configuration of the XIAO ESP32C6 temperature node.
 *
 * Implements: REQ-GEN-02 (all settings are compile-time, in this one file).
 *
 * Every tunable value of the firmware lives here. Nothing is configured at
 * run time. Change a value, then compile and upload again.
 * The list matches Section 7 of requirements.md.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
#define NODE_ID               1      // Unique node identifier (0-255), set per node
#define FW_VERSION            1      // Firmware version, sent in every packet (REQ-GEN-06)

// ---------------------------------------------------------------------------
// Radio / ESP-NOW
// ---------------------------------------------------------------------------
#define ESPNOW_CHANNEL        1      // Wi-Fi channel 1-13, must match the receiver
#define USE_EXTERNAL_ANTENNA  0      // 0 = on-board antenna, 1 = external (U.FL)
#define TX_REPEAT_COUNT       3      // How many times each packet is broadcast
#define TX_REPEAT_DELAY_MS    10     // Pause between two transmissions
#define TX_CB_TIMEOUT_MS      50     // Max wait for the "sent" callback per transmission

// Transmitter power. Leave commented out to use the core's default (REQ-RF-08).
// Example: #define TX_POWER WIFI_POWER_19_5dBm
// #define TX_POWER WIFI_POWER_19_5dBm

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------
#define DS18B20_RESOLUTION    12     // DS18B20 resolution in bits, 9-12
#define CPU_TEMP_SAMPLES      8      // Readings of the internal temperature sensor
#define ADC_SAMPLES           16     // Samples per ADC channel

// ---------------------------------------------------------------------------
// Voltage dividers (two equal resistors -> ratio 2.0)
// ---------------------------------------------------------------------------
#define BAT_DIVIDER_RATIO     2.0f   // Battery divider on A1
#define VBUS_DIVIDER_RATIO    2.0f   // 5V-pin divider on A0

// ---------------------------------------------------------------------------
// Battery thresholds (millivolts)
// ---------------------------------------------------------------------------
#define BAT_NORMAL_MV         3300   // Above this: NORMAL
#define BAT_LOW_MV            3000   // Above this (and <= NORMAL): LOW, otherwise EMPTY
#define BAT_HYSTERESIS_MV     50     // Extra margin needed to move to a better state
#define BAT_VALID_MIN_MV      2500   // Below this the reading is INVALID
#define BAT_VALID_MAX_MV      4500   // Above this the reading is INVALID

#define CHARGE_DETECT_MV      4000   // 5V pin above this = external power present

// ---------------------------------------------------------------------------
// Sleep durations (seconds), chosen by battery state
// ---------------------------------------------------------------------------
#define SLEEP_NORMAL_S        900    // 15 min
#define SLEEP_LOW_S           3600   // 1 h
#define SLEEP_EMPTY_S         18000  // 5 h
#define SLEEP_INVALID_S       3600   // 1 h

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------
#define WDT_TIMEOUT_S         10     // A cycle longer than this is a hang -> reset

// ---------------------------------------------------------------------------
// Debug output over USB serial
// ---------------------------------------------------------------------------
#define DEBUG_ENABLED         0      // 1 = print debug log, 0 = no serial at all
#define SERIAL_BAUD           115200
#define SERIAL_WAIT_MS        1000   // Max wait for the USB-CDC connection

// ---------------------------------------------------------------------------
// Sanity checks: catch bad settings at compile time instead of on the device
// ---------------------------------------------------------------------------
static_assert(DS18B20_RESOLUTION >= 9 && DS18B20_RESOLUTION <= 12,
              "DS18B20_RESOLUTION must be 9..12");
static_assert(ESPNOW_CHANNEL >= 1 && ESPNOW_CHANNEL <= 13,
              "ESPNOW_CHANNEL must be 1..13");
static_assert(CPU_TEMP_SAMPLES >= 3, "CPU_TEMP_SAMPLES must be >= 3 (min/max are dropped)");
static_assert(ADC_SAMPLES >= 3,      "ADC_SAMPLES must be >= 3 (min/max are dropped)");
static_assert(TX_REPEAT_COUNT >= 1,  "TX_REPEAT_COUNT must be >= 1");

#endif  // CONFIG_H
