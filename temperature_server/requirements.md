# XIAO ESP32C6 Temperature Server — Software Requirements Specification

| Field | Value |
|---|---|
| Document | Software Requirements Specification (SRS) |
| Version | 1.1 (aligned with the implementation) |
| Date | 2026-10-13 |
| Target hardware | Seeed Studio XIAO ESP32C6 (bare board, USB powered, no battery, no extra hardware) |
| Development environment | Arduino IDE, ESP32 Arduino core 3.x |
| Counterpart | `../temperature_ndoe/requirements.md` v1.1 (frozen, not modified by this project) |

---

## 1. Project Overview

The server is the receiving half of the room temperature system. It is a bare **XIAO ESP32C6** that is permanently powered and **never sleeps**. Its job is to:

1. listen continuously for ESP-NOW broadcast packets sent by the temperature nodes (4 nodes for now, one per room corner);
2. validate each packet (CRC32, version, length) and remove the duplicate copies (each node sends every packet three times);
3. keep the latest state of every node and a history of its readings, in RAM, with a periodic snapshot to flash;
4. serve a web page, "the face of the project": a top-down room view with a temperature point in each corner, per-node health icons (OK / low battery / charging / stuck) and history graphs.

The server **does not compensate the node hardware**, except for one documented step: when a node has no valid DS18B20 reading, the CPU temperature minus a per-node offset is shown as an *estimated* room temperature (see REQ-DATA-02).

Guiding principle: **simplicity and robustness**. The device runs unattended for months. It uses the same single-`config.h` approach and requirement-traceability style as the node.

## 2. Scope

**In scope:**

- firmware for the receiving/serving ESP32C6;
- ESP-NOW reception of `NodePacket` (format defined by the node, reused unchanged);
- Wi-Fi connectivity (router client with access-point fallback) and first-time Wi-Fi provisioning through the web UI;
- RAM history with flash snapshots;
- embedded web server, REST/JSON API and a single-page web UI.

**Out of scope:**

- any change to the node firmware or packet format (the node is frozen; node channel is changed by hand when needed);
- cloud, MQTT, Home Assistant integration, push notifications, e-mail/alerts;
- HTTPS / user accounts;
- OTA firmware update;
- multi-room support.

## 3. Definitions

| Term | Meaning |
|---|---|
| Node | A battery temperature transmitter (see counterpart SRS) |
| Server | The device covered by this specification |
| Packet | One `NodePacket` (27 bytes) received over ESP-NOW |
| Cycle | One measurement/transmission cycle of a node, identified by `node_id` + `boot_count` |
| Slot | The server-side record of one node (settings, latest state, history) |
| Corner | One of NW, NE, SW, SE position on the room view |
| Node status | OK / LOW / EMPTY / CHARGING / STUCK / WAITING (see REQ-STATE) |
| STA / AP | Wi-Fi station (router client) / soft access point |
| Estimated temperature | CPU temperature minus offset, used when the DS18B20 value is unavailable |
| Compile-time setting | A `#define` in `config.h` |
| SHALL | Mandatory requirement |
| MAY | Optional requirement |

## 4. Hardware

| Item | Value |
|---|---|
| Board | Seeed Studio XIAO ESP32C6 |
| Power | USB (5 V), permanent |
| Peripherals | none required |
| RF switch enable | GPIO3 = LOW (same as node) |
| Antenna selection | GPIO14: LOW = on-board, HIGH = external (`USE_EXTERNAL_ANTENNA`) |
| On-board LED | GPIO15, active low, used as a receive/status indicator |
| Filesystem | LittleFS on a data partition (partition scheme with a filesystem partition, e.g. the default 4 MB scheme) |

---

## 5. Operating Overview

```
BOOT
 ├─ load settings (NVS): Wi-Fi credentials, node slots, PIN, last channel
 ├─ mount LittleFS, restore history snapshot
 ├─ Wi-Fi: try router (STA)
 │    ├─ success → note router channel, start NTP, mDNS
 │    └─ fail    → start own AP on last known channel (keep retrying STA slowly)
 ├─ ESP-NOW init (receive callback → queue)
 ├─ web server start
 └─ forever (loop):
      ├─ drain receive queue → validate → dedupe → update slot + history
      ├─ evaluate node statuses (stuck detection)
      ├─ handle HTTP requests
      ├─ Wi-Fi supervision (reconnect, AP fallback)
      └─ hourly: save history snapshot to flash
```

