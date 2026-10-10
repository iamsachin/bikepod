/*****************************************************************************
  | File        :   Pages.cpp
  | help        :   Swipe between screens (the lean screen's page dots show where you are).
******************************************************************************/
#include "Pages.h"
#include <lvgl.h>
#include "Lean_Screen.h"
#include "Journey_Screen.h"
#include "Map_Layer.h"

#define PAGE_COUNT 3

static int page = 0;

void Pages_Show(int new_page)
{
  if (new_page < 0 || new_page >= PAGE_COUNT || new_page == page) return;
  page = new_page;
  Journey_Screen_Show(page == 1);
  Lean_Screen_Show(page == 2);
  Map_Layer_Set_Visible(page == 0);
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
