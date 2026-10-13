/**
 * @file rx.cpp
 * @brief ESP-NOW receive path (see rx.h).
 */

#include <esp_now.h>

#include "rx.h"
#include "board.h"
#include "config.h"
#include "debug.h"
#include "logic.h"
#include "netmgr.h"
#include "nodes.h"
#include "store.h"

// ---------------------------------------------------------------------------
// Frame queue
// ---------------------------------------------------------------------------

/** One received frame, copied out of the Wi-Fi driver's buffer. */
struct RxFrame {
    uint8_t  data[sizeof(NodePacket)];   // first bytes of the frame (longer frames are cut)
    uint8_t  len;                        // REAL length, clamped to 255, so BAD_LEN can be detected
    int8_t   rssi;                       // dBm, 0 if the driver did not report it
    uint32_t rxMs;                       // millis() at reception (REQ-RX-05)
};

// The three indices are only touched inside portENTER_CRITICAL(&s_qLock), which also
// acts as a memory barrier, so they do not need to be `volatile`.
static RxFrame      s_queue[RX_QUEUE_LEN];
static uint8_t      s_qHead  = 0;        // next slot to write
static uint8_t      s_qTail  = 0;        // next slot to read
static uint8_t      s_qCount = 0;        // frames waiting
static portMUX_TYPE s_qLock  = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_ledOffAtMs = 0;        // 0 = LED not pulsing

/**
 * Put a frame into the queue. Runs in the Wi-Fi task, so it is short and never
 * blocks. When the queue is full the OLDEST frame is dropped to make room for
 * the new one (REQ-RX-04) and the overflow counter goes up.
 */
static void queuePush(const uint8_t *data, int len, int8_t rssi) {
    RxFrame f;
    memset(f.data, 0, sizeof(f.data));
    size_t copy = (size_t)len < sizeof(f.data) ? (size_t)len : sizeof(f.data);
    memcpy(f.data, data, copy);
    f.len  = (len > 255) ? 255 : (uint8_t)len;
    f.rssi = rssi;
    f.rxMs = millis();

    portENTER_CRITICAL(&s_qLock);
    if (s_qCount == RX_QUEUE_LEN) {                       // full: drop the oldest
        s_qTail = (s_qTail + 1) % RX_QUEUE_LEN;
        s_qCount--;
        g_stats.overflow++;
    }
    s_queue[s_qHead] = f;
    s_qHead = (s_qHead + 1) % RX_QUEUE_LEN;
    s_qCount++;
    portEXIT_CRITICAL(&s_qLock);
}

/** Take the oldest frame out of the queue. @return false if the queue is empty. */
static bool queuePop(RxFrame *out) {
    bool got = false;
    portENTER_CRITICAL(&s_qLock);
    if (s_qCount > 0) {
        *out = s_queue[s_qTail];
        s_qTail = (s_qTail + 1) % RX_QUEUE_LEN;
        s_qCount--;
        got = true;
    }
    portEXIT_CRITICAL(&s_qLock);
    return got;
}

// ---------------------------------------------------------------------------
// ESP-NOW callback
// ---------------------------------------------------------------------------

/**
 * ESP-NOW "frame received" callback.
 *
 * Implements: REQ-RX-01, REQ-RX-05.
 *
 * Runs in the Wi-Fi task: it only copies the frame (plus RSSI) into the queue.
 * Arduino core 3.x uses the `esp_now_recv_info_t` form of this callback.
 */
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    int8_t rssi = (info && info->rx_ctrl) ? (int8_t)info->rx_ctrl->rssi : 0;
    queuePush(data, len, rssi);
}

/**
 * Initialise ESP-NOW and register the callback.
 *
 * Implements: REQ-RX-01. No peer needs to be registered to RECEIVE broadcasts.
 */
bool rxInit() {
    if (esp_now_init() != ESP_OK) {
        DBG_PRINTF("RX: ERROR esp_now_init failed");
        return false;
    }
    esp_now_register_recv_cb(onRecv);
    DBG_PRINTF("RX: ESP-NOW listening on channel %u", (unsigned)netChannel());
    return true;
}

