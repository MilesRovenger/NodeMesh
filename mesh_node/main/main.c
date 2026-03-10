#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "mesh_protocol.h"
#include "neighbor_table.h"
#include "routing_table.h"
#include "oled.h"

static const char *TAG = "main";

#define FMTMAC "%02x:%02x:%02x:%02x:%02x:%02x"
#define ARGMAC(m) (m)[0],(m)[1],(m)[2],(m)[3],(m)[4],(m)[5]

// my OLED displays max chars is 21 characters + null terminator (22 bytes), so we limit messages to 21 chars for display purposes
static char last_msg[22] = "waiting...";
// Store the source MAC of the last received message for display purposes (MAC is 6 bytes)
static uint8_t last_src[6] = {0};

static void on_mesh_recv(const mesh_packet_t *packet)
{
    char buf[MESH_PAYLOAD_MAX + 1];

    // Copy payload to buf and ensure size is safe for printing as payload max is 200 (ensures not corrupted)
    uint8_t payload_len = packet->payload_len < MESH_PAYLOAD_MAX ? packet->payload_len : MESH_PAYLOAD_MAX;
    memcpy(buf, packet->payload, payload_len);
    buf[payload_len] = '\0';

    // if its not unicast we can assume its a broadcast
    const char *type = (packet->msg_type == MSG_TYPE_UNICAST) ? "UNICAST" : "BCAST";
    ESP_LOGI(TAG, "APP RX [%s] from " FMTMAC " | \"%s\"",
             type, ARGMAC(packet->src_mac), buf);

    snprintf(last_msg, sizeof(last_msg), "%.21s", buf);

    // Update OLED display with last message and source MAC
    memcpy(last_src, packet->src_mac, 6);
}

/**
 * @brief Resolve a MAC address suffix to a full MAC address. Uses the routing table to find a matching destination MAC.
 * linear scan is fine since the routing table is small and this is only used for user input commands.
 * 
 * @param suffix The last 3 bytes of the MAC address in the format "xx:xx:xx".
 * @param out_mac The full MAC address will be written here if found.
 * @return true If a matching MAC address is found.
 * @return false If no matching MAC address is found.
 */
static bool resolve_mac_suffix(const char *suffix, uint8_t out_mac[6])
{
    // Parse the 3 suffix bytes
    uint8_t s[3];
    if (sscanf(suffix, "%hhx:%hhx:%hhx", &s[0], &s[1], &s[2]) != 3) {
        return false;
    }

    route_entry_t routes[ROUTING_TABLE_SIZE];
    int count = routing_table_get(routes, ROUTING_TABLE_SIZE);

    for (int i = 0; i < count; i++) {
        uint8_t *m = routes[i].dst_mac;
        if (m[3] == s[0] && m[4] == s[1] && m[5] == s[2]) {
            memcpy(out_mac, m, 6);
            return true;
        }
    }
    return false;
}

// ============================================================
// Input commands
//
// Commands (type in the ESP-IDF serial monitor):
//
//   b <message>
//     Broadcast message to all nodes.
//     Example: b hello everyone
//
//   u <xx:xx:xx> <message>
//     Unicast message to node matching last 3 MAC bytes. resolve_mac_suffix looks up the routing table for a 
//     destination MAC whose last 3 bytes match the provided suffix, and sends the unicast message to that MAC if found.
//     Example: u b0:cb:d8 ping
//
//   nodes
//     Print neighbor table and routing table.
// ============================================================

/**
 * @brief Task to handle serial input commands.
 * 
 * @param arg Unused.
 */
