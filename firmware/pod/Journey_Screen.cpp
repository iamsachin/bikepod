/*****************************************************************************
  | File        :   Journey_Screen.cpp
  | help        :   How far through the trip you are (see Journey_Screen.h).
                    The phone sends the numbers with its TRIP line about once a second; the time
                    riding counts up here between them so it ticks evenly.
******************************************************************************/
#include "Journey_Screen.h"
#include "Smooth_Arc.h"
#include <Arduino.h>
#include <stdio.h>
#include "Pages.h"
#include "Settings.h"

LV_FONT_DECLARE(font_big_104);
LV_FONT_DECLARE(font_title_44);
LV_FONT_DECLARE(font_text_32);
LV_FONT_DECLARE(font_small_26);
LV_FONT_DECLARE(font_info_24);

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_GREY      lv_color_hex(0xA8A8A8)
#define COLOR_DIM       lv_color_hex(0x6A6A6A)
#define COLOR_TRACK     lv_color_hex(0x262626)
#define COLOR_AMBER     lv_color_hex(0xF5B83D)
#define COLOR_RED       lv_color_hex(0xE5484D)
#define COLOR_GREEN     lv_color_hex(0x34C759)
#define ICON_PHONE      "\xEE\x8C\xAC"     // U+E32C smartphone
#define ICON_FUEL       "\xEE\x95\x86"     // U+E546 local_gas_station
#define ICON_RAIN       "\xEE\x9E\x98"     // U+E798 water_drop

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

// The row under the stats: phone battery, next fuel, rain (each hidden until known)
static lv_obj_t *phone_cell, *fuel_cell, *rain_cell;
static lv_obj_t *phone_icon, *phone_val, *fuel_val, *rain_icon, *rain_val;

static lv_obj_t *Info_Cell(lv_obj_t *row, const char *glyph, lv_color_t color, lv_obj_t **icon, lv_obj_t **value)
{
  lv_obj_t *cell = Box(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(cell, 5, 0);
  *icon = Label(cell, &font_info_24, color);
  lv_label_set_text(*icon, glyph);
  *value = Label(cell, &font_small_26, COLOR_GREY);
  lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
  return cell;
}

static void Show_Cell(lv_obj_t *cell, bool show)
{
  if (show == lv_obj_has_flag(cell, LV_OBJ_FLAG_HIDDEN)) {
    if (show) lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
  }
}

void Journey_Set_Phone(int percent, bool charging)
{
  char text[12];
  snprintf(text, sizeof(text), "%d%%", percent);
  Set_Text(phone_val, text);
  lv_color_t c = charging ? COLOR_GREEN : percent <= 20 ? COLOR_RED : COLOR_GREY;
  lv_obj_set_style_text_color(phone_icon, c, 0);
  Show_Cell(phone_cell, percent >= 0);
}

void Journey_Set_Fuel(uint32_t metres)
{
  char text[16];
  if (metres >= 10000) snprintf(text, sizeof(text), "%lu km", (unsigned long)((metres + 500) / 1000));
  else snprintf(text, sizeof(text), "%.1f km", metres / 1000.0f);
  Set_Text(fuel_val, text);
  Show_Cell(fuel_cell, metres > 0);
}

void Journey_Set_Rain(int minutes)
{
  char text[16];
  if (minutes < 0) strcpy(text, "Dry");
  else if (minutes == 0) strcpy(text, "Raining");
  else if (minutes < 60) snprintf(text, sizeof(text), "in %d min", minutes);
  else snprintf(text, sizeof(text), "in %d h", (minutes + 30) / 60);    // "in 115 min" ran into the ring
  Set_Text(rain_val, text);
  lv_obj_set_style_text_color(rain_icon, minutes >= 0 && minutes <= 30 ? COLOR_BLUE : COLOR_DIM, 0);
  Show_Cell(rain_cell, true);
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
  // Only on a change: LVGL redraws an object whenever it's hidden or shown, even if it already
  // was, and this runs every second (the whole page every second: 140 ms frames)
  if (lv_obj_has_flag(trip_group, LV_OBJ_FLAG_HIDDEN) == have_trip) {
    lv_obj_add_flag(have_trip ? none_label : trip_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(have_trip ? trip_group : none_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (!have_trip) {
    Smooth_Arc_Set(ring, 0, 0);
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

  Smooth_Arc_Set(ring, 0, km_total > 0 ? LV_CLAMP(0.0f, done / km_total * 360, 360.0f) : 0);
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
  // (Smooth_Arc: LVGL's own arc needed a mask buffer from its memory pool for this radius)
  lv_obj_t *track = Smooth_Arc_Create(root, (RING_SIZE - RING_WIDTH) / 2.0f, RING_WIDTH, COLOR_TRACK);
  Smooth_Arc_Set(track, 0, 360);
  ring = Smooth_Arc_Create(root, (RING_SIZE - RING_WIDTH) / 2.0f, RING_WIDTH, COLOR_BLUE);

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

  // Only as wide as its contents, centred: the round screen is narrow this low down
  lv_obj_t *info = Box(trip_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(info, 16, 0);
  lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 372);
  phone_cell = Info_Cell(info, ICON_PHONE, COLOR_GREY, &phone_icon, &phone_val);
  lv_obj_t *fuel_icon;
  fuel_cell = Info_Cell(info, ICON_FUEL, COLOR_AMBER, &fuel_icon, &fuel_val);
  rain_cell = Info_Cell(info, ICON_RAIN, COLOR_DIM, &rain_icon, &rain_val);

  none_label = Label(root, &font_text_32, COLOR_DIM);
  lv_label_set_text(none_label, "No trip");
  lv_obj_center(none_label);
  lv_obj_add_flag(trip_group, LV_OBJ_FLAG_HIDDEN);     // starts as "No trip" (Refresh toggles on changes only)

  Pages_Add_Dots(root, PAGE_JOURNEY);
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
