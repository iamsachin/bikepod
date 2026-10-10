/*****************************************************************************
  | File        :   Clock.cpp
  | help        :   PCF85063 real-time clock at I2C 0x51 (see Clock.h). Registers from the
                    Waveshare demo (RTC_PCF85063): time in BCD from 0x04, 24-hour mode.
                    The seconds register's top bit (OS) says the oscillator stopped since it
                    was last set, i.e. the time can't be trusted.
******************************************************************************/
#include "Clock.h"
#include <Arduino.h>
#include <stdio.h>
#include "I2C_Driver.h"
#include "Settings.h"

#define RTC_ADDR        0x51
#define RTC_CTRL_1      0x00
#define RTC_SECONDS     0x04
#define CTRL_1_CAP_SEL  0x01      // 12.5 pF crystal load, as on this board
#define SECONDS_OS      0x80

static uint8_t To_Bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int From_Bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

void Clock_Init(void)
{
  uint8_t ctrl = CTRL_1_CAP_SEL;              // running, 24-hour mode
  I2C_Write(RTC_ADDR, RTC_CTRL_1, &ctrl, 1);
}

void Clock_Set(uint8_t hour, uint8_t minute, uint8_t second)
{
  uint8_t h, m, s;
  // Only when it's off by a few seconds: the phone repeats its time, and rewriting the clock
  // on every message would restart its seconds count each time
  if (Clock_Get(&h, &m, &s)) {
    int diff = abs((h * 3600 + m * 60 + s) - (hour * 3600 + minute * 60 + second));
    if (diff <= 3 || diff >= 86400 - 3) return;
  }
  uint8_t buf[3] = { To_Bcd(second), To_Bcd(minute), To_Bcd(hour) };   // OS bit cleared
  I2C_Write(RTC_ADDR, RTC_SECONDS, buf, sizeof(buf));
}

bool Clock_Get(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
  uint8_t buf[3] = { 0 };
  if (I2C_Read(RTC_ADDR, RTC_SECONDS, buf, sizeof(buf))) return false;   // (true = I2C failed)
  if (buf[0] & SECONDS_OS) return false;                                 // lost power since set
  *second = From_Bcd(buf[0] & 0x7F);
  *minute = From_Bcd(buf[1] & 0x7F);
  *hour = From_Bcd(buf[2] & 0x3F);
  return *hour < 24 && *minute < 60;
}

void Clock_Text(char *out, size_t size)
{
  uint8_t h, m, s;
  if (!Clock_Get(&h, &m, &s)) {
    out[0] = 0;
    return;
  }
  char hhmm[8];
  snprintf(hhmm, sizeof(hhmm), "%02u:%02u", h, m);
  Settings_Format_Clock(hhmm, out, size);
}
