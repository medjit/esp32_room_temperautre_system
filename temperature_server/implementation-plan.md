# XIAO ESP32C6 Temperature Server — Implementation Plan

| Field | Value |
|---|---|
| Document | Implementation Plan |
| Version | 1.1 (updated after implementation) |
| Date | 2026-10-13 |
| Based on | `requirements.md` v1.0 |
| Target hardware | Seeed Studio XIAO ESP32C6 |
| Development environment | Arduino IDE, ESP32 Arduino core 3.x |

---

## 1. Purpose

This document describes how the requirements in `requirements.md` are implemented: repository layout, toolchain, module structure, the key technical decisions, implementation phases with verification, traceability and test cases.

## 2. Design Principles

1. **Never block reception.** The ESP-NOW receive callback only copies the frame into a queue. Everything else runs in `loop()`. HTTP handling is the only long-running work and is kept short.
2. **Simplicity, but modular.** The node is one file. The server is larger (network, storage, web), so it is split into small modules with one responsibility each. No RTOS tasks of our own, no dynamic allocation for history or JSON, no third-party libraries.
3. **Pure logic is separated and host-testable.** Everything that decides *what the data means* (frame validation, dedupe, temperature selection, stuck/status evaluation, history ring, battery %) lives in hardware-free headers that also compile on a PC, so it can be unit-tested in seconds without flashing.
4. **UI is developed off-device.** The web page is a normal HTML file served by a small mock server with fake data, so design iteration needs no flashing. A script converts it into the firmware header.
5. **Fail forward.** A failing subsystem (LittleFS, NTP, mDNS, NVS) is logged and bypassed; reception and web keep running.
6. **Traceability.** Every function lists the requirement identifiers it implements, as in the node.

## 3. Repository Layout

The Arduino IDE needs the main sketch file named like its folder; the existing `temperature_server_src/` folder is the sketch folder.

```
temperature_server/
├── requirements.md
├── implementation-plan.md
├── traceability.md                       ← GENERATED: requirement → where implemented
├── README.md
├── temperature_server_src/               ← Arduino sketch folder (open the .ino)
│   ├── temperature_server_src.ino        ← setup()/loop(): module wiring only
│   ├── config.h                          ← all compile-time settings
│   ├── node_packet.h                     ← NodePacket, copied UNCHANGED from the node
│   ├── debug.h                           ← DBG_* macros
│   ├── board.h / board.cpp               ← pins, RF switch / antenna, LED, BOOT button
│   ├── logic.h                           ← pure logic (host-testable, header-only)
│   ├── history.h / history.cpp           ← ring buffers, downsampling, time rebasing
│   ├── nodes.h / nodes.cpp               ← node slots, settings, statistics
│   ├── rx.h / rx.cpp                     ← ESP-NOW init, callback, queue, validation, LED
│   ├── netmgr.h / netmgr.cpp             ← Wi-Fi STA/AP state machine, channel, mDNS, NTP, DNS
│   ├── store.h / store.cpp               ← NVS settings, LittleFS snapshot, factory reset
│   ├── web.h / web.cpp                   ← WebServer routes, JSON writer, PIN, lockout
│   └── web_page.h                        ← GENERATED gzip byte array of the UI
├── web/
│   └── index.html                        ← UI source (HTML + CSS + JS, single file)
├── tools/
│   ├── build_web.py                      ← index.html → minify → gzip → web_page.h
│   ├── mock_server.py                    ← serves index.html + fake /api/* for UI work
│   ├── check_packet.sh                   ← proves node_packet.h equals the node's block
│   ├── traceability.py                   ← writes traceability.md from the REQ tags in the code
│   └── test_sender/test_sender.ino       ← fake nodes for bench tests (not a deliverable)
└── test/
    ├── Makefile
    └── test_logic.cpp                    ← host unit tests for logic.h / history
```

`.gitignore` additions: `*.bin`, `*.elf`, `build/`, `test/test_logic` (binary). `web_page.h` **is** committed so the IDE build needs no Python.

| Module | Responsibility | Main requirements |
|---|---|---|
| `config.h` | Every `#define` of Section 7 + `static_assert` sanity checks | GEN-02, SEC-03 |
| `node_packet.h` | Byte-identical copy of the node's packet block | GEN-06 |
| `debug.h` | `DBG_BEGIN/DBG_PRINTF/DBG_RUN`, compiled out when off | DBG-01 |
| `board` | RF switch / antenna pins, LED, BOOT button | NET-*, RX-06, PERS-06 |
| `logic.h` | `frameValid`, `isDuplicate`, `pickTemperature`, `evalStatus`, `batteryPercent`, `Sample` packing | RX-02/03, DATA-*, STATE-* |
| `history` | Static ring buffers, append, range query, downsample, rebase | HIST-* |
| `nodes` | Slot table, auto-create, corner swap, delete, stats | NODE-*, ERR-02 |
| `rx` | Receive path | RX-* |
| `netmgr` | Connectivity | NET-*, TIME-* |
| `store` | Persistence | PERS-* |
| `web` | HTTP + API + security | WEB-*, SEC-* |
| `web/index.html` | The face of the project | UI-* |

## 4. Toolchain Setup

| Item | Setting |
|---|---|
| IDE | Arduino IDE 2.x |
| Board package | "esp32" by Espressif, 3.x (same as node) |
| Board | XIAO_ESP32C6 |
| USB CDC On Boot | Enabled |
| Partition scheme | One with a LittleFS/SPIFFS data partition. **Minimum 160 KB** for the filesystem (snapshot ≈ 37 KB, the atomic write needs old + temp file ≈ 75 KB). Verified in Phase 3; recorded in `README.md` |
| Libraries | none (all from the core) |
| Host tools | Python 3 (UI build + mock server), `g++`/`make` (host tests) |
| Serial monitor | 115200 baud |

