#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_now.h"

#define MESH_BROADCAST_ADDR { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
#define MESH_PAYLOAD_MAX 200
#define MESH_TTL_DEFAULT 7

#define ACK_MAX_ATTEMPTS 3
#define ACK_TIMEOUT_MS 500

typedef enum {
    MSG_TYPE_DATA = 0x01,           // user payload, broadcast
    MSG_TYPE_HELLO = 0x02,          // neighbor discovery beacon
    MSG_TYPE_ACK = 0x03,            // acknowledge packet receipt, unicast back to sender
    MSG_TYPE_ROUTE_UPDATE = 0x04,   // distance-vector route advertisement
    MSG_TYPE_UNICAST = 0x05,        // addressed to a specific destination
} mesh_msg_type_t;

// this is the actual packet we are sending. packed so that there is no padding between fields
// ensuring the byte layout is exactly as we expect when we read/write raw bytes from esp_now
// as it expects each packet to be the same format, padding would corrupt the data.
typedef struct __attribute__((packed)) {
    uint8_t src_mac[6];
    uint8_t dst_mac[6];    // FF:FF:... = broadcast, last 3 bytes of MAC = unicast
    uint16_t msg_id;
    uint8_t ttl;
    uint8_t msg_type;
    uint8_t payload_len;
    uint8_t payload[MESH_PAYLOAD_MAX];
} mesh_packet_t;

#define MESH_HEADER_SIZE  (sizeof(mesh_packet_t) - MESH_PAYLOAD_MAX)

// This is the route entry format we use in the payload of MSG_TYPE_ROUTE_UPDATE packets.
typedef struct __attribute__((packed)) {
    uint8_t dst_mac[6];
    uint8_t hop_count;
} mesh_route_entry_t;

// Pending ACK entry for tracking unicast message deliveries and retransmissions
typedef struct {
    mesh_packet_t packet;          // full copy of packet for retransmission
    uint8_t next_hop[6];  // resolved next hop at send time
    uint32_t sent_at_ms;   // timestamp of last transmission
    uint8_t attempts;     // number of times sent so far
    bool active;
} pending_ack_entry_t;


// Pending ACK entry for tracking unicast message deliveries and retransmissions
#define PENDING_ACK_TABLE_SIZE  16

#define MESH_MAX_ROUTE_ENTRIES  (MESH_PAYLOAD_MAX / sizeof(mesh_route_entry_t))

/**
 * @brief Initialize the mesh protocol.
 * 
 */
void mesh_protocol_init(void);

/**
 * @brief Broadcast a DATA packet to all nodes in the mesh network.
 * 
 * @param data Pointer to the data to be sent.
 * @param data_len Length of the data to be sent.
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t mesh_send_broadcast(const uint8_t *data, uint8_t data_len);

/**
 * @brief Send a UNICAST packet to a specific destination MAC.
 * 
 * @param dst_mac The MAC address of the destination node.
 * @param data Pointer to the data to be sent as an array of bytes (since the network layer is unaware of our packet structure).
 * @param data_len Length of the data to be sent.
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t mesh_send_unicast(const uint8_t dst_mac[6],
                            const uint8_t *data, uint8_t data_len);

/**
 * @brief Set a callback function to be called when a mesh packet is received.
 * 
 * @param cb The callback function to be called.
 */
typedef void (*mesh_recv_cb_t)(const mesh_packet_t *pkt);

/**
 * @brief Set a callback function to be called when a mesh packet is received.
 * 
 * @param cb The callback function to be called.
 */
void mesh_set_recv_callback(mesh_recv_cb_t cb);

/**
 * @brief Set a callback function to be called when a mesh packet delivery result is available.
 * 
 */
typedef void (*mesh_delivery_cb_t)(uint16_t msg_id, const uint8_t dst_mac[6], bool success);

/**
 * @brief Set a callback function to be called when a mesh packet delivery result is available.
 * 
 * @param cb The callback function to be called.
 */
void mesh_set_delivery_callback(mesh_delivery_cb_t cb);
