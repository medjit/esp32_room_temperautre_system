/**
 * @file netmgr.cpp
 * @brief Wi-Fi state machine, mDNS, NTP, virtual clock (see netmgr.h).
 */

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>

#include "netmgr.h"
#include "config.h"
#include "debug.h"
#include "logic.h"
#include "nodes.h"
#include "store.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum NetState : uint8_t { NET_STA_TRY, NET_STA_OK, NET_FALLBACK };

#define WEB_BUSY_WINDOW_MS     30000UL    // a web client this recent postpones a router retry
#define CHANNEL_CHECK_MS       5000UL     // how often the router channel is re-read while connected

static NetState   s_state          = NET_STA_TRY;
static bool       s_apRunning      = false;
static bool       s_mdnsStarted    = false;
static bool       s_timeWasValid   = false;

static uint32_t   s_tryStartMs     = 0;   // when the current router attempt began
static bool       s_trying         = false; // a router attempt is in progress (fallback state)
static uint32_t   s_lastTryMs      = 0;   // when the last fallback attempt began
static uint32_t   s_linkLostMs     = 0;   // when the router link was first seen down (0 = up)
static uint32_t   s_apStopAtMs     = 0;   // when to stop the AP after a router comeback (0 = never)
static uint32_t   s_lastWebMs      = 0;   // last web activity (0 = none yet)
static uint32_t   s_lastChanCheckMs = 0;

static DNSServer  s_dns;
static bool       s_dnsRunning     = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool netHasCredentials() { return g_settings.ssid[0] != '\0'; }

bool netStaConnected() { return WiFi.status() == WL_CONNECTED; }

uint8_t netChannel() {
    uint8_t primary = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    return primary;
}

const char *netModeName() {
    switch (WiFi.getMode()) {
        case WIFI_AP:     return "ap";
        case WIFI_AP_STA: return "ap+sta";
        default:          return "sta";
    }
}

void netNoteWebActivity() {
    s_lastWebMs = millis();
    if (s_lastWebMs == 0) s_lastWebMs = 1;
}

bool netCaptiveActive() { return s_apRunning && !netHasCredentials(); }

/** Start the router attempt with the stored credentials. */
static void beginStation() {
    WiFi.setAutoReconnect(true);
    WiFi.begin(g_settings.ssid, g_settings.pass);
    s_tryStartMs = millis();
    DBG_PRINTF("NET: joining router \"%s\"", g_settings.ssid);
}

/** Stop searching for the router (a search scans all channels and disturbs ESP-NOW). */
static void abandonStation() {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
}

// ---------------------------------------------------------------------------
// Access point
// ---------------------------------------------------------------------------

/**
 * Start the fallback access point on the last known router channel.
 *
 * Implements: REQ-NET-02, REQ-NET-04, REQ-UI-10.
 *
 * With stored credentials the mode is AP+STA (the router is retried in the
 * background); without credentials it is AP only, which avoids any channel
 * scanning. The captive-portal DNS answers every name with the AP address so a
 * phone opens the setup page automatically.
 */
static void startAccessPoint() {
    if (s_apRunning) return;

    WiFi.mode(netHasCredentials() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, g_settings.lastChannel);
    s_apRunning = true;

    if (!netHasCredentials()) {
        s_dns.start(53, "*", WiFi.softAPIP());
        s_dnsRunning = true;
    }
    DBG_PRINTF("NET: access point \"%s\" on channel %u, ip %s", AP_SSID,
               (unsigned)g_settings.lastChannel, WiFi.softAPIP().toString().c_str());
}

static void stopAccessPoint() {
    if (!s_apRunning) return;
    WiFi.softAPdisconnect(true);
    s_apRunning = false;
    s_apStopAtMs = 0;
    DBG_PRINTF("NET: access point stopped");
}

/** Switch to fallback: AP up, router search stopped, retry timer started. */
static void enterFallback(uint32_t nowMs) {
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_STA || mode == WIFI_AP_STA) abandonStation();   // station interface exists
    startAccessPoint();
    s_state = NET_FALLBACK;
    s_trying = false;
    s_lastTryMs = nowMs;
}

// ---------------------------------------------------------------------------
// Router connection events
// ---------------------------------------------------------------------------

/**
 * The router link just came up.
 *
 * Implements: REQ-NET-03, REQ-NET-04, REQ-NET-06, REQ-NET-08, REQ-NET-10.
 * Remembers the router channel (used for the fallback AP), starts mDNS and
 * NTP, and schedules the AP shutdown after the grace period.
 */
static void onStationConnected(uint32_t nowMs) {
    s_state = NET_STA_OK;
    s_trying = false;
    s_linkLostMs = 0;

    const uint8_t ch = netChannel();
    if (ch >= 1 && ch <= 13 && ch != g_settings.lastChannel) {
        g_settings.lastChannel = ch;
        settingsSave();
    }
    DBG_PRINTF("NET: router connected, ip %s, channel %u. Nodes must use this channel.",
               WiFi.localIP().toString().c_str(), (unsigned)ch);

    if (!s_mdnsStarted && MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        s_mdnsStarted = true;
    }
    configTzTime(TIMEZONE, NTP_SERVER);

    if (s_apRunning) s_apStopAtMs = nowMs + AP_GRACE_S * 1000UL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Start Wi-Fi.
 *
 * Implements: REQ-NET-01, REQ-NET-02, REQ-GEN-04 (modem sleep off).
 * With credentials: try the router first (the result is handled in netTick,
 * so setup() is not blocked). Without: go straight to the access point.
 */
void netBegin() {
    WiFi.persistent(false);                  // we keep credentials ourselves (store.cpp)
    WiFi.setSleep(false);                    // never miss an ESP-NOW frame (REQ-GEN-04)

    if (netHasCredentials()) {
        WiFi.mode(WIFI_STA);
        beginStation();
        s_state = NET_STA_TRY;
    } else {
        enterFallback(millis());
    }
}

/**
 * Join the router with new credentials.
 * Implements: REQ-NET-01 for credentials entered in the UI. A running AP stays
 * up, so the user is not cut off before the result is known.
 */
void netApplyCredentials() {
    if (!netHasCredentials()) return;
    if (s_dnsRunning) { s_dns.stop(); s_dnsRunning = false; }

    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);   // need the station interface
    WiFi.disconnect(false, false);
    beginStation();
    s_state = NET_STA_TRY;
    s_trying = false;
}