The exact core version used for the first successful build is recorded in `README.md`.

## 5. Firmware Structure

### 5.1 Data Structures

```cpp
// history.h — one stored measurement (12 bytes, REQ-HIST-02, limit 16)
typedef struct __attribute__((packed)) {
    uint32_t ts;          // epoch s, or virtual clock s (see 6.4)
    int16_t  room_cC;     // selected room temperature or TEMP_INVALID
    int16_t  cpu_cC;      // raw CPU temperature or TEMP_INVALID
    uint16_t bat_mv;
    uint8_t  bits;        // [1:0] source (0 none,1 ds,2 est) [3:2] bat state [4] charging
    uint8_t  reserved;
} Sample;
static_assert(sizeof(Sample) == 12, "Sample size");

typedef struct {
    Sample   buf[HIST_POINTS];   // static, per slot (REQ-HIST-03)
    uint16_t head, count;
} History;
```

```cpp
// nodes.h — one node slot
typedef struct {
    bool     used;
    uint8_t  node_id;
    // user settings (NVS, REQ-NODE-02)
    char     name[21];
    uint8_t  corner;             // 0 NW 1 NE 2 SW 3 SE
    int16_t  cpu_offset_cC;
    // latest state (RAM, REQ-NODE-05)
    NodePacket last;             // last accepted packet
    bool       have_last;
    uint32_t   last_rx_ms;       // millis() at reception, valid if have_last
    int8_t     rssi;
    uint32_t   rx_count, dup_count, crc_err_count;
    uint32_t   saved_sleep_s;    // restored from NVS after reboot (REQ-STATE-04)
    History    hist;
} NodeSlot;

NodeSlot g_nodes[MAX_NODES];
```

RAM budget: `Sample` 12 B × 768 × 4 ≈ 37 KB history, queue 16 × ~40 B, JSON writer uses streaming (no large buffers). Well inside ESP32-C6 RAM; free heap low-water mark is shown in the UI (REQ-ERR-04).

### 5.2 Core Logic (`logic.h`, host-testable)

| Function | Behaviour | Requirement |
|---|---|---|
| `frameValid(data,len)` | Length 27, `packet_version == 1`, recompute CRC32 over first 23 bytes. Returns an enum `{OK, BAD_LEN, BAD_VER, BAD_CRC}` | RX-02 |
| `isDuplicate(slot, pkt)` | `have_last && last.boot_count == pkt.boot_count` (equality only; lower values are new data) | RX-03 |
| `pickTemperature(pkt, offset)` → `{value, source}` | DS valid → DS; else CPU − offset (`ESTIMATED`); else NONE. Valid = not `-32768` and the corresponding error flags clear | DATA-02 |
| `evalStatus(slot, now_ms)` | Priority STUCK > EMPTY > LOW > BAT_INVALID > OK, WAITING before first packet, uses `STUCK_FACTOR × sleep_time_s`, `millis()` arithmetic using unsigned subtraction (wrap safe after 49 days) | STATE-01..04 |
| `batteryPercent(mv)` | Fixed 11-point Li-ion lookup, linear interpolation, informational | DATA-03 |
| `packSample(...)` / accessors | Fill / read `Sample` | HIST-02 |

CRC note: `esp_rom_crc32_le(0, buf, n)` is the standard zlib CRC-32. On the host the test uses zlib's `crc32()`, or a 10-line table implementation, so no ESP headers are needed. Phase 2 checks it against a real node packet.

### 5.3 Module Functions

| Module | Function | Responsibility | Requirements |
|---|---|---|---|
| `rx` | `rxInit()` | `esp_now_init`, register receive callback (version guard, 5.4), init queue | RX-01 |
| `rx` | `onRecv(...)` | Copy `{data[27], len, rssi, millis()}` into the ring queue under a short critical section; drop oldest when full | RX-01, RX-04, RX-05 |
| `rx` | `rxPoll()` | Called from `loop()`: pop frames, validate, dedupe, find/create slot, update slot, append history, update stats, blink LED | RX-02, RX-03, NODE-01, NODE-05 |
| `nodes` | `nodesInit()`, `findSlot(id)`, `createSlot(id)`, `setCorner(slot,c)` (swap), `forgetSlot(slot)` | Slot management | NODE-01..04 |
| `history` | `histAppend`, `histQuery(slot, from, out, max)`, `histDownsample`, `histRebase(delta)` | Ring operations | HIST-01..07 |
| `netmgr` | `netBegin()`, `netTick()` | State machine (5.5) | NET-01..09 |
| `netmgr` | `netChannel()`, `netInfo()` | For UI and stats | NET-07 |
| `netmgr` | `timeValid()`, `nowEpoch()`, `nowVirtual()` | Clock sources | TIME-01..03, HIST-05 |
| `store` | `settingsLoad/Save`, `snapshotLoad()`, `snapshotSave()`, `factoryReset()`, `resetButtonPoll()` | Persistence | PERS-01..06 |
| `web` | `webBegin()`, `webTick()`, route handlers, `jsonState()`, `jsonHistory()` | HTTP + API | WEB-*, SEC-* |
| `.ino` | `setup()`, `loop()` | Wiring and scheduling | GEN-04, ERR-03 |

### 5.4 `setup()` and `loop()`

