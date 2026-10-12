# XIAO ESP32C6 Temperature Node — Software Requirements Specification

| Field | Value |
|---|---|
| Document | Software Requirements Specification (SRS) |
| Version | 1.1 |
| Date | 2026-10-12 |
| Target hardware | Seeed Studio XIAO ESP32C6 |
| Development environment | Arduino IDE, ESP32 Arduino core 3.x |

---

## 1. Project Overview

The node is a battery-powered room temperature sensor built on the **Seeed Studio XIAO ESP32C6** development board and powered by a single **18650** lithium-ion cell.

The node operates in periodic cycles. On every wake-up it shall:

1. measure the internal processor temperature (CPU temperature) immediately after boot, before the chip has warmed up from its own activity;
2. measure the battery voltage through a voltage divider connected to pin A1;
3. measure the 5V pin voltage through a voltage divider connected to pin A0 in order to determine whether external power is present (i.e. whether the battery is charging);
4. if a DS18B20 sensor is connected to pin D5, read its temperature; otherwise, skip this step;
5. populate a data structure and transmit it **three times** as an ESP-NOW broadcast;
6. turn off the radio and enter deep sleep for a duration determined by the battery level.

The node **does not apply any compensation or correction** to the measured values. It transmits raw measurements, adjusted only by the factory ADC calibration. All further processing is the responsibility of the receiver.

The on-board LED is lit while the node is awake and is off while the node is asleep.

Guiding principle of the implementation: **maximum simplicity**.

## 2. Scope

**In scope:**

- firmware for the transmitting temperature node;
- description of the hardware connections;
- definition of the data packet structure in a clearly delimited section of the firmware source, to be reused unchanged by the receiver.

**Out of scope (separate project):**

- the receiver (gateway): a continuously running ESP32 that receives and stores the data;
- temperature compensation and calibration;
- OTA updates, encryption, bidirectional communication.

## 3. Definitions

| Term | Meaning |
|---|---|
| Node | The transmitting device covered by this specification |
| Receiver | The ESP32 that listens for the broadcast (out of scope) |
| Cycle | A single wake period, from boot until entry into deep sleep |
| RTC memory | Memory that is retained during deep sleep |
| Battery state | NORMAL / LOW / EMPTY / INVALID |
| Compile-time setting | A `#define` in `config.h` |
| SHALL | Mandatory requirement |
| MAY | Optional requirement |

## 4. Hardware

### 4.1 Pin Assignment

| Function | XIAO pin | GPIO | Notes |
|---|---|---|---|
| 5V pin voltage divider | A0 / D0 | GPIO0 | 2 × 220 kΩ, ADC1 |
| Battery (BAT+) voltage divider | A1 / D1 | GPIO1 | 2 × 220 kΩ, ADC1 |
| DS18B20 DATA (optional) | D5 | GPIO23 | 4.7 kΩ pull-up to 3V3 |
| On-board user LED | — | GPIO15 | Active low |
| RF switch enable | — | GPIO3 | LOW = switch enabled |
| Antenna selection | — | GPIO14 | LOW = on-board, HIGH = external |

### 4.2 Voltage Dividers

- Both dividers consist of two 220 kΩ resistors, giving a division ratio of 2.
- At a battery voltage of 4.2 V, pin A1 sees 2.1 V. At 5.0 V on the 5V pin, pin A0 sees 2.5 V. Both values are within the ADC operating range.
- The battery divider draws a continuous current of approximately 9.5 µA at 4.2 V. This loss is considered acceptable.

---

## 5. Operating Cycle (Summary)

```
BOOT
 ├─ LED ON
 ├─ Start watchdog (10 s)
 ├─ 1. CPU temperature (first, before warm-up)
 ├─ 2. Serial port initialisation (DEBUG builds only)
 ├─ 3. Battery (A1) → mV → state (with hysteresis)
 ├─ 4. 5V pin (A0) → mV → charging status
 ├─ 5. DS18B20: detect → convert → read (or skip)
 ├─ 6. Determine sleep duration
 ├─ 7. Populate packet structure + CRC32
 ├─ 8. Wi-Fi/ESP-NOW ON → set channel → 3× broadcast
 ├─ 9. ESP-NOW/Wi-Fi OFF
 ├─ 10. LED OFF
 └─ 11. Deep sleep for the selected duration
```

