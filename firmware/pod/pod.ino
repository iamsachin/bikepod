/*****************************************************************************
  | File        :   pod.ino
  | help        :   Bike navigation pod firmware.
                    Step 3: shows fake turns until the phone sends real ones over BLE.
                    Display/touch drivers come from Waveshare's ESP32-S3-Touch-LCD-2.8C demo
                    (with the 18 MHz pixel clock fix in Display_ST7701.h).
******************************************************************************/
#include "LVGL_Driver.h"
#include "Nav_Screen.h"
#include "BLE_Link.h"

// The phone only sends the distance; the pod works out how far through the approach we are
static uint32_t approach_start_m = 1;
static Turn_Dir last_dir = TURN_STRAIGHT;
static char last_road[64] = "";

static void Apply_Phone_Message(lv_timer_t *timer)
{
  Nav_Message msg;
  if (!BLE_Link_Take_Message(&msg)) return;

  Nav_Fake_Data_Stop();            // real data from now on

  bool new_turn = msg.dir != last_dir || strcmp(msg.road, last_road) != 0
                  || msg.distance_m > approach_start_m;
  if (new_turn) {
    approach_start_m = msg.distance_m > 0 ? msg.distance_m : 1;
    last_dir = msg.dir;
    strlcpy(last_road, msg.road, sizeof(last_road));
  }
  uint16_t progress = 1000 - msg.distance_m * 1000 / approach_start_m;
  Nav_Screen_Update(msg.dir, msg.distance_m, progress, msg.road);
}

void setup()
{
  Serial.begin(115200);
  printf("Pod starting\r\n");

  I2C_Init();
  delay(120);                       // ms
  TCA9554PWR_Init(0x00);            // I/O expander: all pins output
  Set_EXIO(EXIO_PIN8, Low);         // buzzer off
  LCD_Init();                       // screen, touch, backlight
  Set_Backlight(100);               // full brightness for daylight
  Lvgl_Init();
  lv_timer_set_period(_lv_disp_get_refr_timer(lv_disp_get_default()), 16);   // ~60 fps (default 30 ms)

  Nav_Screen_Create();
  Nav_Fake_Data_Start();
  BLE_Link_Init();
  lv_timer_create(Apply_Phone_Message, 50, NULL);
  printf("Pod ready\r\n");
}

void loop()
{
  Lvgl_Loop();
  delay(5);
}
