/*****************************************************************************
  | File        :   pod.ino
  | help        :   Bike navigation pod firmware.
                    Step 2: "approaching turn" screen driven by fake data.
                    Display/touch drivers come from Waveshare's ESP32-S3-Touch-LCD-2.8C demo
                    (with the 18 MHz pixel clock fix in Display_ST7701.h).
******************************************************************************/
#include "LVGL_Driver.h"
#include "Nav_Screen.h"

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

  Nav_Screen_Create();
  Nav_Fake_Data_Start();
  printf("Pod ready\r\n");
}

void loop()
{
  Lvgl_Loop();
  delay(5);
}
