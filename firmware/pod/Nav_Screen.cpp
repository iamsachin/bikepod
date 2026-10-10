/*****************************************************************************
  | File        :   Nav_Screen.cpp
  | help        :   Screens for the 480x480 round display: approaching turn (with a heading-up
                    drawing of the route ahead), waiting for phone, rerouting, arrived, cruise.
                    Black background, white text, one blue accent; amber for rerouting.
******************************************************************************/
#include "Nav_Screen.h"
#include <Arduino.h>
#include "Map_Layer.h"
#include <esp_heap_caps.h>
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

// Map area (screen coordinates) and where the rider's marker sits inside it
#define PATH_X          0         // full width: the round screen itself crops the drawing
#define PATH_Y          196
#define PATH_W          480
#define PATH_H          196
#define MARKER_X        (PATH_W / 2)
#define MARKER_Y        138

static lv_obj_t *ring;
static lv_obj_t *turn_group;      // header + road + route drawing
static lv_obj_t *head_stem;
static lv_obj_t *head_tip;
static lv_obj_t *dist_label;
static lv_obj_t *unit_label;
static lv_obj_t *road_label;
static lv_obj_t *eta_label;       // "18:42 · 6.0 km", shown on turn and rerouting screens
static lv_obj_t *stale_veil;
static lv_obj_t *dbg_glow, *dbg_area, *dbg_marker;   // for serial test commands      // semi-transparent black over everything when the link goes quiet

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

/********************************** Position marker **********************************
 * A navigation arrow with rounded corners, a white border and a light-to-deep blue gradient.
 * LVGL's polygons have sharp corners, so the marker is rasterised here once at start-up:
 * the outline is a polygon whose corners are replaced by arcs, and every pixel is sampled at
 * 4x4 points inside it for smooth edges.
 *************************************************************************************/
#define MARKER_W        42
#define MARKER_H        50
#define MARKER_OX       21        // where the arrow's origin sits in the canvas
#define MARKER_OY       28
#define MARKER_BORDER   2.5f
#define MARKER_MAX_PTS  96

// Corners in px around the arrow's origin (y down), clockwise on screen, with corner radii
static const float ARROW[4][2] = { {0, -19}, {15, 17}, {0, 8}, {-15, 17} };   // tip, right, notch, left
static const float ARROW_R[4] = { 2.5f, 4.0f, 4.0f, 4.0f };   // tip a little tighter so it still points

// Replaces each corner of a closed polygon with an arc of the given radius
static int Round_Corners(const float (*p)[2], const float *radius, int n, float (*out)[2])
{
  int m = 0;
  for (int i = 0; i < n; i++) {
    const float *a = p[(i + n - 1) % n], *b = p[i], *cpt = p[(i + 1) % n];
    float ux = a[0] - b[0], uy = a[1] - b[1], lu = sqrtf(ux * ux + uy * uy);
    float vx = cpt[0] - b[0], vy = cpt[1] - b[1], lv = sqrtf(vx * vx + vy * vy);
    ux /= lu; uy /= lu; vx /= lv; vy /= lv;
    float half = acosf(LV_CLAMP(-1.0f, ux * vx + uy * vy, 1.0f)) / 2;
    float t = LV_MIN(radius[i] / tanf(half), 0.45f * LV_MIN(lu, lv));
    float r = t * tanf(half);
    float bx = ux + vx, by = uy + vy, lb = sqrtf(bx * bx + by * by);
    float d = r / sinf(half);
    float ox = b[0] + bx / lb * d, oy = b[1] + by / lb * d;            // arc centre
    float a1 = atan2f(b[1] + uy * t - oy, b[0] + ux * t - ox);
    float a2 = atan2f(b[1] + vy * t - oy, b[0] + vx * t - ox);
    float sweep = a2 - a1;
    while (sweep > (float)M_PI) sweep -= 2 * (float)M_PI;
    while (sweep < -(float)M_PI) sweep += 2 * (float)M_PI;
    int steps = LV_MAX(2, (int)ceilf(fabsf(sweep) / 0.2f));
    for (int k = 0; k <= steps && m < MARKER_MAX_PTS; k++) {
      float ang = a1 + sweep * k / steps;
      out[m][0] = ox + r * cosf(ang);
      out[m][1] = oy + r * sinf(ang);
      m++;
    }
  }
  return m;
}

