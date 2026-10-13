# Room Temperature Server

The always-on receiver of the room temperature system. A bare **Seeed XIAO ESP32C6** (USB powered, no extra hardware) listens for ESP-NOW packets from battery temperature nodes, keeps their history, and serves a modern web dashboard.

> **Status:** firmware and web UI are implemented. The sketch builds cleanly, the core logic passes its host tests (`make -C test`) and the UI was exercised against a mock server. **Not yet tested on a real board.** See [`requirements.md`](requirements.md), [`implementation-plan.md`](implementation-plan.md) (section 11 has the verification status) and [`traceability.md`](traceability.md).

## Features

- **Room view:** a top-down room with a temperature bubble in each corner (up to 4 nodes), colour-coded from cold to warm.
- **Node health at a glance:** OK, low or empty battery, charging, and *stuck* (no report for 1.5× the node's own sleep time).
- **History graphs:** about 8 days per node, single or combined, kept in RAM and snapshotted to flash hourly so it survives power loss.
- **Smart temperature:** DS18B20 when present, otherwise CPU temperature minus a per-node offset, clearly marked as *estimated*.
- **Zero-reflash setup:** Wi-Fi, node names, corners and offsets are set in the web UI (PIN protected).
- **Works without internet:** the page has no external resources, graphs are drawn as inline SVG.

## How it works

```
 Node ─┐  ESP-NOW broadcast (27-byte packet, CRC32, sent 3×)
 Node ─┤──────────────►  Server ──► validate ► dedupe ► history ► web UI
 Node ─┤                  XIAO ESP32C6          (RAM + LittleFS)   http://roomtemp.local
 Node ─┘
```

The server joins your router and follows its Wi-Fi channel. If the router is unreachable it starts its own access point on the last known channel, so nodes keep being received.

## Important: Wi-Fi channel

ESP-NOW and Wi-Fi share one radio, so **the node's `ESPNOW_CHANNEL` must equal the router's channel**. Nodes are not changed automatically: set it in each node's `config.h` and reflash. The dashboard shows the channel the nodes must use. Setting the router to a **fixed** 2.4 GHz channel is strongly recommended; if it moves, all nodes will show as *stuck*.

## Getting started

1. Arduino IDE 2.x, ESP32 core 3.x, board **XIAO_ESP32C6**, USB CDC On Boot enabled, a partition scheme with a filesystem (≥ 160 KB). No extra libraries needed.
2. Open `temperature_server_src/temperature_server_src.ino`, review `config.h` (**change `AP_PASSWORD` and `DEFAULT_PIN`**), upload.
3. On first boot join the Wi-Fi network `RoomTemp-Setup`, open `http://192.168.4.1`, and enter your router credentials.
4. Open `http://roomtemp.local` once connected. Nodes appear automatically on their first packet.

## Project layout

```
temperature_server/
├── requirements.md, implementation-plan.md, traceability.md
├── temperature_server_src/   Arduino sketch (config.h, node_packet.h, modules)
├── web/index.html            dashboard source (edit this, not web_page.h)
├── tools/                    build_web.py, mock_server.py, check_packet.sh, test_sender/
└── test/                     host unit tests for the core logic
```

`node_packet.h` is copied unchanged from the node firmware (`../temperature_ndoe`).

## Development

```bash
make -C test                          # host unit tests of the core logic
python3 tools/mock_server.py          # UI with fake data at http://localhost:8080 (scenarios: /__scenario/mixed, stuck, firstrun, notime, unknown)
python3 tools/build_web.py            # embed web/index.html into the firmware (web_page.h)
tools/check_packet.sh                 # node and server packet definitions still identical?
python3 tools/traceability.py         # which code implements which requirement
```

`tools/test_sender` is a sketch for a spare XIAO that imitates four nodes (healthy, low battery, no DS18B20, one that goes silent) for bench tests.

## Limitations

- No HTTPS or encryption; ESP-NOW is unauthenticated (CRC only). Intended for a trusted home network.
- Without NTP (access-point-only mode) graphs show relative time.
- At most 4 nodes by default (`MAX_NODES`).
