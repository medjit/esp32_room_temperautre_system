/**
 * @file logic.h
 * @brief Hardware-free decision logic of the server (header-only).
 *
 * Everything that decides WHAT THE DATA MEANS lives here: is a frame valid, is
 * it a duplicate, which temperature is the room temperature, is a node stuck,
 * how a stored sample is packed. There are no Arduino or ESP calls (except the
 * ROM CRC, which has a plain C++ twin), so the same code is unit-tested on a PC
 * (see test/test_logic.cpp) and runs unchanged on the device.
 *
 * REQUIREMENT TRACEABILITY
 *   crc32Le()          REQ-RX-02
 *   frameValid()       REQ-RX-02
 *   isDuplicate()      REQ-RX-03
 *   pickTemperature()  REQ-DATA-01, REQ-DATA-02
 *   batteryPercent()   REQ-DATA-03
 *   evalStatus()       REQ-STATE-01 .. REQ-STATE-04
 *   Sample helpers     REQ-HIST-02
 *   stamp constants    REQ-HIST-05
 */

#ifndef LOGIC_H
#define LOGIC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "config.h"
#include "node_packet.h"

#if defined(ARDUINO)
  #include <esp_rom_crc.h>
#endif

// ===========================================================================
// 1. CRC32
// ===========================================================================

/**
 * CRC32 (the zlib/IEEE variant), chainable over several buffers.
 *
 * Implements: REQ-RX-02.
 *
 * On the device this is the ROM routine `esp_rom_crc32_le`, exactly what the
 * node uses (REQ-PKT-04 of the node). On a PC the bit-by-bit twin below gives
 * identical results, so the host tests can build and check real packets.
 *
 * @param crc  Previous CRC (0 for the first buffer).
 * @param buf  Data.
 * @param len  Number of bytes.
 * @return     Updated CRC.
 */
inline uint32_t crc32Le(uint32_t crc, const uint8_t *buf, size_t len) {
#if defined(ARDUINO)
    return esp_rom_crc32_le(crc, buf, len);
#else
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
#endif
}

// ===========================================================================
// 2. FRAME VALIDATION AND DUPLICATES
// ===========================================================================

/** Result of checking one received ESP-NOW frame. */
enum FrameResult : uint8_t {
    FRAME_OK = 0,
    FRAME_BAD_LEN,    // not exactly sizeof(NodePacket) bytes
    FRAME_BAD_VER,    // unknown packet_version
    FRAME_BAD_CRC     // content damaged or not one of our packets
};

/**
 * Check a received frame and, if it is good, copy it into a NodePacket.
 *
 * Implements: REQ-RX-02.
 *
 * Checks, in this order: length, packet version, CRC32 over every byte before
 * the crc32 field. Any node_id (0..255) is acceptable here; whether there is a
 * free slot for it is decided later (REQ-NODE-01).
 *
 * @param data  Received bytes.
 * @param len   Number of received bytes (may be anything).
 * @param out   Filled only when the result is FRAME_OK.
 * @return      FRAME_OK or the reason for rejection.
 */
inline FrameResult frameValid(const uint8_t *data, size_t len, NodePacket *out) {
    if (len != sizeof(NodePacket)) return FRAME_BAD_LEN;

    NodePacket p;
    memcpy(&p, data, sizeof(p));            // memcpy: the struct is packed, avoid unaligned reads
    if (p.packet_version != PACKET_VERSION) return FRAME_BAD_VER;

    if (crc32Le(0, data, offsetof(NodePacket, crc32)) != p.crc32) return FRAME_BAD_CRC;

    *out = p;
    return FRAME_OK;
}

/**
 * Decide whether a packet is one more copy of the previous one.
 *
 * Implements: REQ-RX-03.
 *
 * The node sends every packet three times with the same boot_count. Only
 * EQUALITY counts as a duplicate: a LOWER boot_count is new data, because the
 * node counter restarts at 1 after a power loss (battery replacement).
 *
 * @param haveLast   false if nothing was accepted from this node yet.
 * @param lastBoot   boot_count of the last accepted packet.
 * @param newBoot    boot_count of the packet just received.
 */
inline bool isDuplicate(bool haveLast, uint32_t lastBoot, uint32_t newBoot) {
    return haveLast && lastBoot == newBoot;
}

// ===========================================================================
// 3. ROOM TEMPERATURE SELECTION
// ===========================================================================

/** Where a temperature value came from. Stored in 2 bits of Sample::bits. */
enum TempSource : uint8_t {
    SRC_NONE = 0,   // no usable value
    SRC_DS   = 1,   // DS18B20 sensor (accurate)
    SRC_EST  = 2    // estimated: CPU temperature minus a per-node offset
};

