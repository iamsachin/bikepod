#pragma once
#include <lvgl.h>

// Acceleration page: estimated g while cornering (lateral) and speeding up / braking
// (longitudinal), a g-circle with a dot, the ride's peaks and a short history. The numbers come
// from the motion sensor (see "Motion" in Lean_Screen.h).
void Accel_Screen_Create(lv_obj_t *parent);
void Accel_Screen_Show(bool show);
