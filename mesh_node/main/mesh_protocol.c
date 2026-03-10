#include "mesh_protocol.h"
#include "dedup_cache.h"
#include "neighbor_table.h"
#include "routing_table.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_log.h"

static const char *TAG = "mesh_protocol";

#define FMTMAC "%02x:%02x:%02x:%02x:%02x:%02x"
#define ARGMAC(m) (m)[0],(m)[1],(m)[2],(m)[3],(m)[4],(m)[5]

#define HELLO_INTERVAL_MS 5000
#define ROUTE_UPDATE_INTERVAL_MS 8000   // advertise our routing table every 8s

// Our own MAC address, 6 bytes as a mac address is 6 bytes
static uint8_t self_mac[6];

// Counter to prevent duplicate message IDs. In a real implementation, 
//this should probably be randomised and persisted across reboots to avoid collisions, 
// but for this simple example it's just a counter.
static uint16_t msg_id_counter = 0;

// Function pointer for application receive callback, set by main.c
static mesh_recv_cb_t recv_cb = NULL;

static const uint8_t BROADCAST_MAC[6] = MESH_BROADCAST_ADDR;

/**
 * @brief Register the broadcast peer for ESP-NOW communication.
 * 
 */
static void register_broadcast_peer(void)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_ERROR_CHECK(ret);
    }
}

/**
 * @brief Ensure a unicast peer is registered for ESP-NOW communication.
 * 
 * @param mac The MAC address of the peer to register.
 * 
 */
static void ensure_unicast_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "failed to add unicast peer: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Build and broadcast our routing table to neighbors.
 * applies split horizon by excluding routes learned from the neighbor we're sending to.
 * Called periodically in route_update_task, and also on startup after a delay to let neighbors be discovered.
 * this prevents routing loops in distance-vector networks.
 * 
 */
static void send_route_update(void)
{
    // Snapshot current routing table
    route_entry_t routes[ROUTING_TABLE_SIZE];
    int count = routing_table_get(routes, ROUTING_TABLE_SIZE);

    // Also add ourselves as a directly reachable destination at 0 hops
    // so neighbors learn they can reach us.
    mesh_route_entry_t entries[MESH_MAX_ROUTE_ENTRIES];
    int n = 0;

    // Self entry
    memcpy(entries[n].dst_mac, self_mac, 6);
    entries[n].hop_count = 0;
    n++;

    // All known routes
    for (int i = 0; i < count && n < (int)MESH_MAX_ROUTE_ENTRIES; i++) {
        memcpy(entries[n].dst_mac, routes[i].dst_mac, 6);
        entries[n].hop_count = routes[i].hop_count;
        n++;
    }

    uint8_t payload_len = n * sizeof(mesh_route_entry_t);

    mesh_packet_t packet = {0};
    memcpy(packet.src_mac, self_mac, 6);
    memset(packet.dst_mac, 0xFF, 6);
    packet.msg_id      = msg_id_counter++;
    packet.ttl         = 2;   // route updates only need to reach direct neighbors
    packet.msg_type    = MSG_TYPE_ROUTE_UPDATE;
    packet.payload_len = payload_len;
    memcpy(packet.payload, entries, payload_len);

    int send_len = MESH_HEADER_SIZE + payload_len;
    esp_err_t ret = esp_now_send(BROADCAST_MAC, (uint8_t *)&packet, send_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "route update send failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "TX route update with %d entries", n);
    }
}

/**
 * @brief Handle an incoming route update packet.
 * 
 * @param packet The received mesh packet containing route updates.
 */
static void handle_route_update(const mesh_packet_t *packet)
{
    int entry_count = packet->payload_len / sizeof(mesh_route_entry_t);
    if (entry_count == 0) return;

    const mesh_route_entry_t *entries = (const mesh_route_entry_t *)packet->payload;

    for (int i = 0; i < entry_count; i++) {
        const mesh_route_entry_t *e = &entries[i];

        // Don't add a route to ourselves
        if (memcmp(e->dst_mac, self_mac, 6) == 0) continue;

        // Split horizon: don't accept a route if the next hop IS
        // the node we'd be routing through, that's a loop.
        // (We learned this route from packet->src_mac, so if the
        // destination IS packet->src_mac, skip it,  we already know
        // how to reach them directly.)
        if (memcmp(e->dst_mac, packet->src_mac, 6) == 0) {
            // Direct neighbor — always 1 hop via themselves
            routing_table_update(e->dst_mac, packet->src_mac, 1);
            continue;
        }

        // Route to a non-neighbor destination: cost is their hop count + 1
        uint8_t new_cost = e->hop_count + 1;
        routing_table_update(e->dst_mac, packet->src_mac, new_cost);
    }
}

