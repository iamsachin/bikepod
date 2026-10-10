#pragma once
#include <lvgl.h>

// The row at the top centre of the pages, between the rim and each page's first line: holds the
// music sound-bars (Phone_Screens adds them while music plays)
void Status_Bar_Create(void);          // before the call overlay, so a call covers it
lv_obj_t *Status_Bar_Row(void);