---

## 6. Requirements

Each requirement carries a unique identifier of the form `REQ-<GROUP>-<NN>`, a statement and, where applicable, a verification method.

### 6.1 General (GEN)

**REQ-GEN-01** — The firmware SHALL compile and upload using the Arduino IDE with ESP32 Arduino core 3.x, board "XIAO_ESP32C6".
*Verification:* successful build and upload.

**REQ-GEN-02** — All node settings SHALL be defined at compile time in a single file, `config.h` (see Section 7). The firmware SHALL NOT use any run-time configuration.
*Verification:* code review; all parameters listed in Section 7 are defined in `config.h`.

**REQ-GEN-03** — The only external libraries SHALL be `OneWire` and `DallasTemperature`. All other functionality SHALL be provided by the ESP32 Arduino core and ESP-IDF.
*Verification:* review of `#include` directives.

**REQ-GEN-04** — All application logic SHALL execute within `setup()`. `loop()` SHALL remain empty, as the node never reaches it.
*Verification:* code review.

**REQ-GEN-05** — The node SHALL NOT apply any compensation, offset or filtering to measured values other than the averaging defined in REQ-ADC-03 and the factory ADC calibration.
*Verification:* code review.

**REQ-GEN-06** — The firmware SHALL define a version number, `FW_VERSION`, which SHALL be included in the packet and printed in the debug output.
*Verification:* the value is present in the packet and in the log.

### 6.2 Boot (BOOT)

**REQ-BOOT-01** — The first action after boot SHALL be to turn on the on-board LED (GPIO15 driven low).
*Verification:* the LED lights immediately upon wake-up.

**REQ-BOOT-02** — Immediately after the LED is turned on, the watchdog SHALL be started (see REQ-WDT-02).

**REQ-BOOT-03** — The CPU temperature measurement SHALL be the first measurement of the cycle, performed before initialisation of the serial port, ADC, 1-Wire bus and radio.
*Verification:* code review of execution order; CPU temperature is the first value reported in the debug log.

**REQ-BOOT-04** — The node SHALL determine the reset/wake-up cause (timer, power-on, watchdog reset, other) and print it in the debug output.

**REQ-BOOT-05** — The boot counter (`boot_count`) SHALL be stored in RTC memory and SHALL be incremented by one on every cycle.
*Verification:* the value increases by one between consecutive packets.

**REQ-BOOT-06** — `boot_count` SHALL be retained across deep sleep and watchdog resets, and SHALL be reset only on loss of power (e.g. battery replacement). This SHALL be achieved using non-initialised RTC memory together with a magic value for validity checking. If the magic value does not match, the counter SHALL start from 0.
*Verification:* a forced watchdog reset does not reset the counter; removing power does.

### 6.3 CPU Temperature Measurement (CPU)

**REQ-CPU-01** — The CPU temperature SHALL be read from the internal temperature sensor of the ESP32-C6 (`temperatureRead()`).

**REQ-CPU-02** — The value SHALL be the mean of `CPU_TEMP_SAMPLES` (default 8) consecutive readings, with the minimum and maximum readings discarded.

**REQ-CPU-03** — The result SHALL be stored in hundredths of a degree Celsius as `int16_t` (e.g. 23.47 °C → 2347).

**REQ-CPU-04** — If the reading is invalid (NaN or outside −40…+125 °C), the field SHALL be set to `TEMP_INVALID` (−32768) and the flag `FLAG_CPU_TEMP_ERR` SHALL be set.

### 6.4 ADC Measurements (ADC)

