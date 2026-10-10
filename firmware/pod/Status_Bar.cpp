/*****************************************************************************
  | File        :   Status_Bar.cpp
  | help        :   Top-centre status row: music playing, battery level.
******************************************************************************/
#include "Status_Bar.h"
#include <Arduino.h>
#include "Battery.h"
#include "BLE_Link.h"
#include "Nav_Screen.h"
#include "Pages.h"
#include "Phone_Screens.h"
#include "Settings.h"

LV_FONT_DECLARE(font_status_20);

#define COLOR_OUTLINE   lv_color_hex(0x8C8C8C)
#define COLOR_FILL      lv_color_hex(0xE6E6E6)
#define COLOR_GREY      lv_color_hex(0xA8A8A8)
#define COLOR_AMBER     lv_color_hex(0xF5B83D)
#define COLOR_RED       lv_color_hex(0xE5484D)

#define LOW_PERCENT       20
#define CRITICAL_PERCENT  10

// Battery drawing: a 30x15 rounded outline, a nub, and a fill bar inside
#define BODY_W    30
#define BODY_H    15
#define BORDER    2
#define INSET     2               // gap between the outline and the fill
#define FILL_W    (BODY_W - 2 * (BORDER + INSET))

static lv_obj_t *row, *battery, *body, *nub, *fill, *percent_label;

static lv_obj_t *Plain(lv_obj_t *parent)
{
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

static void Set_Colour(lv_color_t outline, lv_color_t bar, lv_color_t text)
{
  lv_obj_set_style_border_color(body, outline, 0);
  lv_obj_set_style_bg_color(nub, outline, 0);
  lv_obj_set_style_bg_color(fill, bar, 0);
  lv_obj_set_style_text_color(percent_label, text, 0);
}

static void Set_Opa(void *obj, int32_t v)
{
  lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

// Three soft blinks when the battery crosses into low or critical
static void Pulse(void)
{
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, battery);
  lv_anim_set_exec_cb(&a, Set_Opa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
  lv_anim_set_time(&a, 350);
  lv_anim_set_playback_time(&a, 350);
  lv_anim_set_repeat_count(&a, 3);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

static void Tick(lv_timer_t *timer)
{
  int percent = Battery_Percent();
  if (percent < 0) return;

  // Level 0 = fine, 1 = low, 2 = critical; a few percent of margin before it counts as
  // recovered, so a reading hovering at 20% doesn't blink on and off
  static int level = 0;
  int new_level = percent <= CRITICAL_PERCENT ? 2 : percent <= LOW_PERCENT ? 1 : 0;
  if (new_level < level && percent < (level == 2 ? CRITICAL_PERCENT : LOW_PERCENT) + 3) new_level = level;
  if (new_level != level) {
    level = new_level;
    if (level == 2) Set_Colour(COLOR_RED, COLOR_RED, COLOR_RED);
    else if (level == 1) Set_Colour(COLOR_AMBER, COLOR_AMBER, COLOR_AMBER);
    else Set_Colour(COLOR_OUTLINE, COLOR_FILL, COLOR_GREY);
    if (level > 0) Pulse();
  }

  static int shown = -1;
  if (percent != shown) {
    shown = percent;
    lv_obj_set_width(fill, LV_MAX(2, FILL_W * percent / 100));
    lv_label_set_text_fmt(percent_label, "%d%%", percent);
  }

  int page = Pages_Current();
  bool info_screen = page == 0 && Nav_Screen_Mode() != MODE_TURN;   // waiting, cruise, arrived...
  bool show = !Call_Screen_Showing() &&
              (Settings_Panel_Open() || level > 0 || info_screen || page == 1 || page == 2);
  if (show == lv_obj_has_flag(battery, LV_OBJ_FLAG_HIDDEN)) {
    if (show) lv_obj_clear_flag(battery, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(battery, LV_OBJ_FLAG_HIDDEN);
  }

  // Tell the app: on a change, and every 30 s (so it knows soon after connecting)
  static int sent = -1;
  static uint32_t sent_ms = 0;
  if (BLE_Link_Connected() && (percent != sent || millis() - sent_ms > 30000) && millis() - sent_ms > 5000) {
    char line[24];
    snprintf(line, sizeof(line), "BATTERY|%d|%.3f", percent, Battery_Volts());   // volts: for calibrating
    BLE_Link_Notify(line);
    sent = percent;
    sent_ms = millis();
  }
}

void Status_Bar_Create(void)
{
  row = Plain(lv_layer_top());
  lv_obj_set_size(row, LV_SIZE_CONTENT, 32);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 16, 0);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 54);       // between the rim and each page's first line

  battery = Plain(row);
  lv_obj_set_size(battery, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(battery, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(battery, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(battery, 0, 0);

  body = Plain(battery);
  lv_obj_set_size(body, BODY_W, BODY_H);
  lv_obj_set_style_radius(body, 5, 0);
  lv_obj_set_style_border_width(body, BORDER, 0);
  lv_obj_set_style_border_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(body, INSET, 0);

  fill = Plain(body);
  lv_obj_set_size(fill, FILL_W, BODY_H - 2 * (BORDER + INSET));
  lv_obj_set_style_radius(fill, 2, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);

  nub = Plain(battery);
  lv_obj_set_size(nub, 3, 6);
  lv_obj_set_style_radius(nub, 1, 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(nub, 0, 0);
  lv_obj_set_style_translate_x(nub, 1, 0);           // a hair's gap from the body

  percent_label = lv_label_create(battery);
  lv_obj_set_style_text_font(percent_label, &font_status_20, 0);
  lv_obj_set_style_pad_left(percent_label, 8, 0);
  lv_label_set_text(percent_label, "");

  Set_Colour(COLOR_OUTLINE, COLOR_FILL, COLOR_GREY);
  lv_obj_add_flag(battery, LV_OBJ_FLAG_HIDDEN);
  lv_timer_create(Tick, 1000, NULL);
}

lv_obj_t *Status_Bar_Row(void)
{
  return row;
}