```cpp
void setup() {
    DBG_BEGIN();
    rfSwitchInit();                       // GPIO3 LOW, GPIO14 per USE_EXTERNAL_ANTENNA
    ledInit();
    wdtStart();                           // REQ-ERR-03
    storeInit();                          // NVS + LittleFS (failures non-fatal, REQ-ERR-01)
    nodesInit();                          // settings → slots
    snapshotLoad();                       // history restore, REQ-PERS-05
    netBegin();                           // STA try → AP fallback, mDNS, NTP
    rxInit();                             // after Wi-Fi is up (channel is final)
    webBegin();
}

void loop() {
    esp_task_wdt_reset();
    rxPoll();                             // always first: drain frames
    nodesEvalStatus();                    // 1 Hz: transitions + DBG log
    netTick();                            // reconnect / fallback / grace / NTP rebase
    webTick();                            // server.handleClient()
    storeTick();                          // hourly snapshot, reset button
    rxLedTick();
    heartbeat();                          // DBG only
}
```

`loop()` has no `delay()` longer than 1 ms; a `delay(1)` yield at the end keeps the Wi-Fi task and idle task healthy.

### 5.5 ESP-NOW Receive Callback

The signature differs between Arduino core 3.x builds (`const esp_now_recv_info_t*` in recent ones, `const uint8_t* mac` in older ones). The same compile-time trick as in the node is used: ask the compiler for the parameter type of `esp_now_register_recv_cb`, wrapped in a struct to avoid Arduino prototype generator problems. RSSI is read through `info->rx_ctrl->rssi` where available, otherwise reported as 0 (the UI then hides the signal bars).

Queue: static array of 16 entries + head/tail index guarded by a `portMUX_TYPE` spinlock (callback context is a task, not an ISR, so `portENTER_CRITICAL` is safe and short). On overflow the oldest entry is overwritten and `queue_overflow++`.

### 5.6 Wi-Fi State Machine (`netmgr`)

```
            ┌──────────┐ creds? ┌────────────┐ ok   ┌────────┐
   BOOT ───►│ NO_CREDS │──no───►│ STA_TRY    │─────►│ STA_OK │◄─────────┐
            └──────────┘   │    │ (≤15 s)    │      └───┬────┘          │ ok
                           │    └─────┬──────┘   lost>  │               │
                           │          │fail     30 s    ▼               │
                           ▼          ▼               ┌───────────────┐ │
                      ┌───────────────────────────────┤ FALLBACK_AP   │─┘
                      │  AP on last_channel (AP+STA)  │ retry /5 min  │
                      └───────────────────────────────┴───────────────┘
```

- **STA_OK:** channel = `WiFi.channel()`; stored to NVS when it differs from the stored value (REQ-NET-03/04). mDNS and NTP started once. `WiFi.setSleep(false)` (REQ-GEN-04).
- **FALLBACK_AP:** `WiFi.mode(WIFI_AP_STA)`, `WiFi.softAP(AP_SSID, AP_PASSWORD, last_channel)`. The retry uses `WiFi.begin()` no more often than `STA_RETRY_INTERVAL_S` and not while the web server saw a request in the last 30 s (REQ-NET-05). On success: keep AP for `AP_GRACE_S`, then `softAPdisconnect()`, state → `STA_OK`.
- **Router lost while in STA_OK:** automatic reconnect (core). After 30 s without link switch to FALLBACK_AP so the system stays usable and nodes remain receivable on the last channel (REQ-NET-09).
- Wi-Fi credentials saved from the UI → `WiFi.begin` immediately, state → `STA_TRY`, UI polls `/api/state` for the result.
- **DNS captive portal (MAY, REQ-UI-10):** `DNSServer` answering `*` → `192.168.4.1` only while in AP mode with no credentials.
- A joined-AP channel mismatch is impossible by design: while STA is connected the AP is off or follows the same channel.

## 6. Implementation Details and Decisions

### 6.1 Frame Handling Order

1. `rxPoll()` pops a frame → `frameValid()`; each failure increments its own counter (REQ-ERR-02).
2. `findSlot(node_id)`; if absent and a free slot exists → `createSlot` (name "Node N", next free corner, default offset); if absent and no free slot → `unknown_node++`, remember the id (small set of ≤8 ids) for the UI notice (REQ-NODE-01).
3. `isDuplicate` → `dup_count++`, stop. (The three copies arrive within about 30 ms; this is the common path.)
4. Update `last`, `last_rx_ms`, `rssi`, `saved_sleep_s`, counters.
5. `pickTemperature` → `Sample` → `histAppend`.
6. Persist the new `sleep_time_s` to NVS only if it changed (rare; REQ-PERS-01, limits NVS wear).
7. LED pulse.

### 6.2 Status Evaluation (REQ-STATE)

`nodesEvalStatus()` runs once per second, calls `evalStatus()` for each used slot, and on a change logs it (DBG) and updates the cached status. The reference time for `WAITING/STUCK` before the first packet after boot is `boot_ms = 0`, i.e. `millis()` itself, with `saved_sleep_s` as the timeout base (REQ-STATE-04). If no `saved_sleep_s` exists yet (a brand-new slot always receives a packet first), the node is `WAITING` with no timeout.

### 6.3 Temperature Selection and Offsets (REQ-DATA-02, HIST-04)

The selection is evaluated at reception and stored; the raw CPU value is stored too. The UI, when drawing, may recompute estimated points with the current offset (`room = cpu − offset` for samples whose source is `ESTIMATED`), so changing an offset corrects the whole graph instantly without rewriting RAM.

### 6.4 Time and Timestamps (REQ-HIST-05, TIME)

- `nowEpoch()` returns epoch seconds if `timeValid()` (system time > 2024-01-01 after NTP), else 0.
- Samples recorded **before** NTP use a *virtual clock*: `VIRTUAL_BASE (1 000 000) + uptime_s`. Real epochs (≈1.7 × 10⁹) never collide with it, so no flag bit is needed.
- When NTP first succeeds, `histRebase(epoch_now − (VIRTUAL_BASE + uptime_s_now))` adds the delta to every virtual-clock sample in all slots (single pass over ≤ 3072 samples).
- Snapshot restore: the file header stores `saved_uptime_s`. Virtual samples are shifted back by that amount, so the save moment becomes the new `VIRTUAL_BASE + 0`; real-epoch samples are kept as they are. Once NTP delivers the time, the remaining virtual samples are converted as above. The power-off gap is unknown for virtual samples (they are treated as if the server had not been off); the UI draws the axis as "x min ago" while there is no clock. This limitation is documented.
- `STUCK` and dedupe never use these clocks (REQ-TIME-03).

