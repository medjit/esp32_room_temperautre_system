/**
 * @file test_sender.ino
 * @brief Bench-test tool: pretends to be several temperature nodes (NOT a deliverable).
 *
 * Flash this on a spare XIAO ESP32C6 and watch the server. It broadcasts the same
 * 27-byte NodePacket as the real nodes, three copies per cycle, and sometimes
 * deliberately broken frames so the server's counters can be checked
 * (plan test cases T-2.x, T-7.x).
 *
 * Emulated nodes (ids 1..4), all sending every ROUND_S seconds:
 *   node 1  healthy, DS18B20 value
 *   node 2  LOW battery (sleep announced as 3 x ROUND_S)
 *   node 3  no DS18B20 -> the server shows an ESTIMATED temperature, charging
 *   node 4  healthy, but goes silent after SILENT_AFTER_ROUND rounds -> must turn STUCK
 *
 * The announced sleep time is ROUND_S, so with the default 20 s a node is flagged
 * "stuck" after 30 s (STUCK_FACTOR 1.5) instead of 22.5 minutes: fast tests.
 *
 * Board: XIAO_ESP32C6. The channel must match the server's radio channel.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_rom_crc.h>

#include "../../temperature_server_src/node_packet.h"   // the SAME packet definition as the server

// ---- test settings -------------------------------------------------------
#define CHANNEL              1      // must equal the server's channel
#define ROUND_S              20     // seconds between rounds (and the announced sleep time)
#define SILENT_AFTER_ROUND   6      // node 4 stops after this many rounds (0 = never)
#define SEND_BAD_FRAMES      1      // 1 = every 5th round also send broken frames
// --------------------------------------------------------------------------

static uint32_t g_round = 0;
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/** Fill the CRC field of a packet. */
static void sign(NodePacket &p) {
    p.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&p, offsetof(NodePacket, crc32));
}

/** Build the packet of emulated node `id` for the current round. */
static NodePacket makePacket(uint8_t id) {
    NodePacket p;
    memset(&p, 0, sizeof(p));
    const float wave = sinf((float)g_round / 6.0f + id);

    p.packet_version   = PACKET_VERSION;
    p.fw_version       = 99;                           // visibly "not a real node"
    p.node_id          = id;
    p.boot_count       = g_round + 1;
    p.battery_mv       = 3950 - id * 20;
    p.battery_state    = BAT_STATE_NORMAL;
    p.sleep_time_s     = ROUND_S;
    p.cpu_temp_cC      = (int16_t)(2850 + id * 40 + wave * 60);
    p.ds18b20_temp_cC  = (int16_t)(2100 + id * 50 + wave * 40);

    if (id == 2) { p.battery_mv = 3200; p.battery_state = BAT_STATE_LOW; p.sleep_time_s = 3 * ROUND_S; }
    if (id == 3) { p.ds18b20_temp_cC = TEMP_INVALID; p.status_flags |= FLAG_DS_ABSENT;
                   p.is_charging = 1; p.vbus_mv = 4950; }
    sign(p);
    return p;
}

/** Send one frame three times, 10 ms apart, like a real node. */
static void sendCopies(const void *data, size_t len) {
    for (int i = 0; i < 3; i++) {
        esp_now_send(kBroadcast, (const uint8_t *)data, len);
        delay(10);
    }
}

/** Broken frames: bad CRC, unknown version (with valid CRC), wrong length. */
static void sendBadFrames() {
    NodePacket bad = makePacket(1);
    bad.battery_mv ^= 0x0101;                          // damaged after signing
    esp_now_send(kBroadcast, (const uint8_t *)&bad, sizeof(bad));
    delay(10);

    NodePacket ver = makePacket(1);
    ver.packet_version = 2;
    sign(ver);
    esp_now_send(kBroadcast, (const uint8_t *)&ver, sizeof(ver));
    delay(10);

    NodePacket good = makePacket(1);
    esp_now_send(kBroadcast, (const uint8_t *)&good, sizeof(good) - 1);   // one byte short
    delay(10);
}

void setup() {
    pinMode(3, OUTPUT);  digitalWrite(3, LOW);         // RF switch on
    pinMode(14, OUTPUT); digitalWrite(14, LOW);        // on-board antenna

    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) { Serial.println("esp_now_init failed"); return; }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcast, 6);
    peer.channel = CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    esp_now_add_peer(&peer);
    Serial.printf("test sender ready on channel %d, round every %d s\n", CHANNEL, ROUND_S);
}

void loop() {
    for (uint8_t id = 1; id <= 4; id++) {
        if (id == 4 && SILENT_AFTER_ROUND && g_round >= SILENT_AFTER_ROUND) continue;   // goes quiet
        NodePacket p = makePacket(id);
        sendCopies(&p, sizeof(p));
    }
#if SEND_BAD_FRAMES
    if (g_round % 5 == 4) sendBadFrames();
#endif
    Serial.printf("round %lu sent\n", (unsigned long)g_round);
    g_round++;
    delay(ROUND_S * 1000UL);
}
