/*****************************************************************************
  | File        :   Journey_Screen.cpp
  | help        :   How far through the trip you are (see Journey_Screen.h).
                    The phone sends the numbers with its TRIP line about once a second; the time
                    riding counts up here between them so it ticks evenly.
******************************************************************************/
#include "Journey_Screen.h"
#include <Arduino.h>
#include <stdio.h>
#include "Pages.h"
#include "Settings.h"

LV_FONT_DECLARE(font_big_104);
LV_FONT_DECLARE(font_title_44);
LV_FONT_DECLARE(font_text_32);
LV_FONT_DECLARE(font_small_26);

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_GREY      lv_color_hex(0xA8A8A8)
#define COLOR_DIM       lv_color_hex(0x6A6A6A)
#define COLOR_TRACK     lv_color_hex(0x262626)

#define RING_SIZE       462
#define RING_WIDTH      8

static lv_obj_t *root, *ring;
static lv_obj_t *trip_group, *none_label;
static lv_obj_t *done_label, *total_label, *time_label, *eta_label, *avg_label;
static bool visible = false;

static bool have_trip = false;
static float km_left = 0, km_total = 0;
static uint32_t elapsed_at_msg = 0, msg_ms = 0;
static char eta_text[8] = "";

static lv_obj_t *Box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, h);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return box;
}

static lv_obj_t *Label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

// Setting a label's text redraws it even when nothing changed: only set it on a change
static void Set_Text(lv_obj_t *label, const char *text)
{
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// One stat column: value over a small caption
static lv_obj_t *Stat(lv_obj_t *row, const char *caption)
{
  lv_obj_t *col = Box(row, 124, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *value = Label(col, &font_text_32, lv_color_white());
  lv_obj_t *cap = Label(col, &font_small_26, COLOR_DIM);
  lv_label_set_text(cap, caption);
  return value;
}

static void Refresh(lv_timer_t *timer)
{
  if (!visible) return;
  lv_obj_add_flag(have_trip ? none_label : trip_group, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(have_trip ? trip_group : none_label, LV_OBJ_FLAG_HIDDEN);
  if (!have_trip) {
    lv_arc_set_value(ring, 0);
    return;
  }
  float done = LV_MAX(km_total - km_left, 0.0f);
  uint32_t elapsed = elapsed_at_msg + lv_tick_elaps(msg_ms) / 1000;
  char text[24];

  snprintf(text, sizeof(text), "%.1f", done);
  Set_Text(done_label, text);
  snprintf(text, sizeof(text), "of %.1f km", km_total);
  Set_Text(total_label, text);
  snprintf(text, sizeof(text), "%lu:%02lu", (unsigned long)(elapsed / 3600), (unsigned long)(elapsed / 60 % 60));
  Set_Text(time_label, text);
  char clock[12];
  Settings_Format_Clock(eta_text, clock, sizeof(clock));
  Set_Text(eta_label, clock);
  if (elapsed >= 60) snprintf(text, sizeof(text), "%d km/h", (int)lroundf(done / (elapsed / 3600.0f)));
  else snprintf(text, sizeof(text), "-");
  Set_Text(avg_label, text);

  int16_t deg = km_total > 0 ? (int16_t)LV_CLAMP(0.0f, done / km_total * 360, 360.0f) : 0;
  if (lv_arc_get_value(ring) != deg) lv_arc_set_value(ring, deg);
}

void Journey_Set(const char *eta, float left, float total, uint32_t elapsed_s)
{
  strlcpy(eta_text, eta, sizeof(eta_text));
  km_left = left;
  km_total = total > 0 ? total : left;        // older phone apps don't send the total
  elapsed_at_msg = elapsed_s;
  msg_ms = lv_tick_get();
  have_trip = true;
  Refresh(NULL);
}

void Journey_Clear(void)
{
  have_trip = false;
  Refresh(NULL);
}

void Journey_Screen_Create(lv_obj_t *parent)
{
  root = Box(parent, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  // Trip progress round the edge (starts at 12 o'clock, fills clockwise), on a faint track
  ring = lv_arc_create(root);
  lv_obj_set_size(ring, RING_SIZE, RING_SIZE);
  lv_obj_center(ring);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(ring, 0, 0);
  lv_obj_set_style_arc_width(ring, RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, COLOR_TRACK, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, COLOR_BLUE, LV_PART_INDICATOR);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_range(ring, 0, 360);
  lv_arc_set_value(ring, 0);

  trip_group = Box(root, LV_PCT(100), LV_PCT(100));

  lv_obj_t *caption = Label(trip_group, &font_small_26, COLOR_GREY);
  lv_label_set_text(caption, "TRIP");
  lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 104);

  // Big km done with the unit beside it, sharing a baseline
  lv_obj_t *big = Box(trip_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(big, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(big, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(big, 8, 0);
  lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 136);
  done_label = Label(big, &font_big_104, lv_color_white());
  lv_obj_t *unit = Label(big, &font_title_44, COLOR_GREY);
  lv_label_set_text(unit, "km");
  lv_obj_set_style_translate_y(unit, font_title_44.base_line - font_big_104.base_line, 0);

  total_label = Label(trip_group, &font_text_32, COLOR_GREY);
  lv_obj_align(total_label, LV_ALIGN_TOP_MID, 0, 246);

  lv_obj_t *row = Box(trip_group, 380, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 304);
  time_label = Stat(row, "RIDING");
  eta_label = Stat(row, "ARRIVE");
  avg_label = Stat(row, "AVG");

  none_label = Label(root, &font_text_32, COLOR_DIM);
  lv_label_set_text(none_label, "No trip");
  lv_obj_center(none_label);

  Pages_Add_Dots(root, 1);
  lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
  lv_timer_create(Refresh, 1000, NULL);
}

void Journey_Screen_Show(bool show)
{
  visible = show;
  if (show) {
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    Refresh(NULL);
  } else {
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
  }
}
