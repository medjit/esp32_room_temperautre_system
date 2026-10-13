/**
 * @file netmgr.h
 * @brief Wi-Fi manager: router connection, access-point fallback, time.
 *
 * Implements: REQ-NET-01 .. REQ-NET-10, REQ-TIME-01 .. REQ-TIME-03,
 *             REQ-HIST-05 (virtual clock), REQ-UI-10 (captive portal DNS).
 *
 * WHY THIS IS CAREFUL ABOUT CHANNELS
 *   ESP-NOW and Wi-Fi share one radio, and the radio can only be on ONE channel.
 *   While the server is connected to the router, the channel is the router's
 *   channel, so the nodes must be set to that channel by hand. When the router
 *   is not available the server opens its own access point on the LAST router
 *   channel it used, so the nodes keep being received.
 *
 * STATES
 *   NET_STA_TRY     trying to join the router (boot, or new credentials)
 *   NET_STA_OK      connected to the router
 *   NET_FALLBACK    own access point is running; router is retried slowly
 */

#ifndef NETMGR_H
#define NETMGR_H

#include <Arduino.h>

/** Start Wi-Fi. Non-blocking: the connection result is handled by netTick(). */
void netBegin();

/** Drive the state machine, DNS and time supervision. Call from every loop() pass. */
void netTick(uint32_t nowMs);

/** @return "sta", "ap" or "ap+sta" - the Wi-Fi mode, for the UI (REQ-NET-07). */
const char *netModeName();

/** @return the channel the radio is on right now (what the nodes must use). */
uint8_t netChannel();

/** @return true while connected to the router. */
bool netStaConnected();

/** @return true if a router SSID is stored. */
bool netHasCredentials();

/**
 * Join the router with the credentials currently in g_settings.
 * Called by the web API after the user saved new Wi-Fi credentials.
 */
void netApplyCredentials();

/** Tell the manager that a web client just talked to us (delays router retries, REQ-NET-05). */
void netNoteWebActivity();

/** @return true while the captive-portal redirect should be active (AP, no credentials). */
bool netCaptiveActive();

/**
 * Make sure a Wi-Fi scan is possible (the station interface must be on).
 * @return true if scanning can start.
 */
bool netPrepareScan();

/** @return true once the wall clock has been set by NTP (REQ-TIME-02). */
bool netTimeValid();

/**
 * Timestamp for a new history sample (REQ-HIST-05).
 * @return Unix epoch seconds if the time is valid, otherwise the virtual clock
 *         (VIRTUAL_BASE_S + uptime seconds).
 */
uint32_t netStamp();

#endif  // NETMGR_H
