/**
 * @file store.h
 * @brief Persistent storage: settings in NVS and the history snapshot in LittleFS.
 *
 * Implements: REQ-PERS-01 .. REQ-PERS-06, REQ-ERR-01 (storage failures are
 * not fatal).
 *
 * Two kinds of data are stored:
 *   - SETTINGS (small, change rarely): Wi-Fi credentials, last router channel,
 *     PIN, room name and the per-node settings. They are kept in NVS through
 *     the `Preferences` API and saved immediately when they change.
 *   - HISTORY (bigger, changes with every packet): saved as one snapshot file
 *     on LittleFS once per HIST_SAVE_INTERVAL_S, and only if something changed.
 */

#ifndef STORE_H
#define STORE_H

#include <Arduino.h>

/** Settings shared by several modules. Loaded once at boot, saved by settingsSave(). */
struct Settings {
    char    ssid[33];          // router name (empty = no credentials)
    char    pass[65];          // router password (never sent to the web UI, REQ-SEC-03)
    uint8_t lastChannel;       // last router channel that worked (REQ-NET-04)
    char    pin[9];            // settings PIN, 4-8 digits
    char    room[25];          // room name shown in the page header
};

extern Settings g_settings;

/** Open NVS, load the settings and mount LittleFS. Failures are logged, never fatal. */
void storeInit();

/** Create the node slots saved in NVS (call after nodesInit()). */
void storeLoadNodes();

/** Save g_settings to NVS. */
void settingsSave();

/** Save the user settings of one node slot (or remove them if the slot is free). */
void storeSaveNode(int idx);

/** Tell the store that new history samples exist, so the next hourly save is not skipped. */
void storeMarkHistoryDirty();

/** Restore the history from the snapshot file (call after storeLoadNodes()). */
void snapshotLoad();

/**
 * Write the history snapshot now (atomic: temp file, verify, rename).
 * @return true on success; false if storage is unavailable or the write failed.
 */
bool snapshotSave();

/** Periodic work: hourly snapshot and the BOOT-button factory reset. Call from loop(). */
void storeTick(uint32_t nowMs);

/** @return true if the PIN is still the factory default (the UI warns about it). */
bool storePinIsDefault();

/** Erase history of all nodes (RAM and the next snapshot). */
void storeClearHistory();

/** Erase every setting and the snapshot, then restart. Never returns. */
void storeFactoryReset();

#endif  // STORE_H
