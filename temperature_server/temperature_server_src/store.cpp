/**
 * @file store.cpp
 * @brief NVS settings and LittleFS history snapshot (see store.h).
 *
 * SNAPSHOT FILE FORMAT (little-endian, REQ-PERS-02)
 *
 *   header   : magic(4) | format(2) | slotCount(1) | savedUptimeS(4)
 *   per slot : nodeId(1) | count(2) | count x Sample (12 bytes each, oldest first)
 *   trailer  : crc32(4) over every byte before it
 *
 * `savedUptimeS` lets a restore shift "virtual clock" timestamps (taken before
 * NTP) so that the moment of the save becomes the new boot time (REQ-HIST-05).
 */

#include <LittleFS.h>
#include <Preferences.h>

#include "store.h"
#include "board.h"
#include "config.h"
#include "debug.h"
#include "nodes.h"

Settings g_settings;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE     "rtserver"
#define SNAP_FILE         "/hist.bin"
#define SNAP_TEMP         "/hist.tmp"
#define SNAP_MAGIC        0x52544831UL     // "RTH1"
#define SNAP_FORMAT       1
#define SNAP_CHUNK        32               // samples copied per file write (384 bytes of stack)
#define RESET_WINDOW_MS   30000UL          // BOOT-button reset works only this long after boot

/** One node's saved user settings (REQ-NODE-02, REQ-PERS-01). */
struct __attribute__((packed)) NodeStored {
    uint8_t  used;
    uint8_t  nodeId;
    char     name[NODE_NAME_MAX + 1];
    uint8_t  corner;
    int16_t  cpuOffsetCc;
    uint32_t sleepS;                        // last known sleep time (REQ-STATE-04)
};

struct __attribute__((packed)) SnapHeader {
    uint32_t magic;
    uint16_t format;
    uint8_t  slotCount;
    uint32_t savedUptimeS;
};

struct __attribute__((packed)) SnapSlotHead {
    uint8_t  nodeId;
    uint16_t count;
};

static Preferences s_prefs;
static bool        s_nvsOk        = false;
static bool        s_fsOk         = false;
static bool        s_histDirty    = false;
static uint32_t    s_lastSaveMs   = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/** NVS key of a node slot: "n0", "n1", ... */
static void nodeKey(int idx, char *out) {
    snprintf(out, 4, "n%d", idx);
}