### 6.5 Persistence (REQ-PERS)

**NVS (`Preferences`, namespace `rtserver`):**

| Key | Content |
|---|---|
| `ssid`, `pass` | Wi-Fi credentials |
| `chan` | last router channel |
| `pin` | settings PIN |
| `room` | room name |
| `n<i>` (blob) | per-slot: node_id, name, corner, offset, saved_sleep_s |

**LittleFS snapshot `/hist.bin`:**

```
header: magic(4) | fmt_version(2) | slot_count(1) | saved_uptime_s(4)
per slot: node_id(1) | count(2) | head(2) | samples[count] (12 B each, in chronological order)
trailer: crc32 over everything above
```

Save procedure (REQ-PERS-03): write `/hist.tmp` → flush → re-open and verify the CRC → `LittleFS.remove("/hist.bin")` + `rename("/hist.tmp","/hist.bin")`. If the device loses power between remove and rename, boot finds `/hist.tmp` without `/hist.bin` and promotes the valid temp file. Saved every `HIST_SAVE_INTERVAL_S` only when `dirty` (REQ-PERS-04), and before restart/factory reset. A snapshot takes tens of ms; it runs in `loop()` between frame drains (frames wait in the queue; 16 entries cover ≥ 500 ms of a 4-node burst).

Restore: header magic, version and CRC validated; an invalid file is ignored (REQ-PERS-02). Node ids in the file are matched to NVS slots; unmatched ones are dropped.

**Factory reset (REQ-PERS-06):** `Preferences.clear()`, delete `/hist.bin`, restart. Triggered by the PIN-protected API or by holding BOOT (GPIO9) ≥ `FACTORY_RESET_HOLD_S` within the first 30 s after boot (LED blinks quickly while held).

### 6.6 Web Server, JSON, Security (REQ-WEB, SEC, ERR-05)

- Core `WebServer` on port 80. Handlers are registered once in `webBegin()`.
- **`/`**: `server.sendHeader("Content-Encoding","gzip")` + `server.send_P(200,"text/html", web_page_gz, web_page_gz_len)`; cache headers `no-cache` so a new firmware always shows the new UI.
- **JSON writer:** a tiny class `JsonOut` that writes through `server.sendContent()` in 512-byte chunks (`chunked` transfer), with `key(name)`, `num`, `str` (with escaping), `begin/endObject/Array`. This guarantees bounded memory irrespective of response size (history ≈ 300 points) and satisfies REQ-ERR-05.
- **`/api/state`** (example shape):

```json
{ "server": { "fw":1, "uptime_s":86400, "heap_free":151000, "heap_min":128000,
              "wifi":{"mode":"sta","ssid":"Home","ip":"192.168.1.20","channel":6,"rssi":-58},
              "time_valid":true, "epoch":1760000000, "pin_default":true,
              "stats":{"rx":120,"ok":40,"dup":80,"bad_len":0,"bad_ver":0,"bad_crc":1,"unknown":0,"overflow":0},
              "unknown_ids":[] },
  "room": "Living room",
  "nodes": [ { "id":1, "name":"Window", "corner":0, "status":"OK", "charging":false,
               "temp_cC":2147, "src":"ds", "cpu_cC":3012, "bat_mv":3980, "bat_state":0,
               "bat_pct":82, "vbus_mv":0, "rssi":-61, "sleep_s":900, "age_s":312,
               "flags":0, "fw":1, "boot":57, "rx":57, "dups":114, "crc_err":0,
               "cpu_offset_cC":800 } ] }
```

- **Additional endpoints added during implementation** (all PIN protected, form-encoded): `POST /api/auth` (PIN check for the page), `POST /api/room`, `POST /api/history/clear`.
- **`/api/history?node=<id>&from=<epoch>`**: oldest→newest, reduced to ≤ `HIST_API_MAX_POINTS` by merging equal groups of consecutive samples (time and values averaged, invalid values ignored, flag bits from the newest sample). It returns the parallel arrays `t`, `v`, `s`, `c`, `b` plus `step` (samples merged per point), which the page uses to size its gap detection.
- **Wi-Fi scan:** `GET /api/wifi/scan` starts `WiFi.scanNetworks(true)` (async) and returns `{"scanning":true}` until finished, then the list. A scan temporarily hops channels, so the UI warns that reception pauses briefly and the server only scans on explicit user request.
- **PIN:** header `X-Pin`. Compared in constant time. Wrong attempts counted globally; ≥ `PIN_MAX_ATTEMPTS` → `429` for `PIN_LOCKOUT_S` (REQ-SEC-02). `pin_default` flag in `/api/state` lets the UI warn. The Wi-Fi password is write-only (REQ-SEC-03).
- Request size limits: bodies > 512 B rejected; names truncated to 20 chars and HTML-escaped by the UI (`textContent`, never `innerHTML` for user strings).
- **Handler budget (REQ-WEB-07):** the handlers only read RAM and stream; the longest is the history response (≈ 18 KB, a few ms on the local network). NVS writes (settings) take a few ms.
- Poll rate and client count: the core `WebServer` is single-threaded; 4 clients polling every 10 s is trivially served. Keep-alive is disabled (`Connection: close`) so idle clients do not block the server.

### 6.7 Web UI Design (`web/index.html`)

**Technology:** vanilla JS (ES2020), CSS custom properties, inline SVG. No frameworks, no external requests (REQ-WEB-03). Target gzip size ≤ 40 KB.

