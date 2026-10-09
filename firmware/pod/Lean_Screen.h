#pragma once
#include <lvgl.h>

// Lean screen (sport display): estimated lean angle, deepest lean each side, lateral g, and a
// short history graph, from the board's QMI8658 motion sensor plus GPS speed from the phone.
// Lean and g are estimates for interest, not a safety aid.

// Creates the screen (hidden) on top of `parent`; starts the motion sensor and calibrates it,
// so keep the pod still for the first second after power-on.
void Lean_Screen_Create(lv_obj_t *parent);
void Lean_Screen_Show(bool show);
// Ground speed from the phone's GPS, metres per second (0 if unknown)
void Lean_Set_Speed(float metres_per_second);
// Re-measures which way is down (pod still, bike upright) and clears the session's maximums
void Lean_Calibrate(void);
