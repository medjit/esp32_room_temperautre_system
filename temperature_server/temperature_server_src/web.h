/**
 * @file web.h
 * @brief HTTP server: the web page, the JSON API and the PIN protection.
 *
 * Implements: REQ-WEB-01 .. REQ-WEB-08, REQ-SEC-01 .. REQ-SEC-03, REQ-ERR-05.
 *
 * ENDPOINTS (read-only ones are open, everything that changes something needs
 * the settings PIN in the `X-Pin` request header)
 *
 *   GET  /                      the web page (gzip, embedded in the firmware)
 *   GET  /api/state             server info + state of every node
 *   GET  /api/history           history of one node  (?node=<id>&from=<ts>)
 *   GET  /api/export.csv        history of one node as CSV (?node=<id>)
 *   POST /api/auth              check the PIN
 *   GET  /api/wifi/scan         [PIN] list networks (?start=1 starts a scan)
 *   POST /api/wifi              [PIN] save router credentials and connect
 *   POST /api/node              [PIN] edit name / corner / CPU offset
 *   POST /api/node/delete       [PIN] forget a node and its history
 *   POST /api/room              [PIN] set the room name
 *   POST /api/pin               [PIN] change the PIN
 *   POST /api/history/clear     [PIN] erase all history
 *   POST /api/restart           [PIN] restart the server
 *   POST /api/factory-reset     [PIN] erase everything and restart
 *
 * POST bodies are plain form fields (application/x-www-form-urlencoded).
 *
 * KNOWN LIMITATION (REQ-SEC-04): ESP-NOW broadcasts are unauthenticated and the
 * HTTP server has no TLS. The CRC protects against corruption, not against
 * forgery, and the PIN travels in clear text on the local network. This is
 * acceptable for a home network and is documented in the README.
 */

#ifndef WEB_H
#define WEB_H

#include <Arduino.h>

/** Register all routes and start listening on port 80. */
void webBegin();

/** Serve pending HTTP requests and run delayed restarts. Call from every loop() pass. */
void webTick(uint32_t nowMs);

#endif  // WEB_H
