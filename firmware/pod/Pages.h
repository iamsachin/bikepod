#pragma once

#include <lvgl.h>

// Screens you swipe between: 0 = navigation, 1 = journey, 2 = lean. Swipe right for the next
// screen, left to go back.
void Pages_Init(void);
void Pages_Show(int page);
int Pages_Current(void);
// The navigation screen is what's showing (not another page, not the settings panel)
bool Pages_Nav_Visible(void);
// The row of dots at the bottom of a page showing where you are (`index` = this page)
void Pages_Add_Dots(lv_obj_t *page_root, int index);
