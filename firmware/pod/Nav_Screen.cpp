/*****************************************************************************
  | File        :   Nav_Screen.cpp
  | help        :   "Approaching turn" screen for the 480x480 round display.
                    Black background, white text, one accent colour (cyan).
******************************************************************************/
#include "Nav_Screen.h"
#include <stdio.h>

LV_FONT_DECLARE(font_dist_132);   // Inter Display Bold, digits and "." only
LV_FONT_DECLARE(font_unit_44);    // Inter SemiBold, ASCII
LV_FONT_DECLARE(font_road_30);    // Inter Medium, ASCII

#define COLOR_ACCENT    lv_color_hex(0x00D1FF)
#define COLOR_RING_BG   lv_color_hex(0x1C1C1C)
#define COLOR_TEXT      lv_color_hex(0xFFFFFF)
#define COLOR_UNIT      lv_color_hex(0x9A9A9A)
#define COLOR_ROAD      lv_color_hex(0xC8C8C8)

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
  lv_arc_set_range(ring, 0, 1000);
  lv_arc_set_value(ring, 0);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(ring, 0, 0);
  lv_obj_set_style_arc_width(ring, RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, COLOR_RING_BG, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, COLOR_ACCENT, LV_PART_INDICATOR);

  // Turn arrow, upper part of the circle
  lv_obj_t *arrow_box = lv_obj_create(scr);
  lv_obj_remove_style_all(arrow_box);
  lv_obj_set_size(arrow_box, ARROW_BOX, ARROW_BOX);
  lv_obj_align(arrow_box, LV_ALIGN_CENTER, 0, -112);
  arrow_stem = Arrow_Line_Create(arrow_box);
  arrow_head = Arrow_Line_Create(arrow_box);

  // Distance: huge number plus a smaller unit, sitting on the same baseline
  lv_obj_t *dist_row = lv_obj_create(scr);
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
  road_label = lv_label_create(scr);
  lv_obj_set_size(road_label, 290, 2 * font_road_30.line_height + ROAD_LINE_SPACE);
  lv_label_set_long_mode(road_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(road_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(road_label, &font_road_30, 0);
  lv_obj_set_style_text_line_space(road_label, ROAD_LINE_SPACE, 0);
  lv_obj_set_style_text_color(road_label, COLOR_ROAD, 0);
  lv_obj_align(road_label, LV_ALIGN_TOP_MID, 0, 343);

  Nav_Screen_Update(TURN_RIGHT, 0, 0, "");
}

static void Ring_Anim_Cb(void *obj, int32_t value)
{
  lv_arc_set_value((lv_obj_t *)obj, value);
}

// Updates arrive about once per GPS fix, so glide the ring to the new value over roughly
// the time since the previous update; it then keeps moving until the next one arrives.
static void Ring_Animate_To(uint16_t target)
{
  static uint16_t anim_target = 0;
  static uint32_t last_update_ms = 0;

  uint32_t gap_ms = lv_tick_elaps(last_update_ms);
  last_update_ms = lv_tick_get();
  if (target == anim_target) return;

  lv_anim_del(ring, Ring_Anim_Cb);
  int32_t shown = lv_arc_get_value(ring);
  anim_target = target;

  // A new turn (ring goes back to empty) or a long pause: jump instead of sweeping
  if (target < shown || gap_ms > 3000) {
    lv_arc_set_value(ring, target);
    return;
  }

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, ring);
  lv_anim_set_exec_cb(&a, Ring_Anim_Cb);
  lv_anim_set_values(&a, shown, target);
  lv_anim_set_time(&a, LV_CLAMP(100, gap_ms, 1500));
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_start(&a);
}

void Nav_Screen_Update(Turn_Dir dir, uint32_t distance_m, uint16_t progress, const char *road)
{
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

  Ring_Animate_To(progress);
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
