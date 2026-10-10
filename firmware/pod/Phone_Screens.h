#pragma once
#include <lvgl.h>

// Screens for what the phone shares (see Phone_Notify.h):
//   Incoming call: over everything while the phone rings; caller's name, blue phone icon
//   Now playing:   a swipe page; song, artist, position, playing / paused, with time and speed

void Phone_Screens_Create(lv_obj_t *parent);
void Music_Screen_Show(bool show);
bool Call_Screen_Showing(void);
// A line of album art from the phone (ART|...), from the Bluetooth task
void Album_Art_Line(const char *line);
void Album_Art_Debug(void);                   // serial "art"
// Speed for the music page's top line (metres per second)
void Music_Set_Speed(float metres_per_second);
