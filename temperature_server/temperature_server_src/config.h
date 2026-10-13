/**
 * @file config.h
 * @brief Compile-time configuration of the XIAO ESP32C6 temperature server.
 *
 * Implements: REQ-GEN-02 (all fixed settings live in this one file),
 *             REQ-SEC-03 (AP password length check).
 *
 * Everything the firmware needs to be tuned is here. Values that the user
 * changes WITHOUT reflashing (Wi-Fi credentials, node names, PIN ...) are not
 * in this file: they are edited in the web UI and stored in flash (store.cpp).
 * The list matches Section 7 of requirements.md.
 *
 * This header contains only #defines and static_asserts, so the host unit
 * tests can include it too.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
#define SERVER_FW_VERSION       1       // Shown on the web page and in the log (REQ-GEN-05)

// ---------------------------------------------------------------------------
// Radio / Wi-Fi
// ---------------------------------------------------------------------------
#define ESPNOW_CHANNEL_DEFAULT  1       // AP channel until a router channel has been learned
#define USE_EXTERNAL_ANTENNA    0       // 0 = on-board antenna, 1 = external (U.FL)

#define STA_CONNECT_TIMEOUT_S   15      // Router connect timeout at boot / on retry
#define STA_LOST_FALLBACK_S     30      // Router link lost this long -> start the fallback AP
#define STA_RETRY_INTERVAL_S    300     // Router retry period while in fallback mode
#define AP_GRACE_S              60      // Keep the AP this long after the router is back

#define AP_SSID                 "RoomTemp-Setup"
#define AP_PASSWORD             "roomtemp123"   // >= 8 characters. CHANGE BEFORE USE.
#define MDNS_HOSTNAME           "roomtemp"      // -> http://roomtemp.local
#define NTP_SERVER              "pool.ntp.org"
#define TIMEZONE                "UTC0"          // POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"

// ---------------------------------------------------------------------------
// Nodes and reception
// ---------------------------------------------------------------------------
#define MAX_NODES               4       // Node slots (one per corner, so at most 4)
#define DEFAULT_CPU_OFFSET_CC   0       // Initial CPU->room offset, hundredths of a degree C
#define DEFAULT_ROOM_NAME       "Living room"
#define STUCK_FACTOR            1.5f    // A node is stuck after STUCK_FACTOR x its sleep time (REQ-STATE-06)
                                        // NOTE: one lost cycle already exceeds 1.5, so a node then shows
                                        // "stuck" until its next packet. Use ~2.2 to tolerate one miss.
#define RX_QUEUE_LEN            16      // Frames buffered between the Wi-Fi task and loop()
#define RX_LED_ENABLED          1       // Blink the on-board LED for every new packet
#define RX_LED_PULSE_MS         50

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
#define HIST_POINTS             768     // Samples per node in RAM (~8 days at 15 min)
#define HIST_SAVE_INTERVAL_S    3600    // Flash snapshot period
#define HIST_API_MAX_POINTS     300     // Most points one history response may contain

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------
#define UI_POLL_S               10      // How often the page asks for fresh state
#define UI_TEMP_MIN_C           15      // Cold end of the colour scale
#define UI_TEMP_MAX_C           30      // Warm end of the colour scale

// ---------------------------------------------------------------------------
// Security
// ---------------------------------------------------------------------------
#define DEFAULT_PIN             "1234"  // Initial settings PIN, 4-8 digits. CHANGE IN THE UI.
#define PIN_MAX_ATTEMPTS        5       // Wrong PINs in a row before the lockout
#define PIN_LOCKOUT_S           60
#define FACTORY_RESET_HOLD_S    10      // BOOT button hold time (first 30 s after power-up)

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------
#define WDT_TIMEOUT_S           30      // loop() must run at least this often
#define DAILY_RESTART_ENABLED   0       // 1 = restart once a day (see DAILY_RESTART_HOUR)
#define DAILY_RESTART_HOUR      4       // Local hour of the optional restart

// ---------------------------------------------------------------------------
// Debug output over USB serial
// ---------------------------------------------------------------------------
#define DEBUG_ENABLED           0       // 1 = print debug log, 0 = no serial at all
#define SERIAL_BAUD             115200
#define SERIAL_WAIT_MS          1000    // Max wait for the USB-CDC connection

// ---------------------------------------------------------------------------
// Sanity checks: catch bad settings at compile time instead of on the device
// ---------------------------------------------------------------------------
static_assert(MAX_NODES >= 1 && MAX_NODES <= 4,
              "MAX_NODES must be 1..4 (one corner per node)");
static_assert(HIST_POINTS >= 16 && HIST_POINTS <= 65000,
              "HIST_POINTS must be 16..65000 (16-bit ring indices)");
static_assert(HIST_API_MAX_POINTS >= 2, "HIST_API_MAX_POINTS must be >= 2");
static_assert(RX_QUEUE_LEN >= 4, "RX_QUEUE_LEN must be >= 4 (3 copies per packet)");
static_assert(ESPNOW_CHANNEL_DEFAULT >= 1 && ESPNOW_CHANNEL_DEFAULT <= 13,
              "ESPNOW_CHANNEL_DEFAULT must be 1..13");
static_assert(STUCK_FACTOR > 1.0f, "STUCK_FACTOR must be > 1.0 or healthy nodes look stuck");
static_assert(sizeof(AP_PASSWORD) - 1 >= 8, "AP_PASSWORD must have at least 8 characters");
static_assert(sizeof(DEFAULT_PIN) - 1 >= 4 && sizeof(DEFAULT_PIN) - 1 <= 8,
              "DEFAULT_PIN must have 4..8 characters");
static_assert(UI_TEMP_MAX_C > UI_TEMP_MIN_C, "UI_TEMP_MAX_C must be above UI_TEMP_MIN_C");
static_assert(WDT_TIMEOUT_S >= 10, "WDT_TIMEOUT_S too small for a snapshot or Wi-Fi scan");

#endif  // CONFIG_H
