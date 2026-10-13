/**
 * @file rx.h
 * @brief ESP-NOW receive path: callback, queue, validation, duplicate removal.
 *
 * Implements: REQ-RX-01 .. REQ-RX-06.
 *
 * Data flow:
 *
 *   Wi-Fi task                           loop() task
 *   ----------                           -----------
 *   onRecv()  --copy--> [ring queue] --> rxPoll() --> validate --> dedupe
 *                                                      --> node slot + history
 *
 * The callback runs inside the Wi-Fi driver and must be quick, so it only
 * copies the frame into the queue. Everything else happens in loop().
 */

#ifndef RX_H
#define RX_H

#include <Arduino.h>

/**
 * Start ESP-NOW and register the receive callback.
 * Call after Wi-Fi has been started (netBegin()).
 * @return true when ready to receive.
 */
bool rxInit();

/**
 * Process every queued frame. Call at the top of each loop() pass.
 * Implements: REQ-RX-02, REQ-RX-03, REQ-NODE-01.
 */
void rxPoll();

/** Turn the receive LED off when its pulse is over (REQ-RX-06). Call from loop(). */
void rxLedTick(uint32_t nowMs);

#endif  // RX_H
