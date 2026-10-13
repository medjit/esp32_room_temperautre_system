# ESP32 Room Temperature System

A small, battery-friendly system that shows the temperature of every corner of a room on a clean web dashboard. Built entirely on **Seeed Studio XIAO ESP32C6** boards, with no cloud, no broker and no extra services.

```
   Corner nodes (battery)                         Server (USB powered)
  ┌──────────────────────┐                     ┌─────────────────────────┐
  │ Node 1   Node 2      │   ESP-NOW           │  XIAO ESP32C6           │
  │ Node 3   Node 4      │ ──────────────────► │  listens · stores       │ ──► Web dashboard
  │ sleep · wake · send  │   (3 copies, CRC)   │  history · serves page  │     http://roomtemp.local
  └──────────────────────┘                     └─────────────────────────┘
```

## How it works

1. Each **node** sleeps almost all the time. Every 15 minutes it wakes up, measures, broadcasts one small packet over ESP-NOW and goes back to sleep. A single 18650 cell lasts a long time.
2. The **server** never sleeps. It receives the packets, checks them, removes the duplicate copies, keeps the history and serves a web page.
3. The **dashboard** shows a room with a temperature bubble in each corner, plus battery state, signal, and history graphs. It tells you when a node is running low on battery or has gone silent.

The two halves only share one thing: the 27-byte packet format. The node is the source of truth for it, and the server copies it unchanged.

## Projects

| Folder | What it is | Status |
|---|---|---|
| [`temperature_ndoe/`](temperature_ndoe) | Battery-powered sensor node (transmitter) | Firmware complete and frozen |
| [`temperature_server/`](temperature_server) | Always-on receiver with web dashboard | Firmware and UI written, builds cleanly, host-tested; **not yet tested on a board** |

Each folder has its own README, requirements specification (`requirements.md`) and implementation plan.

### Temperature node

- Measures the CPU temperature first (before the chip warms up), battery voltage, whether USB power is present, and an optional **DS18B20** sensor.
- Adapts its sleep time to the battery: 15 min normal, 1 h low, 5 h empty.
- Broadcasts every packet three times, protected by CRC32, then turns the radio off.
- Deliberately simple: one linear procedure, no compensation on the node, all settings in one `config.h`.

### Temperature server

- **Room view** with a colour-coded temperature bubble per corner (up to 4 nodes).
- **Node health:** OK, low or empty battery, charging, and *stuck* when a node is silent for longer than 1.5× the sleep time it announced.
- **History graphs** for about 8 days, kept in RAM and saved to flash hourly.
- **Smart temperature:** DS18B20 when present, otherwise CPU temperature minus a per-node offset, clearly marked as *estimated*.
- **No reflashing for setup:** Wi-Fi, node names, positions and offsets are set in the web UI (PIN protected).
- Joins your router, or opens its own access point if the router is unreachable. The page works offline, with no external resources.

## Hardware

| Part | Node | Server |
|---|---|---|
| Seeed Studio XIAO ESP32C6 | 1 per corner | 1 |
| 18650 Li-ion cell | yes | no (USB power) |
| 4 × 220 kΩ (two voltage dividers) | yes | no |
| DS18B20 + 4.7 kΩ pull-up | optional | no |

The server is just a bare board. Wiring for the node is in [`temperature_ndoe/README.md`](temperature_ndoe/README.md).

## Quick start

1. **Node:** set `NODE_ID` (unique per node) and `ESPNOW_CHANNEL` in `temperature_ndoe/temperature_node_src/config.h`, then upload it to each node. Needs the `OneWire` and `DallasTemperature` libraries.
2. **Server:** open `temperature_server/temperature_server_src/temperature_server_src.ino`, change `AP_PASSWORD` and `DEFAULT_PIN` in `config.h`, and upload. The server needs no extra libraries.
3. Join the Wi-Fi network `RoomTemp-Setup`, open `http://192.168.4.1` and enter your router details. Afterwards open `http://roomtemp.local`.
4. Nodes appear on the dashboard as soon as they send their first packet.

Both use the Arduino IDE with the ESP32 core 3.x and the board **XIAO_ESP32C6**.

## One thing to get right: the Wi-Fi channel

ESP-NOW and Wi-Fi share a single radio, so the **node's `ESPNOW_CHANNEL` must equal the channel your router uses**. The server follows the router's channel and shows it on the page; the nodes are not changed automatically. Set your router's 2.4 GHz channel to a **fixed** value (not "auto"), and use that value in the nodes. If the channel ever changes, all nodes will show as stuck until they are reflashed.

## Repository layout

```
├── temperature_ndoe/      node firmware, requirements, plan
│   └── temperature_node_src/
└── temperature_server/    server firmware, web UI, tools, tests, requirements, plan
    ├── temperature_server_src/
    ├── web/               dashboard source
    ├── tools/             mock server, build script, test sender
    └── test/              host unit tests
```

## Limitations

- No encryption or authentication: ESP-NOW broadcasts are protected only by a CRC, and the web page uses plain HTTP with a PIN. It is meant for a trusted home network.
- The CPU temperature reads higher than the room, so nodes without a DS18B20 need a per-node offset, calibrated against a reference thermometer.
- Without internet (access-point-only mode) the server has no clock and graphs show relative time.