// The same polygon pushed out by `d` along every edge (mitred corners)
static void Offset_Polygon(const float (*p)[2], int n, float d, float (*out)[2])
{
  for (int i = 0; i < n; i++) {
    const float *prev = p[(i + n - 1) % n], *cur = p[i], *next = p[(i + 1) % n];
    float e1x = cur[0] - prev[0], e1y = cur[1] - prev[1], l1 = sqrtf(e1x * e1x + e1y * e1y);
    float e2x = next[0] - cur[0], e2y = next[1] - cur[1], l2 = sqrtf(e2x * e2x + e2y * e2y);
    float n1x = e1y / l1, n1y = -e1x / l1, n2x = e2y / l2, n2y = -e2x / l2;   // outward (clockwise, y down)
    float mx = n1x + n2x, my = n1y + n2y, lm = sqrtf(mx * mx + my * my);
    float miter = d / LV_MAX(lm / 2, 0.2f);
    out[i][0] = cur[0] + mx / lm * miter;
    out[i][1] = cur[1] + my / lm * miter;
  }
}

static bool Inside(const float (*poly)[2], int n, float x, float y)
{
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    if ((poly[i][1] > y) != (poly[j][1] > y) &&
        x < (poly[j][0] - poly[i][0]) * (y - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0])
      in = !in;
  }
  return in;
}

static void Marker_Create(lv_obj_t *parent)
{
  // Soft blue glow under the marker: drawn once into an image (an LVGL shadow recomputed its
  // blur on every map frame, ~13 ms)
  #define GLOW_SIZE 76
  void *glow_buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(GLOW_SIZE, GLOW_SIZE), MALLOC_CAP_SPIRAM);
  lv_obj_t *glow = lv_canvas_create(parent);
  dbg_glow = glow;
  lv_canvas_set_buffer(glow, glow_buf, GLOW_SIZE, GLOW_SIZE, LV_IMG_CF_TRUE_COLOR_ALPHA);
  for (int y = 0; y < GLOW_SIZE; y++) {
    for (int x = 0; x < GLOW_SIZE; x++) {
      float dx = x + 0.5f - GLOW_SIZE / 2.0f, dy = y + 0.5f - GLOW_SIZE / 2.0f;
      float d = sqrtf(dx * dx + dy * dy) / (GLOW_SIZE / 2.0f);          // 0 centre .. 1 edge
      float a = d >= 1 ? 0 : 0.55f * expf(-d * d * 4.5f) * (1 - d);      // soft, zero at the edge
      lv_canvas_set_px_color(glow, x, y, COLOR_BLUE);
      lv_canvas_set_px_opa(glow, x, y, (lv_opa_t)lroundf(a * 255));
    }
  }
  lv_obj_set_pos(glow, MARKER_X - GLOW_SIZE / 2, MARKER_Y + 2 - GLOW_SIZE / 2);

  // Outlines: the arrow, and the border = arrow pushed out by MARKER_BORDER with radii grown to
  // match (convex corners) or shrunk (the notch, which curves the other way)
  static float inner[MARKER_MAX_PTS][2], outer[MARKER_MAX_PTS][2];
  float pushed[4][2], pushed_r[4];
  Offset_Polygon(ARROW, 4, MARKER_BORDER, pushed);
  for (int i = 0; i < 4; i++) pushed_r[i] = i == 2 ? LV_MAX(ARROW_R[i] - MARKER_BORDER, 0.5f) : ARROW_R[i] + MARKER_BORDER;
  int n_in = Round_Corners(ARROW, ARROW_R, 4, inner);
  int n_out = Round_Corners(pushed, pushed_r, 4, outer);

  void *buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(MARKER_W, MARKER_H), MALLOC_CAP_SPIRAM);
  lv_obj_t *canvas = lv_canvas_create(parent);
  dbg_marker = canvas;
  lv_canvas_set_buffer(canvas, buf, MARKER_W, MARKER_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

  const lv_color_t light = lv_color_hex(0x6FA3FF), deep = lv_color_hex(0x2C64E3);
  for (int py = 0; py < MARKER_H; py++) {
    for (int px = 0; px < MARKER_W; px++) {
      int in_out = 0, in_in = 0;
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          float x = px + (sx + 0.5f) / 4 - MARKER_OX, y = py + (sy + 0.5f) / 4 - MARKER_OY;
          if (Inside(outer, n_out, x, y)) {
            in_out++;
            if (Inside(inner, n_in, x, y)) in_in++;
          }
        }
      }
      if (!in_out) continue;
      // Blue gradient from the tip (light) to the base (deep), white where only the border covers
      float t = LV_CLAMP(0.0f, (py - MARKER_OY + 19) / 36.0f, 1.0f);
      lv_color_t blue = lv_color_mix(deep, light, (lv_opa_t)lroundf(t * 255));
      lv_color_t color = lv_color_mix(blue, lv_color_white(), (lv_opa_t)(in_in * 255 / in_out));
      lv_canvas_set_px_color(canvas, px, py, color);
      lv_canvas_set_px_opa(canvas, px, py, (lv_opa_t)(in_out * 255 / 16));
    }
  }
  lv_obj_set_pos(canvas, MARKER_X - MARKER_OX, MARKER_Y - MARKER_OY);
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