/** The temperature chosen for the room, with its origin. */
struct RoomTemp {
    int16_t cC;     // hundredths of a degree C, TEMP_INVALID if src == SRC_NONE
    uint8_t src;    // TempSource
};

/**
 * Choose the room temperature of a packet.
 *
 * Implements: REQ-DATA-01, REQ-DATA-02, REQ-DATA-05 (raw values are untouched).
 *
 *  1. A valid DS18B20 value wins (source DS).
 *  2. Otherwise a valid CPU value minus the node's offset (source EST).
 *  3. Otherwise there is no value (source NONE).
 * "Valid" means: not TEMP_INVALID and the matching error flags are clear.
 *
 * @param p            Accepted packet.
 * @param cpuOffsetCc  Per-node offset (CPU reads higher than the room), 0.01 C.
 */
inline RoomTemp pickTemperature(const NodePacket &p, int16_t cpuOffsetCc) {
    const bool dsOk  = p.ds18b20_temp_cC != TEMP_INVALID &&
                       !(p.status_flags & (FLAG_DS_ABSENT | FLAG_DS_ERR));
    const bool cpuOk = p.cpu_temp_cC != TEMP_INVALID &&
                       !(p.status_flags & FLAG_CPU_TEMP_ERR);

    if (dsOk) {
        return RoomTemp{p.ds18b20_temp_cC, SRC_DS};
    }
    if (cpuOk) {
        int32_t est = (int32_t)p.cpu_temp_cC - (int32_t)cpuOffsetCc;
        if (est < -5500) est = -5500;                 // keep clear of the TEMP_INVALID marker
        if (est > 12500) est = 12500;
        return RoomTemp{(int16_t)est, SRC_EST};
    }
    return RoomTemp{TEMP_INVALID, SRC_NONE};
}

// ===========================================================================
// 4. BATTERY PERCENTAGE (informational)
// ===========================================================================

/**
 * Rough Li-ion charge percentage from the battery voltage.
 *
 * Implements: REQ-DATA-03. Informational only: the icons follow the node's own
 * battery_state, never this number.
 *
 * Linear interpolation between 11 points of a typical 18650 discharge curve.
 *
 * @param mv  Battery voltage in millivolts.
 * @return    0..100.
 */
inline uint8_t batteryPercent(uint16_t mv) {
    static const uint16_t kMv[11]  = {3300, 3500, 3600, 3680, 3740, 3800, 3880, 3960, 4050, 4150, 4200};
    static const uint8_t  kPct[11] = {   0,    5,   12,   25,   40,   55,   68,   80,   90,   97,  100};

    if (mv <= kMv[0])  return 0;
    if (mv >= kMv[10]) return 100;

    for (int i = 1; i < 11; i++) {
        if (mv <= kMv[i]) {
            uint32_t span = kMv[i] - kMv[i - 1];
            uint32_t gain = (uint32_t)(kPct[i] - kPct[i - 1]) * (mv - kMv[i - 1]);
            return (uint8_t)(kPct[i - 1] + gain / span);
        }
    }
    return 100;   // unreachable, keeps the compiler happy
}

// ===========================================================================
// 5. NODE STATUS AND STUCK DETECTION
// ===========================================================================

/** The single status shown for a node (REQ-STATE-01). CHARGING is a separate overlay. */
enum NodeStatus : uint8_t {
    ST_WAITING = 0,     // slot exists, nothing received yet
    ST_OK,
    ST_LOW,
    ST_EMPTY,
    ST_BAT_INVALID,
    ST_STUCK            // silent for longer than STUCK_FACTOR x sleep time
};

/** Short machine name of a status (used in the JSON API and the debug log). */
inline const char *statusName(NodeStatus s) {
    switch (s) {
        case ST_WAITING:     return "waiting";
        case ST_OK:          return "ok";
        case ST_LOW:         return "low";
        case ST_EMPTY:       return "empty";
        case ST_BAT_INVALID: return "bat_invalid";
        case ST_STUCK:       return "stuck";
    }
    return "?";
}

/** The facts evalStatus() needs about one node. */
struct StatusInput {
    bool     haveLast;   // a packet was accepted since the server booted
    uint32_t lastRxMs;   // millis() of that packet (only if haveLast)
    uint32_t sleepS;     // node sleep time: from the last packet, else restored from flash (0 = unknown)
    uint8_t  batState;   // battery_state of the last packet (only if haveLast)
};

/**
 * Silence time after which a node counts as stuck.
 *
 * Implements: REQ-STATE-02. Example: 900 s x 1.5 = 1350 s (22.5 min).
 *
 * @param sleepS  Sleep time announced by the node, seconds.
 * @return        Timeout in milliseconds (clamped to the uint32_t range).
 */
