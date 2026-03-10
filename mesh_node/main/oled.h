#pragma once

#include <stdint.h>
#include "esp_err.h"

// ============================================================
// SSD1306 OLED display over I2C. Uses a simple framebuffer and
// 5x7 font. Only supports writing text, no graphics.
// ============================================================

#define OLED_I2C_ADDR   0x3C
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      8    // 64px / 8px per page

/**
 * @brief  Initialise I2C and the SSD1306. Call once at startup.
 * @return ESP_OK on success.
 */
esp_err_t oled_init(void);

/**
 * @brief  Clear the display buffer and push to screen.
 */
void oled_clear(void);

/**
 * @brief  Write a string at a character position.
 *
 * @param  page  Row (0–7). Each page is 8 pixels tall.
 * @param  col   Column in pixels (0–127).
 * @param  str   Null-terminated ASCII string.
 */
void oled_write_string(uint8_t page, uint8_t col, const char *str);

/**
 * @brief  Push the local framebuffer to the display.
 *         Call after all oled_write_string() calls for a frame.
 */
void oled_flush(void);
