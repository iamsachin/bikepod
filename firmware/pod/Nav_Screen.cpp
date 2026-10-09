/*****************************************************************************
  | File        :   Nav_Screen.cpp
  | help        :   Screens for the 480x480 round display: approaching turn, waiting for phone,
                    rerouting, arrived, cruise. Black background, white text, one accent colour.
******************************************************************************/
#include "Nav_Screen.h"
#include <stdio.h>
#include <math.h>

LV_FONT_DECLARE(font_dist_132);   // Inter Display Bold, digits and "." only
LV_FONT_DECLARE(font_unit_44);    // Inter SemiBold, ASCII
LV_FONT_DECLARE(font_road_30);    // Inter Medium, ASCII

#define COLOR_ACCENT    lv_color_hex(0x00D1FF)
#define COLOR_RING_BG   lv_color_hex(0x1C1C1C)
#define COLOR_TEXT      lv_color_hex(0xFFFFFF)
#define COLOR_UNIT      lv_color_hex(0x9A9A9A)
#define COLOR_ROAD      lv_color_hex(0xC8C8C8)
#define COLOR_AMBER     lv_color_hex(0xFFB000)

#define RING_SIZE       468
#define RING_WIDTH      14
#define ARROW_BOX       150
#define ARROW_WIDTH     22
#define ROAD_LINE_SPACE (-4)

static lv_obj_t *ring;
static lv_obj_t *arrow_stem;
static lv_obj_t *arrow_head;
static lv_obj_t *dist_label;
static lv_obj_t *unit_label;
static lv_obj_t *road_label;
static lv_obj_t *turn_group;      // arrow + distance + road
static lv_obj_t *info_group;      // waiting / rerouting / arrived / cruise
static lv_obj_t *info_top;
static lv_obj_t *info_main;
static lv_obj_t *info_bottom;
static Screen_Mode mode = MODE_WAITING;
static bool stale = false;

// Arrow shapes inside a 150x150 box: a stem with a bevelled bend, plus a chevron head
static lv_point_t right_stem[] = { {45, 140}, {45, 80}, {75, 50}, {125, 50} };
static lv_point_t right_head[] = { {97, 22}, {125, 50}, {97, 78} };
static lv_point_t left_stem[]  = { {105, 140}, {105, 80}, {75, 50}, {25, 50} };
static lv_point_t left_head[]  = { {53, 22}, {25, 50}, {53, 78} };
static lv_point_t ahead_stem[] = { {75, 140}, {75, 20} };
static lv_point_t ahead_head[] = { {45, 50}, {75, 20}, {105, 50} };

