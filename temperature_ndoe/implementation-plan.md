# XIAO ESP32C6 Temperature Node — Implementation Plan

| Field | Value |
|---|---|
| Document | Implementation Plan |
| Version | 1.0 |
| Date | 2026-10-12 |
| Based on | `requirements.md` v1.1 |
| Target hardware | Seeed Studio XIAO ESP32C6 |
| Development environment | Arduino IDE, ESP32 Arduino core 3.x |

---

## 1. Purpose

This document describes how the requirements defined in `requirements.md` shall be implemented. It specifies the repository layout, the toolchain setup, the internal structure of the firmware, the implementation phases with their verification steps, and the traceability between requirements and code.

## 2. Design Principles

1. **Simplicity first.** The firmware is a single linear procedure executed in `setup()`. No RTOS tasks, no state machines, no dynamic memory allocation.
2. **Two source files only.** All firmware code resides in a single sketch file. Only the compile-time configuration is kept in a separate header, `config.h`.
3. **Fixed execution order.** The order of operations defined in Section 5 of `requirements.md` is preserved exactly. Debug output never changes this order.
4. **Fail forward.** An error in any step is recorded in `status_flags` and execution continues. A packet is always transmitted if the radio can be initialised.
5. **Traceability.** Every function carries a comment listing the requirement identifiers it implements.

## 3. Repository Layout

The Arduino IDE requires the main sketch file to have the same name as the folder that contains it. The firmware therefore resides in a dedicated sub-folder of the repository root.

```
<repository root>/
├── .gitignore
├── README.md
├── requirements.md
├── implementation-plan.md
└── temperature_node_src/
    ├── temperature_node_src.ino    ← all firmware code
    └── config.h                    ← compile-time configuration only
```

| File | Content |
|---|---|
| `temperature_node_src/temperature_node_src.ino` | Includes, constants, packet structure, RTC state, debug macros, all functions, `setup()` and an empty `loop()` |
| `temperature_node_src/config.h` | All `#define` settings listed in Section 7 of `requirements.md`, with comments and default values |
| `requirements.md` | Software Requirements Specification |
| `implementation-plan.md` | This document |
| `README.md` | Short project description, wiring table, build and upload instructions |
| `.gitignore` | Excludes build output and IDE/OS artefacts (`build/`, `*.bin`, `*.elf`, `*.map`, `.vscode/`, `.DS_Store`) |

The repository is opened in the Arduino IDE by opening `temperature_node_src/temperature_node_src.ino`.

## 4. Toolchain Setup

| Item | Setting |
|---|---|
| IDE | Arduino IDE 2.x |
| Board package | "esp32" by Espressif Systems, version 3.x |
| Board | XIAO_ESP32C6 |
| USB CDC On Boot | Enabled (required for `Serial` over USB in DEBUG builds) |
| Libraries | `OneWire`, `DallasTemperature` (installed via Library Manager) |
| Serial monitor | 115200 baud |

The exact versions of the board package and libraries used for the first successful build shall be recorded in `README.md`.

## 5. Firmware Structure

### 5.1 Sections of `temperature_node_src.ino`

The sketch file is organised into the following sections, in this order:

| # | Section | Content |
|---|---|---|
| 1 | Includes | `config.h`, `WiFi.h`, `esp_now.h`, `esp_wifi.h`, `esp_sleep.h`, `esp_task_wdt.h`, `esp_rom_crc.h`, `driver/gpio.h`, `OneWire.h`, `DallasTemperature.h` |
| 2 | Pin definitions | `PIN_VBUS_ADC` (GPIO0), `PIN_BAT_ADC` (GPIO1), `PIN_DS18B20` (GPIO23), `PIN_LED` (GPIO15), `PIN_RF_SWITCH_EN` (GPIO3), `PIN_ANT_SELECT` (GPIO14) |
| 3 | Constants | `PACKET_VERSION`, `TEMP_INVALID`, status flag bit masks, battery state values, RTC magic value |
| 4 | Packet structure | `NodePacket` — clearly delimited block (see 5.2) to be copied unchanged into the receiver |
| 5 | RTC state | Non-initialised RTC structure holding `magic`, `boot_count`, `prev_bat_state` |
| 6 | Debug macros | `DBG_BEGIN()`, `DBG_PRINTF(...)`, `DBG_FLUSH()`, compiled out when `DEBUG_ENABLED == 0` |
| 7 | Helper functions | Trimmed mean, temperature conversion to hundredths of a degree |
| 8 | Step functions | One function per step of the operating cycle (see 5.3) |
| 9 | `setup()` | Calls the step functions in the fixed order |
| 10 | `loop()` | Empty |