---

## 6. Requirements

Each requirement carries a unique identifier `REQ-<GROUP>-<NN>`.

### 6.1 General (GEN)

**REQ-GEN-01** — The firmware SHALL compile and upload with the Arduino IDE, ESP32 Arduino core 3.x, board "XIAO_ESP32C6".

**REQ-GEN-02** — All fixed settings SHALL be defined at compile time in a single file, `config.h`. Settings that the user changes without reflashing (Wi-Fi credentials, node names/positions/offsets, PIN) are the only run-time configuration and SHALL be stored in non-volatile storage (REQ-PERS).

**REQ-GEN-03** — The firmware SHALL use only libraries shipped with the ESP32 Arduino core (WiFi, WebServer, ESPmDNS, LittleFS, Preferences, DNSServer if needed) and ESP-IDF. No third-party library SHALL be required.

**REQ-GEN-04** — The server SHALL run continuously. It SHALL NOT enter light or deep sleep, and Wi-Fi modem sleep SHALL be disabled (`WiFi.setSleep(false)`) so ESP-NOW frames are never missed.

**REQ-GEN-05** — The firmware SHALL print its version (`SERVER_FW_VERSION`) on the debug output and show it on the web page (Settings/About).

**REQ-GEN-06** — The node packet definition SHALL be copied **unchanged** from the node sketch, between the `BEGIN NodePacket` / `END NodePacket` markers, into a separate header (`node_packet.h`) of the server project. A `static_assert` SHALL guarantee `sizeof(NodePacket) == 27`.

### 6.2 Network (NET)

**REQ-NET-01** — On boot the server SHALL operate in station mode and try to connect to the router using the stored credentials, for at most `STA_CONNECT_TIMEOUT_S` (default 15 s).

**REQ-NET-02** — If no credentials are stored, or the connection fails, the server SHALL start its own access point (`AP_SSID`, `AP_PASSWORD`, WPA2) while continuing to operate as STA in the background (AP+STA mode). The web UI SHALL be reachable at the AP address (`192.168.4.1`).

**REQ-NET-03** — In STA mode the server SHALL follow the router's Wi-Fi channel. ESP-NOW reception works only on the current radio channel, therefore **the node channel (`ESPNOW_CHANNEL` in the node's `config.h`) must be set manually to match the router channel**. The server SHALL NOT attempt to change or negotiate the node channel.

**REQ-NET-04** — In AP-only or AP+STA fallback mode the AP channel SHALL be the **last router channel the server successfully used**, stored in NVS (default `ESPNOW_CHANNEL_DEFAULT` = 1 if never connected). This keeps the nodes receivable while the router is unavailable.

**REQ-NET-05** — While in fallback mode the server SHALL retry the router connection no more often than every `STA_RETRY_INTERVAL_S` (default 300 s). Each retry scans and may briefly disturb the radio channel; the interval SHALL therefore be long and the retry SHALL be skipped while a web client is actively using the AP.
*Rationale:* channel scanning interrupts ESP-NOW reception and the AP.

**REQ-NET-06** — When a retry succeeds the server SHALL stop the AP (after `AP_GRACE_S`, default 60 s, so a connected phone can be redirected) and continue on the router channel, updating the stored last channel.

**REQ-NET-07** — The currently used channel, Wi-Fi mode (STA / AP / AP+STA), SSID, IP address, RSSI to the router and the node-matching hint ("nodes must be on channel N") SHALL be visible on the web page.

**REQ-NET-08** — The server SHALL advertise the mDNS name `MDNS_HOSTNAME` (default `roomtemp`, i.e. `http://roomtemp.local`) when connected to a router.

**REQ-NET-09** — The Wi-Fi reconnection after router loss SHALL be automatic and SHALL NOT require a reboot.

**REQ-NET-10** — The server SHALL obtain the time via NTP (`NTP_SERVER`, `TIMEZONE` POSIX string) whenever internet access is available (REQ-TIME).

### 6.3 Packet Reception (RX)

