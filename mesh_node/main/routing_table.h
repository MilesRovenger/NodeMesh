#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================
//
// Maps destination MACs to next-hop MACs and hop counts.
// Updated by processing MSG_TYPE_ROUTE_UPDATE packets from
// neighbors. Used by mesh_protocol to make forwarding decisions
// instead of flooding everything.
//
// ============================================================

#define ROUTING_TABLE_SIZE   32
#define ROUTE_TIMEOUT_MS     30000   // expire routes not updated in 30s
#define ROUTE_MAX_HOPS       16      // drop routes with hop count >= this

// Route entry format. Packed to ensure no padding bytes and exact layout in memory.
typedef struct {
    uint8_t  dst_mac[6];       // destination node
    uint8_t  next_hop_mac[6];  // neighbor to send toward
    uint8_t  hop_count;        // total hops to destination
    uint32_t last_updated_ms;  // timestamp for expiry
    bool     is_valid;
} route_entry_t;

/**
 * @brief  Initialises the routing table.
 */
void routing_table_init(void);

/**
 * @brief  Add or update a route.
 *         If a route to dst already exists with a higher hop count,
 *         it is replaced. Lower or equal hop counts are ignored.
 *
 * @param  dst_mac Destination MAC.
 * @param  next_hop Neighbor to forward toward.
 * @param  hop_count Total hops to destination.
 */
void routing_table_update(const uint8_t dst_mac[6],
                          const uint8_t next_hop[6],
                          uint8_t hop_count);

/**
 * @brief  Look up the next hop for a destination.
 *
 * @param  dst_mac       Destination MAC to look up.
 * @param  out_next_hop  Output buffer (6 bytes) for next hop MAC.
 * @return true if a route was found, false if destination unknown.
 */
bool routing_table_lookup(const uint8_t dst_mac[6], uint8_t out_next_hop[6]);

/**
 * @brief  Remove routes that haven't been updated in ROUTE_TIMEOUT_MS.
 */
void routing_table_expire(void);

/**
 * @brief  Print the routing table to serial.
 */
void routing_table_print(void);

/**
 * @brief  Get a snapshot of all valid routes.
 * @return Number of entries copied.
 */
int routing_table_get(route_entry_t *out_buf, int max_entries);