// ---------------------------------------------------------------------------
// Frame processing (loop context)
// ---------------------------------------------------------------------------

/** Start a short LED pulse for a new packet (REQ-RX-06). */
static void ledPulse() {
#if RX_LED_ENABLED
    ledWrite(true);
    s_ledOffAtMs = millis() + RX_LED_PULSE_MS;
    if (s_ledOffAtMs == 0) s_ledOffAtMs = 1;
#endif
}

void rxLedTick(uint32_t nowMs) {
    if (s_ledOffAtMs != 0 && (int32_t)(nowMs - s_ledOffAtMs) >= 0) {
        ledWrite(false);
        s_ledOffAtMs = 0;
    }
}

/**
 * Count a damaged frame against the node it claims to come from.
 *
 * Implements: REQ-NODE-05 (crc_errors). The node_id byte of a damaged frame
 * may itself be wrong, so this is best effort and only counts for known nodes.
 */
static void countCrcError(const RxFrame &f) {
    if (f.len != sizeof(NodePacket)) return;
    const int idx = nodeFindIndex(f.data[offsetof(NodePacket, node_id)]);
    if (idx >= 0) g_nodes[idx].crcErrCount++;
}

/**
 * Handle one frame: validate, find or create the slot, drop duplicates, then
 * store the packet and a history sample.
 *
 * Implements: REQ-RX-02, REQ-RX-03, REQ-NODE-01, REQ-NODE-05, REQ-DATA-02,
 *             REQ-HIST-02, REQ-ERR-02, REQ-DBG-02.
 */
static void handleFrame(const RxFrame &f) {
    g_stats.frames++;

    // 1. Is it a good packet?
    NodePacket p;
    switch (frameValid(f.data, f.len, &p)) {
        case FRAME_OK:      break;
        case FRAME_BAD_LEN: g_stats.badLen++; DBG_PRINTF("RX: REJECT bad length %u", f.len); return;
        case FRAME_BAD_VER: g_stats.badVer++; DBG_PRINTF("RX: REJECT bad version");           return;
        case FRAME_BAD_CRC: g_stats.badCrc++; DBG_PRINTF("RX: REJECT bad crc");
                            countCrcError(f); return;
    }

    // 2. Which slot does it belong to? Learn new nodes while there is room.
    int idx = nodeFindIndex(p.node_id);
    if (idx < 0) {
        idx = nodeCreate(p.node_id);
        if (idx < 0) {
            g_stats.unknownNode++;
            nodeNoteUnknown(p.node_id);
            DBG_PRINTF("RX: node %u ignored, no free slot", (unsigned)p.node_id);
            return;
        }
        storeSaveNode(idx);
    }
    NodeSlot &n = g_nodes[idx];

    // 3. The node sends every packet three times: keep only the first copy.
    if (isDuplicate(n.haveLast, n.last.boot_count, p.boot_count)) {
        n.dupCount++;
        g_stats.duplicates++;
        DBG_PRINTF("RX: node %u boot %lu duplicate", (unsigned)p.node_id, (unsigned long)p.boot_count);
        return;
    }

    // 4. New data: update the slot and append a history sample.
    const bool sleepChanged = (n.savedSleepS != p.sleep_time_s);
    nodeApplyPacket(n, p, f.rssi, f.rxMs);

    const RoomTemp room = pickTemperature(p, n.cpuOffsetCc);
    histAppend(n.hist, packSample(netStamp(), room, p));
    storeMarkHistoryDirty();
    if (sleepChanged) storeSaveNode(idx);       // keep the stuck timeout across reboots (REQ-STATE-04)

    g_stats.accepted++;
    ledPulse();
    DBG_PRINTF("RX: node %u boot %lu OK temp %d.%02d (%s) bat %u mV rssi %d",
               (unsigned)p.node_id, (unsigned long)p.boot_count,
               room.cC / 100, abs(room.cC % 100),
               room.src == SRC_DS ? "ds" : room.src == SRC_EST ? "est" : "none",
               (unsigned)p.battery_mv, (int)f.rssi);
}

void rxPoll() {
    RxFrame f;
    while (queuePop(&f)) {
        handleFrame(f);
    }
}