**REQ-RX-01** — ESP-NOW SHALL be initialised on the STA interface and a receive callback registered. The callback SHALL only copy the frame (data, length, RSSI, receive time) into a fixed-size queue and return. All processing SHALL happen in `loop()`.
*Rationale:* the callback runs in the Wi-Fi task; no blocking, allocation or printing there.

**REQ-RX-02** — A frame SHALL be accepted only if all hold:
- length == `sizeof(NodePacket)` (27);
- `packet_version == PACKET_VERSION` (1);
- CRC32 recomputed with `esp_rom_crc32_le(0, data, offsetof(NodePacket, crc32))` equals `crc32`;
- `node_id` is within the accepted range (see REQ-NODE-01).

Rejected frames SHALL be counted (statistics, REQ-ERR-02) and discarded.

**REQ-RX-03** — Duplicate removal: a packet whose `(node_id, boot_count)` equals the last accepted `(node_id, boot_count)` of that node SHALL be discarded. A packet with a *different* `boot_count` SHALL be accepted **even if smaller** — the node's counter restarts at 1 after a power loss (battery replacement).

**REQ-RX-04** — The queue SHALL hold at least `RX_QUEUE_LEN` (default 16) frames. On overflow the oldest frame SHALL be dropped and counted.

**REQ-RX-05** — For every accepted packet the server SHALL record the receive time as `millis()` (monotonic, independent of NTP) and the RSSI reported by ESP-NOW (`rx_ctrl->rssi`).

**REQ-RX-06** — The on-board LED SHALL blink briefly (about 50 ms) on every accepted, non-duplicate packet. It SHALL be off otherwise. LED indication MAY be disabled with `RX_LED_ENABLED`.

### 6.4 Node Slots (NODE)

**REQ-NODE-01** — The server SHALL support up to `MAX_NODES` (default 4) nodes. A slot is created automatically when the first valid packet from an unknown `node_id` arrives and a free slot exists. Packets from further unknown ids SHALL be counted and ignored, and the UI SHALL show a notice "N unknown node id(s) heard" with their ids.

**REQ-NODE-02** — Each slot SHALL hold the following user-editable settings, persisted in NVS (REQ-PERS-01):

| Setting | Default |
|---|---|
| `name` (≤ 20 chars) | "Node N" |
| `corner` (NW / NE / SW / SE) | first free corner |
| `cpu_offset_cC` (CPU → room, hundredths °C, signed) | `DEFAULT_CPU_OFFSET_CC` (config.h) |

**REQ-NODE-03** — The user SHALL be able to delete a slot ("forget node") from the settings page. Its history is deleted with it, the `node_id` can then be learned again.

**REQ-NODE-04** — Two slots SHALL NOT share the same corner. Assigning a corner already in use SHALL swap the two corners.

**REQ-NODE-05** — Each slot SHALL keep the most recent values of all packet fields (battery mV and state, charging flag, vbus mV, sleep time, CPU temp, DS18B20 temp, status flags, firmware version, boot count), the receive time (REQ-RX-05), the RSSI and `packets_received` / `crc_errors` counters.

### 6.5 Data Interpretation (DATA)

**REQ-DATA-01** — Temperatures SHALL be converted from hundredths of °C. The value `-32768` (`TEMP_INVALID`) means "no value".

**REQ-DATA-02** — The *room temperature* of a node SHALL be selected as follows:
1. DS18B20 value valid (not `TEMP_INVALID`, and `FLAG_DS_ABSENT`/`FLAG_DS_ERR` clear) → use it, source = `DS18B20`;
2. else CPU value valid (and `FLAG_CPU_TEMP_ERR` clear) → use `cpu_temp_cC − cpu_offset_cC`, source = `ESTIMATED`;
3. else no value, source = `NONE`.

The UI SHALL mark estimated values visibly (e.g. a "~" prefix and a dashed/"est." badge) and SHALL always allow viewing the raw CPU value in the node detail.

**REQ-DATA-03** — Battery indication SHALL be derived from `battery_state` (0 NORMAL, 1 LOW, 2 EMPTY, 3 INVALID) and `is_charging`. The server MAY additionally show an approximate charge percentage from `battery_mv` using a fixed Li-ion lookup table in the firmware (informational only).

**REQ-DATA-04** — `status_flags` SHALL be decoded and shown in the node detail: DS18B20 absent, DS18B20 error, battery invalid, CPU temperature error, watchdog reset on previous cycle.

