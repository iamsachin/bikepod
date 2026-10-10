#pragma once

#include <lvgl.h>

// Screens you swipe between: 0 = navigation, 1 = lean, 2 = journey, 3 = music, round in a circle.
// Swipe left for the next screen, right to go back (like a phone; the dots read left to right).
void Pages_Init(void);
void Pages_Show(int page);
int Pages_Current(void);
// The navigation screen is what's showing (not another page, not the settings panel)
bool Pages_Nav_Visible(void);
// The row of dots at the bottom of a page showing where you are (`index` = this page)
void Pages_Add_Dots(lv_obj_t *page_root, int index);
