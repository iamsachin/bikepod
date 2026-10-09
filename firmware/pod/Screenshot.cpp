/*****************************************************************************
  | File        :   Screenshot.cpp
  | help        :   Sends the pixels currently on the panel (the RGB frame buffer) over serial.
                    Runs from the main loop, so LVGL is not drawing while we read.
******************************************************************************/
#include "Screenshot.h"
#include <Arduino.h>
#include "mbedtls/base64.h"
#include "Display_ST7701.h"
#include "BLE_Link.h"

volatile bool screenshot_sending = false;
bool perf_report = false;

#define RUNS_PER_LINE 48                       // 192 raw bytes -> 256 base64 characters

static void Send_Line(const uint8_t *raw, size_t len)
{
  static char line[8 + 4 * (RUNS_PER_LINE * 4 / 3 + 4)];
  size_t out_len = 0;
  memcpy(line, "SHOT:", 5);
  mbedtls_base64_encode((unsigned char *)line + 5, sizeof(line) - 7, &out_len, raw, len);
  line[5 + out_len] = '\n';
  Serial.write((const uint8_t *)line, 6 + out_len);   // one write per line keeps lines whole
}

static void Screenshot_Send(void)
{
  void *fb = NULL;
  esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 1, &fb);
  if (!fb) {
    Serial.println("SHOT_ERROR no frame buffer");
    return;
  }
  const uint16_t *px = (const uint16_t *)fb;
  const uint32_t total = ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT;

  screenshot_sending = true;
  delay(20);                                  // let any log line already being printed finish
  Serial.printf("SHOT_BEGIN %d %d\n", ESP_PANEL_LCD_WIDTH, ESP_PANEL_LCD_HEIGHT);
  uint8_t raw[RUNS_PER_LINE * 4];
  size_t n = 0;
  for (uint32_t i = 0; i < total;) {
    uint16_t color = px[i];
    uint32_t run = 1;
    while (i + run < total && px[i + run] == color && run < 0xFFFF) run++;
    raw[n++] = run & 0xFF;
    raw[n++] = run >> 8;
    raw[n++] = color & 0xFF;
    raw[n++] = color >> 8;
    i += run;
    if (n == sizeof(raw)) {
      Send_Line(raw, n);
      n = 0;
    }
  }
  if (n) Send_Line(raw, n);
  Serial.println("SHOT_END");
  Serial.flush();
  screenshot_sending = false;
}

// Serial commands from the Mac: "shot", "msg <line>" (inject a phone line), "test off", "perf"
void Screenshot_Poll(void)
{
  static char cmd[256];
  static uint16_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      cmd[len] = 0;
      if (strcmp(cmd, "shot") == 0) Screenshot_Send();
      else if (strncmp(cmd, "msg ", 4) == 0) BLE_Link_Inject(String(cmd + 4));
      else if (strcmp(cmd, "test off") == 0) BLE_Link_End_Test();
      else if (strcmp(cmd, "perf") == 0) perf_report = !perf_report;
      len = 0;
    } else if (len < sizeof(cmd) - 1) {
      cmd[len++] = c;
    }
  }
}