**REQ-DATA-05** — The server SHALL NOT modify raw stored values; offsets and conversions are applied only when presenting or when computing the room temperature for the history (REQ-HIST-02).

### 6.6 Node Status and Stuck Detection (STATE)

**REQ-STATE-01** — Each node SHALL have exactly one *status*, evaluated at least once per second, with the following priority (highest first):

| Status | Condition |
|---|---|
| `STUCK` | no packet for longer than `STUCK_FACTOR` × `sleep_time_s` (see REQ-STATE-02) |
| `EMPTY` | last `battery_state` == 2 |
| `LOW` | last `battery_state` == 1 |
| `BAT_INVALID` | last `battery_state` == 3 |
| `OK` | none of the above |
| `WAITING` | slot exists but no packet received since server boot and `STUCK` timeout not yet reached |

`CHARGING` is an independent **overlay** (plug/bolt icon) shown together with any status when `is_charging == 1`.

**REQ-STATE-02** — The stuck timeout SHALL be `STUCK_FACTOR` (default **1.5**) × the `sleep_time_s` value from the node's last accepted packet. Because the node announces its own next-wake interval, the timeout adapts automatically to NORMAL (900 s), LOW (3600 s) and EMPTY (18000 s) battery states.
*Reference:* with defaults a normal node is flagged after 22.5 min, an EMPTY node after 7.5 h.

**REQ-STATE-03** — The elapsed time SHALL be computed with `millis()` so that stuck detection works without a valid wall clock.

**REQ-STATE-04** — After a server reboot the last `sleep_time_s` of each node SHALL be restored from flash (REQ-PERS-01). The elapsed-time reference after reboot is the server boot time, so a node is `WAITING` until its first packet, and becomes `STUCK` if no packet arrives within the timeout counted from server boot.

**REQ-STATE-05** — A `STUCK` node SHALL keep showing its last known values, greyed out, together with "last seen X ago". It returns to its normal status immediately on the next accepted packet.

**REQ-STATE-06** — `STUCK_FACTOR` SHALL be configurable in `config.h`. A single lost cycle (all three copies lost) exceeds the default factor and therefore shows `STUCK` until the following packet; this behaviour is accepted.

**REQ-STATE-07** — A watchdog-reset flag (`FLAG_WDT_RESET`) in the last packet SHALL raise a non-blocking warning marker on the node card (the node recovered from a hang).

### 6.7 History (HIST)

**REQ-HIST-01** — The server SHALL keep, per node, a ring buffer of `HIST_POINTS` samples (default 768, about 8 days at the 15-minute NORMAL interval). One sample is stored per accepted, non-duplicate packet.

**REQ-HIST-02** — A sample SHALL contain: timestamp, room temperature in hundredths °C (as selected by REQ-DATA-02 at reception time, `TEMP_INVALID` if none), temperature source (DS18B20 / ESTIMATED / NONE), CPU temperature (raw), battery mV, battery state and flags (charging). Size SHALL NOT exceed 16 bytes per sample.

**REQ-HIST-03** — The history SHALL be stored in RAM in a fixed, statically allocated structure. No dynamic memory SHALL be allocated for history at run time.
*Estimate:* 4 nodes × 768 × 16 B ≈ 48 KB.

**REQ-HIST-04** — A change of the CPU offset SHALL NOT rewrite stored samples. Because REQ-HIST-02 stores the raw CPU temperature, the UI MAY recompute estimated points with the current offset when drawing.

**REQ-HIST-05** — Timestamps: if wall-clock time is valid the sample timestamp SHALL be the Unix epoch in seconds. If it is not valid (no NTP yet), the sample SHALL carry a server-uptime based timestamp and be flagged as "relative"; when NTP later succeeds, relative samples SHALL be converted to epoch time using the offset between uptime and wall clock.

**REQ-HIST-06** — The history API SHALL support returning the whole buffer or a time range, and SHALL downsample on the server if more than `HIST_API_MAX_POINTS` (default 300) points would be returned.

**REQ-HIST-07** — Deleting a slot (REQ-NODE-03) SHALL clear its history. A "clear all history" action MAY be provided in settings.

### 6.8 Persistence (PERS)

