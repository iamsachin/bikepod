#pragma once
#include <lvgl.h>

// Journey screen (swipe right from navigation): kilometres done out of the trip's total, with a
// progress ring round the edge, time riding, arrival time and average speed. "No trip" when
// not navigating.

void Journey_Screen_Create(lv_obj_t *parent);
void Journey_Screen_Show(bool show);
// From the phone's TRIP line: arrival time, km left, the route's total km, seconds since the start
void Journey_Set(const char *eta, float km_left, float km_total, uint32_t elapsed_s);
// Navigation ended (arrived, cancelled or phone gone)
void Journey_Clear(void);

// The row under the trip stats (shown once known)
void Journey_Set_Phone(int percent, bool charging);    // the phone's battery
void Journey_Set_Fuel(uint32_t metres);                // next fuel station along the route (0 = none)
void Journey_Set_Rain(int minutes);                    // rain on the route: -1 none, 0 now, else minutes
