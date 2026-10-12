/**
 * @file temperature_node_src.ino
 * @brief XIAO ESP32C6 battery-powered temperature node (ESP-NOW transmitter).
 *
 * WHAT IT DOES
 *   Every time the chip wakes up it runs ONE linear procedure inside setup():
 *     LED on -> watchdog -> CPU temp -> battery -> 5V pin -> DS18B20
 *     -> build packet -> ESP-NOW broadcast x3 -> radio off -> LED off -> deep sleep
 *   Then the chip sleeps (timer wake-up only) and the whole thing repeats.
 *   loop() is never reached.
 *
 * DESIGN RULES
 *   - Simplicity first: no RTOS tasks, no state machines, no dynamic memory.
 *   - Fail forward: an error is written to status_flags and the cycle continues.
 *   - No compensation or filtering of values (REQ-GEN-05); only the averaging
 *     of REQ-ADC-03 / REQ-CPU-02 and the factory ADC calibration.
 *
 * REQUIREMENT TRACEABILITY
 *   Every function below lists the REQ-xxx identifiers it implements in its
 *   header comment. Quick map (requirement group -> where to look):
 *
 *     REQ-GEN-*   whole sketch + config.h        REQ-PKT-*  NodePacket, buildPacket()
 *     REQ-BOOT-*  setup(), ledOn(), wdtStart(),  REQ-RF-*   rfSwitchInit(), sendPacket(),
 *                 rtcStateInit(), getResetInfo()             radioOff()
 *     REQ-CPU-*   measureCpuTemp()               REQ-LED-*  ledOn(), ledOff(), goToSleep()
 *     REQ-ADC-*   readPinMv(), readAdcMv()       REQ-SLP-*  sleepTimeForState(), goToSleep()
 *     REQ-BAT-*   classifyBattery()              REQ-WDT-*  wdtStart(), getResetInfo()
 *     REQ-CHG-*   setup()                        REQ-DBG-*  debug macros, DBG_PRINTF calls
 *     REQ-DS-*    readDs18b20()                  REQ-ERR-*  g_flags, sendPacket()
 *
 * Board : XIAO_ESP32C6, ESP32 Arduino core 3.x (REQ-GEN-01)
 * Libs  : OneWire, DallasTemperature (only external libraries, REQ-GEN-03)
 */

// ===========================================================================
// 1. INCLUDES
// ===========================================================================
#include "config.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_rom_crc.h>
#include <driver/gpio.h>

#include <OneWire.h>
#include <DallasTemperature.h>

// ===========================================================================
// 2. PIN DEFINITIONS (REQ section 4.1)
// ===========================================================================
#define PIN_VBUS_ADC      0    // A0 / D0, 5V pin through 2 x 220k divider
#define PIN_BAT_ADC       1    // A1 / D1, BAT+ through 2 x 220k divider
#define PIN_DS18B20       23   // D5, DS18B20 data line (4.7k pull-up to 3V3)
#define PIN_LED           15   // On-board user LED, ACTIVE LOW
#define PIN_RF_SWITCH_EN  3    // RF switch enable, LOW = enabled
#define PIN_ANT_SELECT    14   // Antenna select, LOW = on-board, HIGH = external

// ===========================================================================
// 3. CONSTANTS
// ===========================================================================
#define TEMP_INVALID      ((int16_t)-32768)   // "No valid temperature" marker

// status_flags bit masks (REQ-PKT-03)
#define FLAG_DS_ABSENT     (1 << 0)   // DS18B20 not detected
#define FLAG_DS_ERR        (1 << 1)   // DS18B20 detected but reading invalid
#define FLAG_BAT_INVALID   (1 << 2)   // Battery voltage outside valid range
#define FLAG_CPU_TEMP_ERR  (1 << 3)   // CPU temperature reading error
#define FLAG_WDT_RESET     (1 << 4)   // Previous cycle ended in a watchdog reset

// Battery states (REQ-BAT-02). UNKNOWN is internal only, never transmitted.
#define BAT_STATE_NORMAL   0
#define BAT_STATE_LOW      1
#define BAT_STATE_EMPTY    2
#define BAT_STATE_INVALID  3
#define BAT_STATE_UNKNOWN  0xFF

// Value that proves the RTC memory was written by this firmware (REQ-BOOT-06)
#define RTC_MAGIC          0xC6A5E5B1UL

// Valid ranges used to judge raw sensor readings
#define CPU_TEMP_MIN_C     (-40.0f)
#define CPU_TEMP_MAX_C     (125.0f)
#define DS_TEMP_MIN_C      (-55.0f)
#define DS_TEMP_MAX_C      (125.0f)
#define DS_POWER_ON_CC     8500       // 85.00 C = DS18B20 power-on reset value