**REQ-ADC-01** — Both channels (A0 and A1) SHALL be on ADC1 and SHALL be sampled with 11 dB attenuation.

**REQ-ADC-02** — Voltages SHALL be read using the factory calibration (`analogReadMilliVolts()`).

**REQ-ADC-03** — Each measurement ("debounce") SHALL consist of `ADC_SAMPLES` (default 16) samples. The minimum and maximum samples SHALL be discarded and the remaining samples averaged.

**REQ-ADC-04** — The averaged value SHALL be multiplied by the corresponding divider ratio (`BAT_DIVIDER_RATIO`, `VBUS_DIVIDER_RATIO`, default 2.0) to obtain the actual voltage in mV.

**REQ-ADC-05** — The battery voltage SHALL be measured before the 5V pin voltage and before the radio is enabled.

### 6.5 Battery (BAT)

**REQ-BAT-01** — The battery voltage SHALL be stored in the packet in mV (`uint16_t battery_mv`).

**REQ-BAT-02** — The battery state SHALL be determined as follows:

| State | Condition | Value | Sleep duration |
|---|---|---|---|
| NORMAL | > 3300 mV | 0 | 15 min |
| LOW | > 3000 mV and ≤ 3300 mV | 1 | 1 h |
| EMPTY | ≤ 3000 mV | 2 | 5 h |
| INVALID | < 2500 mV or > 4500 mV | 3 | 1 h |

**REQ-BAT-03** — A transition to a higher state SHALL require the voltage to exceed the threshold plus `BAT_HYSTERESIS_MV` (default 50 mV):
- LOW → NORMAL: > 3350 mV;
- EMPTY → LOW: > 3050 mV.

A transition to a lower state SHALL occur immediately upon crossing the threshold.
*Verification:* at 3320 mV with a previous state of LOW, the resulting state is LOW.

**REQ-BAT-04** — The previous battery state SHALL be stored in RTC memory. On first boot (no valid previous state), the thresholds SHALL be applied without hysteresis.

**REQ-BAT-05** — The INVALID state SHALL set the flag `FLAG_BAT_INVALID` and SHALL NOT modify the stored previous state.

**REQ-BAT-06** — The battery state SHALL be stored in the packet (`uint8_t battery_state`).

### 6.6 External Power / Charging (CHG)

**REQ-CHG-01** — The 5V pin voltage SHALL be stored in the packet in mV (`uint16_t vbus_mv`).

**REQ-CHG-02** — The node SHALL consider itself to be charging (`is_charging = 1`) if the 5V pin voltage exceeds `CHARGE_DETECT_MV` (default 4000 mV); otherwise `is_charging = 0`.

**REQ-CHG-03** — The sleep duration SHALL follow the battery state regardless of charging status. Charging SHALL NOT alter the sleep duration.

### 6.7 DS18B20 Sensor (DS)

**REQ-DS-01** — On every cycle, the node SHALL check for the presence of a DS18B20 on D5 (GPIO23) using a 1-Wire reset/presence sequence and a device address search.

**REQ-DS-02** — If no device responds, the node SHALL skip all DS18B20 steps, set the field to `TEMP_INVALID` and set the flag `FLAG_DS_ABSENT`. This condition SHALL NOT be treated as an error.

**REQ-DS-03** — The resolution SHALL be configurable via `DS18B20_RESOLUTION` (9–12, default 12).

**REQ-DS-04** — The conversion SHALL be started in non-blocking mode, after which the node SHALL poll for completion with a timeout:

| Resolution | Conversion time | Timeout (+50%) |
|---|---|---|
| 9 bit | 94 ms | 141 ms |
| 10 bit | 188 ms | 282 ms |
| 11 bit | 375 ms | 563 ms |
| 12 bit | 750 ms | 1125 ms |

The timeout SHALL be derived automatically from the configured resolution.

**REQ-DS-05** — The result SHALL be stored in hundredths of a degree Celsius as `int16_t`.

