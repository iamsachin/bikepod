/*****************************************************************************
  | File        :   pod.ino
  | help        :   Bike navigation pod firmware.
                    Shows what the phone sends over BLE (turns + route drawing, rerouting, arrived, cruise),
                    "Waiting for phone" when disconnected, and fades the screen if the link goes quiet.
                    Display/touch drivers come from Waveshare's ESP32-S3-Touch-LCD-2.8C demo
                    (with the 18 MHz pixel clock fix in Display_ST7701.h).
******************************************************************************/
#include "LVGL_Driver.h"
#include "Nav_Screen.h"
#include "BLE_Link.h"
#include "Screenshot.h"
#include "Map_Layer.h"
#include "Lean_Screen.h"
#include "Pages.h"
#include "Journey_Screen.h"
#include "Settings.h"
#include "Clock.h"

// The phone repeats its current message every ~2 s; after this long without one, fade the screen
#define STALE_AFTER_MS  8000

// The phone only sends the distance; the pod works out how far through the approach we are
static uint32_t approach_start_m = 1;
static Turn_Dir last_dir = TURN_STRAIGHT;
static char last_road[64] = "";
static uint32_t last_msg_ms = 0;

static void Show_Turn(const Nav_Message &msg)
{
  bool new_turn = Nav_Screen_Mode() != MODE_TURN
                  || msg.dir != last_dir || strcmp(msg.text, last_road) != 0
                  || msg.distance_m > approach_start_m;
  if (new_turn) {
    approach_start_m = msg.distance_m > 0 ? msg.distance_m : 1;
    last_dir = msg.dir;
    strlcpy(last_road, msg.text, sizeof(last_road));
  }
  uint16_t progress = 1000 - msg.distance_m * 1000 / approach_start_m;
  Nav_Screen_Update(msg.dir, msg.distance_m, progress, msg.text, msg.rb_exit, msg.rb_degrees);
}

static void Pod_Tick(lv_timer_t *timer)
{
  Map_Layer_Poll();

  if (!BLE_Link_Connected()) {
    if (Nav_Screen_Mode() != MODE_WAITING) { Nav_Screen_Show_Waiting(); Journey_Clear(); }
    return;
  }

  Nav_Message msg;
  bool got_any = false;
  while (BLE_Link_Take_Message(&msg)) {
    got_any = true;
    switch (msg.kind) {
      case MSG_TURN:    Show_Turn(msg); break;
      case MSG_TRIP:    Nav_Screen_Set_Trip(msg.clock, msg.km);
                        Journey_Set(msg.clock, msg.km, msg.km_total, msg.elapsed_s); break;
      case MSG_ROUTE:   Nav_Screen_Set_Route(msg.area_id, msg.path, msg.path_count); break;
      case MSG_SPEED:   Lean_Set_Speed(msg.speed_mps); Nav_Screen_Set_Speed(msg.speed_mps); break;
      case MSG_LIMIT:   Nav_Screen_Set_Limit(msg.speed_kmh); break;
      case MSG_BUMP:    Nav_Screen_Set_Bump(msg.distance_m); break;
      case MSG_TIME:    Clock_Set(msg.elapsed_s / 3600, msg.elapsed_s / 60 % 60, msg.elapsed_s % 60); break;
      case MSG_POSE:    Nav_Screen_Set_Pose(msg.area_id, msg.pose[0], msg.pose[1], msg.pose[2]); break;
      case MSG_REROUTE: Nav_Screen_Show_Rerouting(); break;
      case MSG_ARRIVED: Nav_Screen_Show_Arrived(msg.km, msg.trip_min, msg.text); Journey_Clear(); break;
      case MSG_CRUISE:  Nav_Screen_Show_Cruise(msg.speed_kmh, msg.clock); Journey_Clear(); break;
    }
  }
  if (got_any) {
    last_msg_ms = lv_tick_get();
    Nav_Screen_Set_Stale(false);
  } else if (Nav_Screen_Mode() != MODE_WAITING && lv_tick_elaps(last_msg_ms) > STALE_AFTER_MS) {
    Nav_Screen_Set_Stale(true);
  }
}

void setup()
{
  Serial.begin(921600);             // fast enough for screenshots (tools/pod_screenshot.py)
  printf("Pod starting\r\n");

  I2C_Init();
  delay(120);                       // ms
  TCA9554PWR_Init(0x00);            // I/O expander: all pins output
  Clock_Init();                     // real-time clock (set by the phone)
  Set_EXIO(EXIO_PIN8, Low);         // buzzer off
  // Screen on this core (1). Starting it from core 0 put its picture-copying interrupts next to
  // Bluetooth, which delayed them: the picture slipped (the old drift problem) for ~10% speed.
  LCD_Init();                       // screen, touch, backlight
  Settings_Init();                  // saved settings; sets the brightness
  Lvgl_Init();
  lv_timer_set_period(_lv_disp_get_refr_timer(lv_disp_get_default()), 16);   // ~60 fps (default 30 ms)

  Nav_Screen_Create();              // starts on "Waiting for phone"
  Journey_Screen_Create(lv_scr_act());   // swipe right from navigation
  Lean_Screen_Create(lv_scr_act()); // swipe right again; calibrates: keep still at power-on
  Settings_Panel_Create(lv_scr_act());   // swipe down from the top
  Pages_Init();
  BLE_Link_Init();
  lv_timer_create(Pod_Tick, 50, NULL);
  printf("Pod ready\r\n");
}

// Redraw timing, reported with the serial "perf" command: how long frames that drew something took
static void Perf_Measure(uint32_t us)
{
  static uint32_t frames = 0, total_us = 0, worst_us = 0, since_ms = 0;
  if (us > 2000) {                         // shorter calls had nothing to redraw
    frames++;
    total_us += us;
    worst_us = LV_MAX(worst_us, us);
  }
  if (millis() - since_ms >= 2000) {
    extern uint32_t map_perf_roads_us, map_perf_fuel_us, map_perf_route_us, map_perf_draws;
    extern uint32_t flush_perf_us, flush_perf_px;
    if (perf_report && !screenshot_sending) {
      printf("PERF %lu frames in 2 s, avg %lu ms, worst %lu ms\r\n", (unsigned long)frames,
             (unsigned long)(frames ? total_us / frames / 1000 : 0), (unsigned long)(worst_us / 1000));
      if (map_perf_draws)
        printf("PERF map: %lu draw calls, per call roads %lu us, fuel %lu us, route %lu us\r\n",
               (unsigned long)map_perf_draws, (unsigned long)(map_perf_roads_us / map_perf_draws / 1000),
               (unsigned long)(map_perf_fuel_us / map_perf_draws / 1000), (unsigned long)(map_perf_route_us / map_perf_draws / 1000));
    }
    if (perf_report && !screenshot_sending)
      printf("PERF flush: %lu ms total for %lu kpx\r\n", (unsigned long)(flush_perf_us / 1000), (unsigned long)(flush_perf_px / 1000));
    map_perf_roads_us = map_perf_fuel_us = map_perf_route_us = map_perf_draws = 0;
    flush_perf_us = flush_perf_px = 0;
    frames = total_us = worst_us = 0;
    since_ms = millis();
  }
}

void loop()
{
  uint32_t start = micros();
  Lvgl_Loop();
  Perf_Measure(micros() - start);
  Screenshot_Poll();
  delay(5);
}
