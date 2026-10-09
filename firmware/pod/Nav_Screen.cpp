/*****************************************************************************
  | File        :   Nav_Screen.cpp
  | help        :   Screens for the 480x480 round display: approaching turn (with a heading-up
                    drawing of the route ahead), waiting for phone, rerouting, arrived, cruise.
                    Black background, white text, one blue accent; amber for rerouting.
******************************************************************************/
#include "Nav_Screen.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

LV_FONT_DECLARE(font_big_104);    // Barlow Bold, digits and "." only
LV_FONT_DECLARE(font_title_44);   // Barlow SemiBold, ASCII + middle dot
LV_FONT_DECLARE(font_text_32);    // Barlow SemiBold, ASCII + middle dot
LV_FONT_DECLARE(font_small_26);   // Barlow SemiBold, ASCII + middle dot

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_TEXT      lv_color_hex(0xFFFFFF)
#define COLOR_SOFT      lv_color_hex(0xE6E6E6)   // ETA line
#define COLOR_PATH      lv_color_hex(0xD6D6D6)   // route ahead
#define COLOR_PATH_DONE lv_color_hex(0x262626)   // route already ridden
#define COLOR_STUB      lv_color_hex(0x333333)   // side streets
#define COLOR_GREY      lv_color_hex(0xA8A8A8)   // road name, units
#define COLOR_AMBER     lv_color_hex(0xF5B83D)
#define COLOR_RED       lv_color_hex(0xE5484D)

#define MIDDLE_DOT      "\xC2\xB7"

#define RING_SIZE       462
#define RING_WIDTH      8

// Header arrow: drawn in a 70x76 box with rounded strokes
#define HEAD_ARROW_W    70
#define HEAD_ARROW_H    76
#define HEAD_ARROW_LINE 11

// Route drawing area (screen coordinates) and where the rider's marker sits inside it
#define PATH_X          0         // full width: the round screen itself crops the drawing
#define PATH_Y          212
#define PATH_W          480
#define PATH_H          170
#define MARKER_X        (PATH_W / 2)
#define MARKER_Y        122
#define PATH_LINE       14
#define PATH_MAX_POINTS 24
#define MAX_STUBS       16
#define STUB_METRES     22        // drawn length of a side street, in metres at the current zoom...
#define STUB_MIN_PX     12        // ...but at least this long so it is still visible
#define STUB_MAX_PX     28        // ...and at most this long so close-up junctions stay tidy
#define STUB_LINE       8
#define STUB_TURN_LINE  12        // the cross road at the turn is a real road, a little wider
#define STUB_MAX_DRAWN  10        // dense areas: the nearest junctions only
#define STUB_SPACING_PX 10        // skip a junction this close to one already drawn
#define STUB_AT_TURN_PX 300       // streets at the turn: long enough to reach the edge

static lv_obj_t *ring;
static lv_obj_t *turn_group;      // header + road + route drawing
static lv_obj_t *head_stem;
static lv_obj_t *head_tip;
static lv_obj_t *dist_label;
static lv_obj_t *unit_label;
static lv_obj_t *road_label;
static lv_obj_t *path_behind;
static lv_obj_t *path_ahead;
static lv_obj_t *path_head;
static lv_obj_t *eta_label;       // "18:42 · 6.0 km", shown on turn and rerouting screens

static lv_obj_t *info_group;      // waiting / rerouting / arrived / cruise
static lv_obj_t *icon_target;
static lv_obj_t *icon_reroute;
static lv_obj_t *info_top;
static lv_obj_t *info_main;
static lv_obj_t *info_sub;
static lv_obj_t *info_detail;
static lv_obj_t *spinner;         // amber arc running round the edge while rerouting
static lv_timer_t *spinner_timer;

static Screen_Mode mode = MODE_WAITING;
static bool stale = false;
static bool have_trip = false;
static uint32_t turn_distance_m = 0;

