#include "dedup_cache.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "dedup_cache";

typedef struct {
    uint8_t  src_mac[6];
    uint16_t msg_id;
    bool     is_valid;
} dedup_entry_t;

static dedup_entry_t cache[DEDUP_CACHE_SIZE];
static int head = 0; // next slot to overwrite
static SemaphoreHandle_t mutex = NULL; // protect from concurrent tasks

void dedup_cache_init(void)
{
    // initialise cache and mutex
    memset(cache, 0, sizeof(cache));
    head  = 0;
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex != NULL);
    // ESP_LOGI(TAG, "dedup cache initialised (%d slots)", DEDUP_CACHE_SIZE);
}

bool dedup_cache_is_duplicate(const uint8_t src_mac[6], uint16_t msg_id)
{
    bool is_dup = false;

    xSemaphoreTake(mutex, portMAX_DELAY);

    // linear search to find duplicate. this is fine since the cache size is small and fixed.
    for (int i = 0; i < DEDUP_CACHE_SIZE; i++) {
        if (cache[i].is_valid
                && cache[i].msg_id == msg_id
                && memcmp(cache[i].src_mac, src_mac, 6) == 0) {
            is_dup = true;
            break;
        }
    }

    // if not a dup, insert new entry at head for future dedup checks
    if (!is_dup) {
        memcpy(cache[head].src_mac, src_mac, 6);
        cache[head].msg_id = msg_id;
        cache[head].is_valid  = true;
        head = (head + 1) % DEDUP_CACHE_SIZE;
    }

    xSemaphoreGive(mutex);
    return is_dup;
}
