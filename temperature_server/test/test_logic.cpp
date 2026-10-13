/**
 * @file test_logic.cpp
 * @brief Host unit tests for logic.h and history.cpp (test cases T-1.x of the plan).
 *
 * Build and run with `make -C test`. No framework: a tiny CHECK macro counts
 * failures, and the process exit code is the number of failed checks.
 */

#include <cstdio>
#include <cstring>

#include "history.h"
#include "logic.h"

static int g_failed = 0;
static int g_total  = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_total++;                                                           \
        if (!(cond)) {                                                       \
            g_failed++;                                                      \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);            \
        }                                                                    \
    } while (0)

#define TEST(name) static void name(); static struct Reg_##name { Reg_##name() { reg(#name, name); } } reg_##name; static void name()

typedef void (*TestFn)();
struct Entry { const char *name; TestFn fn; };
static Entry g_tests[64];
static int   g_testCount = 0;
static void reg(const char *n, TestFn f) { g_tests[g_testCount++] = {n, f}; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Build a valid packet with a correct CRC. */
static NodePacket makePacket(uint8_t id, uint32_t boot) {
    NodePacket p;
    memset(&p, 0, sizeof(p));
    p.packet_version  = PACKET_VERSION;
    p.fw_version      = 1;
    p.node_id         = id;
    p.boot_count      = boot;
    p.battery_mv      = 3900;
    p.battery_state   = BAT_STATE_NORMAL;
    p.sleep_time_s    = 900;
    p.cpu_temp_cC     = 3012;
    p.ds18b20_temp_cC = 2147;
    p.crc32 = crc32Le(0, (const uint8_t *)&p, offsetof(NodePacket, crc32));
    return p;
}

static Sample makeSample(uint32_t ts, int16_t room) {
    Sample s;
    memset(&s, 0, sizeof(s));
    s.ts = ts; s.room_cC = room; s.cpu_cC = TEMP_INVALID; s.bat_mv = 3900;
    return s;
}

// ---------------------------------------------------------------------------
// T-1.1 .. T-1.4  frame validation (REQ-RX-02)
// ---------------------------------------------------------------------------

TEST(frame_length) {
    NodePacket p = makePacket(1, 1), out;
    uint8_t buf[40];
    memcpy(buf, &p, sizeof(p));
    CHECK(frameValid(buf, sizeof(p) - 1, &out) == FRAME_BAD_LEN);   // T-1.1
    CHECK(frameValid(buf, sizeof(p) + 1, &out) == FRAME_BAD_LEN);
    CHECK(frameValid(buf, 0, &out) == FRAME_BAD_LEN);
}

TEST(frame_version) {
    NodePacket p = makePacket(1, 1), out;
    p.packet_version = 2;
    p.crc32 = crc32Le(0, (const uint8_t *)&p, offsetof(NodePacket, crc32));
    CHECK(frameValid((const uint8_t *)&p, sizeof(p), &out) == FRAME_BAD_VER);   // T-1.2
}

TEST(frame_crc) {
    NodePacket p = makePacket(1, 1), out;
    p.battery_mv ^= 0x0001;                                          // one flipped bit
    CHECK(frameValid((const uint8_t *)&p, sizeof(p), &out) == FRAME_BAD_CRC);   // T-1.3
}

TEST(frame_ok) {
    NodePacket p = makePacket(3, 57), out;
    memset(&out, 0, sizeof(out));
    CHECK(frameValid((const uint8_t *)&p, sizeof(p), &out) == FRAME_OK);        // T-1.4
    CHECK(out.node_id == 3 && out.boot_count == 57 && out.battery_mv == 3900);
}

TEST(crc_matches_zlib_reference) {
    // Well-known CRC-32 check value: crc32("123456789") == 0xCBF43926.
    const char *s = "123456789";
    CHECK(crc32Le(0, (const uint8_t *)s, 9) == 0xCBF43926u);
    // Chaining two buffers must equal one pass.
    uint32_t a = crc32Le(0, (const uint8_t *)s, 4);
    CHECK(crc32Le(a, (const uint8_t *)s + 4, 5) == 0xCBF43926u);
}

// ---------------------------------------------------------------------------
// T-1.5, T-1.6  duplicates (REQ-RX-03)
// ---------------------------------------------------------------------------

TEST(duplicates) {
    CHECK(!isDuplicate(false, 0, 5));      // nothing before: never a duplicate
    CHECK(isDuplicate(true, 5, 5));        // T-1.5: same boot_count -> duplicate
    CHECK(!isDuplicate(true, 5, 6));
    CHECK(!isDuplicate(true, 57, 1));      // T-1.6: node restarted, lower value is NEW data
}

// ---------------------------------------------------------------------------
// T-1.7 .. T-1.9  temperature selection (REQ-DATA-02)
// ---------------------------------------------------------------------------

TEST(temperature_ds) {
    NodePacket p = makePacket(1, 1);
    RoomTemp t = pickTemperature(p, 800);
    CHECK(t.src == SRC_DS && t.cC == 2147);                          // T-1.7
}

TEST(temperature_estimated) {
    NodePacket p = makePacket(1, 1);
    p.status_flags = FLAG_DS_ABSENT;
    p.ds18b20_temp_cC = TEMP_INVALID;
    RoomTemp t = pickTemperature(p, 800);
    CHECK(t.src == SRC_EST && t.cC == 3012 - 800);                   // T-1.8
    p.status_flags = FLAG_DS_ERR;                                    // error flag alone also disqualifies DS
    p.ds18b20_temp_cC = 2147;
    CHECK(pickTemperature(p, 0).src == SRC_EST);
}

TEST(temperature_none) {
    NodePacket p = makePacket(1, 1);
    p.ds18b20_temp_cC = TEMP_INVALID;
    p.cpu_temp_cC = TEMP_INVALID;
    p.status_flags = FLAG_DS_ABSENT | FLAG_CPU_TEMP_ERR;
    RoomTemp t = pickTemperature(p, 0);
    CHECK(t.src == SRC_NONE && t.cC == TEMP_INVALID);                // T-1.9
}

// ---------------------------------------------------------------------------
// T-1.10 .. T-1.12  status (REQ-STATE)
// ---------------------------------------------------------------------------

TEST(status_stuck_boundary) {
    StatusInput in = {true, 1000, 900, BAT_STATE_NORMAL};
    // 900 s x 1.5 = 1350 s = 1 350 000 ms
    CHECK(evalStatus(in, 1000 + 1349000) == ST_OK);                  // T-1.10
    CHECK(evalStatus(in, 1000 + 1350000) == ST_OK);                  // boundary itself is not yet stuck
    CHECK(evalStatus(in, 1000 + 1351000) == ST_STUCK);
}

TEST(status_priority) {
    StatusInput in = {true, 0, 900, BAT_STATE_LOW};
    CHECK(evalStatus(in, 1000) == ST_LOW);
    in.batState = BAT_STATE_EMPTY;   CHECK(evalStatus(in, 1000) == ST_EMPTY);
    in.batState = BAT_STATE_INVALID; CHECK(evalStatus(in, 1000) == ST_BAT_INVALID);
    in.batState = BAT_STATE_LOW;                                     // T-1.11: stuck beats low battery
    CHECK(evalStatus(in, 2000000) == ST_STUCK);
}

TEST(status_waiting_and_restored_sleep) {
    StatusInput in = {false, 0, 0, 0};
    CHECK(evalStatus(in, 5000000) == ST_WAITING);                    // unknown sleep time: never stuck
    in.sleepS = 900;                                                 // restored from flash (REQ-STATE-04)
    CHECK(evalStatus(in, 1349000) == ST_WAITING);
    CHECK(evalStatus(in, 1351000) == ST_STUCK);
}

TEST(status_millis_wrap) {
    // Packet arrived 10 s before millis() wrapped; now it is 10 s after the wrap.
    StatusInput in = {true, 0xFFFFFFFFu - 10000u, 900, BAT_STATE_NORMAL};
    CHECK(evalStatus(in, 10000u) == ST_OK);                          // T-1.12
    CHECK(evalStatus(in, 1400000u) == ST_STUCK);
}

TEST(status_long_sleep) {
    StatusInput in = {true, 0, 18000, BAT_STATE_EMPTY};              // EMPTY battery: 5 h sleep
    CHECK(evalStatus(in, 26999000u) == ST_EMPTY);                    // 7.5 h = 27 000 s
    CHECK(evalStatus(in, 27001000u) == ST_STUCK);
}

// ---------------------------------------------------------------------------
// Battery percentage (REQ-DATA-03)
// ---------------------------------------------------------------------------

TEST(battery_percent) {
    CHECK(batteryPercent(3000) == 0);
    CHECK(batteryPercent(4200) == 100);
    CHECK(batteryPercent(4500) == 100);
    CHECK(batteryPercent(3800) == 55);
    uint8_t prev = 0;                                                // monotonic
    for (uint16_t mv = 3000; mv <= 4300; mv += 10) {
        uint8_t v = batteryPercent(mv);
        CHECK(v >= prev);
        prev = v;
    }
}

// ---------------------------------------------------------------------------
// Sample packing (REQ-HIST-02)
// ---------------------------------------------------------------------------

TEST(sample_packing) {
    NodePacket p = makePacket(1, 1);
    p.battery_state = BAT_STATE_LOW;
    p.is_charging = 1;
    Sample s = packSample(1800000000u, RoomTemp{2147, SRC_EST}, p);
    CHECK(sampleSource(s) == SRC_EST);
    CHECK(sampleBatState(s) == BAT_STATE_LOW);
    CHECK(sampleCharging(s));
    CHECK(s.room_cC == 2147 && s.cpu_cC == 3012 && s.bat_mv == 3900);
}

// ---------------------------------------------------------------------------
// T-1.13 .. T-1.15  history (REQ-HIST)
// ---------------------------------------------------------------------------

static History g_hist;   // 9 KB: keep it off the stack

TEST(history_wraparound) {
    histClear(g_hist);
    for (uint32_t i = 0; i < HIST_POINTS + 32; i++) histAppend(g_hist, makeSample(1800000000u + i, (int16_t)i));
    CHECK(g_hist.count == HIST_POINTS);                              // T-1.13
    CHECK(histAt(g_hist, 0).room_cC == 32);                          // oldest 32 overwritten
    CHECK(histAt(g_hist, HIST_POINTS - 1).room_cC == (int16_t)(HIST_POINTS + 31));
    for (uint16_t i = 1; i < g_hist.count; i++) {                    // order preserved
        if (histAt(g_hist, i).ts != histAt(g_hist, i - 1).ts + 1) { CHECK(false); break; }
    }
}

TEST(history_range) {
    histClear(g_hist);
    for (uint32_t i = 0; i < 10; i++) histAppend(g_hist, makeSample(1800000000u + i * 900, (int16_t)i));
    CHECK(histFirstFrom(g_hist, 0) == 0);
    CHECK(histFirstFrom(g_hist, 1800000000u + 3 * 900) == 3);
    CHECK(histFirstFrom(g_hist, 1800000000u + 9 * 900 + 1) == 10);   // newer than everything
}

TEST(history_downsample) {
    histClear(g_hist);
    for (uint32_t i = 0; i < HIST_POINTS; i++) histAppend(g_hist, makeSample(1800000000u + i * 900, 2000));
    static Sample out[HIST_API_MAX_POINTS];
    uint16_t step = 0;
    uint16_t n = histDownsample(g_hist, 0, HIST_API_MAX_POINTS, out, &step);
    CHECK(n <= HIST_API_MAX_POINTS && n > 0);                        // T-1.14
    CHECK(step == 3);                                                // ceil(768/300)
    CHECK(out[0].room_cC == 2000);                                   // averaging a constant keeps it
    CHECK(out[n - 1].ts > out[0].ts);

    // A short history is returned untouched.
    histClear(g_hist);
    for (uint32_t i = 0; i < 5; i++) histAppend(g_hist, makeSample(1800000000u + i, (int16_t)(100 * i)));
    n = histDownsample(g_hist, 0, HIST_API_MAX_POINTS, out, &step);
    CHECK(n == 5 && step == 1 && out[4].room_cC == 400);
}

TEST(history_downsample_invalid_values) {
    histClear(g_hist);
    histAppend(g_hist, makeSample(1800000000u, TEMP_INVALID));
    histAppend(g_hist, makeSample(1800000001u, 2000));
    histAppend(g_hist, makeSample(1800000002u, TEMP_INVALID));
    histAppend(g_hist, makeSample(1800000003u, TEMP_INVALID));
    Sample out[2]; uint16_t step = 0;
    uint16_t n = histDownsample(g_hist, 0, 2, out, &step);
    CHECK(n == 2 && step == 2);
    CHECK(out[0].room_cC == 2000);                                   // invalid sample ignored in the mean
    CHECK(out[1].room_cC == TEMP_INVALID);                           // nothing valid in the group
}

TEST(history_rebase) {
    histClear(g_hist);
    histAppend(g_hist, makeSample(1800000000u, 1));                  // already real
    histAppend(g_hist, makeSample(VIRTUAL_BASE_S + 100, 2));         // virtual
    histAppend(g_hist, makeSample(VIRTUAL_BASE_S + 200, 3));
    histShiftVirtual(g_hist, 1700000000LL);                          // T-1.15
    CHECK(histAt(g_hist, 0).ts == 1800000000u);                      // untouched
    CHECK(histAt(g_hist, 1).ts == VIRTUAL_BASE_S + 100 + 1700000000u);
    CHECK(!stampIsVirtual(histAt(g_hist, 1).ts));
    histShiftVirtual(g_hist, 1700000000LL);                          // second call changes nothing
    CHECK(histAt(g_hist, 1).ts == VIRTUAL_BASE_S + 100 + 1700000000u);

    histClear(g_hist);
    histAppend(g_hist, makeSample(VIRTUAL_BASE_S + 50, 1));
    histShiftVirtual(g_hist, -(int64_t)VIRTUAL_BASE_S * 2);          // would underflow: clamped
    CHECK(histAt(g_hist, 0).ts == 1);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    for (int i = 0; i < g_testCount; i++) {
        int before = g_failed;
        g_tests[i].fn();
        std::printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", g_tests[i].name);
    }
    std::printf("\n%d checks, %d failed\n", g_total, g_failed);
    return g_failed;
}