// Header arrow shapes (70x76 box). Turn arrows bend through a quarter circle; see Arrows_Init().
#define BEND_STEPS 7
#define STEM_POINTS (BEND_STEPS + 3)          // start, bend points (BEND_STEPS + 1), end
static lv_point_t right_stem[STEM_POINTS];
static lv_point_t left_stem[STEM_POINTS];
static lv_point_t right_tip[]  = { {44, 9}, {58, 23}, {44, 37} };
static lv_point_t left_tip[]   = { {26, 9}, {12, 23}, {26, 37} };
static lv_point_t ahead_stem[] = { {35, 70}, {35, 10} };
static lv_point_t ahead_tip[]  = { {21, 24}, {35, 10}, {49, 24} };

static void Arrows_Init(void)
{
  // Right turn: up the stem, round a 16 px radius bend, then right to the tip. Left is its mirror.
  const float r = 16, cx = 16 + r, cy = 23 + r;
  right_stem[0] = { 16, 70 };
  for (int i = 0; i <= BEND_STEPS; i++) {
    float a = (float)M_PI - i * ((float)M_PI / 2) / BEND_STEPS;   // 180 -> 90 degrees
    right_stem[1 + i] = { (lv_coord_t)lroundf(cx + r * cosf(a)), (lv_coord_t)lroundf(cy - r * sinf(a)) };
  }
  right_stem[STEM_POINTS - 1] = { 58, 23 };
  for (int i = 0; i < STEM_POINTS; i++) {
    left_stem[i] = { (lv_coord_t)(HEAD_ARROW_W - right_stem[i].x), right_stem[i].y };
  }
}

// Route drawing, in pixels inside the path area (after smoothing: up to 4x the points received)
#define PATH_DRAW_MAX   (PATH_MAX_POINTS * 4)
static lv_point_t behind_pts[PATH_DRAW_MAX];
static lv_point_t ahead_pts[PATH_DRAW_MAX];
static lv_point_t head_pts[3];
static lv_obj_t *stub_lines[MAX_STUBS];
static lv_point_t stub_pts[MAX_STUBS][2];
static int16_t stub_data[MAX_STUBS][4];
static uint8_t stub_count = 0;

static lv_obj_t *Line_Create(lv_obj_t *parent, lv_coord_t width, lv_color_t color, lv_opa_t opa)
{
  lv_obj_t *line = lv_line_create(parent);
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_color(line, color, 0);
  lv_obj_set_style_line_opa(line, opa, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  return line;
}

static lv_obj_t *Plain_Box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, h);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return box;
}

static lv_obj_t *Label_Create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

