#include "neighbor_table.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "neighbor_table";

// Format for printing MAC addresses in logs
#define FMTMAC "%02x:%02x:%02x:%02x:%02x:%02x"
// Helper macro to pass a MAC address to FMTMAC
#define ARGMAC(m) (m)[0],(m)[1],(m)[2],(m)[3],(m)[4],(m)[5]

static neighbor_entry_t neighbor_table[NEIGHBOR_TABLE_SIZE];
static SemaphoreHandle_t mutex = NULL;

/**
 * @brief Returns current time in milliseconds since boot.
 * 
 * @return uint32_t 
 */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Find the index of an existing neighbor entry for the given MAC address.
 * 
 * @param mac The MAC address to search for.
 * @return int The index of the neighbor entry, or -1 if not found.
 */
static int find_entry(const uint8_t mac[6])
{
    for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        if (neighbor_table[i].is_valid && memcmp(neighbor_table[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Find an empty slot, or -1 if neighbor_table is full.
 * 
 * @return int The index of the empty slot, or -1 if the table is full.
 */
static int find_empty_slot(void)
{
    for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        if (!neighbor_table[i].is_valid) {
            return i;
        }
    }
    return -1;
}

void neighbor_table_init(void)
{
    memset(neighbor_table, 0, sizeof(neighbor_table));
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex != NULL);
    ESP_LOGI(TAG, "neighbor table initialised (%d slots)", NEIGHBOR_TABLE_SIZE);
}

void neighbor_table_update(const uint8_t mac[6], int8_t rssi)
{
    xSemaphoreTake(mutex, portMAX_DELAY);

    int idx = find_entry(mac);

    if (idx >= 0) {
        // Neighbor found, update RSSI and timestamp.
        neighbor_table[idx].rssi = rssi;
        neighbor_table[idx].last_seen_ms = now_ms();
    } else {
        // New neighbor, find an empty slot.
        idx = find_empty_slot();
        if (idx >= 0) {
            // Insert new neighbor entry.
            memcpy(neighbor_table[idx].mac, mac, 6);
            neighbor_table[idx].rssi = rssi;
            neighbor_table[idx].last_seen_ms = now_ms();
            neighbor_table[idx].is_valid = true;
            ESP_LOGI(TAG, "new neighbor: " FMTMAC " rssi=%d dBm", ARGMAC(mac), rssi);
        } else {
            ESP_LOGW(TAG, "neighbor table full, not accepting more, dropping " FMTMAC, ARGMAC(mac));
        }
    }

    xSemaphoreGive(mutex);
}


void neighbor_table_expire(void)
{
    uint32_t now = now_ms();

    xSemaphoreTake(mutex, portMAX_DELAY);

    for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        if (neighbor_table[i].is_valid) {
            uint32_t age_ms = now - neighbor_table[i].last_seen_ms;
            // Expire if we haven't heard from this neighbor in a while.
            if (age_ms > NEIGHBOR_TIMEOUT_MS) {
                ESP_LOGI(TAG, "neighbor expired: " FMTMAC " (silent for %lums)", ARGMAC(neighbor_table[i].mac), (unsigned long)age_ms);
                // Clear the entry
                memset(&neighbor_table[i], 0, sizeof(neighbor_entry_t));
            }
        }
    }

    xSemaphoreGive(mutex);
}

void neighbor_table_print(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);

    uint32_t now = now_ms();
    int count = 0;

    ESP_LOGI(TAG, "--- neighbor table ---");
    for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        if (neighbor_table[i].is_valid) {
            uint32_t age_ms = now - neighbor_table[i].last_seen_ms;
            ESP_LOGI(TAG, "  [%d] " FMTMAC "  rssi=%4d dBm  age=%lums",
                     count,
                     ARGMAC(neighbor_table[i].mac),
                     neighbor_table[i].rssi,
                     (unsigned long)age_ms);
            count++;
        }
    }

    if (count == 0) {
        ESP_LOGI(TAG, "  (no neighbors)");
    }

    ESP_LOGI(TAG, "--- %d neighbor(s) ---", count);

    xSemaphoreGive(mutex);
}

int neighbor_table_get(neighbor_entry_t *out_buf, int max_entries)
{
    int count = 0;

    xSemaphoreTake(mutex, portMAX_DELAY);

    for (int i = 0; i < NEIGHBOR_TABLE_SIZE && count < max_entries; i++) {
        if (neighbor_table[i].is_valid) {
            out_buf[count++] = neighbor_table[i];
        }
    }

    xSemaphoreGive(mutex);
    return count;
}
