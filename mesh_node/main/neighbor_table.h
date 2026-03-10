#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================
//
// Each node maintains a list of directly reachable neighbors.
// An entry is created or updated every time we receive a HELLO
// beacon from another node. Entries expire if we haven't heard
// from a neighbor in NEIGHBOR_TIMEOUT_MS milliseconds.
//
// ============================================================

#define NEIGHBOR_TABLE_SIZE   16     // max neighbors we track
#define NEIGHBOR_TIMEOUT_MS   15000  // expire after 15 seconds of silence

// Neighbor entry format
typedef struct {
    uint8_t mac[6];          // neighbor's MAC address
    int8_t rssi;            // signal strength of last received packet
    uint32_t last_seen_ms;    // esp_log_timestamp() when last heard
    bool is_valid;           // is this slot occupied
} neighbor_entry_t;

/**
 * @brief Initialize the neighbor table. Call once at startup.
 */
void neighbor_table_init(void);

/**
 * @brief Update the neighbor table with a new or existing neighbor.
 * 
 * @param mac The MAC address of the neighbor.
 * @param rssi The RSSI value (Received Signal Strength Indicator) of the neighbor.
 */
void neighbor_table_update(const uint8_t mac[6], int8_t rssi);

/**
 * @brief Expire old neighbor entries that have not been seen for a while.
 */
void neighbor_table_expire(void);

/**
 * @brief Print the current neighbor table to serial.
 */
void neighbor_table_print(void);

/**
 * @brief Get a snapshot of current neighbors.
 * 
 * @param out_buf Buffer to fill with neighbor entries (caller-allocated).
 * @param max_entries Maximum number of entries to write to out_buf.
 * @return int The number of neighbor entries written to out_buf.
 */
int neighbor_table_get(neighbor_entry_t *out_buf, int max_entries);