**REQ-DS-06** — The following results SHALL be treated as errors, in which case the field SHALL be set to `TEMP_INVALID` and the flag `FLAG_DS_ERR` SHALL be set:
- timeout expired;
- −127.00 °C (device disconnected);
- 85.00 °C (power-on reset value);
- any value outside −55…+125 °C.

**REQ-DS-07** — The sensor is powered from 3V3. After conversion it automatically enters idle mode (approximately 1 µA). No dedicated sleep command exists and none SHALL be sent. The data line SHALL be left in its idle state (pulled high) before deep sleep.

### 6.8 Data Packet (PKT)

**REQ-PKT-01** — The packet structure SHALL be defined in a single, clearly delimited section of the main sketch file (`temperature_node_src.ino`), so that it can be copied unchanged into the receiver project.

**REQ-PKT-02** — The structure SHALL be packed (no padding), little-endian, and SHALL contain the following fields in the order given:

| # | Field | Type | Unit / meaning |
|---|---|---|---|
| 1 | `packet_version` | `uint8_t` | Packet format version, starting at 1 |
| 2 | `fw_version` | `uint16_t` | Firmware version |
| 3 | `node_id` | `uint8_t` | From `NODE_ID` |
| 4 | `boot_count` | `uint32_t` | Cycle counter |
| 5 | `is_charging` | `uint8_t` | 0 / 1 |
| 6 | `battery_mv` | `uint16_t` | mV |
| 7 | `battery_state` | `uint8_t` | 0 NORMAL, 1 LOW, 2 EMPTY, 3 INVALID |
| 8 | `vbus_mv` | `uint16_t` | 5V pin voltage, mV |
| 9 | `sleep_time_s` | `uint32_t` | Seconds until the next cycle |
| 10 | `cpu_temp_cC` | `int16_t` | Hundredths of °C, or −32768 |
| 11 | `ds18b20_temp_cC` | `int16_t` | Hundredths of °C, or −32768 |
| 12 | `status_flags` | `uint8_t` | Bit field (see REQ-PKT-03) |
| 13 | `crc32` | `uint32_t` | CRC32 over fields 1–12 |

**REQ-PKT-03** — `status_flags` SHALL be defined as follows:

| Bit | Flag | Meaning |
|---|---|---|
| 0 | `FLAG_DS_ABSENT` | DS18B20 not detected |
| 1 | `FLAG_DS_ERR` | DS18B20 detected, but the reading is invalid |
| 2 | `FLAG_BAT_INVALID` | Battery voltage outside the valid range |
| 3 | `FLAG_CPU_TEMP_ERR` | CPU temperature reading error |
| 4 | `FLAG_WDT_RESET` | The previous cycle ended in a watchdog reset |
| 5–7 | — | Reserved, set to 0 |

**REQ-PKT-04** — The CRC32 SHALL be computed using `esp_rom_crc32_le(0, data, len)` over all bytes of the structure preceding the `crc32` field.
*Verification:* the receiver computes the same CRC and the values match.

**REQ-PKT-05** — A compile-time check (`static_assert`) SHALL ensure that the structure size is exactly the expected size and does not exceed the ESP-NOW payload limit of 250 bytes.

### 6.9 Radio and ESP-NOW (RF)

**REQ-RF-01** — The radio SHALL be enabled only after all measurements are complete and the packet structure has been populated.

**REQ-RF-02** — Before enabling Wi-Fi, the RF switch SHALL be configured: GPIO3 = LOW, and GPIO14 = LOW (on-board antenna) or HIGH (external antenna), according to `USE_EXTERNAL_ANTENNA`.

**REQ-RF-03** — Wi-Fi SHALL operate in Station mode, without connecting to an access point, on channel `ESPNOW_CHANNEL` (default 1).

**REQ-RF-04** — A broadcast peer `FF:FF:FF:FF:FF:FF` SHALL be registered without encryption on the same channel.

