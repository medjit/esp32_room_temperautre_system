/**
 * @file nodes.cpp
 * @brief Node slot table (see nodes.h).
 */

#include <Arduino.h>

#include "nodes.h"
#include "debug.h"

NodeSlot g_nodes[MAX_NODES];
RxStats  g_stats;

/** Node ids heard without a free slot (small fixed set, REQ-NODE-01). */
#define UNKNOWN_IDS_MAX 8
static uint8_t s_unknownIds[UNKNOWN_IDS_MAX];
static uint8_t s_unknownCount = 0;

void nodesInit() {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(&g_stats, 0, sizeof(g_stats));
    s_unknownCount = 0;
}

int nodeFindIndex(uint8_t nodeId) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].used && g_nodes[i].nodeId == nodeId) return i;
    }
    return -1;
}

/** @return the first corner (NW, NE, SW, SE order) that no slot uses, or NW if all are taken. */
static uint8_t firstFreeCorner() {
    for (uint8_t c = 0; c < 4; c++) {
        bool taken = false;
        for (int i = 0; i < MAX_NODES; i++) {
            if (g_nodes[i].used && g_nodes[i].corner == c) taken = true;
        }
        if (!taken) return c;
    }
    return CORNER_NW;
}

/** Remove a node id from the unknown list (it has a slot now). */
static void forgetUnknown(uint8_t nodeId) {
    for (uint8_t i = 0; i < s_unknownCount; i++) {
        if (s_unknownIds[i] == nodeId) {
            s_unknownIds[i] = s_unknownIds[--s_unknownCount];
            return;
        }
    }
}

/**
 * Create a slot for a new node.
 *
 * Implements: REQ-NODE-01, REQ-NODE-02 defaults: name "Node N", first free
 * corner, DEFAULT_CPU_OFFSET_CC. The caller persists it (storeSaveNode).
 */
int nodeCreate(uint8_t nodeId) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].used) continue;

        NodeSlot &n = g_nodes[i];
        memset(&n, 0, sizeof(n));
        n.used        = true;
        n.nodeId      = nodeId;
        n.corner      = firstFreeCorner();
        n.cpuOffsetCc = DEFAULT_CPU_OFFSET_CC;
        n.status      = ST_WAITING;
        snprintf(n.name, sizeof(n.name), "Node %u", (unsigned)nodeId);
        forgetUnknown(nodeId);
        DBG_PRINTF("NODE: created slot %d for node %u", i, (unsigned)nodeId);
        return i;
    }
    return -1;
}

/**
 * Assign a corner; swap with the current owner if it is taken.
 * Implements: REQ-NODE-04.
 */
void nodeSetCorner(int idx, uint8_t corner) {
    if (idx < 0 || idx >= MAX_NODES || corner > 3) return;

    for (int i = 0; i < MAX_NODES; i++) {
        if (i != idx && g_nodes[i].used && g_nodes[i].corner == corner) {
            g_nodes[i].corner = g_nodes[idx].corner;      // the other node takes our old corner
        }
    }
    g_nodes[idx].corner = corner;
}

/**
 * Set the node name.
 *
 * Implements: REQ-NODE-02. Control characters become spaces, leading and
 * trailing blanks are trimmed, and the text is cut at NODE_NAME_MAX bytes
 * without splitting a UTF-8 character. An empty result falls back to "Node N".
 */
void nodeSetName(int idx, const char *name) {
    if (idx < 0 || idx >= MAX_NODES || !g_nodes[idx].used) return;
    NodeSlot &n = g_nodes[idx];

    char tmp[NODE_NAME_MAX + 1];
    size_t len = 0;
    for (const char *p = name; *p && len < NODE_NAME_MAX; p++) {
        tmp[len++] = ((uint8_t)*p < 0x20) ? ' ' : *p;
    }
    // If the cut landed inside a multi-byte UTF-8 character, drop the partial bytes.
    if (*(name + len) != '\0') {
        while (len > 0 && ((uint8_t)name[len] & 0xC0) == 0x80) len--;
    }
    while (len > 0 && tmp[len - 1] == ' ') len--;           // trim right
    size_t start = 0;
    while (start < len && tmp[start] == ' ') start++;       // trim left

    if (start >= len) {
        snprintf(n.name, sizeof(n.name), "Node %u", (unsigned)n.nodeId);
        return;
    }
    memcpy(n.name, tmp + start, len - start);
    n.name[len - start] = '\0';
}

/** Free a slot. Implements: REQ-NODE-03 (history goes with it). */
void nodeForget(int idx) {
    if (idx < 0 || idx >= MAX_NODES) return;
    DBG_PRINTF("NODE: forgot node %u", (unsigned)g_nodes[idx].nodeId);
    memset(&g_nodes[idx], 0, sizeof(NodeSlot));
}

void nodeNoteUnknown(uint8_t nodeId) {
    for (uint8_t i = 0; i < s_unknownCount; i++) {
        if (s_unknownIds[i] == nodeId) return;               // already listed
    }
    if (s_unknownCount < UNKNOWN_IDS_MAX) s_unknownIds[s_unknownCount++] = nodeId;
}

uint8_t nodeUnknownIds(uint8_t *out, uint8_t max) {
    uint8_t n = (s_unknownCount < max) ? s_unknownCount : max;
    memcpy(out, s_unknownIds, n);
    return n;
}

/**
 * Remember the newest packet of a node.
 * Implements: REQ-NODE-05, REQ-RX-05 (receive time and RSSI).
 */
void nodeApplyPacket(NodeSlot &n, const NodePacket &p, int8_t rssi, uint32_t rxMs) {
    n.last        = p;
    n.haveLast    = true;
    n.lastRxMs    = rxMs;
    n.rssi        = rssi;
    n.savedSleepS = p.sleep_time_s;
    n.rxCount++;
}

uint32_t nodeSleepSeconds(const NodeSlot &n) {
    return n.haveLast ? n.last.sleep_time_s : n.savedSleepS;
}

/**
 * Update the cached status of every used slot.
 *
 * Implements: REQ-STATE-01 (evaluated at least once per second by loop()).
 * A change is logged, which makes STUCK transitions visible in the debug log
 * (REQ-DBG-02).
 */
void nodesEvalStatus(uint32_t nowMs) {
    for (int i = 0; i < MAX_NODES; i++) {
        NodeSlot &n = g_nodes[i];
        if (!n.used) continue;

        StatusInput in;
        in.haveLast = n.haveLast;
        in.lastRxMs = n.lastRxMs;
        in.sleepS   = nodeSleepSeconds(n);
        in.batState = n.haveLast ? n.last.battery_state : BAT_STATE_NORMAL;

        NodeStatus s = evalStatus(in, nowMs);
        if (s != n.status) {
            DBG_PRINTF("STATE: node %u %s -> %s", (unsigned)n.nodeId,
                       statusName(n.status), statusName(s));
            n.status = s;
        }
    }
}

void nodesShiftVirtual(int64_t delta) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].used) histShiftVirtual(g_nodes[i].hist, delta);
    }
}