**Design tokens (CSS variables):** surface/background, text, muted, accent, `--ok` green, `--warn` amber, `--bad` red, `--info` blue, radii, shadows; a second set under `@media (prefers-color-scheme: dark)`. A temperature gradient function `tempColor(t)` interpolates through blue → teal → green → amber → red between `UI_TEMP_MIN_C` and `UI_TEMP_MAX_C` (served by `/api/state` as `ui`).

**Layout (mobile first, grid on wide screens):**

1. **Header strip (UI-04):** room name, clock, average / min / max, health pill ("All 4 nodes OK" / "1 node needs attention").
2. **Room view (UI-01/02):** an SVG/CSS rounded rectangle with soft floor gradient; optional smooth heat-map (an SVG radial-gradient per corner, tinted with the node colour, blended with `mix-blend-mode`), four *node bubbles* anchored to NW/NE/SW/SE. A bubble contains: node name, big temperature with `~` for estimated, a small status icon (UI-03), a tiny battery icon only when LOW/EMPTY/INVALID, and a plug icon when charging. Stuck bubbles are dimmed with a "last seen 2 h ago" caption. Temperature changes animate (number tween, colour transition), disabled under `prefers-reduced-motion`.
3. **Combined graph (UI-06):** all nodes, legend toggles, range chips 6 h / 24 h / 7 d / all.
4. **Node detail sheet (UI-05):** opens from a bubble; temperature chart (area + line), crosshair tooltip on hover/touch, battery voltage chart, decoded fields table, RSSI bars, flags decoded (DATA-04), firmware version, and the PIN-gated settings (name, corner, offset) with live "offset applies to graph" preview.
5. **System section (UI-08):** collapsible; Wi-Fi mode, channel + node-matching hint ("Nodes must use channel 6"), IP, uptime, heap, reception statistics, firmware version.
6. **Settings (UI-09):** Wi-Fi setup (scan list, password), room name, node list with forget buttons, PIN change, display range, restart, factory reset (typed confirmation).
7. **First-run banner (UI-10):** shown when `wifi.mode=="ap"` and no credentials.
8. **Connection banner (UI-12):** shown after 2 failed polls; data stays visible.

**Chart engine (≈ 200 lines):** one function `drawChart(svg, series[], range)`: computes nice axis ticks, builds `<path>` per series, breaks the path where the gap between points is > 2 × the node's sleep time (UI-07), draws estimated segments dashed, handles the "x min ago" axis when `time_valid=false`, and a pointer-based crosshair with nearest-point snapping.

**Accessibility (UI-11):** every icon has `role="img"` + `aria-label`/`<title>`, bubbles are `<button>` elements, focus rings, status carried by icon + text + colour, contrast ≥ 4.5:1 in both themes, touch targets ≥ 44 px.

**Dev workflow:** `tools/mock_server.py` serves `web/index.html` and generates realistic fake `/api/state` and `/api/history` responses (including STUCK, LOW, EMPTY, charging, estimated and no-time scenarios selectable with a query parameter such as `?scenario=stuck`). The UI is iterated in the built-in browser at phone and desktop widths and in light and dark mode. `tools/build_web.py` then minifies (simple regex-based whitespace/comment strip, no npm), gzips and writes `web_page.h`.

### 6.8 Robustness (REQ-ERR)

- **Watchdog:** `esp_task_wdt` reconfigured to `WDT_TIMEOUT_S`, `loop()` feeds it. Blocking calls that could exceed it (Wi-Fi scan, snapshot) are bounded or asynchronous.
- **Heap monitor:** `heap_min` from `ESP.getMinFreeHeap()`, shown in the UI. Soak test (Phase 8) checks it stays flat.
- **Daily restart (optional, REQ-ERR-04):** at 04:00 local time if `DAILY_RESTART_ENABLED` and `timeValid()`; saves the snapshot first.
- **Heartbeat (DBG):** every 60 s: uptime, heap, queue high-water mark, Wi-Fi state.

### 6.9 Debug Output (REQ-DBG)

Same macro style as the node (`[ time ms]` prefix). Log lines: boot, `WIFI: STA_OK ch6`, `RX: node 2 boot 57 OK dup rssi -61`, `RX: REJECT bad_crc`, `STATE: node 3 OK -> STUCK`, `SNAP: saved 36 KB in 41 ms`.

## 7. Implementation Phases

Each phase ends with a build, verification (host test, mock server or hardware) and a git commit referencing the phase and requirement ids, as in the node project.

### Phase 0 — Skeleton and Tooling

| Step | Action |
|---|---|
| 0.1 | Create `temperature_server_src/` skeleton: `.ino` (empty `setup/loop`), `config.h` with every Section 7 setting + `static_assert`s, `debug.h` |
| 0.2 | Copy the NodePacket block unchanged into `node_packet.h`; add `static_assert(sizeof == 27)` |
| 0.3 | `test/Makefile` + empty `test_logic.cpp`; `tools/` placeholders; `.gitignore` update |
| 0.4 | Build and upload the empty sketch; record core version and partition scheme |

**Verification:** build OK; a diff check script (`tools/check_packet.sh`) proves `node_packet.h` equals the block in the node sketch.
**Commit:** `Phase 0: server skeleton, config.h, node_packet.h (REQ-GEN-01..03, GEN-06)`

### Phase 1 — Pure Logic and History (host-tested)

| Step | Action |
|---|---|
| 1.1 | `logic.h`: `frameValid`, `isDuplicate`, `pickTemperature`, `evalStatus`, `batteryPercent`, `packSample` |
| 1.2 | `history.cpp`: ring append, range query, downsample, `histRebase` |
| 1.3 | Host tests T-1.1 … T-1.12 (Section 9) with `make -C test` |
| 1.4 | `nodes.cpp`: slot table, auto-create, corner swap, forget (settings in RAM only for now) |

