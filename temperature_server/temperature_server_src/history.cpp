/**
 * @file history.cpp
 * @brief Ring buffer operations (see history.h).
 */

#include "history.h"

/** Physical array index of the oldest sample. */
static uint16_t oldestIndex(const History &h) {
    return (uint16_t)((h.head + HIST_POINTS - h.count) % HIST_POINTS);
}

void histClear(History &h) {
    h.head  = 0;
    h.count = 0;
}

/**
 * Append a sample.
 *
 * Implements: REQ-HIST-01. The write position simply advances in a circle, so
 * once the buffer is full each new sample overwrites the oldest one.
 */
void histAppend(History &h, const Sample &s) {
    h.buf[h.head] = s;
    h.head = (uint16_t)((h.head + 1) % HIST_POINTS);
    if (h.count < HIST_POINTS) h.count++;
}

const Sample &histAt(const History &h, uint16_t i) {
    return h.buf[(oldestIndex(h) + i) % HIST_POINTS];
}

uint16_t histFirstFrom(const History &h, uint32_t fromTs) {
    if (fromTs == 0) return 0;
    for (uint16_t i = 0; i < h.count; i++) {
        if (histAt(h, i).ts >= fromTs) return i;
    }
    return h.count;
}

/**
 * Move virtual-clock timestamps by `delta` seconds.
 *
 * Implements: REQ-HIST-05. Real epoch stamps (>= EPOCH_VALID_MIN_S) are never
 * touched, so calling this twice is harmless for already converted data.
 */
void histShiftVirtual(History &h, int64_t delta) {
    for (uint16_t i = 0; i < h.count; i++) {
        Sample &s = h.buf[(oldestIndex(h) + i) % HIST_POINTS];
        if (!stampIsVirtual(s.ts)) continue;

        int64_t moved = (int64_t)s.ts + delta;
        if (moved < 1) moved = 1;
        s.ts = (uint32_t)moved;
    }
}

/**
 * Running average of a temperature-like value that may be TEMP_INVALID.
 * Invalid inputs are skipped; if nothing valid was added the result is invalid.
 */
struct Averager {
    int32_t  sum   = 0;
    uint16_t count = 0;

    void add(int16_t v) {
        if (v == TEMP_INVALID) return;
        sum += v;
        count++;
    }
    int16_t result() const {
        if (count == 0) return TEMP_INVALID;
        // Round to nearest so averages do not drift low for negative values.
        int32_t half = (int32_t)(count / 2);
        int32_t avg  = (sum >= 0) ? (sum + half) / count : (sum - half) / (int32_t)count;
        return (int16_t)avg;
    }
};

/**
 * Merge the samples [first, last) into one output sample.
 * Time and values are averaged, flag bits are taken from the newest sample.
 */
static Sample mergeGroup(const History &h, uint16_t first, uint16_t last) {
    Averager room, cpu, bat;
    uint64_t tsSum = 0;

    for (uint16_t i = first; i < last; i++) {
        const Sample &s = histAt(h, i);
        tsSum += s.ts;
        room.add(s.room_cC);
        cpu.add(s.cpu_cC);
        // Battery mV always fits int16_t (max ~5000), so the same averager works.
        bat.add((int16_t)s.bat_mv);
    }

    Sample out = histAt(h, (uint16_t)(last - 1));        // newest: provides the flag bits
    out.ts      = (uint32_t)(tsSum / (last - first));
    out.room_cC = room.result();
    out.cpu_cC  = cpu.result();
    out.bat_mv  = (uint16_t)(bat.count ? bat.result() : 0);
    return out;
}

/**
 * Select a time range and shrink it to at most maxOut points.
 *
 * Implements: REQ-HIST-06. See history.h for the merge rules.
 */
uint16_t histDownsample(const History &h, uint32_t fromTs, uint16_t maxOut,
                        Sample *out, uint16_t *step) {
    const uint16_t start = histFirstFrom(h, fromTs);
    const uint16_t total = (uint16_t)(h.count - start);

    *step = 1;
    if (total == 0 || maxOut == 0) return 0;

    // Smallest group size that makes the result fit: ceil(total / maxOut).
    *step = (uint16_t)((total + maxOut - 1) / maxOut);

    uint16_t written = 0;
    for (uint16_t first = start; first < h.count; first = (uint16_t)(first + *step)) {
        uint16_t last = (uint16_t)(first + *step);
        if (last > h.count) last = h.count;
        out[written++] = (*step == 1) ? histAt(h, first) : mergeGroup(h, first, last);
    }
    return written;
}
