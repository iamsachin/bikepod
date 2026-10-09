/*****************************************************************************
  | File        :   Pages.cpp
  | help        :   Swipe between screens, with the screen's name shown for a second.
******************************************************************************/
#include "Pages.h"
#include <lvgl.h>
#include "Lean_Screen.h"

LV_FONT_DECLARE(font_small_26);

#define PAGE_COUNT      2
#define NAME_SHOW_MS    1000
#define NAME_FADE_MS    300

static const char *const page_names[PAGE_COUNT] = { "Navigation", "Lean" };
static int page = 0;
static lv_obj_t *name_pill;
static lv_obj_t *name_label;

static void Fade_Cb(void *obj, int32_t value)
{
  lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void Show_Name(const char *name)
{
  lv_label_set_text(name_label, name);
  lv_anim_del(name_pill, Fade_Cb);
  lv_obj_set_style_opa(name_pill, LV_OPA_COVER, 0);
  lv_obj_clear_flag(name_pill, LV_OBJ_FLAG_HIDDEN);

  lv_anim_t a;                         // hold for a second, then fade out
  lv_anim_init(&a);
  lv_anim_set_var(&a, name_pill);
  lv_anim_set_exec_cb(&a, Fade_Cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, NAME_SHOW_MS);
  lv_anim_set_time(&a, NAME_FADE_MS);
  lv_anim_start(&a);
}

void Pages_Show(int new_page)
{
  if (new_page < 0 || new_page >= PAGE_COUNT || new_page == page) return;
  page = new_page;
  Lean_Screen_Show(page == 1);
  Show_Name(page_names[page]);
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

  // Name pill on the top layer, above every screen
  name_pill = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(name_pill);
  lv_obj_set_size(name_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(name_pill, lv_color_hex(0x1C1C1C), 0);
  lv_obj_set_style_bg_opa(name_pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(name_pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(name_pill, 18, 0);
  lv_obj_set_style_pad_ver(name_pill, 6, 0);
  lv_obj_clear_flag(name_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(name_pill, LV_ALIGN_BOTTOM_MID, 0, -34);
  name_label = lv_label_create(name_pill);
  lv_obj_set_style_text_font(name_label, &font_small_26, 0);
  lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
  lv_obj_add_flag(name_pill, LV_OBJ_FLAG_HIDDEN);
}
