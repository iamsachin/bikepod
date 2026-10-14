/*****************************************************************************
  | File        :   Status_Bar.cpp
  | help        :   Top-centre status row (see Status_Bar.h).
******************************************************************************/
#include "Status_Bar.h"
#include "Nav_Screen.h"
#include "Pages.h"
#include "Phone_Screens.h"
#include "Settings.h"

LV_FONT_DECLARE(font_trip_28);
LV_FONT_DECLARE(font_phone_28);

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define ICON_TRIP       "\xEE\x95\x9D"      // U+E55D "navigation", Material Icons Round
#define ICON_PHONE      "\xEE\x8C\xAC"      // U+E32C "smartphone"
#define COLOR_RED       lv_color_hex(0xE5484D)

static lv_obj_t *row, *trip_btn, *phone_low;
static bool phone_low_on = false;     // the phone at 20% or less and not charging

static void Trip_Click(lv_event_t *e)
{
  Pages_Show(PAGE_NAV);
}

// The trip icon: while a trip is on (directions or rerouting), on the other pages
static void Tick(lv_timer_t *timer)
{
  Screen_Mode mode = Nav_Screen_Mode();
  bool trip = mode == MODE_TURN || mode == MODE_REROUTING;
  bool show = trip && Pages_Current() != PAGE_NAV && !Settings_Panel_Open() && !Call_Screen_Showing();
  if (show == lv_obj_has_flag(trip_btn, LV_OBJ_FLAG_HIDDEN)) {
    if (show) lv_obj_clear_flag(trip_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(trip_btn, LV_OBJ_FLAG_HIDDEN);
  }
  // Low phone battery: on every page (navigation drains the phone on long rides)
  bool low = phone_low_on && !Settings_Panel_Open() && !Call_Screen_Showing();
  if (low == lv_obj_has_flag(phone_low, LV_OBJ_FLAG_HIDDEN)) {
    if (low) lv_obj_clear_flag(phone_low, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(phone_low, LV_OBJ_FLAG_HIDDEN);
  }
}

void Status_Bar_Set_Phone(int percent, bool charging)
{
  phone_low_on = percent >= 0 && percent <= 20 && !charging;
}

void Status_Bar_Create(void)
{
  row = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(row);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(row, LV_SIZE_CONTENT, 32);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 22, 0);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 54);

  // First in the row, so it sits left of the music icon
  trip_btn = lv_label_create(row);
  lv_obj_set_style_text_font(trip_btn, &font_trip_28, 0);
  lv_obj_set_style_text_color(trip_btn, COLOR_BLUE, 0);
  lv_label_set_text(trip_btn, ICON_TRIP);
  lv_obj_add_flag(trip_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(trip_btn, 20);       // easier to hit with gloves
  lv_obj_add_event_cb(trip_btn, Trip_Click, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(trip_btn, LV_OBJ_FLAG_HIDDEN);

  phone_low = lv_label_create(row);
  lv_obj_set_style_text_font(phone_low, &font_phone_28, 0);
  lv_obj_set_style_text_color(phone_low, COLOR_RED, 0);
  lv_label_set_text(phone_low, ICON_PHONE);
  lv_obj_add_flag(phone_low, LV_OBJ_FLAG_HIDDEN);
  lv_timer_create(Tick, 200, NULL);
}

lv_obj_t *Status_Bar_Row(void)
{
  return row;
}
