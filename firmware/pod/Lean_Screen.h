#pragma once
#include <lvgl.h>

// Lean screen (sport display): estimated lean angle, deepest lean each side, lateral g, and a
// short history graph, from the board's QMI8658 motion sensor plus GPS speed from the phone.
// Lean and g are estimates for interest, not a safety aid.

// Creates the screen (hidden) on top of `parent`; starts the motion sensor and measures the
// gyro's bias over the first still second (any tilt: the mounting is saved, see Lean_Calibrate)
void Lean_Screen_Create(lv_obj_t *parent);
void Lean_Screen_Show(bool show);
// Ground speed from the phone's GPS, metres per second (0 if unknown)
void Lean_Set_Speed(float metres_per_second);
// Re-measures and saves which way is up (pod still, bike upright) and clears the maximums
void Lean_Calibrate(void);

/* Motion, for the acceleration page: g felt along and across the bike. Estimates, not
   measurements: lateral is speed x turn rate (0 when stopped), longitudinal the accelerometer
   along the bike corrected by GPS speed changes. Peaks are since power-on (on bike power, the
   ride) or since "Calibrate lean". History: one point every 0.2 s, oldest first. */
#define MOTION_HISTORY_POINTS 80
struct Motion {
  float lateral_g;          // + = to the right
  float longitudinal_g;     // + = speeding up, - = braking
  float peak_accel, peak_decel;            // decel is negative
  float peak_lat_left, peak_lat_right;     // both positive
  uint8_t history_head;     // changes when a point is added
  float history_lat[MOTION_HISTORY_POINTS], history_lon[MOTION_HISTORY_POINTS];
};
void Motion_Get(Motion *out);
