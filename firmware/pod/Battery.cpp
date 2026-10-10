#include "Battery.h"
#include <Arduino.h>

#define BATTERY_PIN       4
#define DIVIDER           3.0f
#define CALIBRATION       0.980952f      // Waveshare's measured correction for this board
#define SAMPLES           60             // averaged: 30 s at one sample every 500 ms (radio bursts dip it)

static float samples[SAMPLES];
static int count = 0, head = 0;
static int fake = -1;

void Battery_Init(void)
{
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
}

void Battery_Update(void)
{
  static uint32_t last_ms = 0;
  if (count && millis() - last_ms < 500) return;
  last_ms = millis();
  samples[head] = analogReadMilliVolts(BATTERY_PIN) * DIVIDER / 1000.0f / CALIBRATION;
  head = (head + 1) % SAMPLES;
  if (count < SAMPLES) count++;
}

float Battery_Volts(void)
{
  if (!count) return 0;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += samples[i];
  return sum / count;
}

void Battery_Fake(int percent)
{
  fake = percent;
}

// Charge left from the averaged voltage, before smoothing (see Battery_Percent)
static int Percent_From_Volts(void)
{
  // Resting voltage of a single LiPo cell against charge left (typical curve)
  static const float volts[]   = { 3.30f, 3.50f, 3.60f, 3.68f, 3.74f, 3.79f, 3.85f, 3.92f, 4.00f, 4.08f, 4.18f };
  static const int   percent[] = {    0,     5,    10,    20,    30,    40,    50,    60,    70,    85,   100 };
  float v = Battery_Volts();
  if (v <= volts[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (v < volts[i]) {
      float t = (v - volts[i - 1]) / (volts[i] - volts[i - 1]);
      return (int)lroundf(percent[i - 1] + t * (percent[i] - percent[i - 1]));
    }
  }
  return 100;
}

int Battery_Percent(void)
{
  if (fake >= 0) return fake;
  if (!count) return -1;
  // On battery the charge only goes down: small rises are the voltage recovering after a load,
  // not charge, and showing them made the level bounce. A clear rise means the charger (or a
  // long rest): then the new value counts.
  static int shown = -1;
  int now = Percent_From_Volts();
  if (shown < 0 || now < shown || now >= shown + 5) shown = now;
  return shown;
}