**Verification:** all host tests pass; includes `millis()` wrap, boot_count restart (lower value accepted), three identical copies → one sample.
**Commit:** `Phase 1: logic, history ring, node slots (REQ-RX-02/03, REQ-DATA-*, REQ-STATE-*, REQ-HIST-*, REQ-NODE-*)`

### Phase 2 — ESP-NOW Reception

| Step | Action |
|---|---|
| 2.1 | `rfSwitchInit()`, Wi-Fi STA on `ESPNOW_CHANNEL_DEFAULT` (temporary), `WiFi.setSleep(false)` |
| 2.2 | `rx.cpp`: callback with version-guard, ring queue, overflow handling |
| 2.3 | `rxPoll()` wired to logic + nodes + history; statistics; LED pulse |
| 2.4 | `tools/test_sender/test_sender.ino`: sends crafted packets (good, bad CRC, wrong version, duplicates, other node ids, `boot_count` restarting) |
| 2.5 | Test with a real node (`DEBUG_ENABLED 1` on the receiver) |

**Verification:** T-2.x. CRC from the host test equals the CRC accepted from a **real** node packet; three copies produce one stored sample; counters tally.
**Commit:** `Phase 2: ESP-NOW receive path (REQ-RX-*, REQ-ERR-02, REQ-DBG-*)`

### Phase 3 — Persistence

| Step | Action |
|---|---|
| 3.1 | `store.cpp`: NVS settings load/save for slots, room name, PIN, channel |
| 3.2 | LittleFS mount (failure = persistence off, REQ-ERR-01); snapshot format, save, atomic rename, recovery of `hist.tmp` |
| 3.3 | Restore at boot; `dirty` flag; hourly timer (shortened by a debug define for tests) |
| 3.4 | Factory reset function and BOOT-button hold detection |
| 3.5 | Verify partition size margin (snapshot + temp file) |

**Verification:** T-3.x: power-cycle keeps history and settings; pulling power mid-save leaves the previous snapshot intact; corrupt file is ignored.
**Commit:** `Phase 3: NVS settings and LittleFS history snapshot (REQ-PERS-*, REQ-STATE-04)`

### Phase 4 — Network Manager, Time

| Step | Action |
|---|---|
| 4.1 | `netmgr.cpp` state machine: STA try, AP fallback, channel store, retry rules, grace period |
| 4.2 | mDNS, NTP (`configTzTime`), `timeValid()`, virtual clock, `histRebase` on first sync |
| 4.3 | Captive-portal DNS in AP/no-credentials mode (MAY) |
| 4.4 | `netInfo()` data for UI; connect to ESP-NOW *after* the channel is settled; re-assert channel on every state change |
| 4.5 | Bench: router on a fixed channel ≠ 1, node configured to the same channel |

**Verification:** T-4.x including router power-off → AP on last channel while nodes continue to be received; router back → AP removed; no reboot needed.
**Commit:** `Phase 4: Wi-Fi manager, AP fallback, NTP, mDNS (REQ-NET-*, REQ-TIME-*, REQ-HIST-05)`

### Phase 5 — Web Server and API

| Step | Action |
|---|---|
| 5.1 | `web.cpp`: server setup, `/` placeholder page, `JsonOut` streaming writer |
| 5.2 | `/api/state`, `/api/history` (with downsampling) |
| 5.3 | PIN auth, lockout, `/api/node`, `/api/node/delete`, `/api/pin`, `/api/wifi`, `/api/wifi/scan`, `/api/restart`, `/api/factory-reset` |
| 5.4 | `/api/export.csv` (MAY) |
| 5.5 | `curl` test script covering every endpoint; load test with 4 parallel pollers |

**Verification:** T-5.x; reception counters keep increasing under polling load; no heap growth.
**Commit:** `Phase 5: web server and API (REQ-WEB-*, REQ-SEC-*, REQ-ERR-05)`

### Phase 6 — Web UI (developed against the mock server)

| Step | Action |
|---|---|
| 6.1 | `tools/mock_server.py` with scenarios; `tools/build_web.py` |
| 6.2 | Design tokens, themes, header strip, polling and connection banner |
| 6.3 | Room view with bubbles, colour scale, status icons, estimated badge, stuck dimming |
| 6.4 | Chart engine; node detail sheet (temperature + battery graphs, fields, RSSI) |
| 6.5 | Combined graph with legend toggles and range chips |
| 6.6 | System section, settings page, PIN prompt, first-run banner, factory reset confirmation |
| 6.7 | Responsive, dark mode, reduced-motion, keyboard and screen-reader pass; size budget check |
| 6.8 | Generate `web_page.h`, flash and verify on a phone and a desktop against real data |

**Verification:** T-6.x — visual review in the browser pane at 375 px, 768 px and desktop width in light/dark; every scenario of the mock renders correctly; the gzip size is ≤ 40 KB; the page works with the device offline from the internet.
**Commit:** `Phase 6: web UI (REQ-UI-*, REQ-WEB-02/03)`

### Phase 7 — Robustness

| Step | Action |
|---|---|
| 7.1 | Watchdog, heap monitor, heartbeat, daily restart option |
| 7.2 | Full statistics in UI System section |
| 7.3 | Fault injection: LittleFS unmounted, NVS erased, NTP unreachable, queue flooded (test sender at max rate) |
| 7.4 | 72-hour soak test with 4 real nodes (or test sender emulating 4 nodes at an accelerated interval) |

**Verification:** T-7.x; heap low-water mark flat; no unexplained resets; the status of an unplugged node turns STUCK at exactly 1.5 × its sleep time.
**Commit:** `Phase 7: robustness and soak (REQ-ERR-*, REQ-GEN-04)`

