#include "routing_table.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "routing_table";

#define FMTMAC "%02x:%02x:%02x:%02x:%02x:%02x"
#define ARGMAC(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

static route_entry_t route_table[ROUTING_TABLE_SIZE];
static SemaphoreHandle_t mutex = NULL;

/**
 * @brief returns current time in milliseconds since boot.
 *
 * @return uint32_t
 */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief finds the index of a route for a given destination MAC address.
 *
 * @param dst_mac destination MAC address
 * @return int index of the route in the routing table, or -1 if not found
 */
static int find_route(const uint8_t dst_mac[6])
{
    for (int i = 0; i < ROUTING_TABLE_SIZE; i++)
    {
        if (route_table[i].is_valid &&
            memcmp(route_table[i].dst_mac, dst_mac, 6) == 0)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief finds an empty slot in the routing table.
 *
 * @return int index of the empty slot, or -1 if the table is full
 */
static int find_empty_slot(void)
{
    for (int i = 0; i < ROUTING_TABLE_SIZE; i++)
    {
        if (!route_table[i].is_valid)
            return i;
    }
    return -1;
}

void routing_table_init(void)
{
    memset(route_table, 0, sizeof(route_table));
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex != NULL);
    ESP_LOGI(TAG, "routing table initialised (%d slots)", ROUTING_TABLE_SIZE);
}

void routing_table_update(const uint8_t dst_mac[6],
                          const uint8_t next_hop[6],
                          uint8_t hop_count)
{
    if (hop_count >= ROUTE_MAX_HOPS)
    {
        return; // discard unreachable routes
    }

    xSemaphoreTake(mutex, portMAX_DELAY);

    int idx = find_route(dst_mac);

    if (idx >= 0)
    {
        if (hop_count < route_table[idx].hop_count)
        {
            // Better route found, update existing entry
            memcpy(route_table[idx].next_hop_mac, next_hop, 6);
            route_table[idx].hop_count = hop_count;
            route_table[idx].last_updated_ms = now_ms();
            ESP_LOGI(TAG, "improved route: dst=" FMTMAC " hops %u->%u via " FMTMAC,
                     ARGMAC(dst_mac), route_table[idx].hop_count, hop_count, ARGMAC(next_hop));
        }
        else if (hop_count == route_table[idx].hop_count &&
                 memcmp(route_table[idx].next_hop_mac, next_hop, 6) == 0)
        {
            // Same route, same cost — just refresh timestamp
            route_table[idx].last_updated_ms = now_ms();
        }
    }
    else
    {
        // New destination — find an empty slot.
        idx = find_empty_slot();
        if (idx >= 0)
        {
            memcpy(route_table[idx].dst_mac, dst_mac, 6);
            memcpy(route_table[idx].next_hop_mac, next_hop, 6);
            route_table[idx].hop_count = hop_count;
            route_table[idx].last_updated_ms = now_ms();
            route_table[idx].is_valid = true;

            ESP_LOGI(TAG, "new route: dst=" FMTMAC " via=" FMTMAC " hops=%u",
                     ARGMAC(dst_mac), ARGMAC(next_hop), hop_count);
        }
        else
        {
            ESP_LOGW(TAG, "routing table full");
        }
    }

    xSemaphoreGive(mutex);
}

bool routing_table_lookup(const uint8_t dst_mac[6], uint8_t out_next_hop[6])
{
    bool found = false;

    xSemaphoreTake(mutex, portMAX_DELAY);

    int idx = find_route(dst_mac);
    if (idx >= 0)
    {
        memcpy(out_next_hop, route_table[idx].next_hop_mac, 6);
        found = true;
    }

    xSemaphoreGive(mutex);
    return found;
}

void routing_table_expire(void)
{
    uint32_t now = now_ms();

    xSemaphoreTake(mutex, portMAX_DELAY);

    for (int i = 0; i < ROUTING_TABLE_SIZE; i++)
    {
        if (route_table[i].is_valid)
        {
            uint32_t age = now - route_table[i].last_updated_ms;
            if (age > ROUTE_TIMEOUT_MS)
            {
                ESP_LOGI(TAG, "route expired: dst=" FMTMAC " (age=%lums)",
                         ARGMAC(route_table[i].dst_mac), (unsigned long)age);
                memset(&route_table[i], 0, sizeof(route_entry_t));
            }
        }
    }

    xSemaphoreGive(mutex);
}

void routing_table_print(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);

    int count = 0;
    ESP_LOGI(TAG, "--- routing table ---");

    for (int i = 0; i < ROUTING_TABLE_SIZE; i++)
    {
        if (route_table[i].is_valid)
        {
            ESP_LOGI(TAG, "  [%d] dst=" FMTMAC "  via=" FMTMAC "  hops=%u",
                     count,
                     ARGMAC(route_table[i].dst_mac),
                     ARGMAC(route_table[i].next_hop_mac),
                     route_table[i].hop_count);
            count++;
        }
    }

    if (count == 0)
        ESP_LOGI(TAG, "  (no routes)");
    ESP_LOGI(TAG, "--- %d route(s) ---", count);

    xSemaphoreGive(mutex);
}

int routing_table_get(route_entry_t *out_buf, int max_entries)
{
    int count = 0;

    xSemaphoreTake(mutex, portMAX_DELAY);

    for (int i = 0; i < ROUTING_TABLE_SIZE && count < max_entries; i++)
    {
        if (route_table[i].is_valid)
        {
            out_buf[count++] = route_table[i];
        }
    }

    xSemaphoreGive(mutex);
    return count;
}
