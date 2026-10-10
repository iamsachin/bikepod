/*****************************************************************************
  | File        :   Accel_Screen.cpp
  | help        :   Acceleration page (see Accel_Screen.h): lateral g left,
                    a g-circle with a dot in the middle, longitudinal g right, the ride's peaks,
                    and a short history (blue = pull / braking, grey = cornering).
******************************************************************************/
#include "Accel_Screen.h"
#include <Arduino.h>
#include <math.h>
#include "Lean_Screen.h"
#include "Pages.h"

LV_FONT_DECLARE(font_title_44);
LV_FONT_DECLARE(font_small_26);

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_RING      lv_color_hex(0x4A4A4A)
#define COLOR_FAINT     lv_color_hex(0x2A2A2A)
#define COLOR_GREY      lv_color_hex(0xA8A8A8)
#define COLOR_DIM       lv_color_hex(0x6A6A6A)
#define MIDDLE_DOT      "\xC2\xB7"

#define CIRCLE_X        240
#define CIRCLE_Y        210
#define CIRCLE_R        64        // 1 g at the outer ring
#define DOT_D           16
#define GRAPH_W         (MOTION_HISTORY_POINTS * 2)
#define GRAPH_H         40
#define DEADBAND        0.02f     // smaller than this shows as 0.00 (sensor noise standing still)

static lv_obj_t *root, *dot;
static lv_obj_t *lat_value, *lat_caption, *lon_value, *lon_caption;
static lv_obj_t *peaks_lon, *peaks_lat;
static lv_obj_t *graph_lat, *graph_lon;
static lv_point_t pts_lat[MOTION_HISTORY_POINTS], pts_lon[MOTION_HISTORY_POINTS];
static bool visible = false;

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

static lv_obj_t *Ring(lv_obj_t *parent, lv_coord_t r, lv_coord_t width, lv_color_t color)
{
  lv_obj_t *ring = Box(parent, 2 * r, 2 * r);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, width, 0);
  lv_obj_set_style_border_color(ring, color, 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
  lv_obj_set_pos(ring, CIRCLE_X - r, CIRCLE_Y - r);
  return ring;
}