**REQ-PERS-01** — The following SHALL be stored in NVS (`Preferences`): Wi-Fi SSID and password, last router channel, the settings PIN, node slot settings (REQ-NODE-02), per-node `node_id` ↔ slot mapping and the last known `sleep_time_s` of each node.

**REQ-PERS-02** — The history SHALL be saved to a single file on LittleFS every `HIST_SAVE_INTERVAL_S` (default 3600 s), and also before a controlled restart requested from the UI. The file SHALL carry a magic value, format version and CRC; an invalid file SHALL be ignored (history starts empty).

**REQ-PERS-03** — The save SHALL be atomic with respect to power loss: write to a temporary file, verify, then rename over the old file. Loss of power during a save SHALL never corrupt the previous snapshot.

**REQ-PERS-04** — The save SHALL be skipped if no new sample was added since the last save, to limit flash wear.

**REQ-PERS-05** — On boot the history SHALL be restored from the snapshot. Samples older than the retained window are naturally overwritten by the ring buffer. Data received after the last snapshot and before a power loss (up to `HIST_SAVE_INTERVAL_S`) is lost; this is accepted.

**REQ-PERS-06** — A factory reset (erase Wi-Fi credentials, nodes, PIN, history) SHALL be available from the settings page (PIN protected, with confirmation) and by holding the XIAO BOOT button (GPIO9) for `FACTORY_RESET_HOLD_S` (default 10 s) during the first 30 s after boot.

### 6.9 Web Server and API (WEB)

**REQ-WEB-01** — The server SHALL serve HTTP on port 80 with the core `WebServer` library. HTTPS is not required.

**REQ-WEB-02** — The complete web UI SHALL be a single self-contained page embedded in the firmware (HTML + CSS + JavaScript, stored in flash/PROGMEM, gzip-compressed if practical) and kept in its own header (`web_page.h`).

**REQ-WEB-03** — The page SHALL **not depend on any external resource** (no CDN, web fonts, icon fonts or chart libraries). The server may be used in AP mode without internet access, so graphs and icons SHALL be drawn with inline SVG/CSS/Canvas code included in the page.

**REQ-WEB-04** — The following endpoints SHALL be provided (JSON built by hand, no JSON library):

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | the web page |
| GET | `/api/state` | server info + current state of all nodes |
| GET | `/api/history?node=<id>&from=<epoch>` | history of one node |
| GET | `/api/wifi/scan` | list of visible networks (settings, PIN) |
| POST | `/api/wifi` | save SSID/password and reconnect (PIN) |
| POST | `/api/node` | edit name / corner / offset (PIN) |
| POST | `/api/node/delete` | forget a node (PIN) |
| POST | `/api/pin` | change PIN (PIN) |
| POST | `/api/auth` | check the PIN before the page shows settings (PIN) |
| POST | `/api/room` | set the room name (PIN) |
| POST | `/api/history/clear` | erase all history (PIN) |
| POST | `/api/restart` | controlled restart (PIN) |
| POST | `/api/factory-reset` | factory reset (PIN) |
| GET | `/api/export.csv?node=<id>` | MAY: history as CSV |

**REQ-WEB-05** — `/api/state` SHALL contain, per node: id, name, corner, status, overlay flags, room temperature + source, CPU raw temperature, battery mV / state / charging, vbus mV, RSSI, sleep time, `seconds_since_last_packet`, packet counters, flags, firmware version. It SHALL also contain server info: uptime, free heap, Wi-Fi mode/channel/SSID/IP, time validity, firmware version and reception statistics (REQ-ERR-02).

**REQ-WEB-06** — The page SHALL refresh state by polling `/api/state` every `UI_POLL_S` (default 10 s) and SHALL refresh the open graph at a lower rate (e.g. 60 s). Opening the page SHALL be possible from several clients simultaneously (at least 4).

**REQ-WEB-07** — Handling of HTTP requests SHALL NOT block packet reception: the receive callback and queue (REQ-RX-01) guarantee that frames are held while a request is processed. No handler SHALL block for longer than 200 ms, except the Wi-Fi scan (asynchronous, polled by the page).

**REQ-WEB-08** — All endpoints that change configuration (`POST` list above) SHALL be protected by the PIN (REQ-SEC-01). Read-only endpoints SHALL be open.

