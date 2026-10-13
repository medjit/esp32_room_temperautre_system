/**
 * @file debug.h
 * @brief Debug logging macros. They compile to nothing when DEBUG_ENABLED is 0.
 *
 * Implements: REQ-DBG-01.
 *
 * Same style as the node firmware: every line gets a "[ time ms]" prefix and a
 * trailing newline. With DEBUG_ENABLED 0 the serial port is never initialised
 * and no debug code is compiled at all.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>
#include "config.h"

#if DEBUG_ENABLED
  /** Start USB serial, wait up to SERIAL_WAIT_MS for a PC, then carry on anyway. */
  #define DBG_BEGIN()  do { Serial.begin(SERIAL_BAUD);                              \
                            uint32_t t0_ = millis();                                \
                            while (!Serial && millis() - t0_ < SERIAL_WAIT_MS) {}   \
                          } while (0)
  /** printf with a "[ time ms]" prefix and a trailing newline. */
  #define DBG_PRINTF(fmt, ...) \
      Serial.printf("[%7lu ms] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
  /** Run a statement only in debug builds. */
  #define DBG_RUN(stmt) stmt
#else
  #define DBG_BEGIN()
  #define DBG_PRINTF(fmt, ...)
  #define DBG_RUN(stmt)
#endif

#endif  // DEBUG_H