### 5.2 Packet Structure

```cpp
// ===== BEGIN NodePacket — copy unchanged into the receiver project =====
#define PACKET_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t  packet_version;
    uint16_t fw_version;
    uint8_t  node_id;
    uint32_t boot_count;
    uint8_t  is_charging;
    uint16_t battery_mv;
    uint8_t  battery_state;
    uint16_t vbus_mv;
    uint32_t sleep_time_s;
    int16_t  cpu_temp_cC;
    int16_t  ds18b20_temp_cC;
    uint8_t  status_flags;
    uint32_t crc32;
} NodePacket;

static_assert(sizeof(NodePacket) == 27, "NodePacket size mismatch");
static_assert(sizeof(NodePacket) <= 250, "NodePacket exceeds ESP-NOW limit");
// ===== END NodePacket =====
```

The CRC32 is calculated over `offsetof(NodePacket, crc32)` bytes, i.e. the first 23 bytes.

### 5.3 Function List

| Function | Responsibility | Requirements |
|---|---|---|
| `ledOn()` / `ledOff()` | Release GPIO hold, drive LED; set LED off and enable hold before sleep | REQ-BOOT-01, REQ-LED-01..03 |
| `wdtStart()` | Reconfigure the task watchdog to `WDT_TIMEOUT_S` with panic enabled and subscribe the current task | REQ-BOOT-02, REQ-WDT-02 |
| `rtcStateInit()` | Validate RTC magic; reset state on power-on or invalid magic; increment `boot_count` | REQ-BOOT-05, REQ-BOOT-06, REQ-BAT-04 |
| `getResetInfo()` | Read `esp_reset_reason()` and `esp_sleep_get_wakeup_cause()`; set `FLAG_WDT_RESET` | REQ-BOOT-04, REQ-WDT-03 |
| `measureCpuTemp()` | `CPU_TEMP_SAMPLES` × `temperatureRead()`, trimmed mean, range check | REQ-BOOT-03, REQ-CPU-01..04 |
| `readAdcMv(pin, ratio)` | `ADC_SAMPLES` × `analogReadMilliVolts()`, trimmed mean, scaling by the divider ratio | REQ-ADC-01..04 |
| `classifyBattery(mv, prev)` | Validity check, thresholds with upward hysteresis | REQ-BAT-02, REQ-BAT-03, REQ-BAT-05 |
| `readDs18b20()` | Detection, resolution, non-blocking conversion with timeout, result validation | REQ-DS-01..07 |
| `sleepTimeForState(state)` | Map battery state to sleep duration in seconds | REQ-SLP-01, REQ-SLP-04, REQ-CHG-03 |
| `buildPacket()` | Populate `NodePacket`, compute CRC32 | REQ-PKT-01..05 |
| `rfSwitchInit()` | Configure GPIO3 and GPIO14 according to `USE_EXTERNAL_ANTENNA` | REQ-RF-02 |
| `sendPacket()` | Wi-Fi STA, channel, ESP-NOW init, broadcast peer, `TX_REPEAT_COUNT` transmissions with callback wait | REQ-RF-01, REQ-RF-03..06, REQ-RF-08, REQ-ERR-02 |
| `radioOff()` | `esp_now_deinit()`, `WiFi.mode(WIFI_OFF)` | REQ-RF-07 |
| `goToSleep(seconds)` | Print awake time, flush serial, LED off with hold, timer wake-up, deep sleep | REQ-SLP-02, REQ-SLP-03, REQ-WDT-04 |

### 5.4 `setup()` Sequence