// DS18B20 conversion timeout: 94 ms at 9 bit, doubling per bit, plus 50 %
// (REQ-DS-04). Result: 141 / 282 / 564 / 1128 ms for 9 / 10 / 11 / 12 bit.
#define DS_TIMEOUT_MS      (((94 << (DS18B20_RESOLUTION - 9)) * 3) / 2)
#define DS_POLL_MS         10         // How often to ask "conversion done?"

// ===========================================================================
// 4. PACKET STRUCTURE (REQ-PKT-01..05)
//    Everything between BEGIN and END can be copied unchanged to the receiver.
// ===========================================================================
// ===== BEGIN NodePacket — copy unchanged into the receiver project =====
#define PACKET_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t  packet_version;     // Packet format version (starts at 1)
    uint16_t fw_version;         // Firmware version
    uint8_t  node_id;            // Node identifier
    uint32_t boot_count;         // Cycle counter (same in all 3 copies)
    uint8_t  is_charging;        // 0 / 1
    uint16_t battery_mv;         // Battery voltage, mV
    uint8_t  battery_state;      // 0 NORMAL, 1 LOW, 2 EMPTY, 3 INVALID
    uint16_t vbus_mv;            // 5V pin voltage, mV
    uint32_t sleep_time_s;       // Seconds until the next cycle
    int16_t  cpu_temp_cC;        // CPU temperature, 0.01 C, or -32768
    int16_t  ds18b20_temp_cC;    // DS18B20 temperature, 0.01 C, or -32768
    uint8_t  status_flags;       // FLAG_* bit field
    uint32_t crc32;              // esp_rom_crc32_le(0, bytes, 23) of fields above
} NodePacket;

static_assert(sizeof(NodePacket) == 27, "NodePacket size mismatch");
static_assert(sizeof(NodePacket) <= 250, "NodePacket exceeds ESP-NOW limit");
// ===== END NodePacket =====

// ===========================================================================
// 5. STATE
// ===========================================================================

/** Data that must survive deep sleep and watchdog resets (REQ-BOOT-06). */
typedef struct {
    uint32_t magic;            // RTC_MAGIC when the content is valid
    uint32_t boot_count;       // Number of cycles since power was applied
    uint8_t  prev_bat_state;   // Battery state of the previous cycle (REQ-BAT-04)
} RtcState;

/** All measured values of one cycle, collected in setup() and given to buildPacket(). */
typedef struct {
    int16_t  cpuTemp;       // 0.01 degC or TEMP_INVALID
    uint16_t batteryMv;
    uint8_t  batteryState;
    uint16_t vbusMv;
    bool     charging;
    int16_t  dsTemp;        // 0.01 degC or TEMP_INVALID
    uint32_t sleepSeconds;
} CycleData;

// RTC_NOINIT_ATTR = not cleared on software / watchdog / deep-sleep resets,
// only lost when power is removed.
RTC_NOINIT_ATTR RtcState g_rtc;

/** Status flags collected during this cycle; copied into the packet. */
static uint8_t g_flags = 0;

/** Why the chip started this cycle (filled by getResetInfo()). */
static esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;

/** Written by the ESP-NOW send callback, read by the sending code. */
static volatile bool                 g_txDone   = false;
static volatile esp_now_send_status_t g_txStatus = ESP_NOW_SEND_FAIL;

// ===========================================================================
// 6. DEBUG MACROS (REQ-DBG-01..04)
//    With DEBUG_ENABLED 0 every macro expands to nothing, so no serial code
//    is compiled and the serial port is never initialised.
// ===========================================================================
#if DEBUG_ENABLED
  // Start USB serial, wait up to SERIAL_WAIT_MS for a PC, then carry on anyway.
  #define DBG_BEGIN()  do { Serial.begin(SERIAL_BAUD);                       \
                            uint32_t t0_ = millis();                         \
                            while (!Serial && millis() - t0_ < SERIAL_WAIT_MS) {} \
                          } while (0)
  // printf with a "[ time ms]" prefix (REQ-DBG-04) and a trailing newline.
  #define DBG_PRINTF(fmt, ...) Serial.printf("[%5lu ms] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
  #define DBG_FLUSH()  Serial.flush()
  // Run a statement only in debug builds (used for debug-only helper calls).
  #define DBG_RUN(stmt) stmt
#else
  #define DBG_BEGIN()
  #define DBG_PRINTF(fmt, ...)
  #define DBG_FLUSH()
  #define DBG_RUN(stmt)
#endif

