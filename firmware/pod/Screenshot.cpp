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
#include "Pages.h"
#include "Settings.h"
#include "Phone_Notify.h"
#include "Nav_Screen.h"
#include "Map_Layer.h"
#include "Lean_Screen.h"
#include "Gyro_QMI8658.h"
#include <lvgl.h>

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

// Serial commands from the Mac: "shot", "msg <line>" (inject a phone line), "test off", "perf",
// "page <n>" (switch screen), "cal" (recalibrate lean), "imu" (print sensor readings)
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
      else if (strncmp(cmd, "page ", 5) == 0) Pages_Show(atoi(cmd + 5));
      else if (strcmp(cmd, "cal") == 0) Lean_Calibrate();
      else if (strcmp(cmd, "cam") == 0) Map_Layer_Debug_Print();
      else if (strcmp(cmd, "settings") == 0) Settings_Panel_Show(!Settings_Panel_Open());
      else if (strcmp(cmd, "waiting") == 0) Nav_Screen_Show_Waiting();
      else if (strncmp(cmd, "fakecall", 8) == 0) Phone_Notify_Fake_Call(cmd[8] ? cmd + 9 : "");
      else if (strncmp(cmd, "fakemusic", 9) == 0) {
        // fakemusic Title|Artist|1
        char buf[120];
        strlcpy(buf, cmd[9] ? cmd + 10 : "", sizeof(buf));
        char *artist = strchr(buf, '|');
        char *playing = artist ? strchr(artist + 1, '|') : NULL;
        if (artist) *artist++ = 0;
        if (playing) *playing++ = 0;
        Phone_Notify_Fake_Music(buf, artist ? artist : "", !playing || *playing == '1');
      }
      else if (strcmp(cmd, "camlog") == 0) { extern bool map_cam_log; map_cam_log = !map_cam_log; }
      else if (strcmp(cmd, "bench") == 0) Nav_Screen_Bench();
      else if (strcmp(cmd, "rastbench") == 0) Map_Layer_Raster_Bench();
      else if (strcmp(cmd, "cpu") == 0) printf("CPU %lu MHz, free internal %lu, free psram %lu\r\n", (unsigned long)getCpuFrequencyMhz(),
                                              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      else if (strcmp(cmd, "hide fade") == 0) { extern bool fade_on; fade_on = false; }
      else if (strcmp(cmd, "show fade") == 0) { extern bool fade_on; fade_on = true; }
      else if (strncmp(cmd, "hide ", 5) == 0) Nav_Screen_Debug(cmd + 5, true);
      else if (strncmp(cmd, "show ", 5) == 0) Nav_Screen_Debug(cmd + 5, false);
      else if (strcmp(cmd, "mem") == 0) {
        lv_mem_monitor_t m;
        lv_mem_monitor(&m);
        printf("MEM lvgl total %lu, free %lu, biggest free block %lu, used %d%%, frag %d%%\r\n",
               (unsigned long)m.total_size, (unsigned long)m.free_size, (unsigned long)m.free_biggest_size,
               m.used_pct, m.frag_pct);
      }
      else if (strcmp(cmd, "imu") == 0)
        printf("IMU acc %.2f %.2f %.2f g, gyro %.1f %.1f %.1f dps\r\n", Accel.x, Accel.y, Accel.z, Gyro.x, Gyro.y, Gyro.z);
      len = 0;
    } else if (len < sizeof(cmd) - 1) {
      cmd[len++] = c;
    }
  }
}