/**
 * @brief Callback for ESP-NOW data sent events. Logs the status of the transmission.
 * 
 * @param tx_info Information about the transmitted data.
 * @param status Status of the transmission.
 */
static void on_data_sent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    ESP_LOGD(TAG, "send status: %s",
             status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

/**
 * @brief Callback for ESP-NOW data received events. Processes incoming mesh packets.
 * 
 * @param recv_info Information about the received data.
 * @param data Pointer to the received data.
 * @param data_len Length of the received data.
 */
static void on_data_recv(const esp_now_recv_info_t *recv_info,
                         const uint8_t *data, int data_len)
{
    if (data_len < (int)MESH_HEADER_SIZE) {
        ESP_LOGW(TAG, "undersized frame (%d bytes), dropping", data_len);
        return;
    }

    const mesh_packet_t *packet = (const mesh_packet_t *)data;

    // Drop our own transmissions
    if (memcmp(packet->src_mac, self_mac, 6) == 0) return;

    // Update neighbor table for the sender
    neighbor_table_update(packet->src_mac, recv_info->rx_ctrl->rssi);

    // Deduplication
    if (dedup_cache_is_duplicate(packet->src_mac, packet->msg_id)) {
        ESP_LOGD(TAG, "duplicate packet id=%u, dropping", packet->msg_id);
        return;
    }

    // Dispatch by message type
    if (packet->msg_type == MSG_TYPE_HELLO) {
        ESP_LOGI(TAG, "HELLO from " FMTMAC " (rssi=%d dBm)",
                 ARGMAC(packet->src_mac), recv_info->rx_ctrl->rssi);
        // HELLO packets flood normally so far-away nodes discover
        // each other through relays, but don't go to app layer.

    } else if (packet->msg_type == MSG_TYPE_ROUTE_UPDATE) {
        handle_route_update(packet);
        // Route updates flood to TTL=2 (set at send time), handled below.

    } else if (packet->msg_type == MSG_TYPE_UNICAST) {
        // Is this packet addressed to us?
        if (memcmp(packet->dst_mac, self_mac, 6) == 0) {
            ESP_LOGI(TAG, "UNICAST RX id=%u from " FMTMAC,
                     packet->msg_id, ARGMAC(packet->src_mac));
            if (recv_cb) recv_cb(packet);
            return;  // don't forward packets addressed to us
        }

        // Not for us — forward toward destination using routing table
        uint8_t next_hop[6];
        if (routing_table_lookup(packet->dst_mac, next_hop)) {
            ensure_unicast_peer(next_hop);
            mesh_packet_t fwd = *packet;
            fwd.ttl--;
            if (fwd.ttl == 0) {
                ESP_LOGW(TAG, "unicast TTL exhausted, dropping");
                return;
            }
            int send_len = MESH_HEADER_SIZE + fwd.payload_len;
            esp_now_send(next_hop, (uint8_t *)&fwd, send_len);
            ESP_LOGD(TAG, "forwarded unicast toward " FMTMAC " via " FMTMAC,
                     ARGMAC(packet->dst_mac), ARGMAC(next_hop));
        } else {
            ESP_LOGW(TAG, "no route to " FMTMAC ", dropping unicast",
                     ARGMAC(packet->dst_mac));
        }
        return;  // unicast never floods

    } else {
        // MSG_TYPE_DATA — broadcast data, deliver to app layer
        ESP_LOGI(TAG, "RX id=%u ttl=%u len=%u from " FMTMAC " (rssi=%d dBm)",
                 packet->msg_id, packet->ttl, packet->payload_len,
                 ARGMAC(packet->src_mac), recv_info->rx_ctrl->rssi);
        if (recv_cb) recv_cb(packet);
    }

    // Flooding relay for broadcast packet types
    // (DATA, HELLO, ROUTE_UPDATE — not UNICAST)
    if (packet->ttl > 1) {
        mesh_packet_t relay = *packet;
        relay.ttl--;
        int send_len = MESH_HEADER_SIZE + relay.payload_len;
        esp_err_t ret = esp_now_send(BROADCAST_MAC, (uint8_t *)&relay, send_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "relay failed: %s", esp_err_to_name(ret));
        }
    }
}

