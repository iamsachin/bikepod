#pragma once
#include <lvgl.h>
#include <stddef.h>

// Pod settings, kept in flash (NVS) so they survive power-off, and the panel that changes them:
// swipe down from the top of any screen to open it, swipe up to close.

void Settings_Init(void);                       // loads the settings and applies the brightness
void Settings_Panel_Create(lv_obj_t *parent);
void Settings_Panel_Show(bool show);            // slides down / up
bool Settings_Panel_Open(void);

// "16:30" from the phone -> "16:30" or "4:30 PM", as chosen
void Settings_Format_Clock(const char *hhmm, char *out, size_t size);
