/**
 * @file node_packet.h
 * @brief The packet format sent by the temperature nodes (ESP-NOW payload).
 *
 * Implements: REQ-GEN-06.
 *
 * The block between BEGIN and END is a BYTE-FOR-BYTE copy of the one in
 * temperature_ndoe/temperature_node_src/temperature_node_src.ino. Never edit it
 * here: change the node first, then copy it again. `tools/check_packet.sh`
 * compares both blocks and fails if they differ.
 *
 * The constants after the block (flags, battery states, TEMP_INVALID) are
 * defined in the node sketch OUTSIDE its packet block, so the server repeats
 * them here with the same values.
 *
 * Only <stdint.h> and <stddef.h> are needed, so the host tests can use it too.
 */

#ifndef NODE_PACKET_H
#define NODE_PACKET_H

#include <stdint.h>
#include <stddef.h>

// ===== BEGIN NodePacket — copy unchanged into the receiver project =====
#define PACKET_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t  packet_version;     // Packet format version (starts at 1)
    uint16_t fw_version;         // Firmware version
    uint8_t  node_id;            // Node identifier
    uint32_t boot_count;         // Cycle counter (same in all 3 copies)
    uint8_t  is_charging;        // 0 / 1
    uint16_t battery_mv;         // Battery voltage, mV
    uint8_t  battery_state;      // 0 NORMAL, 1 LOW, 2 EMPTY, 3 INVALID
    uint16_t vbus_mv;            // 5V pin voltage, mV
    uint32_t sleep_time_s;       // Seconds until the next cycle
    int16_t  cpu_temp_cC;        // CPU temperature, 0.01 C, or -32768
    int16_t  ds18b20_temp_cC;    // DS18B20 temperature, 0.01 C, or -32768
    uint8_t  status_flags;       // FLAG_* bit field
    uint32_t crc32;              // esp_rom_crc32_le(0, bytes, 23) of fields above
} NodePacket;

static_assert(sizeof(NodePacket) == 27, "NodePacket size mismatch");
static_assert(sizeof(NodePacket) <= 250, "NodePacket exceeds ESP-NOW limit");
// ===== END NodePacket =====

// ---------------------------------------------------------------------------
// Constants the node defines outside of its packet block (same values!)
// ---------------------------------------------------------------------------

/** "No valid temperature" marker used in both temperature fields. */
#define TEMP_INVALID        ((int16_t)-32768)

/** status_flags bit masks (REQ-PKT-03 of the node). */
#define FLAG_DS_ABSENT      (1 << 0)   // DS18B20 not detected
#define FLAG_DS_ERR         (1 << 1)   // DS18B20 detected but reading invalid
#define FLAG_BAT_INVALID    (1 << 2)   // Battery voltage outside the valid range
#define FLAG_CPU_TEMP_ERR   (1 << 3)   // CPU temperature reading error
#define FLAG_WDT_RESET      (1 << 4)   // Previous node cycle ended in a watchdog reset

/** battery_state values (REQ-BAT-02 of the node). */
#define BAT_STATE_NORMAL    0
#define BAT_STATE_LOW       1
#define BAT_STATE_EMPTY     2
#define BAT_STATE_INVALID   3

#endif  // NODE_PACKET_H
