/**
 * @file board.h
 * @brief XIAO ESP32C6 pin definitions and the few direct hardware helpers.
 *
 * Implements: antenna / RF switch setup (needed by REQ-NET-* and REQ-RX-*),
 *             REQ-RX-06 (LED), REQ-PERS-06 (BOOT button).
 */

#ifndef BOARD_H
#define BOARD_H

#include <Arduino.h>

#define PIN_LED           15   // On-board user LED, ACTIVE LOW
#define PIN_RF_SWITCH_EN  3    // RF switch enable, LOW = enabled
#define PIN_ANT_SELECT    14   // Antenna select, LOW = on-board, HIGH = external
#define PIN_BOOT_BUTTON   9    // BOOT button, LOW = pressed

/** Set up the RF switch and antenna selection, and switch the LED off. Call before Wi-Fi starts. */
void boardInit();

/** Switch the on-board LED on (true) or off (false). */
void ledWrite(bool on);

/** @return true while the BOOT button is held down. */
bool bootButtonPressed();

#endif  // BOARD_H
