/**
 * @file board.cpp
 * @brief Direct hardware helpers (RF switch, antenna, LED, BOOT button).
 */

#include "board.h"
#include "config.h"

/**
 * Configure the RF switch, choose the antenna and switch the LED off.
 *
 * GPIO3 LOW enables the RF switch. GPIO14 LOW picks the on-board antenna and
 * HIGH the external one (USE_EXTERNAL_ANTENNA). This must happen BEFORE Wi-Fi
 * is started, otherwise the radio has no antenna path.
 */
void boardInit() {
    pinMode(PIN_RF_SWITCH_EN, OUTPUT);
    digitalWrite(PIN_RF_SWITCH_EN, LOW);

    pinMode(PIN_ANT_SELECT, OUTPUT);
    digitalWrite(PIN_ANT_SELECT, USE_EXTERNAL_ANTENNA ? HIGH : LOW);

    pinMode(PIN_LED, OUTPUT);
    ledWrite(false);

    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
}

/**
 * Drive the on-board LED. The LED is active low: LOW = lit.
 *
 * @param on  true = lit, false = dark.
 */
void ledWrite(bool on) {
    digitalWrite(PIN_LED, on ? LOW : HIGH);
}

/** @return true while the BOOT button (GPIO9, pulled up) is pressed. */
bool bootButtonPressed() {
    return digitalRead(PIN_BOOT_BUTTON) == LOW;
}