/** Copy a Preferences string into a fixed buffer (always zero-terminated). */
static void loadString(const char *key, const char *fallback, char *dst, size_t dstSize) {
    String v = s_prefs.getString(key, fallback);
    strncpy(dst, v.c_str(), dstSize - 1);
    dst[dstSize - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Settings (NVS)
// ---------------------------------------------------------------------------

/**
 * Open NVS, load all settings and mount the filesystem.
 *
 * Implements: REQ-PERS-01, REQ-ERR-01. If NVS cannot be opened the defaults
 * stay in place and the server still works; if LittleFS cannot be mounted,
 * only the history snapshot is disabled.
 */
void storeInit() {
    // Defaults first: they are what we use if anything below fails.
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.lastChannel = ESPNOW_CHANNEL_DEFAULT;
    strncpy(g_settings.pin,  DEFAULT_PIN,       sizeof(g_settings.pin) - 1);
    strncpy(g_settings.room, DEFAULT_ROOM_NAME, sizeof(g_settings.room) - 1);

    s_nvsOk = s_prefs.begin(NVS_NAMESPACE, false);
    if (s_nvsOk) {
        loadString("ssid", "", g_settings.ssid, sizeof(g_settings.ssid));
        loadString("pass", "", g_settings.pass, sizeof(g_settings.pass));
        loadString("pin",  DEFAULT_PIN,       g_settings.pin,  sizeof(g_settings.pin));
        loadString("room", DEFAULT_ROOM_NAME, g_settings.room, sizeof(g_settings.room));
        uint8_t ch = s_prefs.getUChar("chan", ESPNOW_CHANNEL_DEFAULT);
        g_settings.lastChannel = (ch >= 1 && ch <= 13) ? ch : ESPNOW_CHANNEL_DEFAULT;
    } else {
        DBG_PRINTF("STORE: ERROR NVS unavailable, using defaults");
    }

    s_fsOk = LittleFS.begin(true);           // true = format on first use
    if (!s_fsOk) {
        DBG_PRINTF("STORE: ERROR LittleFS mount failed, history will not be saved");
    }
    s_lastSaveMs = millis();
}

void settingsSave() {
    if (!s_nvsOk) return;
    s_prefs.putString("ssid", g_settings.ssid);
    s_prefs.putString("pass", g_settings.pass);
    s_prefs.putString("pin",  g_settings.pin);
    s_prefs.putString("room", g_settings.room);
    s_prefs.putUChar("chan",  g_settings.lastChannel);
}

bool storePinIsDefault() {
    return strcmp(g_settings.pin, DEFAULT_PIN) == 0;
}

// ---------------------------------------------------------------------------
// Node settings (NVS)
// ---------------------------------------------------------------------------

/** Create slots for the nodes saved earlier. Implements: REQ-PERS-01. */
void storeLoadNodes() {
    if (!s_nvsOk) return;

    for (int i = 0; i < MAX_NODES; i++) {
        char key[4];
        nodeKey(i, key);
        NodeStored st;
        if (s_prefs.getBytesLength(key) != sizeof(st)) continue;
        s_prefs.getBytes(key, &st, sizeof(st));
        if (!st.used) continue;

        NodeSlot &n = g_nodes[i];             // the slot index is part of the saved identity
        memset(&n, 0, sizeof(n));
        n.used        = true;
        n.nodeId      = st.nodeId;
        n.corner      = (st.corner <= 3) ? st.corner : (uint8_t)CORNER_NW;
        n.cpuOffsetCc = st.cpuOffsetCc;
        n.savedSleepS = st.sleepS;
        n.status      = ST_WAITING;
        st.name[NODE_NAME_MAX] = '\0';
        strncpy(n.name, st.name, NODE_NAME_MAX);
        DBG_PRINTF("STORE: restored node %u \"%s\"", (unsigned)n.nodeId, n.name);
    }
}

void storeSaveNode(int idx) {
    if (!s_nvsOk || idx < 0 || idx >= MAX_NODES) return;

    const NodeSlot &n = g_nodes[idx];
    NodeStored st;
    memset(&st, 0, sizeof(st));
    st.used        = n.used ? 1 : 0;
    st.nodeId      = n.nodeId;
    st.corner      = n.corner;
    st.cpuOffsetCc = n.cpuOffsetCc;
    st.sleepS      = n.savedSleepS;
    strncpy(st.name, n.name, NODE_NAME_MAX);

    char key[4];
    nodeKey(idx, key);
    s_prefs.putBytes(key, &st, sizeof(st));
}

// ---------------------------------------------------------------------------
// History snapshot (LittleFS)
// ---------------------------------------------------------------------------

void storeMarkHistoryDirty() { s_histDirty = true; }

/** Write bytes to the file and add them to the running CRC. @return false on a short write. */
static bool writeCrc(File &f, uint32_t &crc, const void *data, size_t len) {
    crc = crc32Le(crc, (const uint8_t *)data, len);
    return f.write((const uint8_t *)data, len) == len;
}

/**
 * Check a snapshot file: it must be big enough and its trailer CRC must match.
 * Implements: REQ-PERS-02 (invalid files are ignored) and the verify step of
 * REQ-PERS-03.
 */
static bool snapshotVerify(const char *path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    const size_t size = f.size();
    if (size < sizeof(SnapHeader) + 4) { f.close(); return false; }

    uint32_t crc = 0;
    size_t   left = size - 4;                 // everything except the trailer
    uint8_t  buf[128];
    while (left > 0) {
        size_t n = f.read(buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n == 0) { f.close(); return false; }
        crc = crc32Le(crc, buf, n);
        left -= n;
    }
    uint32_t stored = 0;
    bool ok = (f.read((uint8_t *)&stored, 4) == 4) && stored == crc;
    f.close();
    return ok;
}

/**
 * Save the history of all nodes.
 *
 * Implements: REQ-PERS-02, REQ-PERS-03, REQ-PERS-04.
 *
 * Atomic with respect to power loss: the data is written to a temporary file,
 * verified, and only then replaces the old snapshot. Samples are copied in
 * chronological order in small chunks, so the ring buffer layout does not
 * matter to the file format.
 */
bool snapshotSave() {
    if (!s_fsOk) return false;
    DBG_RUN(const uint32_t t0 = millis();)      // only used by the debug log

    File f = LittleFS.open(SNAP_TEMP, "w");
    if (!f) { DBG_PRINTF("SNAP: ERROR cannot create temp file"); return false; }

    uint32_t crc = 0;
    bool ok = true;

    SnapHeader hdr;
    hdr.magic = SNAP_MAGIC;
    hdr.format = SNAP_FORMAT;
    hdr.slotCount = 0;
    for (int i = 0; i < MAX_NODES; i++) if (g_nodes[i].used) hdr.slotCount++;
    hdr.savedUptimeS = millis() / 1000UL;
    ok = ok && writeCrc(f, crc, &hdr, sizeof(hdr));

    for (int i = 0; i < MAX_NODES && ok; i++) {
        const NodeSlot &n = g_nodes[i];
        if (!n.used) continue;

        SnapSlotHead sh = {n.nodeId, n.hist.count};
        ok = ok && writeCrc(f, crc, &sh, sizeof(sh));

        Sample chunk[SNAP_CHUNK];
        for (uint16_t at = 0; at < n.hist.count && ok; ) {
            uint16_t k = 0;
            while (k < SNAP_CHUNK && at < n.hist.count) chunk[k++] = histAt(n.hist, at++);
            ok = ok && writeCrc(f, crc, chunk, k * sizeof(Sample));
        }
    }
    ok = ok && (f.write((const uint8_t *)&crc, 4) == 4);
    f.close();

    // Verify BEFORE touching the old snapshot, then swap.
    if (!ok || !snapshotVerify(SNAP_TEMP)) {
        LittleFS.remove(SNAP_TEMP);
        DBG_PRINTF("SNAP: ERROR write or verify failed, old snapshot kept");
        return false;
    }
    LittleFS.remove(SNAP_FILE);
    LittleFS.rename(SNAP_TEMP, SNAP_FILE);

    s_histDirty  = false;
    s_lastSaveMs = millis();
    DBG_PRINTF("SNAP: saved in %lu ms", (unsigned long)(millis() - t0));
    return true;
}

/**
 * Restore the history from the snapshot.
 *
 * Implements: REQ-PERS-05. If power was lost between "remove old" and "rename"
 * of a save, only the verified temp file exists: it is promoted here. Nodes in
 * the file that have no slot are skipped. Virtual timestamps are shifted back
 * by the uptime at save time (REQ-HIST-05).
 */
void snapshotLoad() {
    if (!s_fsOk) return;

    if (!LittleFS.exists(SNAP_FILE) && LittleFS.exists(SNAP_TEMP) && snapshotVerify(SNAP_TEMP)) {
        LittleFS.rename(SNAP_TEMP, SNAP_FILE);
        DBG_PRINTF("SNAP: promoted temp file after interrupted save");
    }
    if (!LittleFS.exists(SNAP_FILE)) return;
    if (!snapshotVerify(SNAP_FILE)) {
        DBG_PRINTF("SNAP: file invalid, ignored");
        return;
    }

    File f = LittleFS.open(SNAP_FILE, "r");
    SnapHeader hdr;
    if (!f || f.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr) ||
        hdr.magic != SNAP_MAGIC || hdr.format != SNAP_FORMAT) {
        if (f) f.close();
        DBG_PRINTF("SNAP: unknown header, ignored");
        return;
    }

    uint32_t restored = 0;
    for (uint8_t s = 0; s < hdr.slotCount; s++) {
        SnapSlotHead sh;
        if (f.read((uint8_t *)&sh, sizeof(sh)) != sizeof(sh)) break;

        const int idx = nodeFindIndex(sh.nodeId);
        if (idx >= 0) histClear(g_nodes[idx].hist);

        Sample chunk[SNAP_CHUNK];
        for (uint16_t left = sh.count; left > 0; ) {
            uint16_t k = left < SNAP_CHUNK ? left : SNAP_CHUNK;
            if (f.read((uint8_t *)chunk, k * sizeof(Sample)) != k * sizeof(Sample)) { left = 0; break; }
            if (idx >= 0) {
                for (uint16_t i = 0; i < k; i++) histAppend(g_nodes[idx].hist, chunk[i]);
                restored += k;
            }
            left -= k;
        }
        if (idx >= 0) histShiftVirtual(g_nodes[idx].hist, -(int64_t)hdr.savedUptimeS);
    }
    f.close();
    DBG_PRINTF("SNAP: restored %lu samples", (unsigned long)restored);
}

void storeClearHistory() {
    for (int i = 0; i < MAX_NODES; i++) histClear(g_nodes[i].hist);
    s_histDirty = true;
}

// ---------------------------------------------------------------------------
// Periodic work and factory reset
// ---------------------------------------------------------------------------

/**
 * Erase everything and restart.
 * Implements: REQ-PERS-06.
 */
void storeFactoryReset() {
    DBG_PRINTF("STORE: FACTORY RESET");
    if (s_nvsOk) s_prefs.clear();
    if (s_fsOk) {
        LittleFS.remove(SNAP_FILE);
        LittleFS.remove(SNAP_TEMP);
    }
    ledWrite(false);
    delay(200);
    ESP.restart();
}

/**
 * Hold the BOOT button for FACTORY_RESET_HOLD_S seconds, within the first
 * 30 s after power-up, to reset the server (REQ-PERS-06). The LED blinks fast
 * while the button is held, so the user sees that the press was noticed.
 */
static void resetButtonTick(uint32_t nowMs) {
    static uint32_t heldSince = 0;           // 0 = not held

    if (nowMs > RESET_WINDOW_MS || !bootButtonPressed()) {
        if (heldSince) { heldSince = 0; ledWrite(false); }
        return;
    }
    if (!heldSince) heldSince = nowMs ? nowMs : 1;

    ledWrite(((nowMs / 100) & 1) == 0);
    if (nowMs - heldSince >= FACTORY_RESET_HOLD_S * 1000UL) storeFactoryReset();
}

/**
 * Do the periodic storage work. Implements: REQ-PERS-02, REQ-PERS-04.
 * The snapshot is written only when the interval has passed AND new samples
 * arrived since the last save, which keeps flash wear negligible.
 */
void storeTick(uint32_t nowMs) {
    resetButtonTick(nowMs);

    if (s_histDirty && nowMs - s_lastSaveMs >= HIST_SAVE_INTERVAL_S * 1000UL) {
        snapshotSave();
    }
}