### 6.10 User Interface (UI)

The page is the face of the project: it SHALL look modern, clean and calm, work on phone and desktop, and be readable at a glance from across a room.

**REQ-UI-01** — **Room view.** The main screen SHALL show a top-down stylised room (rounded rectangle, subtle floor texture/gradient) with up to four *temperature points*, one in each corner according to the node's corner setting. Each point is a rounded card/bubble showing: node name, the room temperature in large digits, and the status icon.

**REQ-UI-02** — **Temperature colour.** The point SHALL be tinted by temperature on a continuous cold→warm scale (e.g. blue → green → amber → red) over a configurable range (`UI_TEMP_MIN_C` / `UI_TEMP_MAX_C`, default 15–30 °C). The room view MAY additionally render a soft heat-map gradient between the points.

**REQ-UI-03** — **Status icons** (small, next to the temperature, with accessible text/tooltip):

| Status | Icon | Colour |
|---|---|---|
| OK | check / filled dot | green |
| LOW | half-empty battery | amber |
| EMPTY | empty battery, red | red |
| BAT_INVALID | battery with "?" | grey/red |
| STUCK | warning triangle / "no signal" | red; whole card dimmed, "last seen X ago" |
| WAITING | spinner / clock | grey |
| CHARGING (overlay) | plug / lightning bolt | blue |
| Estimated temperature | "~" badge | neutral |
| Watchdog warning | small flag | amber |

A battery icon SHALL be shown prominently only when the battery is not OK (LOW / EMPTY / INVALID); with a normal battery only the OK mark is shown to keep the view calm.

**REQ-UI-04** — **Summary strip.** A header SHALL show the room name (settings), the current time, the average and min/max room temperature across non-stuck nodes with a valid value, and an overall health message (e.g. "All 4 nodes OK" / "1 node needs attention").

**REQ-UI-05** — **Node detail.** Selecting a point SHALL open a detail panel (modal or side sheet) with:
- the temperature **history graph** (line chart with area fill, hover/touch crosshair with value + time, range selector 6 h / 24 h / 7 d / all);
- a secondary graph or toggle for battery voltage;
- all decoded fields (REQ-DATA-04, REQ-NODE-05), RSSI as bars, last seen, firmware version;
- the settings for that node (name, corner, CPU offset) behind the PIN.

**REQ-UI-06** — **Combined graph.** A page section SHALL show all nodes in one chart with one line per node (distinct colours consistent with the room view) and a legend that toggles lines.

