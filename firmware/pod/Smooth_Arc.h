#pragma once
#include <lvgl.h>

// A ring arc drawn with anti-aliased edges and round ends at any angle. LVGL 8's own arcs came
// out jagged at the screen's rim (large radius) and only move in whole degrees.
// Centred on the screen; angles in degrees clockwise from 12 o'clock.
lv_obj_t *Smooth_Arc_Create(lv_obj_t *parent, float radius, float width, lv_color_t color);
void Smooth_Arc_Set(lv_obj_t *arc, float start_deg, float sweep_deg);   // sweep 0 = nothing
