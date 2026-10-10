#include "Battery.h"
#include <Arduino.h>

#define BATTERY_PIN       4
#define DIVIDER           3.0f
#define CALIBRATION       0.980952f      // Waveshare's measured correction for this board
#define SAMPLES           60             // averaged: 30 s at one sample every 500 ms (radio bursts dip it)

static float samples[SAMPLES];
static int count = 0, head = 0;
static int fake = -1;
static bool fake_charging = false;

/* Charging. The charger's status only lights the CHG LED (the schematic has no pin for it, nor
   for USB power), so it is read from the voltage: plugging in lifts the battery's voltage by
   about 0.1 V within a second or two (charge current in, and the load taken over), unplugging
   drops it as much. Steps are found by comparing 2 s averages 10 s apart; a reading at the
   charger's full voltage also counts as charging. */
#define STEP_VOLTS        0.06f
#define FULL_VOLTS        4.19f
#define HISTORY           240            // 2 s averages: the last 8 minutes (serial "batlog")
static float history[HISTORY];
static int history_count = 0, history_head = 0;
static bool charging = false;

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
  float volts = analogReadMilliVolts(BATTERY_PIN) * DIVIDER / 1000.0f / CALIBRATION;
  samples[head] = volts;
  head = (head + 1) % SAMPLES;
  if (count < SAMPLES) count++;

  // Every 2 s: the 2 s average into the history, then look for a step. Not in the first 4 s:
  // the first readings after power-on come out low (3.98 V on the charger) and looked like a step
  if (millis() < 4000) return;
  static float sum2 = 0;
  static int n2 = 0;
  sum2 += volts;
  if (++n2 < 4) return;
  float now = sum2 / n2;
  sum2 = 0;
  n2 = 0;
  history[history_head] = now;
  history_head = (history_head + 1) % HISTORY;
  if (history_count < HISTORY) history_count++;
  if (history_count <= 5) return;
  float before = history[(history_head + HISTORY - 6) % HISTORY];   // 10 s earlier
  bool was = charging;
  if (now - before >= STEP_VOLTS) charging = true;
  else if (now - before <= -STEP_VOLTS) charging = false;
  else if (now >= FULL_VOLTS) charging = true;
  if (charging != was) {
    printf("BAT: %s (%.3f -> %.3f V)\r\n", charging ? "charging" : "on battery", before, now);
    // Start the average afresh at the new level, rather than drifting across from the old one
    for (int i = 0; i < SAMPLES; i++) samples[i] = now;
    count = SAMPLES;
  }
}

bool Battery_Charging(void)
{
  return fake < 0 ? charging : fake_charging;
}

void Battery_Print_History(void)
{
  printf("BAT history (2 s averages, oldest first), %s\r\n", charging ? "charging" : "on battery");
  for (int i = 0; i < history_count; i++) {
    printf("%.3f%s", history[(history_head + HISTORY - history_count + i) % HISTORY], (i % 10 == 9) ? "\r\n" : " ");
  }
  printf("\r\n");
}

float Battery_Volts(void)
{
  if (!count) return 0;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += samples[i];
  return sum / count;
}

void Battery_Fake(int percent, bool charging_too)
{
  fake = percent;
  fake_charging = charging_too;
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
  // (Free to move until the 30 s average has filled after power-on, and while charging)
  if (shown < 0 || count < SAMPLES || charging || now < shown || now >= shown + 5) shown = now;
  return shown;
}