**REQ-UI-07** — **Graph behaviour.** Gaps (no data for more than 2 × the node's sleep time) SHALL be drawn as breaks in the line, not interpolated. Estimated segments SHALL be drawn dashed. The graph SHALL work with the relative-time fallback (REQ-HIST-05) by labelling the axis "x min ago".

**REQ-UI-08** — **Server/network section.** A collapsible "System" section SHALL show Wi-Fi mode, channel and the node-matching hint (REQ-NET-07), IP, uptime, free memory, reception statistics and firmware version.

**REQ-UI-09** — **Settings.** A settings page SHALL provide: Wi-Fi setup (scan, choose SSID, password, save), node list management, PIN change, restart, factory reset, room name, and the temperature range of the colour scale (stored in the browser, not on the server).

**REQ-UI-10** — **First-run flow.** When the server is in AP mode with no stored credentials, the page SHALL show a clear banner "Connect me to your Wi-Fi" leading directly to the Wi-Fi setup. The AP SHOULD use a captive-portal DNS redirect so phones open the page automatically (MAY).

**REQ-UI-11** — **Theme and layout.** The page SHALL follow the system light/dark preference, be responsive (phone portrait through wide desktop), use smooth transitions for value changes, honour `prefers-reduced-motion`, keep touch targets ≥ 44 px, and use a legible type scale. Colour SHALL never be the only carrier of status (icons and text accompany it).

**REQ-UI-12** — **Connection loss.** If the page cannot reach the server it SHALL show a non-intrusive "Connection lost, retrying…" banner and keep the last data visible.

**REQ-UI-13** — **Units and language.** Temperatures in °C; UI language English. A °C/°F toggle MAY be added.

### 6.11 Security (SEC)

**REQ-SEC-01** — Configuration changes SHALL require a numeric PIN (default `DEFAULT_PIN` in `config.h`, changeable from the UI; the UI SHALL warn while the default is in use). The PIN is sent in a request header or body, never in the URL.

**REQ-SEC-02** — After `PIN_MAX_ATTEMPTS` (default 5) consecutive wrong PINs from any client the server SHALL refuse PIN-protected requests for `PIN_LOCKOUT_S` (default 60 s).

**REQ-SEC-03** — The Wi-Fi password SHALL never be returned by any API endpoint. The AP SHALL use WPA2 with `AP_PASSWORD` (≥ 8 characters, enforced by a `static_assert`).

**REQ-SEC-04** — ESP-NOW reception is unauthenticated broadcast, as in the node design. The CRC protects only against corruption, not forgery. This is a documented limitation (encryption is out of scope for both projects).

### 6.12 Time (TIME)

**REQ-TIME-01** — The server SHALL start NTP synchronisation (`configTzTime`) after a successful router connection and re-sync periodically (core default).

**REQ-TIME-02** — Wall-clock time SHALL be considered valid only after the first successful NTP sync. Until then the API SHALL report `time_valid: false` and the UI SHALL show relative times ("5 min ago").

**REQ-TIME-03** — Stuck detection, duplicate suppression and Wi-Fi supervision SHALL use `millis()` only and SHALL NOT depend on wall-clock time.

### 6.13 Error Handling and Robustness (ERR)

**REQ-ERR-01** — A failure of any subsystem (LittleFS mount, NTP, mDNS, NVS read) SHALL NOT stop packet reception or the web server. The server SHALL log the failure and continue with defaults; a LittleFS failure SHALL disable history persistence only.

**REQ-ERR-02** — Reception statistics SHALL be kept and shown: frames received, accepted, duplicates, bad length, bad version, bad CRC, unknown node, queue overflows.

**REQ-ERR-03** — The task watchdog SHALL be active (`WDT_TIMEOUT_S`, default 30 s) and fed from `loop()`. A hang SHALL reset the server; the history snapshot and NVS settings make the restart non-destructive.

**REQ-ERR-04** — The server SHOULD restart itself once a day at a quiet time (`DAILY_RESTART_ENABLED`, default 0) to mitigate long-run Wi-Fi/heap issues. When enabled it SHALL save the history first (REQ-PERS-02). Free-heap low-water mark SHALL be tracked and shown in the System section.

**REQ-ERR-05** — All JSON/HTML output buffers SHALL have bounded size; the firmware SHALL NOT assemble unbounded `String` objects from client-controlled input.

### 6.14 Debug Output (DBG)

**REQ-DBG-01** — Debug output over USB serial SHALL be switchable with `DEBUG_ENABLED` (0/1); when 0 it is compiled out, same macro style as the node (`DBG_PRINTF` with `[ time ms]` prefix).

**REQ-DBG-02** — With debug on, the server SHALL log: boot info, Wi-Fi mode/channel changes, every received frame (node, boot_count, accepted/duplicate/rejected with reason, RSSI), status transitions per node (including STUCK), history saves and their duration, and a periodic heartbeat (uptime, heap, queue depth).

---

## 7. Compile-Time Configuration (`config.h`)

| Name | Default | Description |
|---|---|---|
| `SERVER_FW_VERSION` | 1 | Server firmware version |
| `ESPNOW_CHANNEL_DEFAULT` | 1 | AP/ESP-NOW channel when no router channel has ever been learned |
| `USE_EXTERNAL_ANTENNA` | 0 | 0 = on-board, 1 = external |
| `STA_CONNECT_TIMEOUT_S` | 15 | Router connect timeout at boot |
| `STA_RETRY_INTERVAL_S` | 300 | Router retry interval in fallback mode |
| `AP_GRACE_S` | 60 | Delay before stopping the AP after router reconnect |
| `AP_SSID` | "RoomTemp-Setup" | Fallback access-point name |
| `AP_PASSWORD` | "roomtemp123" | AP password (≥ 8 chars) — **change before use** |
| `MDNS_HOSTNAME` | "roomtemp" | `roomtemp.local` |
| `NTP_SERVER` | "pool.ntp.org" | NTP server |
| `TIMEZONE` | "UTC0" | POSIX TZ string |
| `MAX_NODES` | 4 | Number of node slots |
| `DEFAULT_CPU_OFFSET_CC` | 0 | Initial CPU-to-room offset, hundredths °C |
| `STUCK_FACTOR` | 1.5 | Stuck timeout = factor × node sleep time |
| `RX_QUEUE_LEN` | 16 | ESP-NOW receive queue length |
| `RX_LED_ENABLED` | 1 | Blink LED on received packet |
| `HIST_POINTS` | 768 | Samples per node in RAM |
| `HIST_SAVE_INTERVAL_S` | 3600 | Flash snapshot interval |
| `HIST_API_MAX_POINTS` | 300 | Max points returned by the history API |
| `UI_POLL_S` | 10 | Page state polling interval |
| `UI_TEMP_MIN_C` / `UI_TEMP_MAX_C` | 15 / 30 | Colour scale range |
| `DEFAULT_PIN` | "1234" | Initial settings PIN |
| `PIN_MAX_ATTEMPTS` | 5 | Wrong PINs before lockout |
| `PIN_LOCKOUT_S` | 60 | Lockout duration |
| `FACTORY_RESET_HOLD_S` | 10 | BOOT-button hold time for factory reset |
| `WDT_TIMEOUT_S` | 30 | Task watchdog timeout |
| `DAILY_RESTART_ENABLED` | 0 | Optional daily restart |
| `DEBUG_ENABLED` | 0 | Debug output |
| `SERIAL_BAUD` | 115200 | Serial baud rate |

---

## 8. Assumptions and Notes

- **One radio, one channel.** ESP-NOW and Wi-Fi share a single radio; the server can only listen on the channel its Wi-Fi is on. In STA mode that is the router's channel, so the node's `ESPNOW_CHANNEL` must be changed manually whenever the router channel changes. The UI displays the channel to make this obvious. A router set to "auto channel" may silently move and cause all nodes to appear `STUCK`; a fixed router channel is strongly recommended.
- **Stuck sensitivity.** `STUCK_FACTOR = 1.5` flags a node after a single lost cycle (see REQ-STATE-06). The node uses an internal RC timer for deep sleep, so its real interval drifts by a few percent; 1.5 leaves ample margin for that.
- **Node timing independent.** The nodes are not synchronised to the server and never wait for acknowledgements; the server only listens.
- **Node reset.** If a node loses power, its `boot_count` restarts at 1; the dedupe rule (REQ-RX-03) handles this.
- **Estimated temperature.** The CPU sensor reads high due to self-heating. The offset is per node and user-adjustable; a typical value is a few °C and must be calibrated against a reference thermometer.
- **Flash wear.** One snapshot per hour of ~50 KB is negligible for flash life, and is skipped when nothing changed.
- **Memory.** History (~48 KB) plus web server and Wi-Fi fit comfortably in the ~512 KB RAM of the ESP32-C6.
- **Frozen node.** No requirement here causes a node change. Anything the server wants from the node that the packet does not carry is out of reach by design.

## 9. Decisions Taken Without Your Explicit Answer (please confirm or change)

| # | Decision | Where |
|---|---|---|
| D1 | One "STUCK" threshold (1.5 × sleep time); no separate "late" state | REQ-STATE-02 |
| D2 | Node slots auto-created from packets (max 4), then edited in UI | REQ-NODE-01 |
| D3 | AP fallback uses the **last router channel**, stored in NVS (default 1) | REQ-NET-04 |
| D4 | Router retries only every 5 min (channel scans disturb ESP-NOW) | REQ-NET-05 |
| D5 | History: 768 points/node (≈ 8 days), snapshot hourly to LittleFS | REQ-HIST, REQ-PERS |
| D6 | Web UI has **no external resources**, charts are hand-drawn SVG, since AP mode has no internet | REQ-WEB-03 |
| D7 | PIN (default 1234) for all changes; viewing is open | REQ-SEC |
| D8 | Polling every 10 s instead of WebSocket/SSE (data arrives every 15 min) | REQ-WEB-06 |
| D9 | Temperature in °C, English UI | REQ-UI-13 |
| D10 | Battery % is informational only; icon logic follows the node's `battery_state` | REQ-DATA-03 |