// ===========================================================================
// 7. HELPERS: trimmed mean, unit conversion, debug names
// ===========================================================================

/** Running sum / min / max, so a trimmed mean needs no array and no sorting. */
typedef struct {
    float    sum;
    float    minV;
    float    maxV;
    uint16_t count;
} TrimAcc;

/** Reset an accumulator before a new series of samples. */
static void accInit(TrimAcc &a) {
    a.sum   = 0.0f;
    a.minV  = 1e30f;
    a.maxV  = -1e30f;
    a.count = 0;
}

/** Add one sample to the accumulator. */
static void accAdd(TrimAcc &a, float v) {
    a.sum += v;
    if (v < a.minV) a.minV = v;
    if (v > a.maxV) a.maxV = v;
    a.count++;
}

/**
 * Mean of all samples after discarding the lowest and the highest one.
 * Used for both the CPU temperature (REQ-CPU-02) and the ADC (REQ-ADC-03).
 * Needs at least 3 samples (guaranteed by static_asserts in config.h).
 */
static float accTrimmedMean(const TrimAcc &a) {
    return (a.sum - a.minV - a.maxV) / (float)(a.count - 2);
}

/** Convert degrees Celsius to hundredths of a degree, rounded (REQ-CPU-03, REQ-DS-05). */
static int16_t toCentiDegrees(float celsius) {
    return (int16_t)lroundf(celsius * 100.0f);
}

#if DEBUG_ENABLED
/** Human readable reset reason for the debug log (REQ-BOOT-04). */
static const char *resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_BROWNOUT: return "brown-out";
        case ESP_RST_DEEPSLEEP:return "deep-sleep wake";
        case ESP_RST_SW:       return "software reset";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_PANIC:    return "panic";
        default:               return "other";
    }
}

/** Human readable wake-up cause for the debug log (REQ-BOOT-04). */
static const char *wakeCauseName(esp_sleep_wakeup_cause_t c) {
    switch (c) {
        case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "none (not from sleep)";
        default:                         return "other";
    }
}

/** Human readable battery state for the debug log. */
static const char *batStateName(uint8_t s) {
    switch (s) {
        case BAT_STATE_NORMAL:  return "NORMAL";
        case BAT_STATE_LOW:     return "LOW";
        case BAT_STATE_EMPTY:   return "EMPTY";
        case BAT_STATE_INVALID: return "INVALID";
        default:                return "UNKNOWN";
    }
}
#endif  // DEBUG_ENABLED

// ===========================================================================
// 8. STEP FUNCTIONS — LED, WATCHDOG, RTC STATE, RESET INFO
// ===========================================================================

/**
 * Switch the on-board LED on.
 *
 * Implements: REQ-BOOT-01, REQ-LED-01, REQ-LED-03.
 *
 * The LED is active low (GPIO15 LOW = lit). After deep sleep the pin may still
 * be latched by the "hold" function that ledOff() enabled, so the hold is
 * released first; otherwise digitalWrite() would have no effect.
 * Works the same whether DEBUG_ENABLED is 0 or 1.
 */
static void ledOn() {
    gpio_hold_dis((gpio_num_t)PIN_LED);   // Release latch from the last sleep
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);           // LOW = LED lit
}

/**
 * Switch the LED off and keep it off during deep sleep.
 *
 * Implements: REQ-LED-01, REQ-LED-02.
 *
 * Driving the pin HIGH turns the LED off, but in deep sleep the pin would
 * float and the LED could glow faintly. Latching the pin level with the GPIO
 * hold feature keeps it HIGH for the whole sleep.
 * (Fallback if it still glows: use pinMode(PIN_LED, INPUT) instead.)
 */
static void ledOff() {
    digitalWrite(PIN_LED, HIGH);          // HIGH = LED dark
    gpio_hold_en((gpio_num_t)PIN_LED);    // Latch this level (kept in deep sleep on ESP32-C6)
}

/**
 * Start the hang-protection watchdog.
 *
 * Implements: REQ-BOOT-02, REQ-WDT-01, REQ-WDT-02.
 *
 * The Arduino core already runs a task watchdog with a short timeout. We
 * reconfigure it to WDT_TIMEOUT_S and subscribe the main task. The watchdog
 * is NEVER fed: a normal cycle (about 1-1.5 s) ends in deep sleep long before
 * the timeout, so it can only fire if the code really hangs. On timeout the
 * chip panics and resets (trigger_panic = true).
 */
static void wdtStart() {
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms     = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;        // Watch only our task, not the idle tasks
    cfg.trigger_panic  = true;     // Reset the chip when the timeout expires

    // If the core has not started the watchdog yet, init it; else reconfigure it.
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
        esp_task_wdt_init(&cfg);
    }
    esp_task_wdt_add(NULL);        // Subscribe current task (ok if already added)
}