```cpp
void setup() {
    ledOn();                                  // REQ-BOOT-01
    wdtStart();                               // REQ-BOOT-02
    int16_t cpuTemp = measureCpuTemp();       // REQ-BOOT-03 — must stay first measurement
    DBG_BEGIN();                              // REQ-DBG-02, REQ-DBG-03
    rtcStateInit();                           // REQ-BOOT-05, REQ-BOOT-06
    getResetInfo();                           // REQ-BOOT-04, REQ-WDT-03
    // print header, reset info, CPU temperature
    uint16_t batMv  = readAdcMv(PIN_BAT_ADC,  BAT_DIVIDER_RATIO);   // REQ-ADC-05
    uint8_t  state  = classifyBattery(batMv, rtc.prev_bat_state);
    uint16_t vbusMv = readAdcMv(PIN_VBUS_ADC, VBUS_DIVIDER_RATIO);
    bool charging   = vbusMv > CHARGE_DETECT_MV;                    // REQ-CHG-02
    int16_t dsTemp  = readDs18b20();
    uint32_t sleepS = sleepTimeForState(state);
    buildPacket(...);
    sendPacket();                             // includes rfSwitchInit()
    radioOff();
    goToSleep(sleepS);
}

void loop() {}                                // REQ-GEN-04
```

## 6. Implementation Details and Decisions

### 6.1 RTC State (REQ-BOOT-05, REQ-BOOT-06, REQ-BAT-04)

- The RTC structure is declared with `RTC_NOINIT_ATTR`, so it is not initialised on software or watchdog resets.
- On `ESP_RST_POWERON`, `ESP_RST_BROWNOUT`, or when `magic != RTC_MAGIC`, the structure is reset: `boot_count = 0`, `prev_bat_state = BAT_STATE_UNKNOWN`, `magic = RTC_MAGIC`.
- `boot_count` is incremented immediately after validation, so the first transmitted packet after power-on carries `boot_count = 1`.

### 6.2 Watchdog (REQ-WDT-01..04)

- The Arduino core initialises the task watchdog at start-up. `wdtStart()` reconfigures it with `esp_task_wdt_reconfigure()` using `timeout_ms = WDT_TIMEOUT_S * 1000`, `trigger_panic = true`, and subscribes the main task with `esp_task_wdt_add(NULL)`.
- The watchdog is never fed during the procedure. A complete cycle is far shorter than the timeout, so a timeout only occurs in case of a genuine hang.
- After a watchdog reset, `esp_reset_reason()` returns `ESP_RST_TASK_WDT` and `FLAG_WDT_RESET` is set.

### 6.3 Debug Macros (REQ-DBG-01..06)

```cpp
#if DEBUG_ENABLED
  #define DBG_BEGIN()  do { Serial.begin(SERIAL_BAUD); \
                            uint32_t t0 = millis(); \
                            while (!Serial && millis() - t0 < SERIAL_WAIT_MS) {} } while (0)
  #define DBG_PRINTF(fmt, ...) Serial.printf("[%6lu ms] " fmt "\n", millis(), ##__VA_ARGS__)
  #define DBG_FLUSH()  Serial.flush()
#else
  #define DBG_BEGIN()
  #define DBG_PRINTF(fmt, ...)
  #define DBG_FLUSH()
#endif
```

### 6.4 Measurements (REQ-CPU, REQ-ADC)

- A single helper, `trimmedMean()`, sorts nothing. It accumulates the sum, minimum and maximum, then returns `(sum − min − max) / (n − 2)`. This keeps the code short and allocation-free.
- `analogReadResolution(12)` and `analogSetPinAttenuation(pin, ADC_11db)` are applied before ADC sampling.
- Temperatures are converted to hundredths of a degree with rounding: `(int16_t)lroundf(t * 100.0f)`.

### 6.5 DS18B20 (REQ-DS-01..07)

- Presence: `DallasTemperature::begin()` followed by `getAddress(addr, 0)`. If no address is found, the sensor is considered absent.
- Conversion: `setResolution(addr, DS18B20_RESOLUTION)`, `setWaitForConversion(false)`, `requestTemperaturesByAddress(addr)`, then polling `isConversionComplete()` every 10 ms until done or until the timeout expires.
- Timeout: `DS_TIMEOUT_MS = (94 << (DS18B20_RESOLUTION − 9)) * 3 / 2`, computed at compile time.
- Validation: `DEVICE_DISCONNECTED_C` (−127), exactly 85.00 °C, and values outside −55…+125 °C are treated as errors.