static void serial_input_task(void *arg)
{
    char line[128];
    int  pos = 0;

    // Print usage once on boot
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("\n=== MESH CONSOLE ===\n");
    printf("  b <msg>            broadcast\n");
    printf("  u <xx:xx:xx> <msg> unicast (last 3 MAC bytes)\n");
    printf("  nodes              show topology\n");
    printf("====================\n\n");

    while(true) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Echo character back so you can see what you're typing
        putchar(c);
        fflush(stdout);

        if (c == '\n' || c == '\r') {
            line[pos] = '\0';
            pos = 0;

            if (strlen(line) == 0) continue;

            // parse command
            if (strncmp(line, "b ", 2) == 0) {
                // Broadcast message
                const char *msg = line + 2;
                // Send broadcast and print result
                esp_err_t err = mesh_send_broadcast(
                    (uint8_t *)msg, (uint8_t)strlen(msg));
                if (err == ESP_OK) {
                    printf("[TX BCAST] \"%s\"\n", msg);
                } else {
                    printf("[ERROR] broadcast failed: %s\n",
                           esp_err_to_name(err));
                }

            } else if (strncmp(line, "u ", 2) == 0) {
                // Unicast: u xx:xx:xx message
                char suffix[12];
                char msg[MESH_PAYLOAD_MAX];

                // Parse the suffix and message from the input line
                int parsed = sscanf(line + 2, "%11s %[^\n]", suffix, msg);
                if (parsed < 2) {
                    printf("[ERROR] usage: u <xx:xx:xx> <message>\n");
                    continue;
                }

                // Resolve suffix to full MAC address using routing table lookup
                uint8_t dst_mac[6];
                if (!resolve_mac_suffix(suffix, dst_mac)) {
                    printf("[ERROR] no route to %s — known nodes:\n", suffix);
                    routing_table_print();
                    continue;
                }

                // Networks transmit raw bytes and dont know our packet structure, so we need to cast our message to uint8_t* and specify the length as uint8_t
                esp_err_t err = mesh_send_unicast(
                    dst_mac, (uint8_t *)msg, (uint8_t)strlen(msg));
                if (err == ESP_OK) {
                    printf("[TX UNICAST] to %s: \"%s\"\n", suffix, msg);
                } else if (err == ESP_ERR_NOT_FOUND) {
                    printf("[ERROR] no route to %s\n", suffix);
                } else {
                    printf("[ERROR] %s\n", esp_err_to_name(err));
                }

            } else if (strcmp(line, "nodes") == 0) {
                neighbor_table_print();
                routing_table_print();

            } else {
                printf("[ERROR] unknown command: \"%s\"\n", line);
                printf("  b <msg> | u <xx:xx:xx> <msg> | nodes\n");
            }

        } else {
            // Accumulate character
            if (pos < (int)sizeof(line) - 1) {
                line[pos++] = (char)c;
            }
        }
    }
}


/**
 * @brief Task to update the OLED display with mesh node information.
 * 
 * @param arg Unused.
 */
static void display_task(void *arg)
{
    uint8_t my_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);

    while(true) {
        neighbor_entry_t neighbors[NEIGHBOR_TABLE_SIZE];
        int nbr_count = neighbor_table_get(neighbors, NEIGHBOR_TABLE_SIZE);

        route_entry_t routes[ROUTING_TABLE_SIZE];
        int rte_count = routing_table_get(routes, ROUTING_TABLE_SIZE);

        char line[22];

        oled_clear();

        oled_write_string(0, 0, "MY ADDRESS (3 bytes)");

        snprintf(line, sizeof(line), "MAC:%02x:%02x:%02x",
                 my_mac[3], my_mac[4], my_mac[5]);
        oled_write_string(1, 0, line);

        // some cool padding formatting :]
        snprintf(line, sizeof(line), "NGHBR:%-2d  ROUTES:%-2d",
                 nbr_count, rte_count);
        oled_write_string(3, 0, line);

        // Show up to 2 neighbors due to space constraints, showing their MAC suffix and RSSI
        for (int i = 0; i < nbr_count && i < 2; i++) {
            snprintf(line, sizeof(line), "%02x:%02x:%02x %4ddBm",
                     neighbors[i].mac[3],
                     neighbors[i].mac[4],
                     neighbors[i].mac[5],
                     neighbors[i].rssi);
            oled_write_string(4 + i, 0, line);
        }

        oled_write_string(6, 0, "RX:");
        oled_write_string(6, 20, last_msg);

        if (last_src[0] != 0) {
            snprintf(line, sizeof(line), "   %02x:%02x:%02x",
                     last_src[3], last_src[4], last_src[5]);
            oled_write_string(7, 0, line);
        }

        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/**
 * @brief every 30s, print the current neighbor and routing tables to the console for debugging and visualization of the mesh topology.
 * 
 * @param arg Unused.
 */
static void topology_task(void *arg)
{
    while(true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        neighbor_table_expire();
        neighbor_table_print();
        routing_table_print();
    }
}

/**
 * @brief Entry point of the application.
 * 
 */
void app_main(void)
{
    // Initialize NVS flash, required for WiFi and ESP-NOW
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize the mesh protocol, which sets up Wi-Fi, ESP-NOW, and the routing/neighbor tables
    mesh_protocol_init();
    // Set our callback for when we recieve a mesh message, this will be called from the mesh_protocol on_mesh_recv function when a message is received and identified as a user payload (not a HELLO or ROUTE_UPDATE)
    mesh_set_recv_callback(on_mesh_recv);

    if (oled_init() != ESP_OK) {
        ESP_LOGW(TAG, "OLED not found, continuing without display");
    }

    // Create tasks for handling serial input, updating the OLED display, and printing topology information periodically
    xTaskCreate(serial_input_task, "serial", 4096, NULL, 1, NULL);
    // Creates task to update OLED display with mesh information and last received message
    xTaskCreate(display_task, "display", 4096, NULL, 1, NULL);
    // Creates task to print neighbor and routing tables every 30s for visualization of mesh topology, also calls neighbor_table_expire to remove stale neighbors before printing
    xTaskCreate(topology_task, "topology", 4096, NULL, 1, NULL);

    ESP_LOGI(TAG, "Stage 6 running — type 'nodes' in monitor to see topology");
}