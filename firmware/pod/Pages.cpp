/*****************************************************************************
  | File        :   Pages.cpp
  | help        :   Swipe between screens (the lean screen's page dots show where you are).
******************************************************************************/
#include "Pages.h"
#include <lvgl.h>
#include "Lean_Screen.h"
#include "Journey_Screen.h"
#include "Map_Layer.h"
#include "Settings.h"
#include "Phone_Screens.h"
#include "Accel_Screen.h"


static int page = 0;

void Pages_Show(int new_page)
{
  if (new_page < 0 || new_page >= PAGE_COUNT || new_page == page) return;
  page = new_page;
  Lean_Screen_Show(page == PAGE_LEAN);
  Accel_Screen_Show(page == PAGE_ACCEL);
  Journey_Screen_Show(page == PAGE_JOURNEY);
  Music_Screen_Show(page == PAGE_MUSIC);
  Map_Layer_Set_Visible(page == PAGE_NAV);
}

int Pages_Current(void)
{
  return page;
}

bool Pages_Nav_Visible(void)
{
  return page == PAGE_NAV && !Settings_Panel_Open() && !Call_Screen_Showing();
}

// Swipe left/right between pages; down from the top opens settings, up closes them
static void Gesture_Cb(lv_event_t *e)
{
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (Settings_Panel_Open()) {
    if (dir == LV_DIR_TOP) Settings_Panel_Show(false);
    return;
  }
  // Like a phone: swipe left for the next page (the dots show it to the right), right to go back
  // ...and round in a circle: past the last page comes navigation again
  if (dir == LV_DIR_LEFT) Pages_Show((page + 1) % PAGE_COUNT);
  else if (dir == LV_DIR_RIGHT) Pages_Show((page + PAGE_COUNT - 1) % PAGE_COUNT);
  else if (dir == LV_DIR_BOTTOM) Settings_Panel_Show(true);
}

void Pages_Add_Dots(lv_obj_t *page_root, int index)
{
  lv_obj_t *dots = lv_obj_create(page_root);
  lv_obj_remove_style_all(dots);
  lv_obj_set_size(dots, LV_SIZE_CONTENT, 4);
  lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(dots, 6, 0);
  lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, 432);
  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_t *dot = lv_obj_create(dots);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, i == index ? 18 : 6, 4);
    lv_obj_set_style_radius(dot, 2, 0);
    lv_obj_set_style_bg_color(dot, i == index ? lv_color_white() : lv_color_hex(0x6A6A6A), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  }
}

void Pages_Init(void)
{
  lv_obj_add_event_cb(lv_scr_act(), Gesture_Cb, LV_EVENT_GESTURE, NULL);
}