### 6.6 ESP-NOW (REQ-RF-01..08)

- Sequence: `rfSwitchInit()` → `WiFi.mode(WIFI_STA)` → optional `WiFi.setTxPower(TX_POWER)` → `esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE)` → `esp_now_init()` → `esp_now_register_send_cb()` → `esp_now_add_peer()` (broadcast address, channel `ESPNOW_CHANNEL`, `WIFI_IF_STA`, no encryption).
- The send callback sets a `volatile` flag and stores the status. After each `esp_now_send()`, the code waits for the flag for up to `TX_CB_TIMEOUT_MS`, then waits `TX_REPEAT_DELAY_MS` before the next transmission.
- **Compatibility note:** the send callback signature differs between ESP-IDF versions used by Arduino core 3.x (`const uint8_t *mac` in older versions, `const wifi_tx_info_t *info` in newer ones). The callback shall be declared with a version guard on `ESP_IDF_VERSION`.

### 6.7 LED During Deep Sleep (REQ-LED-02)

- Before sleep: `digitalWrite(PIN_LED, HIGH)` (LED off), then `gpio_hold_en(GPIO_NUM_15)` and `gpio_deep_sleep_hold_en()`, so the pin state is latched during deep sleep.
- At boot: `gpio_hold_dis(GPIO_NUM_15)` is called before the LED is driven, otherwise the pin would remain latched in the off state.
- Fallback if the hold does not keep the LED fully dark on this hardware: configure the pin as an input before sleep and verify visually. The chosen solution shall be recorded in `README.md`.

### 6.8 Deep Sleep (REQ-SLP-01..04)

- `esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL)` followed by `esp_deep_sleep_start()`.
- The 64-bit cast is mandatory: 18000 s × 1 000 000 exceeds the 32-bit range.

## 7. Implementation Phases

Each phase ends with a successful build, a verification on hardware and a git commit. The commit message references the phase number and the requirement identifiers covered.

### Phase 0 — Repository and Skeleton

| Step | Action |
|---|---|
| 0.1 | Create the repository root folder and run `git init` |
| 0.2 | Add `requirements.md`, `implementation-plan.md`, `README.md` (skeleton), `.gitignore` |
| 0.3 | Create `temperature_node_src/` with `temperature_node_src.ino` (empty `setup()`/`loop()`) and `config.h` containing all settings from Section 7 of `requirements.md` |
| 0.4 | Build and upload the empty sketch |

**Verification:** the sketch builds and uploads without errors.
**Commit:** `Phase 0: repository skeleton and config.h (REQ-GEN-01, REQ-GEN-02)`

### Phase 1 — Boot, LED, Debug, Watchdog, RTC State

| Step | Action |
|---|---|
| 1.1 | Pin definitions, constants, debug macros |
| 1.2 | `ledOn()` / `ledOff()` |
| 1.3 | `wdtStart()` |
| 1.4 | RTC structure and `rtcStateInit()` |
| 1.5 | `getResetInfo()` and header printout |
| 1.6 | Temporary deep sleep of 10 s at the end of `setup()` for testing |

**Verification:**
- The LED is lit while awake and off while asleep.
- `boot_count` increases by one on every wake-up.
- A temporary infinite loop triggers a watchdog reset after 10 s, the reset reason is reported as watchdog and `boot_count` is preserved.
- Disconnecting power resets `boot_count`.
- With `DEBUG_ENABLED 0`, no serial output is produced.

**Commit:** `Phase 1: boot sequence, LED, debug, watchdog, RTC state (REQ-BOOT-*, REQ-LED-01, REQ-WDT-02, REQ-WDT-03, REQ-DBG-01..04)`

### Phase 2 — CPU Temperature

| Step | Action |
|---|---|
| 2.1 | `trimmedMean()` helper and temperature conversion |
| 2.2 | `measureCpuTemp()` called as the first measurement |

**Verification:** the CPU temperature is the first measured value; the value is plausible and stable between cycles.
**Commit:** `Phase 2: CPU temperature (REQ-BOOT-03, REQ-CPU-*)`

