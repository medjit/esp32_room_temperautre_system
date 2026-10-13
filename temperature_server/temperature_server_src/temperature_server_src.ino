/**
 * @file temperature_server_src.ino
 * @brief XIAO ESP32C6 room temperature server (ESP-NOW receiver + web dashboard).
 *
 * WHAT IT DOES
 *   The server is always on. It listens for the ESP-NOW packets of the battery
 *   temperature nodes, checks and stores them, and serves a web page with the
 *   room view, node health and history graphs.
 *
 *     node --ESP-NOW--> rx (queue) --> nodes + history --> web page / API
 *                                          |
 *                                          +--> store (NVS settings, LittleFS snapshot)
 *     netmgr keeps Wi-Fi up: router first, own access point as fallback.
 *
 * DESIGN RULES
 *   - Never block reception: the receive callback only fills a queue, and
 *     loop() drains it first on every pass.
 *   - Simplicity: no RTOS tasks of our own, no dynamic memory for history or
 *     JSON, no third-party libraries.
 *   - Fail forward: a failing subsystem (flash, NTP, mDNS) is logged and
 *     skipped, the rest keeps running.
 *
 * MODULES (one responsibility each, see the plan for details)
 *   board    pins, RF switch, LED, BOOT button    store    NVS + LittleFS snapshot
 *   rx       ESP-NOW receive path                 web      HTTP server + JSON API
 *   nodes    node slots, statistics               netmgr   Wi-Fi, AP fallback, time
 *   history  ring buffers                         logic.h  pure decision logic (host-tested)
 *
 * REQUIREMENT TRACEABILITY
 *   Every function lists the REQ-xxx identifiers it implements in its header
 *   comment. Group -> module:
 *     REQ-GEN-*   whole project, config.h        REQ-NODE-*  nodes, store, web
 *     REQ-NET-*   netmgr                         REQ-DATA-*  logic.h, web
 *     REQ-RX-*    rx, logic.h                    REQ-STATE-* logic.h, nodes
 *     REQ-HIST-*  history, netmgr                REQ-PERS-*  store
 *     REQ-WEB-*   web, web_page.h                REQ-UI-*    web/index.html
 *     REQ-SEC-*   web                            REQ-TIME-*  netmgr
 *     REQ-ERR-*   all modules, this file         REQ-DBG-*   debug.h, all modules
 *
 * Board : XIAO_ESP32C6, ESP32 Arduino core 3.x (REQ-GEN-01)
 * Libs  : none beyond the core (REQ-GEN-03)
 */

#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#include "debug.h"
#include "board.h"
#include "nodes.h"
#include "store.h"
#include "netmgr.h"
#include "rx.h"
#include "web.h"

// ===========================================================================
// WATCHDOG
// ===========================================================================

/**
 * Start the hang-protection watchdog.
 *
 * Implements: REQ-ERR-03. loop() feeds it on every pass; if loop() ever stops
 * for WDT_TIMEOUT_S seconds the chip resets. Settings and the history snapshot
 * are in flash, so a restart loses at most the last hour of history.
 */
static void wdtStart() {
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms     = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;          // watch only our task, not the idle tasks
    cfg.trigger_panic  = true;       // reset the chip when the timeout expires

    // If the core has not started the watchdog yet, init it; else reconfigure it.
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
        esp_task_wdt_init(&cfg);
    }
    esp_task_wdt_add(NULL);          // subscribe the loop task
}

// ===========================================================================
// PERIODIC EXTRAS
// ===========================================================================

#if DEBUG_ENABLED
/**
 * Print a status line every 60 s (uptime, memory, Wi-Fi, queue statistics).
 * Implements: REQ-DBG-02 (periodic heartbeat).
 */
static void heartbeat(uint32_t nowMs) {
    static uint32_t last = 0;
    if (nowMs - last < 60000UL) return;
    last = nowMs;

    DBG_PRINTF("HEARTBEAT: up %lu s, heap %lu (min %lu), wifi %s ch %u, rx ok %lu dup %lu bad %lu overflow %lu",
               (unsigned long)(nowMs / 1000UL), (unsigned long)ESP.getFreeHeap(),
               (unsigned long)ESP.getMinFreeHeap(), netModeName(), (unsigned)netChannel(),
               (unsigned long)g_stats.accepted, (unsigned long)g_stats.duplicates,
               (unsigned long)(g_stats.badLen + g_stats.badVer + g_stats.badCrc),
               (unsigned long)g_stats.overflow);
}
#endif

#if DAILY_RESTART_ENABLED
/**
 * Restart once a day at DAILY_RESTART_HOUR local time.
 *
 * Implements: REQ-ERR-04 (optional). Needs a valid clock and an uptime of at
 * least two hours, so it can never cause a restart loop within the same hour.
 * The history is saved first.
 */
static void dailyRestartTick(uint32_t nowMs) {
    if (!netTimeValid() || nowMs < 2UL * 3600UL * 1000UL) return;

    struct tm t;
    if (getLocalTime(&t, 0) && t.tm_hour == DAILY_RESTART_HOUR) {
        DBG_PRINTF("DAILY: restarting");
        snapshotSave();
        delay(100);
        ESP.restart();
    }
}
#endif

// ===========================================================================
// setup() AND loop()
// ===========================================================================

/**
 * One-time start-up, in this order:
 *  1. hardware (RF switch / antenna BEFORE Wi-Fi), watchdog
 *  2. storage: settings, node slots, history restore (so the page has data at once)
 *  3. network, then ESP-NOW (its channel is the Wi-Fi channel), then the web server
 *
 * Implements: REQ-GEN-04, REQ-ERR-01, REQ-ERR-03.
 */
void setup() {
    DBG_BEGIN();
    DBG_PRINTF("Temperature server, firmware %d, packet version %d", SERVER_FW_VERSION, PACKET_VERSION);

    boardInit();
    wdtStart();

    nodesInit();
    storeInit();
    storeLoadNodes();
    snapshotLoad();

    netBegin();
    rxInit();
    webBegin();
}

/**
 * The server's main loop. The ORDER matters:
 *  - rxPoll() first, so received frames are processed with minimal delay;
 *  - HTTP work last, because it is the only step that can take a while.
 * Nothing here blocks; the 1 ms delay only lets the Wi-Fi and idle tasks run.
 */
void loop() {
    const uint32_t now = millis();
    esp_task_wdt_reset();

    rxPoll();                                        // REQ-RX-*
    rxLedTick(now);                                  // REQ-RX-06

    static uint32_t lastStatusMs = 0;
    if (now - lastStatusMs >= 1000UL) {              // REQ-STATE-01: at least once per second
        lastStatusMs = now;
        nodesEvalStatus(now);
    }

    netTick(now);                                    // REQ-NET-*, REQ-TIME-*
    storeTick(now);                                  // REQ-PERS-*
    webTick(now);                                    // REQ-WEB-*

#if DAILY_RESTART_ENABLED
    dailyRestartTick(now);
#endif
    DBG_RUN(heartbeat(now));

    delay(1);
}
