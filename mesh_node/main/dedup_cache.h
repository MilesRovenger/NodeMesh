#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================
//
// Flooding means every node rebroadcasts every packet it hears.
// Without dedup, a 3-node ring would loop packets forever.
//
// We track (src_mac, msg_id) pairs in a fixed-size ring buffer.
//
// ============================================================

#define DEDUP_CACHE_SIZE  32   // slots; tune based on RAM budget

/**
 * @brief  Initialise the dedup cache. Call once at startup.
 */
void dedup_cache_init(void);

/**
 * @brief  Check whether we have already seen this (src, id) pair.
 *         If not seen, adds it to the cache and returns false.
 *         If already seen, returns true (caller should drop packet).
 *
 * @param  src_mac  6-byte source MAC address.
 * @param  msg_id   16-bit message ID from the packet header.
 * @return true  → duplicate, drop it.
 *         false → first time we've seen this, process and forward.
 */
bool dedup_cache_is_duplicate(const uint8_t src_mac[6], uint16_t msg_id);