### Phase 3 — Battery and Charging Detection

| Step | Action |
|---|---|
| 3.1 | ADC configuration and `readAdcMv()` |
| 3.2 | `classifyBattery()` with hysteresis and validity check |
| 3.3 | 5V pin measurement and charging detection |
| 3.4 | `sleepTimeForState()` |

**Verification:**
- The measured battery voltage matches a multimeter reading within ±50 mV.
- With USB connected, `is_charging = 1`; on battery only, `is_charging = 0`.
- Hysteresis is tested using a laboratory power supply connected to BAT (see test cases T-3.x in Section 9).
- With the battery input disconnected or out of range, the state is INVALID and `FLAG_BAT_INVALID` is set.

**Commit:** `Phase 3: battery measurement, state and charging detection (REQ-ADC-*, REQ-BAT-*, REQ-CHG-*)`

### Phase 4 — DS18B20

| Step | Action |
|---|---|
| 4.1 | `readDs18b20()` with detection, resolution and timeout |
| 4.2 | Result validation and flags |

**Verification:**
- With the sensor connected: plausible temperature, conversion time matches the resolution.
- Without the sensor: the step is skipped within a few milliseconds and `FLAG_DS_ABSENT` is set.
- With the sensor disconnected during conversion: `FLAG_DS_ERR` is set.
- Resolutions 9 and 12 bits are both tested.

**Commit:** `Phase 4: DS18B20 support (REQ-DS-*)`

### Phase 5 — Packet Structure

| Step | Action |
|---|---|
| 5.1 | `NodePacket` definition and `static_assert` checks |
| 5.2 | `buildPacket()` and CRC32 |
| 5.3 | Debug printout of all fields, size and CRC |

**Verification:** the structure size is 27 bytes; the printed CRC matches an independent CRC32 calculation over the printed bytes.
**Commit:** `Phase 5: packet structure and CRC32 (REQ-PKT-*)`

### Phase 6 — Radio and ESP-NOW

| Step | Action |
|---|---|
| 6.1 | `rfSwitchInit()` |
| 6.2 | `sendPacket()` with callback wait |
| 6.3 | `radioOff()` |
| 6.4 | Minimal test receiver sketch (not part of the repository deliverables) that prints received packets and verifies the CRC |

**Verification:**
- The test receiver on the same channel receives three identical packets per cycle with a valid CRC.
- A receiver on a different channel receives nothing.
- Each transmission result is printed in the debug log.
- Switching `USE_EXTERNAL_ANTENNA` changes the GPIO14 level.

**Commit:** `Phase 6: ESP-NOW broadcast (REQ-RF-*, REQ-ERR-02)`

### Phase 7 — Shutdown and Deep Sleep

| Step | Action |
|---|---|
| 7.1 | `goToSleep()` with the final sequence: awake time, flush, LED hold, timer, sleep |
| 7.2 | Remove the temporary test sleep from Phase 1 |

**Verification:**
- The LED is completely dark during deep sleep.
- The interval between packets matches the selected sleep duration.
- The total awake time is reported and is below 1.5 s (12-bit DS18B20, DEBUG disabled, measured via LED or a current probe).

**Commit:** `Phase 7: deep sleep sequence (REQ-SLP-*, REQ-LED-02, REQ-WDT-04)`

### Phase 8 — Integration and Final Verification

| Step | Action |
|---|---|
| 8.1 | Run the complete test plan (Section 9) with `DEBUG_ENABLED 0` and `1` |
| 8.2 | Code review against every requirement in the traceability matrix (Section 8) |
| 8.3 | Complete `README.md`: wiring, build settings, library versions, LED hold solution |
| 8.4 | Tag the release `v1.0` |

**Commit:** `Phase 8: integration, documentation, v1.0`

## 8. Traceability Matrix

