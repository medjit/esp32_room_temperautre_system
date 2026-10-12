# XIAO ESP32C6 Temperature Node

Battery-powered room temperature sensor node based on the **Seeed Studio XIAO ESP32C6**. The node wakes up periodically, takes its measurements, broadcasts them over **ESP-NOW** and goes back to deep sleep.

The firmware is deliberately simple: one linear procedure, no compensation on the node, and all settings fixed at compile time.

## Features

- Internal CPU temperature measured first after boot, before the chip warms up
- Optional **DS18B20** sensor, detected automatically on every cycle
- Battery voltage monitoring with three battery states and hysteresis
- Charging detection via the 5V pin
- Adaptive sleep interval based on the battery state
- Packet broadcast three times over ESP-NOW, protected by CRC32
- On-board LED lit while awake, off while asleep
- Hardware watchdog protection against hangs
- Optional step-by-step debug output over USB serial

## How It Works

```
Wake up → LED on → CPU temp → battery → 5V pin → DS18B20 (if present)
        → build packet → ESP-NOW broadcast ×3 → radio off → LED off → deep sleep
```

| Battery state | Voltage | Sleep interval |
|---|---|---|
| NORMAL | > 3.30 V | 15 min |
| LOW | > 3.00 V | 1 h |
| EMPTY | ≤ 3.00 V | 5 h |
| INVALID | < 2.50 V or > 4.50 V | 1 h |

## Hardware

### Bill of Materials

| Qty | Part |
|---|---|
| 1 | Seeed Studio XIAO ESP32C6 |
| 1 | 18650 Li-ion cell (protected recommended) and holder |
| 4 | 220 kΩ resistor (two voltage dividers) |
| 1 | DS18B20 temperature sensor *(optional)* |
| 1 | 4.7 kΩ resistor, DS18B20 pull-up *(optional)* |

### Wiring

| Function | XIAO pin | GPIO | Notes |
|---|---|---|---|
| 5V pin divider | A0 / D0 | GPIO0 | 2 × 220 kΩ, 5V → A0 → GND |
| Battery divider | A1 / D1 | GPIO1 | 2 × 220 kΩ, BAT+ → A1 → GND |
| DS18B20 data | D5 | GPIO23 | 4.7 kΩ pull-up to 3V3 |
| On-board LED | — | GPIO15 | Active low |

The battery is connected to the BAT+/BAT− pads on the underside of the board. The on-board charger charges it whenever USB power is present.

## Data Packet

Each cycle broadcasts a packed, 27-byte structure (`NodePacket`):

| Field | Type | Description |
|---|---|---|
| `packet_version` | `uint8_t` | Packet format version |
| `fw_version` | `uint16_t` | Firmware version |
| `node_id` | `uint8_t` | Node identifier |
| `boot_count` | `uint32_t` | Cycle counter, kept in RTC memory |
| `is_charging` | `uint8_t` | 1 if external power is present |
| `battery_mv` | `uint16_t` | Battery voltage, mV |
| `battery_state` | `uint8_t` | 0 NORMAL, 1 LOW, 2 EMPTY, 3 INVALID |
| `vbus_mv` | `uint16_t` | 5V pin voltage, mV |
| `sleep_time_s` | `uint32_t` | Seconds until the next cycle |
| `cpu_temp_cC` | `int16_t` | CPU temperature, 0.01 °C |
| `ds18b20_temp_cC` | `int16_t` | DS18B20 temperature, 0.01 °C |
| `status_flags` | `uint8_t` | Error and status bits |
| `crc32` | `uint32_t` | CRC32 of all preceding bytes |

Invalid temperatures are sent as `-32768` with the corresponding bit set in `status_flags`. The structure definition is delimited in the sketch so it can be copied unchanged into a receiver project.

> The receiver (gateway) is a separate project and is not part of this repository.

## Configuration

All settings live in [`temperature_node_src/config.h`](temperature_node_src/config.h). The most commonly changed ones are:

| Setting | Default | Description |
|---|---|---|
| `NODE_ID` | `1` | Unique ID, set per node |
| `ESPNOW_CHANNEL` | `1` | Wi-Fi channel, must match the receiver |
| `DS18B20_RESOLUTION` | `12` | 9–12 bits |
| `USE_EXTERNAL_ANTENNA` | `0` | `1` selects the U.FL connector |
| `DEBUG_ENABLED` | `0` | `1` enables serial output at 115200 baud |

See `requirements.md` for the full list.

## Building and Flashing

1. Install **Arduino IDE 2.x**.
2. Add the **esp32 by Espressif Systems** board package, version 3.x.
3. Install the **OneWire** and **DallasTemperature** libraries from the Library Manager.
4. Open `temperature_node_src/temperature_node_src.ino`.
5. Select **Tools → Board → XIAO_ESP32C6**.
6. For debug output, set **Tools → USB CDC On Boot → Enabled**.
7. Set `NODE_ID` in `config.h`, then compile and upload.

> **Tip:** once the node is in deep sleep, the USB port disappears. To re-flash, hold **BOOT**, press **RESET**, then release **BOOT**.

## Project Structure

```
├── README.md
├── requirements.md            # Software Requirements Specification
├── implementation-plan.md     # Implementation phases, traceability, tests
└── temperature_node_src/
    ├── temperature_node_src.ino
    └── config.h
```

## Documentation

- [Requirements](requirements.md) — complete numbered requirements (`REQ-*`)
- [Implementation Plan](implementation-plan.md) — firmware structure, phases and test cases

## Limitations

- No temperature compensation on the node. The CPU temperature has a significant offset and should be corrected by the receiver.
- ESP-NOW broadcast is unencrypted and unacknowledged.
- The node does not protect the cell from deep discharge. Use a protected 18650 cell.

## Status

Firmware implemented (all requirements covered; see the traceability header in `temperature_node_src.ino`). Hardware verification per the implementation plan is pending.