/********************************** Soft edges **********************************/
// The map fades into black at the screen's rim and towards the text above and below,
// through an alpha-only image laid over it (LVGL can't blur live; a fixed fade looks the same).

static float Smoothstep(float edge0, float edge1, float x)
{
  float t = LV_CLAMP(0.0f, (x - edge0) / (edge1 - edge0), 1.0f);
  return t * t * (3 - 2 * t);
}

static void Vignette_Create(lv_obj_t *parent)
{
  // The map applies this fade to its own pixels as it draws them: towards the rim, under the road
  // name and above the arrival time (no overlay image to blend every frame)
  Map_Layer_Set_Fade(60, 50, 165, 226);
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

  // Map and route drawing first, so the header and road name are drawn over its faded top.
  // Clipped to its own band; the round screen crops the sides.
  lv_obj_t *path_area = Plain_Box(turn_group, PATH_W, PATH_H);
  lv_obj_set_pos(path_area, PATH_X, PATH_Y);
  dbg_area = path_area;
  Map_Layer_Create(path_area, PATH_W, PATH_H, MARKER_X, MARKER_Y);   // streets, fuel and the route
  Vignette_Create(path_area);
  Marker_Create(path_area);

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
  lv_obj_align(road_label, LV_ALIGN_TOP_MID, 0, 180);

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

  // The map band's edge fade would otherwise paint over the ring where they overlap
  lv_obj_move_foreground(ring);
  lv_obj_move_foreground(ring_tip);
  lv_obj_move_foreground(spinner);

  stale_veil = Plain_Box(scr, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(stale_veil, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(stale_veil, LV_OPA_60, 0);
  lv_obj_add_flag(stale_veil, LV_OBJ_FLAG_HIDDEN);

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
    Map_Layer_Clear_Route();
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
  // A plain veil: setting opacity on whole groups made LVGL render them through a layer,
  // which broke with the map's draw callback
  Show_If(stale_veil, stale);
}

void Nav_Screen_Set_Pose(uint16_t area_id, float x, float y, float heading_deg)
{
  Map_Layer_Set_Pose(area_id, x, y, heading_deg);
}

void Nav_Screen_Set_Route(uint16_t area_id, const int16_t (*pts)[2], uint8_t count)
{
  Map_Layer_Set_Route(area_id, pts, count);
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
  }
  turn_distance_m = distance_m;
  Map_Layer_Set_Turn_Distance(distance_m);   // the map zooms in as the turn gets closer

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

// Serial "hide glow" / "hide vignette" (and "show ..."): for measuring what costs frame time
void Nav_Screen_Debug(const char *what, bool hide)
{
  lv_obj_t *obj = strcmp(what, "glow") == 0 ? dbg_glow
                : strcmp(what, "marker") == 0 ? dbg_marker : strcmp(what, "map") == 0 ? lv_obj_get_child(dbg_area, 0)
                : strcmp(what, "ring") == 0 ? ring : strcmp(what, "eta") == 0 ? eta_label : NULL;
  if (obj) Show_If(obj, !hide);
}

// Serial "bench": redraw the map band 10 times and report the average time
void Nav_Screen_Bench(void)
{
  extern uint32_t map_perf_roads_us, map_perf_fuel_us, map_perf_route_us, map_perf_draws;
  extern uint32_t flush_perf_us, flush_perf_px, map_perf_capsules, map_perf_pixels;
  map_perf_capsules = map_perf_pixels = 0;
  map_perf_roads_us = map_perf_fuel_us = map_perf_route_us = map_perf_draws = 0;
  flush_perf_us = flush_perf_px = 0;
  uint32_t total = 0;
  for (int i = 0; i < 10; i++) {
    lv_obj_invalidate(dbg_area);
    uint32_t t0 = micros();
    lv_refr_now(NULL);
    total += micros() - t0;
  }
  printf("BENCH capsules per redraw %lu, pixels checked %lu\r\n", (unsigned long)(map_perf_capsules / 10), (unsigned long)(map_perf_pixels / 10));
  printf("BENCH map band redraw: %lu us; per redraw: map draw calls %lu, roads %lu us, fuel %lu us, route %lu us, flush %lu us for %lu px\r\n",
         (unsigned long)(total / 10), (unsigned long)(map_perf_draws / 10), (unsigned long)(map_perf_roads_us / 10),
         (unsigned long)(map_perf_fuel_us / 10), (unsigned long)(map_perf_route_us / 10),
         (unsigned long)(flush_perf_us / 10), (unsigned long)(flush_perf_px / 10));
}