| Requirement group | Implemented in | Phase |
|---|---|---|
| REQ-GEN-01..06 | Whole sketch, `config.h` | 0, 8 |
| REQ-BOOT-01..06 | `ledOn()`, `wdtStart()`, `measureCpuTemp()`, `rtcStateInit()`, `getResetInfo()` | 1, 2 |
| REQ-CPU-01..04 | `measureCpuTemp()` | 2 |
| REQ-ADC-01..05 | `readAdcMv()`, `setup()` order | 3 |
| REQ-BAT-01..06 | `classifyBattery()`, RTC state | 3 |
| REQ-CHG-01..03 | `setup()`, `sleepTimeForState()` | 3 |
| REQ-DS-01..07 | `readDs18b20()` | 4 |
| REQ-PKT-01..05 | `NodePacket`, `buildPacket()` | 5 |
| REQ-RF-01..08 | `rfSwitchInit()`, `sendPacket()`, `radioOff()` | 6 |
| REQ-LED-01..03 | `ledOn()`, `ledOff()`, `goToSleep()` | 1, 7 |
| REQ-SLP-01..04 | `sleepTimeForState()`, `goToSleep()` | 3, 7 |
| REQ-WDT-01..04 | `wdtStart()`, `getResetInfo()` | 1, 7 |
| REQ-DBG-01..06 | Debug macros, all step functions | 1–7 |
| REQ-ERR-01..02 | All step functions, `sendPacket()` | 2–6 |

## 9. Test Cases

| ID | Requirement(s) | Setup | Expected result |
|---|---|---|---|
| T-1.1 | REQ-BOOT-05 | Normal operation, 3 cycles | `boot_count` = n, n+1, n+2 |
| T-1.2 | REQ-BOOT-06, REQ-WDT-03 | Forced hang | Reset after 10 s, `FLAG_WDT_RESET` set, `boot_count` preserved |
| T-1.3 | REQ-BOOT-06 | Power removed and restored | `boot_count` = 1 |
| T-2.1 | REQ-CPU-01..03 | Room temperature | Plausible value, printed first |
| T-3.1 | REQ-BAT-02 | 3.80 V on BAT | NORMAL, 900 s |
| T-3.2 | REQ-BAT-02 | 3.20 V on BAT | LOW, 3600 s |
| T-3.3 | REQ-BAT-02 | 2.90 V on BAT | EMPTY, 18000 s |
| T-3.4 | REQ-BAT-03 | Previous LOW, 3.32 V | LOW (hysteresis) |
| T-3.5 | REQ-BAT-03 | Previous LOW, 3.36 V | NORMAL |
| T-3.6 | REQ-BAT-05 | BAT input out of range | INVALID, 3600 s, flag set, previous state unchanged |
| T-3.7 | REQ-CHG-02 | USB connected | `is_charging = 1`, `vbus_mv` ≈ 5000 |
| T-4.1 | REQ-DS-01, REQ-DS-05 | Sensor connected | Plausible temperature |
| T-4.2 | REQ-DS-02 | No sensor | `FLAG_DS_ABSENT`, `TEMP_INVALID` |
| T-4.3 | REQ-DS-06 | Sensor removed during conversion | `FLAG_DS_ERR`, `TEMP_INVALID` |
| T-5.1 | REQ-PKT-04, REQ-PKT-05 | Any | Size 27 bytes, CRC verified by test receiver |
| T-6.1 | REQ-RF-03..05 | Test receiver on same channel | 3 identical packets per cycle |
| T-6.2 | REQ-RF-03 | Test receiver on another channel | No packets |
| T-7.1 | REQ-LED-02 | Dark environment | LED fully off during sleep |
| T-7.2 | REQ-WDT-04 | DEBUG disabled | Awake time < 1.5 s |
| T-8.1 | REQ-DBG-01 | `DEBUG_ENABLED 0` | No serial output, identical packet contents |

## 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| ESP-NOW callback signature differs between core versions | Build failure | Version guard on `ESP_IDF_VERSION` (6.6) |
| GPIO hold not effective on GPIO15 during deep sleep | LED glows faintly | Fallback described in 6.7, verified in T-7.1 |
| High-impedance dividers (220 kΩ) affect ADC accuracy | Voltage error | Trimmed mean; optional 100 nF capacitor across the lower resistor; divider ratio adjustable in `config.h` |
| Brown-out below approximately 3.0 V | Repeated resets, `boot_count` reset | Documented limitation (`requirements.md`, Section 8) |
| CPU temperature offset | Inaccurate ambient value | Compensation by the receiver (out of scope) |
