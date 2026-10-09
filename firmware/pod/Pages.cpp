/*****************************************************************************
  | File        :   Pages.cpp
  | help        :   Swipe between screens (the lean screen's page dots show where you are).
******************************************************************************/
#include "Pages.h"
#include <lvgl.h>
#include "Lean_Screen.h"

#define PAGE_COUNT 2

static int page = 0;

void Pages_Show(int new_page)
{
  if (new_page < 0 || new_page >= PAGE_COUNT || new_page == page) return;
  page = new_page;
  Lean_Screen_Show(page == 1);
}

int Pages_Current(void)
{
  return page;
}

static void Gesture_Cb(lv_event_t *e)
{
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_RIGHT) Pages_Show(page + 1);
  else if (dir == LV_DIR_LEFT) Pages_Show(page - 1);
}

void Pages_Init(void)
{
  lv_obj_add_event_cb(lv_scr_act(), Gesture_Cb, LV_EVENT_GESTURE, NULL);
}
