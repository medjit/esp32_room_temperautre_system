/**
 * @file history.h
 * @brief Fixed-size ring buffer of measurements, one per node.
 *
 * Implements: REQ-HIST-01, REQ-HIST-03, REQ-HIST-05, REQ-HIST-06.
 *
 * The buffer is a plain array inside the node slot: no dynamic memory, no
 * pointers. When it is full the oldest sample is overwritten. Index 0 always
 * means "the oldest stored sample", index count-1 "the newest".
 *
 * This module has no Arduino dependency and is unit-tested on a PC.
 */

#ifndef HISTORY_H
#define HISTORY_H

#include "logic.h"

/** Ring buffer of HIST_POINTS samples. Zero-initialised means "empty". */
struct History {
    Sample   buf[HIST_POINTS];
    uint16_t head;     // slot that the next sample is written to
    uint16_t count;    // number of valid samples (0..HIST_POINTS)
};

/** Remove all samples. */
void histClear(History &h);

/** Append a sample; the oldest one is dropped when the buffer is full. */
void histAppend(History &h, const Sample &s);

/** @return the sample at logical index i (0 = oldest). i must be < h.count. */
const Sample &histAt(const History &h, uint16_t i);

/** @return the logical index of the first sample with ts >= fromTs, or h.count if none. */
uint16_t histFirstFrom(const History &h, uint32_t fromTs);

/**
 * Shift the timestamp of every VIRTUAL sample by delta seconds.
 *
 * Used when NTP delivers the wall clock (virtual -> real epoch) and when a
 * snapshot is restored after a reboot (old virtual stamps move back in time).
 * Samples that already carry a real epoch are left alone. Results are clamped
 * to >= 1.
 */
void histShiftVirtual(History &h, int64_t delta);

/**
 * Copy a time range into `out`, reducing it to at most `maxOut` points.
 *
 * Implements: REQ-HIST-06. If the range holds more than maxOut samples, equal
 * groups of `step` consecutive samples are merged into one: timestamp and
 * numeric values are averaged (invalid values ignored), the flag bits come
 * from the newest sample of the group.
 *
 * @param h       History to read.
 * @param fromTs  Only samples with ts >= fromTs are used (0 = all).
 * @param maxOut  Capacity of `out` (and the limit for the result size).
 * @param out     Receives the (possibly merged) samples, oldest first.
 * @param step    Receives how many stored samples were merged per output point.
 * @return        Number of samples written to `out`.
 */
uint16_t histDownsample(const History &h, uint32_t fromTs, uint16_t maxOut,
                        Sample *out, uint16_t *step);

#endif  // HISTORY_H