### Phase 8 — Integration and Final Verification

| Step | Action |
|---|---|
| 8.1 | Run the complete test plan (Section 9) with `DEBUG_ENABLED 0` and `1` |
| 8.2 | Requirement-by-requirement review against the traceability matrix (Section 8) |
| 8.3 | Complete `README.md`: wiring (none), build settings, first-time setup walkthrough, channel instructions for nodes, router recommendations, screenshots |
| 8.4 | Change default AP password/PIN guidance check; tag `server-v1.0` |

**Commit:** `Phase 8: integration, documentation, server v1.0`

## 8. Traceability Matrix

| Requirement group | Implemented in | Phase |
|---|---|---|
| REQ-GEN-01..06 | whole project, `config.h`, `node_packet.h`, `loop()` | 0, 2, 7 |
| REQ-NET-01..10 | `netmgr` | 4 |
| REQ-RX-01..06 | `rx`, `logic.h` | 1, 2 |
| REQ-NODE-01..05 | `nodes`, `store`, `web` | 1, 3, 5 |
| REQ-DATA-01..05 | `logic.h`, `web` (JSON), `index.html` | 1, 5, 6 |
| REQ-STATE-01..07 | `logic.h::evalStatus`, `nodes`, `store` | 1, 3 |
| REQ-HIST-01..07 | `history`, `netmgr` (rebase), `web` | 1, 4, 5 |
| REQ-PERS-01..06 | `store` | 3 |
| REQ-WEB-01..08 | `web`, `web_page.h` | 5, 6 |
| REQ-UI-01..13 | `web/index.html` | 6 |
| REQ-SEC-01..04 | `web`, `config.h` | 5 |
| REQ-TIME-01..03 | `netmgr`, `logic.h` | 4 |
| REQ-ERR-01..05 | all modules, `.ino` | 2–7 |
| REQ-DBG-01..02 | `debug.h`, all modules | 0–7 |

## 9. Test Cases

### 9.1 Host Unit Tests (`make -C test`)

| ID | Requirement(s) | Case | Expected |
|---|---|---|---|
| T-1.1 | RX-02 | 26 / 28 byte frame | `BAD_LEN` |
| T-1.2 | RX-02 | `packet_version = 2` | `BAD_VER` |
| T-1.3 | RX-02 | One flipped bit | `BAD_CRC` |
| T-1.4 | RX-02 | Valid frame generated with zlib CRC | `OK` |
| T-1.5 | RX-03 | Same `(id, boot)` three times | 1 accepted, 2 duplicates |
| T-1.6 | RX-03 | `boot` 57 → 1 (node power loss) | accepted as new |
| T-1.7 | DATA-02 | DS valid | source DS, value = DS |
| T-1.8 | DATA-02 | DS absent flag, CPU 30.12, offset 8.00 | `ESTIMATED` 22.12 |
| T-1.9 | DATA-02 | Both invalid | `NONE` |
| T-1.10 | STATE-01/02 | sleep 900, age 1349 s / 1351 s | OK / STUCK (boundary 1350 s) |
| T-1.11 | STATE-01 | priority: stuck + low battery | STUCK; charging overlay independent |
| T-1.12 | STATE-03 | `millis()` wraps between packet and evaluation | still correct |
| T-1.13 | HIST-01 | Append 800 samples into 768 | oldest 32 overwritten, order preserved |
| T-1.14 | HIST-06 | 768 samples → API limit 300 | ≤ 300 points, `step` = 3, averages correct, invalid values ignored |
| T-1.15 | HIST-05 | Rebase of virtual samples | timestamps shifted, real ones untouched |
| T-1.16 | NODE-04 | Assign corner in use | corners swapped |

### 9.2 Hardware / Integration

| ID | Requirement(s) | Setup | Expected result |
|---|---|---|---|
| T-2.1 | RX-01..03 | Real node sends 3 copies | one history sample, `dup=2`, LED blinks once |
| T-2.2 | RX-02 | Test sender: bad CRC / version / length | counted in the matching counters, no slot change |
| T-2.3 | RX-04 | Sender floods > 16 frames in a burst while a snapshot is written | `overflow` counter explains any loss; no crash |
| T-2.4 | NODE-01 | 5th node id | not added; UI notice with its id |
| T-3.1 | PERS-01/05 | Reboot | settings, history and `saved_sleep_s` restored |
| T-3.2 | PERS-03 | Pull power during snapshot (repeat 10×) | previous or new snapshot valid, never corrupt |
| T-3.3 | PERS-02 | Corrupt `hist.bin` | ignored, history empty, device runs |
| T-3.4 | PERS-06 | BOOT held 10 s after boot / UI reset | all data erased, AP + first-run flow |
| T-4.1 | NET-01/03 | Router on channel 6, node set to 6 | packets received in STA mode |
| T-4.2 | NET-02/04 | Wrong password / router off | AP on the stored channel; nodes still received |
| T-4.3 | NET-05/06 | Router returns | server rejoins within the retry interval; AP stops after grace; no reboot |
| T-4.4 | NET-08 | LAN | `roomtemp.local` resolves |
| T-4.5 | TIME-01/02 | NTP reachable / unreachable | `time_valid` true / false; history rebased on first sync |
| T-4.6 | NET-03 | Router channel changed 6 → 11 without touching the node | nodes turn STUCK after 1.5 × sleep; UI shows channel 11 and the hint |
| T-5.1 | WEB-04/05 | `curl` all GET endpoints | valid JSON, expected fields |
| T-5.2 | WEB-08, SEC-01 | POST without / with wrong / correct PIN | 401 / 401 / 200 |
| T-5.3 | SEC-02 | 5 wrong PINs | 429 for 60 s |
| T-5.4 | SEC-03 | Inspect all responses | Wi-Fi password never present |
| T-5.5 | WEB-06/07 | 4 clients poll every 2 s for 10 min | no lost frames beyond baseline, no heap growth |
| T-6.1 | UI-01..03 | Mock scenarios OK / LOW / EMPTY / INVALID / STUCK / WAITING / charging / estimated | each has correct icon, tint, text |
| T-6.2 | UI-05..07 | Graph with gaps, estimated segments, no-time mode | breaks drawn, dashed estimate, "x min ago" axis |
| T-6.3 | UI-11 | Light/dark, 375 px/desktop, reduced motion, keyboard only | all usable |
| T-6.4 | WEB-03 | Device without internet | page fully functional (no external fetches in network panel) |
| T-6.5 | UI-10 | Fresh device | first-run banner, Wi-Fi setup works end to end |
| T-6.6 | UI-12 | Server unplugged while page is open | banner appears, data remains |
| T-6.7 | UI-04 | One node STUCK | summary excludes it; health pill shows attention |
| T-7.1 | ERR-01 | LittleFS unmountable | persistence disabled, everything else works |
| T-7.2 | ERR-03 | Forced hang | watchdog reset, state restored from flash |
| T-7.3 | STATE-02 | Unplug a real node (normal state) | STUCK at 22.5 min ± 5 s |
| T-7.4 | ERR-04, GEN-04 | 72 h soak | no resets, heap_min flat, no missed cycles |
| T-8.1 | DBG-01 | `DEBUG_ENABLED 0` | no serial output, identical behaviour |