/**
 * Validate the RTC memory and count this cycle.
 *
 * Implements: REQ-BOOT-05, REQ-BOOT-06, REQ-BAT-04.
 *
 * The counter must survive deep sleep and watchdog resets but start from zero
 * after power loss. So the memory is cleared when the reset reason is
 * power-on / brown-out, or when the magic value is wrong (never initialised).
 * boot_count is incremented right away, so the first packet after power-on
 * carries boot_count = 1.
 */
static void rtcStateInit() {
    esp_reset_reason_t reason = esp_reset_reason();
    bool powerLost = (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT);

    if (powerLost || g_rtc.magic != RTC_MAGIC) {
        g_rtc.magic          = RTC_MAGIC;
        g_rtc.boot_count     = 0;
        g_rtc.prev_bat_state = BAT_STATE_UNKNOWN;
    }
    g_rtc.boot_count++;
}

/**
 * Find out why the chip started and flag a watchdog restart.
 *
 * Implements: REQ-BOOT-04, REQ-WDT-03.
 *
 * Any watchdog type (task, interrupt, other) sets FLAG_WDT_RESET so the
 * receiver learns that the previous cycle hung. The cycle then runs normally.
 */
static void getResetInfo() {
    g_resetReason = esp_reset_reason();

    if (g_resetReason == ESP_RST_TASK_WDT ||
        g_resetReason == ESP_RST_INT_WDT  ||
        g_resetReason == ESP_RST_WDT) {
        g_flags |= FLAG_WDT_RESET;
    }

    DBG_PRINTF("Reset reason : %s", resetReasonName(g_resetReason));
    DBG_PRINTF("Wake cause   : %s", wakeCauseName(esp_sleep_get_wakeup_cause()));
    DBG_PRINTF("boot_count   : %lu", (unsigned long)g_rtc.boot_count);
}

// ===========================================================================
// 9. STEP FUNCTIONS — MEASUREMENTS
// ===========================================================================

/**
 * Measure the internal CPU temperature.
 *
 * Implements: REQ-BOOT-03, REQ-CPU-01, REQ-CPU-02, REQ-CPU-03, REQ-CPU-04.
 *
 * MUST be the first measurement of the cycle: the chip heats up while it
 * works, so we read it right after boot (before serial, ADC, radio).
 * Takes CPU_TEMP_SAMPLES readings, drops the lowest and highest, averages
 * the rest. A NaN or out-of-range reading marks the whole result invalid.
 *
 * @return Temperature in 0.01 degC, or TEMP_INVALID (and FLAG_CPU_TEMP_ERR set).
 */
static int16_t measureCpuTemp() {
    TrimAcc acc;
    accInit(acc);
    for (int i = 0; i < CPU_TEMP_SAMPLES; i++) {
        accAdd(acc, temperatureRead());      // Internal sensor, degC (NaN on error)
    }

    float t = accTrimmedMean(acc);
    if (isnan(t) || t < CPU_TEMP_MIN_C || t > CPU_TEMP_MAX_C) {
        g_flags |= FLAG_CPU_TEMP_ERR;
        return TEMP_INVALID;
    }
    return toCentiDegrees(t);
}

/**
 * Read one ADC pin and return the trimmed-mean voltage AT THE PIN in mV.
 *
 * Implements: REQ-ADC-01, REQ-ADC-02, REQ-ADC-03.
 *
 * Both pins (GPIO0, GPIO1) are on ADC1 and use 11 dB attenuation.
 * analogReadMilliVolts() applies the factory calibration. ADC_SAMPLES samples
 * are taken; the lowest and highest are discarded, the rest averaged.
 */
static float readPinMv(uint8_t pin) {
    analogReadResolution(12);
    analogSetPinAttenuation(pin, ADC_11db);

    TrimAcc acc;
    accInit(acc);
    for (int i = 0; i < ADC_SAMPLES; i++) {
        accAdd(acc, (float)analogReadMilliVolts(pin));
    }
    return accTrimmedMean(acc);
}

/**
 * Measure a voltage behind a resistor divider.
 *
 * Implements: REQ-ADC-04 (scaling), uses REQ-ADC-01..03 via readPinMv().
 *
 * @param pin    ADC pin of the divider midpoint.
 * @param ratio  Divider ratio (2.0 for two equal resistors).
 * @param name   Label used only in the debug log.
 * @return Real voltage in mV (pin voltage x ratio), limited to uint16_t.
 */