**REQ-RF-05** — The packet SHALL be transmitted `TX_REPEAT_COUNT` times (default 3), with a delay of `TX_REPEAT_DELAY_MS` (default 10 ms) between transmissions. All copies SHALL be identical, including `boot_count`, which the receiver uses for duplicate removal.

**REQ-RF-06** — After each transmission, the node SHALL wait for the send callback for no longer than `TX_CB_TIMEOUT_MS` (default 50 ms). The result (success/failure) SHALL be printed in the debug output. No additional retries SHALL be performed.

**REQ-RF-07** — After the final transmission, `esp_now_deinit()` SHALL be called and Wi-Fi SHALL be turned off (`WiFi.mode(WIFI_OFF)`) before entering deep sleep.

**REQ-RF-08** — The transmitter output power MAY be configured via `TX_POWER`. By default, the core's default value SHALL be used.

### 6.10 LED Indication (LED)

**REQ-LED-01** — The LED SHALL be lit from the beginning of the cycle (REQ-BOOT-01) until immediately before entering deep sleep.

**REQ-LED-02** — The LED SHALL be off during deep sleep and SHALL NOT glow faintly due to a floating pin.
*Verification:* visual inspection; the LED is fully off while the node is asleep.

**REQ-LED-03** — LED indication SHALL operate independently of `DEBUG_ENABLED`.

### 6.11 Deep Sleep (SLP)

**REQ-SLP-01** — The sleep duration SHALL be determined by the battery state (REQ-BAT-02) and SHALL be stored in the packet in seconds.

**REQ-SLP-02** — Wake-up SHALL be by timer only.

**REQ-SLP-03** — Before deep sleep, the following SHALL be performed in this order: radio shutdown (REQ-RF-07), serial buffer flush (DEBUG builds only), LED off, deep sleep entry.

**REQ-SLP-04** — Sleep durations SHALL be configurable via `SLEEP_NORMAL_S` (900), `SLEEP_LOW_S` (3600), `SLEEP_EMPTY_S` (18000) and `SLEEP_INVALID_S` (3600).

### 6.12 Watchdog and Hang Protection (WDT)

**REQ-WDT-01** — The node SHALL execute the complete procedure on every cycle. No step SHALL be skipped due to elapsed time.

**REQ-WDT-02** — If a cycle does not complete within `WDT_TIMEOUT_S` (default 10 s), a hardware watchdog SHALL reset the node. This mechanism serves solely as protection against a genuine hang.

**REQ-WDT-03** — After a watchdog reset, the node SHALL set `FLAG_WDT_RESET` in the packet of the current cycle and SHALL execute the procedure normally.

**REQ-WDT-04** — The nominal cycle duration (12-bit DS18B20, DEBUG disabled) is expected to be approximately 1–1.5 s, well below `WDT_TIMEOUT_S`.
*Verification:* total awake time is reported in the debug log.

### 6.13 Debug Output (DBG)

**REQ-DBG-01** — Debug output SHALL be enabled via `DEBUG_ENABLED` (0/1). When set to 0, the serial port SHALL NOT be initialised and debug code SHALL be excluded from compilation (via macros).

**REQ-DBG-02** — The serial port baud rate SHALL be `SERIAL_BAUD` (115200).

**REQ-DBG-03** — In DEBUG builds, the node SHALL wait for the USB-CDC connection for up to `SERIAL_WAIT_MS` (default 1000 ms) and SHALL then proceed regardless of connection status.

**REQ-DBG-04** — Each line SHALL be prefixed with the time since boot in milliseconds, e.g. `[  152 ms]`.

**REQ-DBG-05** — The debug output SHALL describe the complete procedure step by step and SHALL include at least:
- a header line: node ID, firmware version, packet version;
- reset/wake-up cause and `boot_count` value;
- CPU temperature: number of samples and result in °C;
- battery: number of samples, mV at the pin, mV after divider scaling, previous and new state;
- 5V pin: mV and charging status;
- DS18B20: detected or not, device address, resolution, conversion time, result or error;
- packet contents: all fields, size in bytes and CRC32 in hexadecimal;
- radio: node MAC address, channel, antenna (on-board/external);
- each transmission: sequence number and callback result;
- selected sleep duration;
- total awake time in ms before entering sleep.