static lv_obj_t *Arrow_Line_Create(lv_obj_t *parent)
{
  lv_obj_t *line = lv_line_create(parent);
  lv_obj_set_style_line_width(line, ARROW_WIDTH, 0);
  lv_obj_set_style_line_color(line, COLOR_ACCENT, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  return line;
}

/******************************** Smooth progress ring ********************************
 * Updates arrive roughly once per GPS fix, at uneven intervals. Instead of jumping (or
 * restarting an animation each time), the ring keeps moving at the measured speed between
 * updates and eases towards each new value. LVGL 8 draws arcs in whole degrees (about 4 px at
 * this radius), so a round dot drawn at the exact tip position hides those 4 px steps.
 ***************************************************************************************/
#define RING_FRAME_MS     16                          // ~60 fps
#define RING_RADIUS       ((RING_SIZE - RING_WIDTH) / 2)  // centre line of the ring
#define RING_MAX_COAST_MS 1500                        // stop predicting if updates stop

static lv_obj_t *ring_tip;
static float ring_target = 0;     // last value received (0..1000)
static float ring_rate = 0;       // measured speed, units per ms
static float ring_shown = 0;      // value currently drawn
static uint32_t ring_target_ms = 0;

static void Ring_Draw(float value)
{
  int32_t deg = (int32_t)(value * 360.0f / 1000.0f);   // whole degrees, always behind the tip
  lv_arc_set_value(ring, deg);

  if (value <= 0) {
    lv_obj_add_flag(ring_tip, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  float rad = value / 1000.0f * 2.0f * (float)M_PI;     // 0 = 12 o'clock, clockwise
  lv_obj_clear_flag(ring_tip, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(ring_tip, LV_ALIGN_CENTER,
               (lv_coord_t)lroundf(RING_RADIUS * sinf(rad)),
               (lv_coord_t)lroundf(-RING_RADIUS * cosf(rad)));
}

static void Ring_Frame(lv_timer_t *timer)
{
  float coast = LV_MIN(lv_tick_elaps(ring_target_ms), RING_MAX_COAST_MS);
  float predicted = LV_MIN(ring_target + ring_rate * coast, 1000.0f);
  float next = ring_shown + (predicted - ring_shown) * 0.15f;   // ease in, ~100 ms time constant
  if (next < ring_shown) next = ring_shown;                    // never run backwards mid-approach
  if (fabsf(next - ring_shown) < 0.05f) return;                // nothing visible to redraw
  ring_shown = next;
  Ring_Draw(ring_shown);
}

static void Ring_Set_Target(uint16_t target)
{
  uint32_t gap_ms = lv_tick_elaps(ring_target_ms);
  ring_target_ms = lv_tick_get();

  // A new turn (ring goes back to empty): restart from the new value
  if (target + 5 < ring_target) {
    ring_target = ring_shown = target;
    ring_rate = 0;
    Ring_Draw(ring_shown);
    return;
  }
  if (gap_ms >= 50 && gap_ms <= 3000) {
    float measured = (target - ring_target) / gap_ms;
    ring_rate = ring_rate == 0 ? measured : 0.5f * ring_rate + 0.5f * measured;
  } else if (gap_ms > 3000) {
    ring_rate = 0;
  }
  ring_target = target;
}

// For the non-turn screens: a still ring (0 = empty, 1000 = full) in the given colour
static void Ring_Set_Fixed(uint16_t value, lv_color_t color)
{
  lv_obj_set_style_arc_color(ring, color, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(ring_tip, color, 0);
  ring_target = ring_shown = value;
  ring_rate = 0;
  Ring_Draw(ring_shown);
}

static lv_obj_t *Full_Screen_Group(lv_obj_t *parent)
{
  lv_obj_t *group = lv_obj_create(parent);
  lv_obj_remove_style_all(group);
  lv_obj_set_size(group, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return group;
}

static lv_obj_t *Info_Label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

static void Set_Mode(Screen_Mode new_mode)
{
  mode = new_mode;
  if (mode == MODE_TURN) {
    lv_obj_clear_flag(turn_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(info_group, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(turn_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(info_group, LV_OBJ_FLAG_HIDDEN);
  }
}

static void Set_Info(const char *top, const char *main, const lv_font_t *main_font,
                     lv_color_t main_color, const char *bottom)
{
  lv_label_set_text(info_top, top);
  lv_label_set_text(info_main, main);
  lv_obj_set_style_text_font(info_main, main_font, 0);
  lv_obj_set_style_text_color(info_main, main_color, 0);
  lv_label_set_text(info_bottom, bottom);
  // Empty labels would still take up a line and push the main text off centre
  lv_obj_add_flag(info_top, LV_OBJ_FLAG_HIDDEN);
  if (top[0]) lv_obj_clear_flag(info_top, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(info_bottom, LV_OBJ_FLAG_HIDDEN);
  if (bottom[0]) lv_obj_clear_flag(info_bottom, LV_OBJ_FLAG_HIDDEN);
}

void Nav_Screen_Create(void)
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Progress ring around the edge: starts at 12 o'clock and fills clockwise
  ring = lv_arc_create(scr);
  lv_obj_set_size(ring, RING_SIZE, RING_SIZE);
  lv_obj_center(ring);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_range(ring, 0, 360);
  lv_arc_set_value(ring, 0);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(ring, 0, 0);
  lv_obj_set_style_arc_width(ring, RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, COLOR_RING_BG, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, COLOR_ACCENT, LV_PART_INDICATOR);

  turn_group = Full_Screen_Group(scr);

  // Turn arrow, upper part of the circle
  lv_obj_t *arrow_box = lv_obj_create(turn_group);
  lv_obj_remove_style_all(arrow_box);
  lv_obj_set_size(arrow_box, ARROW_BOX, ARROW_BOX);
  lv_obj_align(arrow_box, LV_ALIGN_CENTER, 0, -112);
  arrow_stem = Arrow_Line_Create(arrow_box);
  arrow_head = Arrow_Line_Create(arrow_box);

  // Distance: huge number plus a smaller unit, sitting on the same baseline
  lv_obj_t *dist_row = lv_obj_create(turn_group);
  lv_obj_remove_style_all(dist_row);
  lv_obj_set_size(dist_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(dist_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dist_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(dist_row, 8, 0);
  lv_obj_align(dist_row, LV_ALIGN_CENTER, 0, 36);

  dist_label = lv_label_create(dist_row);
  lv_obj_set_style_text_font(dist_label, &font_dist_132, 0);
  lv_obj_set_style_text_color(dist_label, COLOR_TEXT, 0);

  unit_label = lv_label_create(dist_row);
  lv_obj_set_style_text_font(unit_label, &font_unit_44, 0);
  lv_obj_set_style_text_color(unit_label, COLOR_UNIT, 0);
  lv_obj_set_style_pad_bottom(unit_label, font_dist_132.base_line - font_unit_44.base_line, 0);

  // Road name, lower part of the circle
  // Up to two lines; "..." only if it still doesn't fit. Width is what the circle allows at line 2.
  road_label = lv_label_create(turn_group);
  lv_obj_set_size(road_label, 290, 2 * font_road_30.line_height + ROAD_LINE_SPACE);
  lv_label_set_long_mode(road_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(road_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(road_label, &font_road_30, 0);
  lv_obj_set_style_text_line_space(road_label, ROAD_LINE_SPACE, 0);
  lv_obj_set_style_text_color(road_label, COLOR_ROAD, 0);
  lv_obj_align(road_label, LV_ALIGN_TOP_MID, 0, 343);

  ring_tip = lv_obj_create(scr);
  lv_obj_remove_style_all(ring_tip);
  lv_obj_set_size(ring_tip, RING_WIDTH, RING_WIDTH);
  lv_obj_set_style_radius(ring_tip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ring_tip, COLOR_ACCENT, 0);
  lv_obj_set_style_bg_opa(ring_tip, LV_OPA_COVER, 0);
  lv_obj_add_flag(ring_tip, LV_OBJ_FLAG_HIDDEN);
  lv_timer_create(Ring_Frame, RING_FRAME_MS, NULL);

  // Info screens: up to three centred lines, the middle one large
  info_group = Full_Screen_Group(scr);
  lv_obj_set_flex_flow(info_group, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(info_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(info_group, 4, 0);
  info_top = Info_Label(info_group, &font_road_30, COLOR_ROAD);
  info_main = Info_Label(info_group, &font_unit_44, COLOR_TEXT);
  info_bottom = Info_Label(info_group, &font_road_30, COLOR_UNIT);

  Nav_Screen_Show_Waiting();
}

void Nav_Screen_Show_Waiting(void)
{
  Set_Mode(MODE_WAITING);
  Nav_Screen_Set_Stale(false);
  Ring_Set_Fixed(0, COLOR_ACCENT);
  Set_Info("", "BikePod", &font_unit_44, COLOR_TEXT, "Waiting for phone");
}

void Nav_Screen_Show_Rerouting(void)
{
  if (mode != MODE_REROUTING) {                // repeats of the same screen only update the text
    Set_Mode(MODE_REROUTING);
    Ring_Set_Fixed(1000, COLOR_AMBER);
  }
  Set_Info("", "Rerouting", &font_unit_44, COLOR_AMBER, "Finding a new route");
}

void Nav_Screen_Show_Arrived(float trip_km, uint32_t trip_min)
{
  char summary[32];
  snprintf(summary, sizeof(summary), "%.1f km  /  %lu min", trip_km, (unsigned long)trip_min);
  if (mode != MODE_ARRIVED) {                // repeats of the same screen only update the text
    Set_Mode(MODE_ARRIVED);
    Ring_Set_Fixed(1000, COLOR_ACCENT);
  }
  Set_Info("", "Arrived", &font_unit_44, COLOR_ACCENT, summary);
}

void Nav_Screen_Show_Cruise(uint32_t speed_kmh, const char *clock)
{
  char speed[8];
  snprintf(speed, sizeof(speed), "%lu", (unsigned long)speed_kmh);
  if (mode != MODE_CRUISE) {                // repeats of the same screen only update the text
    Set_Mode(MODE_CRUISE);
    Ring_Set_Fixed(0, COLOR_ACCENT);
  }
  Set_Info(clock, speed, &font_dist_132, COLOR_TEXT, "km/h");
}

// No message for a while: fade everything so an old distance never looks current
void Nav_Screen_Set_Stale(bool now_stale)
{
  if (now_stale == stale) return;
  stale = now_stale;
  lv_opa_t opa = stale ? LV_OPA_40 : LV_OPA_COVER;
  lv_obj_set_style_opa(turn_group, opa, 0);
  lv_obj_set_style_opa(info_group, opa, 0);
  lv_obj_set_style_opa(ring, opa, 0);
  lv_obj_set_style_opa(ring_tip, opa, 0);
}

Screen_Mode Nav_Screen_Mode(void)
{
  return mode;
}

void Nav_Screen_Update(Turn_Dir dir, uint32_t distance_m, uint16_t progress, const char *road)
{
  if (mode != MODE_TURN) {
    Set_Mode(MODE_TURN);
    Ring_Set_Fixed(0, COLOR_ACCENT);   // a fresh approach after another screen
  }
  switch (dir) {
    case TURN_LEFT:
      lv_line_set_points(arrow_stem, left_stem, 4);
      lv_line_set_points(arrow_head, left_head, 3);
      break;
    case TURN_RIGHT:
      lv_line_set_points(arrow_stem, right_stem, 4);
      lv_line_set_points(arrow_head, right_head, 3);
      break;
    default:
      lv_line_set_points(arrow_stem, ahead_stem, 2);
      lv_line_set_points(arrow_head, ahead_head, 3);
      break;
  }

  // Round like a car navigator: 1.2 km, 350 m (10 m steps), 45 m (5 m steps)
  char num[12];
  if (distance_m >= 1000) {
    snprintf(num, sizeof(num), "%.1f", distance_m / 1000.0f);
    lv_label_set_text(unit_label, "km");
  } else {
    uint32_t step = distance_m >= 100 ? 10 : 5;
    snprintf(num, sizeof(num), "%lu", (unsigned long)((distance_m + step / 2) / step * step));
    lv_label_set_text(unit_label, "m");
  }
  lv_label_set_text(dist_label, num);

  Ring_Set_Target(progress);
  lv_label_set_text(road_label, road);
}

/********************************* Fake data for testing without a phone *********************************/
struct Fake_Turn {
  Turn_Dir dir;
  uint32_t start_m;
  const char *road;
};

static const Fake_Turn fake_turns[] = {
  { TURN_RIGHT,    400,  "MG Road" },
  { TURN_LEFT,     1500, "Outer Ring Road" },
  { TURN_STRAIGHT, 250,  "NH 48" },
};

#define FAKE_TICK_MS    100
#define FAKE_TICKS      80      // each approach lasts 8 s
#define FAKE_HOLD_TICKS 15      // stay on "0 m" for 1.5 s before the next turn

static void Fake_Data_Tick(lv_timer_t *timer)
{
  static uint8_t turn = 0;
  static uint16_t tick = 0;
  const Fake_Turn &t = fake_turns[turn];

  uint16_t done = tick > FAKE_TICKS ? FAKE_TICKS : tick;
  uint32_t distance = t.start_m - t.start_m * done / FAKE_TICKS;
  Nav_Screen_Update(t.dir, distance, done * 1000 / FAKE_TICKS, t.road);

  if (++tick > FAKE_TICKS + FAKE_HOLD_TICKS) {
    tick = 0;
    turn = (turn + 1) % (sizeof(fake_turns) / sizeof(fake_turns[0]));
  }
}

static lv_timer_t *fake_timer = NULL;

void Nav_Fake_Data_Start(void)
{
  if (!fake_timer) fake_timer = lv_timer_create(Fake_Data_Tick, FAKE_TICK_MS, NULL);
}

void Nav_Fake_Data_Stop(void)
{
  if (fake_timer) {
    lv_timer_del(fake_timer);
    fake_timer = NULL;
  }
}