static uint16_t readAdcMv(uint8_t pin, float ratio, const char *name) {
    float pinMv  = readPinMv(pin);
    float realMv = pinMv * ratio;
    if (realMv > 65535.0f) realMv = 65535.0f;     // Protect the uint16_t cast

    DBG_PRINTF("%s: %d samples, pin %.0f mV, scaled %.0f mV",
               name, ADC_SAMPLES, pinMv, realMv);
    return (uint16_t)lroundf(realMv);
}

/**
 * Convert a battery voltage into a state, using the 'raised thresholds' of
 * the hysteresis rule. Helper of classifyBattery().
 *
 * @param mv      Battery voltage in mV.
 * @param margin  Amount added to each threshold (0 = plain thresholds).
 */
static uint8_t stateFromThresholds(uint16_t mv, uint16_t margin) {
    if (mv > BAT_NORMAL_MV + margin) return BAT_STATE_NORMAL;
    if (mv > BAT_LOW_MV    + margin) return BAT_STATE_LOW;
    return BAT_STATE_EMPTY;
}

/**
 * Decide the battery state, with hysteresis on the way up.
 *
 * Implements: REQ-BAT-02, REQ-BAT-03, REQ-BAT-04, REQ-BAT-05.
 *
 * Rules:
 *  - Outside BAT_VALID_MIN_MV..BAT_VALID_MAX_MV -> INVALID, flag set, and the
 *    stored previous state is NOT changed (done by the caller).
 *  - Going to a WORSE state happens immediately at the plain threshold.
 *  - Going to a BETTER state needs the voltage to exceed the threshold plus
 *    BAT_HYSTERESIS_MV, so it does not flip back and forth near a limit.
 *  - With no previous state (first boot) plain thresholds are used.
 *
 * State numbers grow as the battery gets worse (NORMAL 0 < LOW 1 < EMPTY 2).
 *
 * @param mv    Battery voltage in mV.
 * @param prev  Previous state, or BAT_STATE_UNKNOWN.
 * @return New battery state.
 */
static uint8_t classifyBattery(uint16_t mv, uint8_t prev) {
    if (mv < BAT_VALID_MIN_MV || mv > BAT_VALID_MAX_MV) {
        g_flags |= FLAG_BAT_INVALID;
        return BAT_STATE_INVALID;
    }

    uint8_t plain = stateFromThresholds(mv, 0);
    if (prev == BAT_STATE_UNKNOWN || plain >= prev) {
        return plain;                       // First boot, same, or worse: no hysteresis
    }

    // Candidate improvement: allowed only as far as the raised thresholds permit.
    uint8_t withMargin = stateFromThresholds(mv, BAT_HYSTERESIS_MV);
    return (withMargin < prev) ? withMargin : prev;
}

/**
 * Read the DS18B20 temperature sensor, if one is connected.
 *
 * Implements: REQ-DS-01 .. REQ-DS-07, REQ-ERR-01.
 *
 * Steps: detect the sensor (reset/presence + address search) -> set the
 * resolution -> start a non-blocking conversion -> poll until finished or the
 * timeout (derived from the resolution) -> validate the value.
 *
 *  - No device found: not an error. FLAG_DS_ABSENT, TEMP_INVALID.
 *  - Timeout, -127 C, exactly 85.00 C, or outside -55..125 C: FLAG_DS_ERR,
 *    TEMP_INVALID.
 *  - The sensor has no sleep command (REQ-DS-07), so none is sent. The 1-Wire
 *    bus is left idle (released, pulled high by the 4.7k resistor).
 *
 * @return Temperature in 0.01 degC, or TEMP_INVALID.
 */
static int16_t readDs18b20() {
    OneWire           oneWire(PIN_DS18B20);
    DallasTemperature sensors(&oneWire);
    DeviceAddress     addr;

    sensors.begin();
    if (!sensors.getAddress(addr, 0)) {            // Nobody answered
        g_flags |= FLAG_DS_ABSENT;
        DBG_PRINTF("DS18B20: not detected, skipped");
        return TEMP_INVALID;
    }
    DBG_PRINTF("DS18B20: found %02X%02X%02X%02X%02X%02X%02X%02X, %d bit",
               addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
               DS18B20_RESOLUTION);

    sensors.setResolution(addr, DS18B20_RESOLUTION);
    sensors.setWaitForConversion(false);           // Do not block inside the library
    sensors.requestTemperaturesByAddress(addr);

    // Poll for completion, but never longer than the timeout.
    uint32_t start = millis();
    while (!sensors.isConversionComplete()) {
        if (millis() - start > DS_TIMEOUT_MS) {
            g_flags |= FLAG_DS_ERR;
            DBG_PRINTF("DS18B20: ERROR conversion timeout (%d ms)", DS_TIMEOUT_MS);
            return TEMP_INVALID;
        }
        delay(DS_POLL_MS);
    }
    DBG_PRINTF("DS18B20: conversion took %lu ms (timeout %d ms)",
               (unsigned long)(millis() - start), DS_TIMEOUT_MS);

    float tempC = sensors.getTempC(addr);          // -127 if disconnected
    int16_t cC  = toCentiDegrees(tempC);
    bool bad = (tempC == DEVICE_DISCONNECTED_C) || (cC == DS_POWER_ON_CC) ||
               (tempC < DS_TEMP_MIN_C) || (tempC > DS_TEMP_MAX_C);
    if (bad) {
        g_flags |= FLAG_DS_ERR;
        DBG_PRINTF("DS18B20: ERROR invalid value %.2f C", tempC);
        return TEMP_INVALID;
    }
    DBG_PRINTF("DS18B20: %.2f C", tempC);
    return cC;
}

