#pragma once

// The LiPo, measured on GPIO 4 through the board's 3:1 divider. Readings are averaged (the ADC
// is noisy, and the voltage dips while the radio transmits).
void Battery_Init(void);
void Battery_Update(void);      // main loop; samples every 500 ms
float Battery_Volts(void);
// 0-100 from the usual single-cell LiPo curve; -1 while no reading yet
int Battery_Percent(void);
// Charger connected (worked out from the voltage: see Battery.cpp)
bool Battery_Charging(void);
void Battery_Print_History(void);    // serial "batlog"
// Testing (serial "batfake 15", "batfake 60c" = charging, "batfake -1" = real readings again)
void Battery_Fake(int percent, bool charging);