static void Show_If(lv_obj_t *obj, bool show)
{
  if (show) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
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

// For the non-turn screens: a still ring (0 = empty, 1000 = full)
static void Ring_Set_Fixed(uint16_t value)
{
  ring_target = ring_shown = value;
  ring_rate = 0;
  Ring_Draw(ring_shown);
}

/********************************* Rerouting spinner *********************************/
static void Spinner_Tick(lv_timer_t *timer)
{
  static int16_t start = 0;
  start = (start + 8) % 360;                  // one turn every ~1.4 s
  lv_arc_set_angles(spinner, start, start + 70);
}

/************************************* Icons *************************************/
static void Icons_Create(lv_obj_t *parent)
{
  // Arrived: a red target
  icon_target = Plain_Box(parent, 52, 60);
  lv_obj_t *outer = Plain_Box(icon_target, 48, 48);
  lv_obj_align(outer, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(outer, 7, 0);
  lv_obj_set_style_border_color(outer, COLOR_RED, 0);
  lv_obj_t *dot = Plain_Box(icon_target, 16, 16);
  lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 16);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, COLOR_RED, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

  // Rerouting: a circular arrow
  icon_reroute = Plain_Box(parent, 64, 72);
  lv_obj_t *loop = lv_arc_create(icon_reroute);
  lv_obj_set_size(loop, 56, 56);
  lv_obj_align(loop, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_remove_style(loop, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(loop, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(loop, 0, 0);
  lv_obj_set_style_arc_opa(loop, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(loop, 7, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(loop, COLOR_AMBER, LV_PART_INDICATOR);
  lv_arc_set_bg_angles(loop, 0, 360);
  lv_arc_set_angles(loop, 300, 240);          // open gap at the top-right
  static lv_point_t tip[] = { {33, 0}, {44, 9}, {32, 18} };
  lv_obj_t *tip_line = Line_Create(icon_reroute, 7, COLOR_AMBER, LV_OPA_COVER);
  lv_line_set_points(tip_line, tip, 3);
}

/******************************* Route drawing *******************************/
static float path_scale = 0;      // pixels per metre

static void Draw_Stubs(void);

static void Path_Clear(void)
{
  stub_count = 0;
  Draw_Stubs();
  lv_obj_add_flag(path_behind, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(path_ahead, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(path_head, LV_OBJ_FLAG_HIDDEN);
}

// Douglas-Peucker in screen pixels: drops bumps smaller than ~3 px (real road jogs of a few
// metres that look like wobbles at this size). Keeps both end points.
static uint8_t Simplify(float (*p)[2], uint8_t n, float tolerance)
{
  if (n <= 2) return n;
  static bool keep[PATH_MAX_POINTS];
  memset(keep, 0, sizeof(keep));
  keep[0] = keep[n - 1] = true;
  static uint8_t stack[PATH_MAX_POINTS][2];
  int top = 0;
  stack[top][0] = 0; stack[top][1] = n - 1; top++;
  while (top > 0) {
    top--;
    uint8_t a = stack[top][0], b = stack[top][1];
    float ax = p[a][0], ay = p[a][1], dx = p[b][0] - ax, dy = p[b][1] - ay;
    float len = sqrtf(dx * dx + dy * dy);
    float worst = 0;
    uint8_t index = 0;
    for (uint8_t i = a + 1; i < b; i++) {
      float d = len > 0 ? fabsf((p[i][0] - ax) * dy - (p[i][1] - ay) * dx) / len
                        : sqrtf((p[i][0] - ax) * (p[i][0] - ax) + (p[i][1] - ay) * (p[i][1] - ay));
      if (d > worst) { worst = d; index = i; }
    }
    if (worst > tolerance) {
      keep[index] = true;
      stack[top][0] = a; stack[top][1] = index; top++;
      stack[top][0] = index; stack[top][1] = b; top++;
    }
  }
  uint8_t m = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (keep[i]) { p[m][0] = p[i][0]; p[m][1] = p[i][1]; m++; }
  }
  return m;
}

// Chaikin corner cutting, twice: turns OpenStreetMap's little zig-zags into smooth curves.
// The two end points stay put, so the line still starts exactly at the rider.
static uint8_t Smooth(const float (*in)[2], uint8_t n, lv_point_t *out)
{
  static float a[PATH_DRAW_MAX][2], b[PATH_DRAW_MAX][2];
  memcpy(a, in, n * sizeof(a[0]));
  for (int pass = 0; pass < 2 && n >= 3 && 2 * n <= PATH_DRAW_MAX; pass++) {
    uint8_t m = 0;
    b[m][0] = a[0][0]; b[m][1] = a[0][1]; m++;
    for (uint8_t i = 0; i + 1 < n; i++) {
      float x0 = a[i][0], y0 = a[i][1], x1 = a[i + 1][0], y1 = a[i + 1][1];
      if (i > 0)     { b[m][0] = 0.75f * x0 + 0.25f * x1; b[m][1] = 0.75f * y0 + 0.25f * y1; m++; }
      if (i + 2 < n) { b[m][0] = 0.25f * x0 + 0.75f * x1; b[m][1] = 0.25f * y0 + 0.75f * y1; m++; }
    }
    b[m][0] = a[n - 1][0]; b[m][1] = a[n - 1][1]; m++;
    memcpy(a, b, m * sizeof(a[0]));
    n = m;
  }
  for (uint8_t i = 0; i < n; i++) out[i] = { (lv_coord_t)lroundf(a[i][0]), (lv_coord_t)lroundf(a[i][1]) };
  return n;
}

// Side streets as short dim lines from each junction, drawn under the route. They follow the
// route drawing's zoom for position but keep a fixed length on screen.
static void Draw_Stubs(void)
{
  // Stubs arrive nearest first. In dense areas, zoomed out, many lanes would pile up into a
  // striped blob, so skip junctions too close to one already drawn and cap the number shown.
  float length = LV_CLAMP(STUB_MIN_PX, STUB_METRES * path_scale, STUB_MAX_PX);
  lv_point_t drawn[MAX_STUBS];
  uint8_t n_drawn = 0, n_lines = 0;
  for (uint8_t i = 0; i < stub_count && path_scale > 0; i++) {
    lv_point_t at = { (lv_coord_t)lroundf(MARKER_X + stub_data[i][0] * path_scale),
                      (lv_coord_t)lroundf(MARKER_Y - stub_data[i][1] * path_scale) };
    bool same_junction = false, too_close = false;
    for (uint8_t k = 0; k < n_drawn; k++) {
      int dx = at.x - drawn[k].x, dy = at.y - drawn[k].y;
      if (dx == 0 && dy == 0) same_junction = true;            // another street at a junction we show
      else if (dx * dx + dy * dy < STUB_SPACING_PX * STUB_SPACING_PX) too_close = true;
    }
    if (too_close && !same_junction) continue;
    if (!same_junction) {
      if (n_drawn == STUB_MAX_DRAWN) break;
      drawn[n_drawn++] = at;
    }
    float a = stub_data[i][2] * (float)M_PI / 180;      // 0 = up the screen, clockwise
    float len = stub_data[i][3] ? STUB_AT_TURN_PX : length;   // the turn's cross road runs off the screen
    stub_pts[n_lines][0] = at;
    stub_pts[n_lines][1] = { (lv_coord_t)lroundf(at.x + len * sinf(a)), (lv_coord_t)lroundf(at.y - len * cosf(a)) };
    lv_line_set_points(stub_lines[n_lines], stub_pts[n_lines], 2);
    lv_obj_set_style_line_width(stub_lines[n_lines], stub_data[i][3] ? STUB_TURN_LINE : STUB_LINE, 0);
    lv_obj_clear_flag(stub_lines[n_lines], LV_OBJ_FLAG_HIDDEN);
    n_lines++;
  }
  for (uint8_t i = n_lines; i < MAX_STUBS; i++) lv_obj_add_flag(stub_lines[i], LV_OBJ_FLAG_HIDDEN);
}

void Nav_Screen_Set_Stubs(const int16_t (*stubs)[4], uint8_t count)
{
  stub_count = count > MAX_STUBS ? MAX_STUBS : count;
  memcpy(stub_data, stubs, stub_count * sizeof(stub_data[0]));
  Draw_Stubs();
}

void Nav_Screen_Set_Path(const int16_t (*pts)[2], uint8_t count)
{
  if (count > PATH_MAX_POINTS) count = PATH_MAX_POINTS;

  // Zoom so the turn sits about 78 px ahead of the rider (room above for the exit and its side
  // streets); ease zoom changes between updates
  float target = 78.0f / LV_MAX((float)turn_distance_m, 30.0f);
  target = LV_CLAMP(0.3f, target, 2.6f);
  path_scale = path_scale == 0 ? target : path_scale * 0.6f + target * 0.4f;
  Draw_Stubs();

  // The rider's position is the (0,0) point: before it is ridden, after it is ahead
  uint8_t split = 0;
  while (split < count && !(pts[split][0] == 0 && pts[split][1] == 0)) split++;
  if (split == count) split = 0;

  static float behind_px[PATH_MAX_POINTS][2], ahead_px[PATH_MAX_POINTS][2];
  uint8_t n_behind = 0, n_ahead = 0;
  for (uint8_t i = 0; i < count; i++) {
    float x = MARKER_X + pts[i][0] * path_scale;
    float y = MARKER_Y - pts[i][1] * path_scale;          // +y ahead = up the screen
    if (i <= split) { behind_px[n_behind][0] = x; behind_px[n_behind][1] = y; n_behind++; }
    if (i >= split) { ahead_px[n_ahead][0] = x; ahead_px[n_ahead][1] = y; n_ahead++; }
  }

  n_behind = Simplify(behind_px, n_behind, 3);
  n_ahead = Simplify(ahead_px, n_ahead, 3);
  if (n_behind >= 2) lv_line_set_points(path_behind, behind_pts, Smooth(behind_px, n_behind, behind_pts));
  Show_If(path_behind, n_behind >= 2);
  if (n_ahead < 2) {
    lv_obj_add_flag(path_ahead, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(path_head, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  n_ahead = Smooth(ahead_px, n_ahead, ahead_pts);
  lv_line_set_points(path_ahead, ahead_pts, n_ahead);
  lv_obj_clear_flag(path_ahead, LV_OBJ_FLAG_HIDDEN);

  // Chevron on the far end, pointing along the end of the line
  lv_point_t end = ahead_pts[n_ahead - 1], prev = ahead_pts[n_ahead >= 3 ? n_ahead - 3 : n_ahead - 2];
  float dx = end.x - prev.x, dy = end.y - prev.y, len = sqrtf(dx * dx + dy * dy);
  if (len < 1) {
    lv_obj_add_flag(path_head, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  dx /= len; dy /= len;
  const float wing = 16, c = cosf(0.75f), s = sinf(0.75f);  // wings ~43 degrees either side
  head_pts[0].x = (lv_coord_t)lroundf(end.x - wing * (dx * c - dy * s));
  head_pts[0].y = (lv_coord_t)lroundf(end.y - wing * (dx * s + dy * c));
  head_pts[1] = end;
  head_pts[2].x = (lv_coord_t)lroundf(end.x - wing * (dx * c + dy * s));
  head_pts[2].y = (lv_coord_t)lroundf(end.y - wing * (-dx * s + dy * c));
  lv_line_set_points(path_head, head_pts, 3);
  lv_obj_clear_flag(path_head, LV_OBJ_FLAG_HIDDEN);
}

#define MARKER_W 32
#define MARKER_H 38

static void Marker_Create(lv_obj_t *parent)
{
  // Soft blue glow under the marker
  lv_obj_t *glow = Plain_Box(parent, 30, 30);
  lv_obj_set_pos(glow, MARKER_X - 15, MARKER_Y - 13);
  lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(glow, COLOR_BLUE, 0);
  lv_obj_set_style_bg_opa(glow, LV_OPA_30, 0);
  lv_obj_set_style_shadow_color(glow, COLOR_BLUE, 0);
  lv_obj_set_style_shadow_width(glow, 36, 0);
  lv_obj_set_style_shadow_opa(glow, LV_OPA_50, 0);

  // Navigation arrow: two triangles make the notched shape
  static lv_color_t buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(MARKER_W, MARKER_H)];
  lv_obj_t *canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvas, buf, MARKER_W, MARKER_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
  lv_draw_rect_dsc_t fill;
  lv_draw_rect_dsc_init(&fill);
  fill.bg_color = COLOR_BLUE;
  fill.bg_opa = LV_OPA_COVER;
  lv_point_t left[]  = { {16, 0}, {16, 27}, {1, 37} };
  lv_point_t right[] = { {16, 0}, {31, 37}, {16, 27} };
  lv_canvas_draw_polygon(canvas, left, 3, &fill);
  lv_canvas_draw_polygon(canvas, right, 3, &fill);
  lv_obj_set_pos(canvas, MARKER_X - MARKER_W / 2, MARKER_Y - MARKER_H / 2);
}

/********************************* Screen setup *********************************/
static void Set_Mode(Screen_Mode new_mode)
{
  mode = new_mode;
  Show_If(turn_group, mode == MODE_TURN);
  Show_If(info_group, mode != MODE_TURN);

  bool rerouting = mode == MODE_REROUTING;
  Show_If(spinner, rerouting);
  Show_If(ring, !rerouting);
  if (rerouting) lv_timer_resume(spinner_timer);
  else lv_timer_pause(spinner_timer);

  Show_If(eta_label, have_trip && (mode == MODE_TURN || rerouting));
}

// Info screens: optional icon, then up to four centred lines. Empty lines are hidden so the
// rest stays centred.
static void Set_Info(lv_obj_t *icon, const char *top, const char *main, const lv_font_t *main_font,
                     const char *sub, const char *detail)
{
  Show_If(icon_target, icon == icon_target);
  Show_If(icon_reroute, icon == icon_reroute);
  lv_label_set_text(info_top, top);
  lv_label_set_text(info_main, main);
  lv_obj_set_style_text_font(info_main, main_font, 0);
  lv_label_set_text(info_sub, sub);
  lv_label_set_text(info_detail, detail);
  Show_If(info_top, top[0]);
  Show_If(info_sub, sub[0]);
  Show_If(info_detail, detail[0]);
}

static lv_obj_t *Edge_Arc(lv_obj_t *parent, lv_color_t color)
{
  lv_obj_t *arc = lv_arc_create(parent);
  lv_obj_set_size(arc, RING_SIZE, RING_SIZE);
  lv_obj_center(arc);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(arc, 0, 0);
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);     // no track, just the indicator
  lv_obj_set_style_arc_width(arc, RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
  lv_arc_set_bg_angles(arc, 0, 360);
  return arc;
}

void Nav_Screen_Create(void)
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  Arrows_Init();

  // Thin progress ring near the edge: starts at 12 o'clock and fills clockwise
  ring = Edge_Arc(scr, COLOR_BLUE);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_range(ring, 0, 360);
  lv_arc_set_value(ring, 0);

  ring_tip = Plain_Box(scr, RING_WIDTH, RING_WIDTH);
  lv_obj_set_style_radius(ring_tip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ring_tip, COLOR_BLUE, 0);
  lv_obj_set_style_bg_opa(ring_tip, LV_OPA_COVER, 0);
  lv_obj_add_flag(ring_tip, LV_OBJ_FLAG_HIDDEN);
  lv_timer_create(Ring_Frame, RING_FRAME_MS, NULL);

  spinner = Edge_Arc(scr, COLOR_AMBER);
  lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  spinner_timer = lv_timer_create(Spinner_Tick, 30, NULL);
  lv_timer_pause(spinner_timer);

  /* Turn screen */
  turn_group = Plain_Box(scr, LV_PCT(100), LV_PCT(100));

  // Header: arrow, distance and unit on one line, sharing a baseline
  lv_obj_t *header = Plain_Box(turn_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 88);

  lv_obj_t *arrow_box = Plain_Box(header, HEAD_ARROW_W, HEAD_ARROW_H);
  head_stem = Line_Create(arrow_box, HEAD_ARROW_LINE, COLOR_TEXT, LV_OPA_COVER);
  head_tip = Line_Create(arrow_box, HEAD_ARROW_LINE, COLOR_TEXT, LV_OPA_COVER);

  dist_label = Label_Create(header, &font_big_104, COLOR_TEXT);
  unit_label = Label_Create(header, &font_text_32, COLOR_GREY);
  // The fonts leave different space under their baselines; shift the unit so both sit on one line
  lv_obj_set_style_translate_y(unit_label, font_text_32.base_line - font_big_104.base_line, 0);

  road_label = Label_Create(turn_group, &font_text_32, COLOR_GREY);
  lv_obj_set_width(road_label, 380);
  lv_label_set_long_mode(road_label, LV_LABEL_LONG_DOT);
  lv_obj_align(road_label, LV_ALIGN_TOP_MID, 0, 172);

  // Route drawing, clipped to its own area so it never runs into the text
  lv_obj_t *path_area = Plain_Box(turn_group, PATH_W, PATH_H);
  lv_obj_set_pos(path_area, PATH_X, PATH_Y);
  for (uint8_t i = 0; i < MAX_STUBS; i++) {             // created first, so they sit under the route
    stub_lines[i] = Line_Create(path_area, STUB_LINE, COLOR_STUB, LV_OPA_COVER);
    lv_obj_add_flag(stub_lines[i], LV_OBJ_FLAG_HIDDEN);
  }
  // Solid colours: see-through segments overlap at every joint and look blotchy
  path_behind = Line_Create(path_area, PATH_LINE - 2, COLOR_PATH_DONE, LV_OPA_COVER);
  path_ahead = Line_Create(path_area, PATH_LINE, COLOR_PATH, LV_OPA_COVER);
  path_head = Line_Create(path_area, PATH_LINE - 3, COLOR_PATH, LV_OPA_COVER);
  Path_Clear();
  Marker_Create(path_area);

  /* Shared bottom line: arrival time and distance left */
  eta_label = Label_Create(scr, &font_text_32, COLOR_SOFT);
  lv_obj_align(eta_label, LV_ALIGN_TOP_MID, 0, 386);
  lv_obj_add_flag(eta_label, LV_OBJ_FLAG_HIDDEN);

  /* Info screens */
  info_group = Plain_Box(scr, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(info_group, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(info_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(info_group, 6, 0);
  Icons_Create(info_group);
  info_top = Label_Create(info_group, &font_text_32, COLOR_GREY);
  info_main = Label_Create(info_group, &font_title_44, COLOR_TEXT);
  info_sub = Label_Create(info_group, &font_small_26, COLOR_GREY);
  lv_obj_set_width(info_sub, 340);
  lv_label_set_long_mode(info_sub, LV_LABEL_LONG_DOT);
  info_detail = Label_Create(info_group, &font_text_32, COLOR_SOFT);

  Nav_Screen_Show_Waiting();
}

/********************************* Screens *********************************/
void Nav_Screen_Show_Waiting(void)
{
  have_trip = false;
  Set_Mode(MODE_WAITING);
  Nav_Screen_Set_Stale(false);
  Ring_Set_Fixed(0);
  Set_Info(NULL, "", "BikePod", &font_title_44, "Waiting for phone", "");
}

void Nav_Screen_Show_Rerouting(void)
{
  if (mode != MODE_REROUTING) {             // repeats of the same screen only update the text
    Set_Mode(MODE_REROUTING);
    Path_Clear();
  }
  Set_Info(icon_reroute, "", "Rerouting", &font_title_44, "Finding a new route", "");
}

void Nav_Screen_Show_Arrived(float trip_km, uint32_t trip_min, const char *place)
{
  char summary[40];
  snprintf(summary, sizeof(summary), "%.1f km " MIDDLE_DOT " %lu min", trip_km, (unsigned long)trip_min);
  have_trip = false;
  if (mode != MODE_ARRIVED) {
    Set_Mode(MODE_ARRIVED);
    Ring_Set_Fixed(1000);
  }
  Set_Info(icon_target, "", "Arrived", &font_title_44, place, summary);
}

void Nav_Screen_Show_Cruise(uint32_t speed_kmh, const char *clock)
{
  char speed[8];
  snprintf(speed, sizeof(speed), "%lu", (unsigned long)speed_kmh);
  have_trip = false;
  if (mode != MODE_CRUISE) {
    Set_Mode(MODE_CRUISE);
    Ring_Set_Fixed(0);
  }
  Set_Info(NULL, clock, speed, &font_big_104, "km/h", "");
}

void Nav_Screen_Set_Trip(const char *eta, float km_left)
{
  char text[40];
  snprintf(text, sizeof(text), "%s " MIDDLE_DOT " %.1f km", eta, km_left);
  lv_label_set_text(eta_label, text);
  have_trip = true;
  Show_If(eta_label, mode == MODE_TURN || mode == MODE_REROUTING);
}

// No message for a while: fade everything so an old distance never looks current
void Nav_Screen_Set_Stale(bool now_stale)
{
  if (now_stale == stale) return;
  stale = now_stale;
  lv_opa_t opa = stale ? LV_OPA_40 : LV_OPA_COVER;
  lv_obj_set_style_opa(turn_group, opa, 0);
  lv_obj_set_style_opa(info_group, opa, 0);
  lv_obj_set_style_opa(eta_label, opa, 0);
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
    Ring_Set_Fixed(0);   // a fresh approach after another screen
    path_scale = 0;
  }
  if (distance_m > turn_distance_m + 30) path_scale = 0;   // a new turn: snap the zoom, don't ease
  turn_distance_m = distance_m;

  switch (dir) {
    case TURN_LEFT:
      lv_line_set_points(head_stem, left_stem, STEM_POINTS);
      lv_line_set_points(head_tip, left_tip, 3);
      break;
    case TURN_RIGHT:
      lv_line_set_points(head_stem, right_stem, STEM_POINTS);
      lv_line_set_points(head_tip, right_tip, 3);
      break;
    default:
      lv_line_set_points(head_stem, ahead_stem, 2);
      lv_line_set_points(head_tip, ahead_tip, 3);
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

  // Long names: try the smaller font before cutting the name off with "..."
  lv_point_t size;
  lv_txt_get_size(&size, road, &font_text_32, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  lv_obj_set_style_text_font(road_label, size.x > lv_obj_get_width(road_label) ? &font_small_26 : &font_text_32, 0);
  lv_label_set_text(road_label, road);
}
