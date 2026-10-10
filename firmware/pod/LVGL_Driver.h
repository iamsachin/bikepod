#pragma once

#include <lvgl.h>
#include "lv_conf.h"
#include <esp_heap_caps.h>
#include "Display_ST7701.h"
#include "Touch_GT911.h"

#define LVGL_WIDTH     ESP_PANEL_LCD_WIDTH
#define LVGL_HEIGHT    ESP_PANEL_LCD_HEIGHT
// LVGL draws into two 20-line buffers in fast internal RAM (it renders a frame in strips): the
// map's pixel-by-pixel drawing is several times slower into PSRAM. Kept small because Bluetooth
// needs internal RAM at start-up (2 x 40 lines plus other buffers left it short).
#define LVGL_BUF_LINES 20
#define LVGL_BUF_LEN  (LVGL_WIDTH * LVGL_BUF_LINES * sizeof(lv_color_t))

#define EXAMPLE_LVGL_TICK_PERIOD_MS  2


extern lv_disp_drv_t disp_drv;

void Lvgl_print(const char * buf);
void Lvgl_Display_LCD( lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p ); // Displays LVGL content on the LCD.    This function implements associating LVGL data to the LCD screen
void Lvgl_Touchpad_Read( lv_indev_drv_t * indev_drv, lv_indev_data_t * data );                // Read the touchpad
void example_increase_lvgl_tick(void *arg);

void Lvgl_Init(void);
void Lvgl_Loop(void);
