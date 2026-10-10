#pragma once

#include <lvgl.h>

// Screens you swipe between, in this order and round in a circle. Swipe left for the next
// screen, right to go back (like a phone; the dots read left to right).
enum Page { PAGE_NAV, PAGE_LEAN, PAGE_ACCEL, PAGE_JOURNEY, PAGE_MUSIC, PAGE_COUNT };
void Pages_Init(void);
void Pages_Show(int page);
int Pages_Current(void);
// The navigation screen is what's showing (not another page, not the settings panel)
bool Pages_Nav_Visible(void);
// The row of dots at the bottom of a page showing where you are (`index` = this page)
void Pages_Add_Dots(lv_obj_t *page_root, int index);