/**
 * Map the battery state to the sleep duration.
 *
 * Implements: REQ-SLP-01, REQ-SLP-04, REQ-CHG-03.
 *
 * Charging is deliberately NOT considered: the sleep time follows the battery
 * state only (REQ-CHG-03).
 *
 * @return Seconds to sleep.
 */
static uint32_t sleepTimeForState(uint8_t state) {
    switch (state) {
        case BAT_STATE_NORMAL: return SLEEP_NORMAL_S;
        case BAT_STATE_LOW:    return SLEEP_LOW_S;
        case BAT_STATE_EMPTY:  return SLEEP_EMPTY_S;
        default:               return SLEEP_INVALID_S;
    }
}

// ===========================================================================
// 10. STEP FUNCTIONS — PACKET
// ===========================================================================

/**
 * Fill the NodePacket and compute its CRC32.
 *
 * Implements: REQ-PKT-01 .. REQ-PKT-05, REQ-BAT-01, REQ-BAT-06, REQ-CHG-01,
 *             REQ-GEN-06.
 *
 * The CRC covers every byte before the crc32 field (offsetof = 23 bytes) and
 * is calculated with esp_rom_crc32_le(0, data, len). The packet is built once;
 * all transmitted copies are identical (REQ-RF-05).
 *
 * @param d  Values measured in this cycle.
 * @return   The finished packet.
 */
static NodePacket buildPacket(const CycleData &d) {
    NodePacket p;
    memset(&p, 0, sizeof(p));

    p.packet_version  = PACKET_VERSION;
    p.fw_version      = FW_VERSION;
    p.node_id         = NODE_ID;
    p.boot_count      = g_rtc.boot_count;
    p.is_charging     = d.charging ? 1 : 0;
    p.battery_mv      = d.batteryMv;
    p.battery_state   = d.batteryState;
    p.vbus_mv         = d.vbusMv;
    p.sleep_time_s    = d.sleepSeconds;
    p.cpu_temp_cC     = d.cpuTemp;
    p.ds18b20_temp_cC = d.dsTemp;
    p.status_flags    = g_flags;

    p.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&p, offsetof(NodePacket, crc32));
    return p;
}

#if DEBUG_ENABLED
/** Print every packet field, the size and the CRC (REQ-DBG-05). */
static void printPacket(const NodePacket &p) {
    DBG_PRINTF("Packet: ver=%u fw=%u node=%u boot=%lu",
               p.packet_version, p.fw_version, p.node_id, (unsigned long)p.boot_count);
    DBG_PRINTF("Packet: charging=%u bat=%u mV state=%u(%s) vbus=%u mV",
               p.is_charging, p.battery_mv, p.battery_state,
               batStateName(p.battery_state), p.vbus_mv);
    DBG_PRINTF("Packet: sleep=%lu s cpu=%d ds=%d flags=0x%02X",
               (unsigned long)p.sleep_time_s, p.cpu_temp_cC, p.ds18b20_temp_cC,
               p.status_flags);
    DBG_PRINTF("Packet: size=%u bytes crc32=0x%08lX",
               (unsigned)sizeof(p), (unsigned long)p.crc32);
}
#endif

// ===========================================================================
// 11. STEP FUNCTIONS — RADIO (ESP-NOW)
// ===========================================================================

/**
 * Helper to pull the first-parameter type out of the core's send-callback type.
 *
 * Why: the ESP-NOW send callback changed its first parameter between
 * ESP-IDF versions (a MAC pointer in older ones, a tx-info struct pointer in
 * newer ones). Instead of guessing the version, we ask the compiler what
 * esp_now_register_send_cb() expects and build a matching function.
 */