## 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Router channel differs from node channel | No data, all nodes STUCK | Channel and hint always on the page (NET-07); README procedure; fixed router channel recommended |
| Wi-Fi scan / STA retry disturbs ESP-NOW and AP channel | Missed frames for seconds | Retry interval 5 min, skip while web client active, scan only on user request (NET-05); nodes send 3 copies |
| ESP-NOW receive callback signature differs per core version | Build failure | Compiler-deduced parameter type (5.5); RSSI optional |
| ESP32-C6 coexistence: AP+STA+ESP-NOW on one radio | Reduced throughput/latency | Packets are tiny and every 15 min; Phase 4 bench confirms; modem sleep disabled |
| Filesystem partition too small for atomic snapshot | Persistence disabled | Verify in Phase 3; ≥ 160 KB; fallback: single-file write with CRC and keep previous in RAM |
| Flash wear | Early device failure | One save per hour, only if dirty (< 9 000 saves/year at ~37 KB) |
| Heap fragmentation over months | Crash | No dynamic history/JSON; streaming writer; watchdog; optional daily restart; soak test |
| Core `WebServer` is single-threaded | One slow client stalls others | `Connection: close`, small handlers, request body limit |
| Node RC sleep timer drift | False STUCK | Factor 1.5 leaves ~50 % margin; configurable |
| Single lost cycle shows STUCK (accepted, REQ-STATE-06) | Occasional transient warnings | Auto-clears on the next packet; factor adjustable |
| Unauthenticated ESP-NOW / HTTP without TLS | Spoofed data, PIN sniffable on LAN | Documented limitation (SEC-04); acceptable for home use |
| Time unavailable in AP-only mode | No absolute timestamps | Virtual clock + "x min ago" axis; rebased when NTP appears |
| UI scope creep | Delays | Phase 6 split into 8 reviewable steps; MAY items (CSV, captive DNS, °F) last |

## 11. Implementation Status (2026-10-13)

All phases were implemented. What has been **verified so far** and what still needs a device:

| Phase | Result | How it was verified |
|---|---|---|
| 0 Skeleton and tooling | Done | Builds with `arduino-cli` for `XIAO_ESP32C6` (core 3.3.12) without warnings, also with `DEBUG_ENABLED 1`, `DAILY_RESTART_ENABLED 1`, `RX_LED_ENABLED 0`; `tools/check_packet.sh` confirms the packet block equals the node's |
| 1 Logic and history | Done | `make -C test`: 189 checks pass (frame validation, CRC reference value, duplicates, temperature choice, stuck boundary at exactly 1350 s, `millis()` wrap, ring buffer, downsampling, time rebasing) |
| 2 ESP-NOW reception | Written, compiles | **Needs hardware:** T-2.x with a real node and `tools/test_sender` |
| 3 Persistence | Written, compiles | **Needs hardware:** T-3.x, in particular power loss during a snapshot |
| 4 Network manager | Written, compiles | **Needs hardware:** T-4.x (router, AP fallback, channel behaviour, NTP, mDNS) |
| 5 Web server and API | Written, compiles | **Needs hardware** for T-5.x; the same API shape and PIN rules are exercised by the mock server |
| 6 Web UI | Done | Exercised against the mock server in the browser: all six scenarios, phone width and desktop, light and dark, PIN flow, node editing with corner swap, Wi-Fi setup flow, chart tooltip, relative-time mode. Gzip size 19.5 KB (budget 40 KB) |
| 7 Robustness | Written | Watchdog, heap monitor, heartbeat, optional daily restart are in; the 72 h soak test (T-7.4) **needs hardware** |
| 8 Integration | Partly | `traceability.md` shows every one of the 87 requirements referenced in code; the full test plan on hardware is still open |

Differences from the plan above: a small `board` module (pins, RF switch, LED, BOOT button) was split out; three extra PIN-protected endpoints exist (`/api/auth`, `/api/room`, `/api/history/clear`); history downsampling averages instead of keeping min/max; the snapshot header has no `saved_epoch`; the colour-scale range of the room view is a per-browser setting; the optional °C/°F toggle (REQ-UI-13, MAY) is not implemented.

Flash use is 88 % of the default 4 MB scheme's app partition, RAM 26 %. The default partition table has a 1.4 MB filesystem partition, far above the 160 KB minimum.