**REQ-DBG-06** — Debug output SHALL NOT alter the order of operations. The sole exception is the CPU temperature, which is measured before serial port initialisation and printed afterwards.

### 6.14 Error Handling (ERR)

**REQ-ERR-01** — An error in one step SHALL NOT prevent execution of the remaining steps. The node SHALL always transmit a packet containing the available data together with the appropriate flags.

**REQ-ERR-02** — If ESP-NOW initialisation fails, the node SHALL print the error (DEBUG builds), skip transmission and enter sleep normally in accordance with REQ-SLP-01.

---

## 7. Compile-Time Configuration (`config.h`)

| Name | Default | Description |
|---|---|---|
| `NODE_ID` | 1 | Unique node identifier |
| `FW_VERSION` | 1 | Firmware version |
| `ESPNOW_CHANNEL` | 1 | Wi-Fi channel (1–13) |
| `USE_EXTERNAL_ANTENNA` | 0 | 0 = on-board, 1 = external |
| `TX_REPEAT_COUNT` | 3 | Number of transmissions |
| `TX_REPEAT_DELAY_MS` | 10 | Delay between transmissions |
| `TX_CB_TIMEOUT_MS` | 50 | Maximum wait for the send callback |
| `TX_POWER` | (core default) | Transmitter output power |
| `DS18B20_RESOLUTION` | 12 | 9–12 bits |
| `CPU_TEMP_SAMPLES` | 8 | CPU temperature samples |
| `ADC_SAMPLES` | 16 | Samples per ADC channel |
| `BAT_DIVIDER_RATIO` | 2.0 | Battery divider ratio |
| `VBUS_DIVIDER_RATIO` | 2.0 | 5V pin divider ratio |
| `BAT_NORMAL_MV` | 3300 | NORMAL/LOW threshold |
| `BAT_LOW_MV` | 3000 | LOW/EMPTY threshold |
| `BAT_HYSTERESIS_MV` | 50 | Upward hysteresis |
| `BAT_VALID_MIN_MV` | 2500 | Lower validity limit |
| `BAT_VALID_MAX_MV` | 4500 | Upper validity limit |
| `CHARGE_DETECT_MV` | 4000 | External power detection threshold |
| `SLEEP_NORMAL_S` | 900 | 15 min |
| `SLEEP_LOW_S` | 3600 | 1 h |
| `SLEEP_EMPTY_S` | 18000 | 5 h |
| `SLEEP_INVALID_S` | 3600 | 1 h |
| `WDT_TIMEOUT_S` | 10 | Watchdog timeout |
| `DEBUG_ENABLED` | 0 | Debug output |
| `SERIAL_BAUD` | 115200 | Serial port baud rate |
| `SERIAL_WAIT_MS` | 1000 | USB-CDC connection wait |

---

## 8. Assumptions and Notes

- **DS18B20 sleep.** The sensor has no sleep command and enters idle mode automatically after conversion. Should complete power-down be required in the future, the sensor may be powered from a GPIO pin. This is a hardware change outside the current scope.
- **Low voltage operation.** The ESP32-C6 requires a minimum supply of 3.0 V. Brown-out resets may occur when the battery voltage is at or below approximately 3.0 V. The EMPTY state (5 h sleep) reduces consumption but does not protect the cell against deep discharge. Such protection is the responsibility of the battery protection circuit (BMS) or the charging circuit.
- **Charging.** It is assumed that the sleep duration is not altered while charging (REQ-CHG-03).
- **CPU temperature accuracy.** The internal sensor exhibits a significant offset relative to ambient temperature. Correction is performed by the receiver (out of scope).
- **Duplicates.** The receiver will receive up to three identical packets per cycle and shall distinguish them by `node_id` + `boot_count`.