template <typename T> struct SendCbArg;
template <typename A>
struct SendCbArg<void (*)(A, esp_now_send_status_t)> { typedef A type; };

/**
 * ESP-NOW "packet was sent" callback. Records the result for transmitOnce().
 * The first parameter is unused, only the status matters.
 *
 * It lives inside a struct only because the Arduino IDE's automatic
 * prototype generator cannot handle free template functions.
 */
struct EspNowCallback {
    template <typename A>
    static void onDataSent(A, esp_now_send_status_t status) {
        g_txStatus = status;
        g_txDone   = true;
    }
};

/**
 * Set the RF switch and antenna selection pins.
 *
 * Implements: REQ-RF-02.
 *
 * GPIO3 LOW enables the RF switch. GPIO14 LOW picks the on-board antenna,
 * HIGH the external one (USE_EXTERNAL_ANTENNA). Must run before Wi-Fi starts.
 */
static void rfSwitchInit() {
    pinMode(PIN_RF_SWITCH_EN, OUTPUT);
    digitalWrite(PIN_RF_SWITCH_EN, LOW);
    pinMode(PIN_ANT_SELECT, OUTPUT);
    digitalWrite(PIN_ANT_SELECT, USE_EXTERNAL_ANTENNA ? HIGH : LOW);

    DBG_PRINTF("Antenna      : %s", USE_EXTERNAL_ANTENNA ? "external" : "on-board");
}

/**
 * Start Wi-Fi in station mode (no access point) on the fixed ESP-NOW channel.
 *
 * Implements: REQ-RF-03, REQ-RF-08.
 *
 * @return true when Wi-Fi is up and the channel is set.
 */
static bool wifiStart() {
    WiFi.mode(WIFI_STA);
#ifdef TX_POWER
    WiFi.setTxPower(TX_POWER);
#endif
    if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        DBG_PRINTF("Radio: ERROR setting channel");
        return false;
    }
    DBG_PRINTF("Radio        : MAC %s, channel %d",
               WiFi.macAddress().c_str(), ESPNOW_CHANNEL);
    return true;
}

/**
 * Initialise ESP-NOW and register the unencrypted broadcast peer.
 *
 * Implements: REQ-RF-04, REQ-ERR-02.
 *
 * @return true when ESP-NOW is ready to send; false on any failure.
 */
static bool espNowStart() {
    if (esp_now_init() != ESP_OK) {
        DBG_PRINTF("ESP-NOW: ERROR init failed, transmission skipped");
        return false;
    }
    esp_now_register_send_cb(EspNowCallback::onDataSent<SendCbArg<esp_now_send_cb_t>::type>);

    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, sizeof(peer.peer_addr));   // FF:FF:FF:FF:FF:FF
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        DBG_PRINTF("ESP-NOW: ERROR adding broadcast peer, transmission skipped");
        return false;
    }
    return true;
}

/**
 * Send the packet once and wait (briefly) for the send callback.
 *
 * Implements: REQ-RF-06.
 *
 * No retry is done on failure; the result is only logged.
 *
 * @param p    Packet to send.
 * @param seq  1-based transmission number, for the debug log.
 */
static void transmitOnce(const NodePacket &p, int seq) {
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    g_txDone = false;
    esp_err_t err = esp_now_send(broadcast, (const uint8_t *)&p, sizeof(p));
    if (err != ESP_OK) {
        DBG_PRINTF("TX %d/%d     : send call failed (err %d)", seq, TX_REPEAT_COUNT, (int)err);
        return;
    }

    uint32_t start = millis();
    while (!g_txDone && millis() - start < TX_CB_TIMEOUT_MS) {
        delay(1);
    }

    if (!g_txDone) {
        DBG_PRINTF("TX %d/%d     : no callback within %d ms", seq, TX_REPEAT_COUNT, TX_CB_TIMEOUT_MS);
    } else {
        DBG_PRINTF("TX %d/%d     : %s", seq, TX_REPEAT_COUNT,
                   g_txStatus == ESP_NOW_SEND_SUCCESS ? "success" : "FAILED");
    }
}

/**
 * Broadcast the packet TX_REPEAT_COUNT times over ESP-NOW.
 *
 * Implements: REQ-RF-01, REQ-RF-03 .. REQ-RF-06, REQ-RF-08, REQ-ERR-02.
 *
 * Called only after all measurements are done (REQ-RF-01). If Wi-Fi or
 * ESP-NOW cannot start, transmission is skipped and the node simply goes to
 * sleep as usual (REQ-ERR-02). All copies are identical (REQ-RF-05).
 */
