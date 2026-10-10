#pragma once
#include <lvgl.h>

// The row at the top centre of the pages: the music sound-bars (Phone_Screens adds them) and
// the battery. The battery shows on the waiting / cruise, lean and journey screens and in the
// settings panel; when low it shows everywhere (amber at 20%, red at 10%) and pulses once on
// crossing each level; a green bar with a bolt while charging. Also sent to the app:
// BATTERY|percent|volts|charging.
void Status_Bar_Create(void);          // before the call overlay, so a call covers it
lv_obj_t *Status_Bar_Row(void);