bool netPrepareScan() {
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    return true;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/** @return true once the system clock holds a plausible date (NTP succeeded). */
bool netTimeValid() { return (uint32_t)time(nullptr) >= EPOCH_VALID_MIN_S; }

uint32_t netStamp() {
    if (netTimeValid()) return (uint32_t)time(nullptr);
    return VIRTUAL_BASE_S + millis() / 1000UL;
}

/**
 * Convert the history to real time as soon as NTP delivers it.
 *
 * Implements: REQ-HIST-05. Samples taken earlier carry virtual-clock stamps;
 * they are shifted by the difference between the real and the virtual "now".
 */
static void tickTime() {
    if (s_timeWasValid || !netTimeValid()) return;
    s_timeWasValid = true;

    const int64_t delta = (int64_t)time(nullptr) - (int64_t)(VIRTUAL_BASE_S + millis() / 1000UL);
    nodesShiftVirtual(delta);
    storeMarkHistoryDirty();
    DBG_PRINTF("TIME: clock set, history converted (shift %lld s)", (long long)delta);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

/** STA_TRY: wait for the router, give up after STA_CONNECT_TIMEOUT_S (REQ-NET-01/02). */
static void tickTry(uint32_t nowMs) {
    if (netStaConnected()) {
        onStationConnected(nowMs);
    } else if (nowMs - s_tryStartMs > STA_CONNECT_TIMEOUT_S * 1000UL) {
        DBG_PRINTF("NET: router not reachable, falling back to access point");
        enterFallback(nowMs);
    }
}

/**
 * STA_OK: watch the link and the channel.
 *
 * Implements: REQ-NET-09 (automatic reconnect: the driver retries by itself;
 * only a long outage starts the AP) and REQ-NET-04 (keep the stored channel in
 * line with the router, e.g. after the router changed it).
 */
static void tickConnected(uint32_t nowMs) {
    if (!netStaConnected()) {
        if (s_linkLostMs == 0) s_linkLostMs = nowMs ? nowMs : 1;
        if (nowMs - s_linkLostMs > STA_LOST_FALLBACK_S * 1000UL) {
            DBG_PRINTF("NET: router link lost, starting access point");
            enterFallback(nowMs);
        }
        return;
    }
    s_linkLostMs = 0;

    if (nowMs - s_lastChanCheckMs > CHANNEL_CHECK_MS) {
        s_lastChanCheckMs = nowMs;
        const uint8_t ch = netChannel();
        if (ch >= 1 && ch <= 13 && ch != g_settings.lastChannel) {
            DBG_PRINTF("NET: router channel changed %u -> %u", (unsigned)g_settings.lastChannel, (unsigned)ch);
            g_settings.lastChannel = ch;
            settingsSave();
        }
    }

    // Router is back and the grace period is over: the AP is no longer needed.
    if (s_apStopAtMs != 0 && (int32_t)(nowMs - s_apStopAtMs) >= 0) {
        stopAccessPoint();
        if (s_dnsRunning) { s_dns.stop(); s_dnsRunning = false; }
        WiFi.mode(WIFI_STA);
    }
}

/**
 * FALLBACK: the AP is up; try the router now and then.
 *
 * Implements: REQ-NET-05, REQ-NET-06. A router attempt scans all channels,
 * which interrupts ESP-NOW reception and the AP, so attempts are rare
 * (STA_RETRY_INTERVAL_S), short (STA_CONNECT_TIMEOUT_S) and skipped while a web
 * client has been active in the last 30 s.
 */
static void tickFallback(uint32_t nowMs) {
    if (s_dnsRunning) s_dns.processNextRequest();
    if (!netHasCredentials()) return;

    if (s_trying) {
        if (netStaConnected()) {
            onStationConnected(nowMs);
        } else if (nowMs - s_tryStartMs > STA_CONNECT_TIMEOUT_S * 1000UL) {
            abandonStation();
            s_trying = false;
            s_lastTryMs = nowMs;
        }
        return;
    }

    const bool due  = nowMs - s_lastTryMs >= STA_RETRY_INTERVAL_S * 1000UL;
    const bool busy = s_lastWebMs != 0 && nowMs - s_lastWebMs < WEB_BUSY_WINDOW_MS;
    if (due && !busy) {
        if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
        beginStation();
        s_trying = true;
    }
}

void netTick(uint32_t nowMs) {
    switch (s_state) {
        case NET_STA_TRY:  tickTry(nowMs);       break;
        case NET_STA_OK:   tickConnected(nowMs); break;
        case NET_FALLBACK: tickFallback(nowMs);  break;
    }
    tickTime();
}