inline uint32_t stuckTimeoutMs(uint32_t sleepS) {
    double ms = (double)sleepS * 1000.0 * (double)STUCK_FACTOR;
    return ms > 4.0e9 ? 4000000000u : (uint32_t)ms;
}

/**
 * Work out the status of one node.
 *
 * Implements: REQ-STATE-01 .. REQ-STATE-04.
 *
 * Priority (highest first): STUCK, EMPTY, LOW, BAT_INVALID, OK; WAITING if no
 * packet has arrived since the server booted. All time arithmetic uses
 * unsigned subtraction on millis(), so it stays correct when millis() wraps
 * (after 49.7 days) and never needs the wall clock (REQ-STATE-03).
 *
 * After a server reboot there is no previous packet, so the silence is counted
 * from server boot (millis() == 0) using the sleep time restored from flash
 * (REQ-STATE-04).
 *
 * @param in     Facts about the node.
 * @param nowMs  Current millis().
 */
inline NodeStatus evalStatus(const StatusInput &in, uint32_t nowMs) {
    const uint32_t reference = in.haveLast ? in.lastRxMs : 0;
    const uint32_t silentMs  = nowMs - reference;

    if (in.sleepS > 0 && silentMs > stuckTimeoutMs(in.sleepS)) return ST_STUCK;
    if (!in.haveLast) return ST_WAITING;

    switch (in.batState) {
        case BAT_STATE_EMPTY:   return ST_EMPTY;
        case BAT_STATE_LOW:     return ST_LOW;
        case BAT_STATE_INVALID: return ST_BAT_INVALID;
        default:                return ST_OK;
    }
}

// ===========================================================================
// 6. HISTORY SAMPLE
// ===========================================================================

/**
 * One stored measurement (12 bytes, REQ-HIST-02 allows up to 16).
 *
 * `bits` layout: [1:0] TempSource, [3:2] battery state, [4] charging.
 */
typedef struct __attribute__((packed)) {
    uint32_t ts;        // timestamp: epoch seconds, or "virtual clock" seconds (see below)
    int16_t  room_cC;   // selected room temperature, or TEMP_INVALID
    int16_t  cpu_cC;    // raw CPU temperature, or TEMP_INVALID
    uint16_t bat_mv;    // battery voltage
    uint8_t  bits;      // packed source / battery state / charging
    uint8_t  reserved;  // keeps the size at 12 and the file format simple
} Sample;

static_assert(sizeof(Sample) == 12, "Sample must stay 12 bytes (snapshot format)");
static_assert(sizeof(Sample) <= 16, "REQ-HIST-02: a sample must not exceed 16 bytes");

/**
 * Timestamps before the wall clock is known (REQ-HIST-05).
 *
 * Without NTP the server counts "virtual" seconds: VIRTUAL_BASE_S + uptime.
 * The base is large (3 years) so older samples can be shifted backwards after a
 * reboot without underflow, yet far below any real epoch (2024 = 1.7e9), so a
 * timestamp below EPOCH_VALID_MIN_S is always virtual.
 */
#define VIRTUAL_BASE_S      100000000UL
#define EPOCH_VALID_MIN_S   1704067200UL     // 2024-01-01 00:00:00 UTC

/** @return true if the stamp is a virtual-clock value, not a real epoch. */
inline bool stampIsVirtual(uint32_t ts) { return ts < EPOCH_VALID_MIN_S; }

/** Pack a measurement into a Sample. */
inline Sample packSample(uint32_t ts, const RoomTemp &room, const NodePacket &p) {
    Sample s;
    s.ts       = ts;
    s.room_cC  = room.cC;
    s.cpu_cC   = p.cpu_temp_cC;
    s.bat_mv   = p.battery_mv;
    s.bits     = (uint8_t)((room.src & 0x03) | ((p.battery_state & 0x03) << 2) |
                           (p.is_charging ? 0x10 : 0));
    s.reserved = 0;
    return s;
}

/** @return the TempSource of a sample. */
inline uint8_t sampleSource(const Sample &s)       { return s.bits & 0x03; }
/** @return the battery state (0..3) of a sample. */
inline uint8_t sampleBatState(const Sample &s)     { return (s.bits >> 2) & 0x03; }
/** @return true if the node was charging when the sample was taken. */
inline bool    sampleCharging(const Sample &s)     { return (s.bits & 0x10) != 0; }

// ===========================================================================
// 7. SMALL NAMES
// ===========================================================================

/** Corner indices used by the node slots and the room view. */
enum Corner : uint8_t { CORNER_NW = 0, CORNER_NE = 1, CORNER_SW = 2, CORNER_SE = 3 };

#endif  // LOGIC_H
