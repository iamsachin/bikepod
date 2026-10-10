/*****************************************************************************
  | File        :   Settings.cpp
  | help        :   Settings panel (see Settings.h). Big pill buttons, easy to hit with gloves:
                    clock 12 h / 24 h, brightness in four steps, and lean calibration.
******************************************************************************/
#include "Settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>
#include "Display_ST7701.h"
#include "Lean_Screen.h"
#include "Map_Layer.h"
#include "Pages.h"

LV_FONT_DECLARE(font_text_32);
LV_FONT_DECLARE(font_small_26);

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_PILL      lv_color_hex(0x262626)
#define COLOR_GREY      lv_color_hex(0xA8A8A8)
#define COLOR_DIM       lv_color_hex(0x6A6A6A)

static const uint8_t BRIGHTNESS[4] = { 25, 50, 75, 100 };
static const char *BRIGHTNESS_NAME[4] = { "Low", "Mid", "High", "Max" };

static Preferences prefs;
static bool clock_24h = false;               // 12 h unless changed
static uint8_t brightness_step = 3;

static lv_obj_t *panel;
static lv_obj_t *clock_btn[2], *bright_btn[4], *cal_label;
static bool panel_open = false;

/********************************* Settings *********************************/
void Settings_Init(void)
{
  prefs.begin("pod", false);
  clock_24h = prefs.getBool("clock24", false);
  brightness_step = LV_MIN(prefs.getUChar("bright", 3), 3);
  Set_Backlight(BRIGHTNESS[brightness_step]);
}

void Settings_Format_Clock(const char *hhmm, char *out, size_t size)
{
  int h, m;
  if (clock_24h || sscanf(hhmm, "%d:%d", &h, &m) != 2) {
    strlcpy(out, hhmm, size);
    return;
  }
  snprintf(out, size, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
}

/*********************************** Panel ***********************************/
static void Style_Pill(lv_obj_t *btn, bool selected)
{
  lv_obj_set_style_bg_color(btn, selected ? COLOR_BLUE : COLOR_PILL, 0);
}

static void Refresh_Choices(void)
{
  for (int i = 0; i < 2; i++) Style_Pill(clock_btn[i], (i == 1) == clock_24h);
  for (int i = 0; i < 4; i++) Style_Pill(bright_btn[i], i == brightness_step);
}

static lv_obj_t *Pill(lv_obj_t *parent, lv_coord_t w, const char *text, lv_event_cb_t cb, intptr_t value)
{
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, w, 58);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(btn, COLOR_PILL, 0);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)value);
  lv_obj_t *label = lv_label_create(btn);
  lv_obj_set_style_text_font(label, &font_text_32, 0);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

static lv_obj_t *Row(lv_obj_t *parent, lv_coord_t y, lv_coord_t w)
{
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, w, LV_SIZE_CONTENT);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  return row;
}

static lv_obj_t *Caption(lv_obj_t *parent, const char *text, lv_coord_t y, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, &font_small_26, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_text(label, text);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  return label;
}

static void Clock_Cb(lv_event_t *e)
{
  clock_24h = (intptr_t)lv_event_get_user_data(e) == 1;
  prefs.putBool("clock24", clock_24h);
  Refresh_Choices();
}

static void Bright_Cb(lv_event_t *e)
{
  brightness_step = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  prefs.putUChar("bright", brightness_step);
  Set_Backlight(BRIGHTNESS[brightness_step]);
  Refresh_Choices();
}

static void Cal_Done(lv_timer_t *timer)
{
  lv_label_set_text(cal_label, "Calibrate lean");
}

static void Cal_Cb(lv_event_t *e)
{
  Lean_Calibrate();                   // takes about a second: the pod must be still and upright
  lv_label_set_text(cal_label, "Keep still...");
  lv_timer_t *t = lv_timer_create(Cal_Done, 1500, NULL);
  lv_timer_set_repeat_count(t, 1);
}

void Settings_Panel_Create(lv_obj_t *parent)
{
  panel = lv_obj_create(parent);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  Caption(panel, "SETTINGS", 62, COLOR_GREY);

  Caption(panel, "Clock", 112, COLOR_DIM);
  lv_obj_t *row = Row(panel, 146, 290);
  clock_btn[0] = Pill(row, 140, "12 h", Clock_Cb, 0);
  clock_btn[1] = Pill(row, 140, "24 h", Clock_Cb, 1);

  Caption(panel, "Brightness", 222, COLOR_DIM);
  row = Row(panel, 256, 400);
  for (int i = 0; i < 4; i++) bright_btn[i] = Pill(row, 94, BRIGHTNESS_NAME[i], Bright_Cb, i);

  row = Row(panel, 340, 290);
  lv_obj_t *cal = Pill(row, 290, "", Cal_Cb, 0);
  cal_label = lv_obj_get_child(cal, 0);
  lv_label_set_text(cal_label, "Calibrate lean");

  Caption(panel, "Swipe up to close", 418, COLOR_DIM);

  Refresh_Choices();
  lv_obj_set_y(panel, -LV_VER_RES);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

static void Set_Y(void *obj, int32_t y)
{
  lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)y);
}

static void Hide_After(lv_anim_t *a)
{
  if (!panel_open) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

void Settings_Panel_Show(bool show)
{
  if (show == panel_open) return;
  panel_open = show;
  if (show) lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(panel);
  // The map underneath stops redrawing while it's covered
  Map_Layer_Set_Visible(Pages_Nav_Visible());

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, panel);
  lv_anim_set_exec_cb(&a, Set_Y);
  lv_anim_set_values(&a, lv_obj_get_y(panel), show ? 0 : -LV_VER_RES);
  lv_anim_set_time(&a, 250);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, Hide_After);
  lv_anim_start(&a);
}

bool Settings_Panel_Open(void)
{
  return panel_open;
}