static lv_obj_t *Line(lv_obj_t *parent, lv_coord_t width, lv_color_t color)
{
  lv_obj_t *line = lv_line_create(parent);
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_color(line, color, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  return line;
}

// A value over its caption, centred on x
static void Column(lv_coord_t x, lv_obj_t **value, lv_obj_t **caption)
{
  lv_obj_t *col = Box(root, 150, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(col, x - 75, CIRCLE_Y - 42);
  *value = Label(col, &font_title_44, lv_color_white());
  *caption = Label(col, &font_small_26, COLOR_DIM);
}

static void Set_Text(lv_obj_t *label, const char *text)
{
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static float Shown(float g)
{
  return fabsf(g) < DEADBAND ? 0 : g;
}

static void Refresh(lv_timer_t *timer)
{
  if (!visible) return;
  static Motion m;                                      // big (history): not on the stack
  Motion_Get(&m);
  float lat = Shown(m.lateral_g), lon = Shown(m.longitudinal_g);

  char text[64];
  snprintf(text, sizeof(text), "%.2f", fabsf(lat));
  Set_Text(lat_value, text);
  Set_Text(lat_caption, lat > 0 ? "LAT g " MIDDLE_DOT " R" : lat < 0 ? "LAT g " MIDDLE_DOT " L" : "LAT g");
  snprintf(text, sizeof(text), "%s%.2f", lon > 0 ? "+" : lon < 0 ? "-" : "", fabsf(lon));
  Set_Text(lon_value, text);
  Set_Text(lon_caption, lon > 0 ? "LON g " MIDDLE_DOT " ACCEL" : lon < 0 ? "LON g " MIDDLE_DOT " BRAKE" : "LON g");

  // Dot: right for a right-hand turn, up for speeding up; at most at the 1 g ring
  float len = sqrtf(lat * lat + lon * lon), k = len > 1 ? 1 / len : 1;
  lv_obj_set_pos(dot, (lv_coord_t)lroundf(CIRCLE_X + lat * k * CIRCLE_R - DOT_D / 2.0f),
                 (lv_coord_t)lroundf(CIRCLE_Y - lon * k * CIRCLE_R - DOT_D / 2.0f));

  // Signs only on non-zero values ("+0.00" / "-0.00" read oddly)
  char accel[12], decel[12];
  snprintf(accel, sizeof(accel), "%s%.2f", m.peak_accel >= 0.005f ? "+" : "", m.peak_accel);
  snprintf(decel, sizeof(decel), "%s%.2f", m.peak_decel <= -0.005f ? "-" : "", fabsf(m.peak_decel));
  snprintf(text, sizeof(text), "PEAK ACCEL #ffffff %s#   DECEL #ffffff %s#", accel, decel);
  Set_Text(peaks_lon, text);
  snprintf(text, sizeof(text), "PEAK LAT  L #ffffff %.2f#   R #ffffff %.2f#", m.peak_lat_left, m.peak_lat_right);
  Set_Text(peaks_lat, text);

  static uint8_t drawn_head = 0xFF;                     // the graph only changes with a new point
  if (drawn_head != m.history_head) {
    drawn_head = m.history_head;
    // +-1 g = +-18 px about the middle; a 3-point average keeps the lines smooth
    for (int i = 0; i < MOTION_HISTORY_POINTS; i++) {
      float sum_lat = 0, sum_lon = 0;
      int n = 0;
      for (int j = LV_MAX(0, i - 1); j <= LV_MIN(MOTION_HISTORY_POINTS - 1, i + 1); j++) {
        sum_lat += m.history_lat[j];
        sum_lon += m.history_lon[j];
        n++;
      }
      float a = fmaxf(-1, fminf(1, sum_lat / n)), b = fmaxf(-1, fminf(1, sum_lon / n));
      pts_lat[i] = { (lv_coord_t)(i * 2), (lv_coord_t)lroundf(GRAPH_H / 2 - a * 18) };
      pts_lon[i] = { (lv_coord_t)(i * 2), (lv_coord_t)lroundf(GRAPH_H / 2 - b * 18) };
    }
    lv_line_set_points(graph_lat, pts_lat, MOTION_HISTORY_POINTS);
    lv_line_set_points(graph_lon, pts_lon, MOTION_HISTORY_POINTS);
  }
}

void Accel_Screen_Create(lv_obj_t *parent)
{
  root = Box(parent, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  lv_obj_t *title = Label(root, &font_small_26, COLOR_GREY);
  lv_label_set_text(title, "ACCELERATION " MIDDLE_DOT " ESTIMATED");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 98);

  // g-circle: 1 g ring, 0.5 g ring, crosshair, and the dot
  Ring(root, CIRCLE_R, 3, COLOR_RING);
  Ring(root, CIRCLE_R / 2, 2, COLOR_FAINT);
  static lv_point_t across[] = { {0, 0}, {2 * (CIRCLE_R - 3), 0} };
  static lv_point_t down[] = { {0, 0}, {0, 2 * (CIRCLE_R - 3)} };
  lv_obj_t *h = Line(root, 2, COLOR_FAINT);
  lv_line_set_points(h, across, 2);
  lv_obj_set_pos(h, CIRCLE_X - CIRCLE_R + 3, CIRCLE_Y - 1);
  lv_obj_t *v = Line(root, 2, COLOR_FAINT);
  lv_line_set_points(v, down, 2);
  lv_obj_set_pos(v, CIRCLE_X - 1, CIRCLE_Y - CIRCLE_R + 3);
  dot = Box(root, DOT_D, DOT_D);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, COLOR_BLUE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_pos(dot, CIRCLE_X - DOT_D / 2, CIRCLE_Y - DOT_D / 2);

  Column(100, &lat_value, &lat_caption);
  Column(380, &lon_value, &lon_caption);

  peaks_lon = Label(root, &font_small_26, COLOR_GREY);
  lv_label_set_recolor(peaks_lon, true);
  lv_obj_align(peaks_lon, LV_ALIGN_TOP_MID, 0, 294);
  peaks_lat = Label(root, &font_small_26, COLOR_GREY);
  lv_label_set_recolor(peaks_lat, true);
  lv_obj_align(peaks_lat, LV_ALIGN_TOP_MID, 0, 324);

  lv_obj_t *graph_box = Box(root, GRAPH_W, GRAPH_H);
  lv_obj_align(graph_box, LV_ALIGN_TOP_MID, 0, 368);
  static lv_point_t mid[] = { {0, GRAPH_H / 2}, {GRAPH_W - 2, GRAPH_H / 2} };
  lv_obj_t *mid_line = Line(graph_box, 2, COLOR_FAINT);
  lv_line_set_points(mid_line, mid, 2);
  graph_lat = Line(graph_box, 3, COLOR_GREY);
  graph_lon = Line(graph_box, 3, COLOR_BLUE);

  Pages_Add_Dots(root, PAGE_ACCEL);
  lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
  lv_timer_create(Refresh, 33, NULL);
}

void Accel_Screen_Show(bool show)
{
  visible = show;
  if (show) {
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    Refresh(NULL);
  } else {
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
  }
}