/**
 * @brief Task to periodically broadcast a HELLO message to announce our presence to neighbors.
 * 
 * @param arg Unused.
 */
static void hello_task(void *arg)
{
    while(true) {
        mesh_packet_t packet = {0};
        memcpy(packet.src_mac, self_mac, 6);
        memset(packet.dst_mac, 0xFF, 6);
        packet.msg_id = msg_id_counter++;
        packet.ttl = MESH_TTL_DEFAULT;
        packet.msg_type = MSG_TYPE_HELLO;
        packet.payload_len = 0;
        esp_now_send(BROADCAST_MAC, (uint8_t *)&packet, MESH_HEADER_SIZE);
        vTaskDelay(pdMS_TO_TICKS(HELLO_INTERVAL_MS));
    }
}

/**
 * @brief Task to periodically broadcast route updates to neighbors.
 * 
 * @param arg Unused.
 */
static void route_update_task(void *arg)
{
    // Wait a bit on startup so we have neighbors before advertising
    vTaskDelay(pdMS_TO_TICKS(3000));
    while(true) {
        send_route_update();
        routing_table_expire();
        vTaskDelay(pdMS_TO_TICKS(ROUTE_UPDATE_INTERVAL_MS));
    }
}

void mesh_protocol_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // We use Wi-Fi and ESP-NOW in station mode, but we don't actually connect to an AP.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    // Get our MAC address for later use in packets and logging
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, self_mac));
    ESP_LOGI(TAG, "node MAC: " FMTMAC, ARGMAC(self_mac));

    // ESP-NOW initialization
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // Register broadcast peer and initialize modules
    register_broadcast_peer();
    dedup_cache_init();
    neighbor_table_init();
    routing_table_init();

    xTaskCreate(hello_task, "hello", 2048, NULL, 4, NULL);
    xTaskCreate(route_update_task, "route_update", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "mesh_protocol ready");
}

esp_err_t mesh_send_broadcast(const uint8_t *data, uint8_t data_len)
{
    if (data_len > MESH_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    mesh_packet_t packet = {0};
    // Copy MAC address (6 bytes)
    memcpy(packet.src_mac, self_mac, 6);
    // Broadcast packets have a destination MAC of FF:FF:FF:FF:FF:FF
    memset(packet.dst_mac, 0xFF, 6);
    packet.msg_id = msg_id_counter++;
    packet.ttl = MESH_TTL_DEFAULT;
    packet.msg_type = MSG_TYPE_DATA;
    packet.payload_len = data_len;
    memcpy(packet.payload, data, data_len);

    ESP_LOGI(TAG, "TX broadcast id=%u ttl=%u len=%u", packet.msg_id, packet.ttl, data_len);
    return esp_now_send(BROADCAST_MAC, (uint8_t *)&packet, MESH_HEADER_SIZE + data_len);
}

esp_err_t mesh_send_unicast(const uint8_t dst_mac[6],
                            const uint8_t *data, uint8_t data_len)
{
    if (data_len > MESH_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    // Look up next hop from routing table as we don't send data directly to the destination, 
    // we send to the next hop toward the destination.
    uint8_t next_hop[6];
    if (!routing_table_lookup(dst_mac, next_hop)) {
        ESP_LOGW(TAG, "no route to " FMTMAC, ARGMAC(dst_mac));
        return ESP_ERR_NOT_FOUND;
    }

    ensure_unicast_peer(next_hop);

    mesh_packet_t packet = {0};
    memcpy(packet.src_mac, self_mac, 6);
    memcpy(packet.dst_mac, dst_mac, 6);
    packet.msg_id = msg_id_counter++;
    packet.ttl = MESH_TTL_DEFAULT;
    packet.msg_type = MSG_TYPE_UNICAST;
    packet.payload_len = data_len;
    memcpy(packet.payload, data, data_len);

    ESP_LOGI(TAG, "TX unicast to " FMTMAC " via " FMTMAC " id=%u",
             ARGMAC(dst_mac), ARGMAC(next_hop), packet.msg_id);

    return esp_now_send(next_hop, (uint8_t *)&packet, MESH_HEADER_SIZE + data_len);
}

void mesh_set_recv_callback(mesh_recv_cb_t cb)
{
    recv_cb = cb;
}