static void sendPacket(const NodePacket &p) {
    rfSwitchInit();
    if (!wifiStart() || !espNowStart()) {
        return;
    }

    for (int seq = 1; seq <= TX_REPEAT_COUNT; seq++) {
        transmitOnce(p, seq);
        if (seq < TX_REPEAT_COUNT) {
            delay(TX_REPEAT_DELAY_MS);
        }
    }
}

/**
 * Shut the radio down completely.
 *
 * Implements: REQ-RF-07.
 *
 * Safe to call even if ESP-NOW never started (the deinit error is ignored).
 */
static void radioOff() {
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
}

// ===========================================================================
// 12. STEP FUNCTION — DEEP SLEEP
// ===========================================================================

/**
 * Enter deep sleep for the given time. Never returns.
 *
 * Implements: REQ-SLP-02, REQ-SLP-03, REQ-LED-01, REQ-LED-02, REQ-WDT-04.
 *
 * Order (REQ-SLP-03): radio is already off (done by the caller) -> print
 * awake time and sleep duration -> flush serial -> LED off -> sleep.
 * Wake-up is by timer only. The 64-bit cast is required because
 * 18000 s x 1,000,000 us does not fit in 32 bits.
 *
 * @param seconds  Sleep duration in seconds.
 */
static void goToSleep(uint32_t seconds) {
    DBG_PRINTF("Sleep        : %lu s", (unsigned long)seconds);
    DBG_PRINTF("Awake time   : %lu ms", (unsigned long)millis());
    DBG_FLUSH();

    ledOff();
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();
}

// ===========================================================================
// 13. setup() — THE WHOLE CYCLE, IN FIXED ORDER (requirements Section 5)
// ===========================================================================

/**
 * Run one complete measure-and-transmit cycle, then deep sleep.
 *
 * Implements: REQ-GEN-04, REQ-BOOT-01..03, REQ-ADC-05, REQ-CHG-01/02,
 *             REQ-RF-01, REQ-DBG-06, REQ-ERR-01.
 *
 * The order below must not change:
 *  1. LED on, 2. watchdog, 3. CPU temperature (first measurement!),
 *  4. serial (debug only), 5. battery BEFORE 5V pin and radio,
 *  6. 5V pin / charging, 7. DS18B20, 8. sleep time, 9. packet,
 *  10. radio on + send + radio off, 11. LED off + deep sleep.
 * Each step handles its own errors via g_flags, so later steps always run.
 */
void setup() {
    ledOn();                                         // REQ-BOOT-01
    wdtStart();                                      // REQ-BOOT-02

    CycleData d;
    d.cpuTemp = measureCpuTemp();                    // REQ-BOOT-03: first measurement

    DBG_BEGIN();                                     // REQ-DBG-02, REQ-DBG-03
    rtcStateInit();                                  // REQ-BOOT-05, REQ-BOOT-06
    DBG_PRINTF("Node %d, firmware %d, packet version %d",
               NODE_ID, FW_VERSION, PACKET_VERSION); // REQ-DBG-05 header
    getResetInfo();                                  // REQ-BOOT-04, REQ-WDT-03
    DBG_PRINTF("CPU temp     : %d samples, %.2f C", CPU_TEMP_SAMPLES, d.cpuTemp / 100.0f);

    d.batteryMv    = readAdcMv(PIN_BAT_ADC, BAT_DIVIDER_RATIO, "Battery"); // REQ-ADC-05
    d.batteryState = classifyBattery(d.batteryMv, g_rtc.prev_bat_state);
    DBG_PRINTF("Battery state: %s -> %s", batStateName(g_rtc.prev_bat_state),
               batStateName(d.batteryState));
    if (d.batteryState != BAT_STATE_INVALID) {
        g_rtc.prev_bat_state = d.batteryState;       // REQ-BAT-05: INVALID is not stored
    }

    d.vbusMv   = readAdcMv(PIN_VBUS_ADC, VBUS_DIVIDER_RATIO, "5V pin");
    d.charging = d.vbusMv > CHARGE_DETECT_MV;        // REQ-CHG-02
    DBG_PRINTF("Charging     : %s", d.charging ? "yes" : "no");

    d.dsTemp       = readDs18b20();                  // REQ-DS-*
    d.sleepSeconds = sleepTimeForState(d.batteryState);

    NodePacket packet = buildPacket(d);              // REQ-PKT-*
    DBG_RUN(printPacket(packet));

    sendPacket(packet);                              // REQ-RF-*
    radioOff();                                      // REQ-RF-07
    goToSleep(d.sleepSeconds);                       // REQ-SLP-*, never returns
}

/** Never reached: the node always ends setup() in deep sleep (REQ-GEN-04). */
void loop() {}
