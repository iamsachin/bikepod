#pragma once
#include <lvgl.h>

// The row at the top centre of the pages, between the rim and each page's first line: a trip icon
// while a trip is on (tap: back to navigation) and the music sound-bars (Phone_Screens adds them
// while music plays; tap: Now playing)
void Status_Bar_Create(void);          // before the call overlay, so a call covers it
lv_obj_t *Status_Bar_Row(void);
// The phone's battery: a red phone icon in the row at 20% or less (not charging)
void Status_Bar_Set_Phone(int percent, bool charging);
