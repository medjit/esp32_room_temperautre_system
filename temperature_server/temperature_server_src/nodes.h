/**
 * @file nodes.h
 * @brief Node slots: settings, latest state and history of every known node.
 *
 * Implements: REQ-NODE-01 .. REQ-NODE-05, REQ-ERR-02 (statistics).
 *
 * A "slot" is the server-side record of one node. Slots are created
 * automatically when the first valid packet of an unknown node_id arrives (up
 * to MAX_NODES). The user-editable part of a slot (name, corner, CPU offset)
 * is saved in flash by store.cpp; the rest lives in RAM.
 */

#ifndef NODES_H
#define NODES_H

#include "history.h"
#include "logic.h"

/** Longest node name (without the terminating zero). */
#define NODE_NAME_MAX  20

/** All data the server keeps about one node. */
struct NodeSlot {
    bool     used;                     // false = free slot

    // ---- identity and user settings (saved in flash, REQ-NODE-02) ----
    uint8_t  nodeId;
    char     name[NODE_NAME_MAX + 1];
    uint8_t  corner;                   // Corner enum (NW/NE/SW/SE)
    int16_t  cpuOffsetCc;              // CPU -> room offset, hundredths of a degree C

    // ---- latest state (RAM only, REQ-NODE-05) ----
    NodePacket last;                   // last accepted packet
    bool       haveLast;               // a packet was accepted since boot
    uint32_t   lastRxMs;               // millis() when it arrived
    int8_t     rssi;                   // signal strength of that packet, dBm (0 = unknown)
    uint32_t   savedSleepS;            // sleep time restored from flash (REQ-STATE-04)
    NodeStatus status;                 // cached result of the last status evaluation
    uint32_t   rxCount;                // accepted packets (duplicates excluded)
    uint32_t   dupCount;               // duplicate copies discarded
    uint32_t   crcErrCount;            // damaged frames that still name this node_id (best effort)

    History    hist;                   // measurement history (REQ-HIST-01)
};

/** Reception statistics of the whole server (REQ-ERR-02). */
struct RxStats {
    uint32_t frames;        // every frame taken from the queue
    uint32_t accepted;      // valid, non-duplicate packets
    uint32_t duplicates;    // valid packets that repeated the previous boot_count
    uint32_t badLen;
    uint32_t badVer;
    uint32_t badCrc;
    uint32_t unknownNode;   // valid packets of a node_id without a free slot
    uint32_t overflow;      // frames lost because the queue was full
};

extern NodeSlot g_nodes[MAX_NODES];
extern RxStats  g_stats;

/** Clear all slots and statistics. Call once at start-up. */
void nodesInit();

/** @return index of the slot used by this node_id, or -1. */
int nodeFindIndex(uint8_t nodeId);

/** Create a slot with default settings. @return its index, or -1 if all slots are in use. */
int nodeCreate(uint8_t nodeId);

/**
 * Give a slot a corner. If another slot already has it, the two swap (REQ-NODE-04).
 * @param idx     Slot to change.
 * @param corner  New corner (Corner enum, 0..3).
 */
void nodeSetCorner(int idx, uint8_t corner);

/** Set a node name; control characters are removed, length is limited (REQ-NODE-02). */
void nodeSetName(int idx, const char *name);

/** Free a slot together with its history (REQ-NODE-03). */
void nodeForget(int idx);

/** Remember a node_id that was heard but has no slot, for the UI notice (REQ-NODE-01). */
void nodeNoteUnknown(uint8_t nodeId);

/** Copy up to `max` unknown node ids into `out`. @return how many were copied. */
uint8_t nodeUnknownIds(uint8_t *out, uint8_t max);

/** Store a freshly accepted packet in the slot (REQ-NODE-05). */
void nodeApplyPacket(NodeSlot &n, const NodePacket &p, int8_t rssi, uint32_t rxMs);

/** @return the node's sleep time: from its last packet, else restored from flash. */
uint32_t nodeSleepSeconds(const NodeSlot &n);

/**
 * Re-evaluate the status of every used slot and log changes.
 * Implements: REQ-STATE-01. Call about once per second.
 */
void nodesEvalStatus(uint32_t nowMs);

/** Shift virtual timestamps of all histories by delta seconds (see histShiftVirtual). */
void nodesShiftVirtual(int64_t delta);

#endif  // NODES_H
