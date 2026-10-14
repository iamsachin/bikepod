/*****************************************************************************
  | File        :   Nav_Screen.cpp
  | help        :   Screens for the 480x480 round display: approaching turn (with a heading-up
                    drawing of the route ahead), waiting for phone, rerouting, arrived, cruise.
                    Black background, white text, one blue accent; amber for rerouting.
******************************************************************************/
#include "Nav_Screen.h"
#include <Arduino.h>
#include "Map_Layer.h"
#include "Pages.h"
#include "Settings.h"
#include "Clock.h"
LV_IMG_DECLARE(app_icon);
#include <esp_heap_caps.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "Smooth_Arc.h"
#include "Lean_Screen.h"

LV_FONT_DECLARE(font_big_104);    // Barlow Bold, digits and "." only
LV_FONT_DECLARE(font_title_44);   // Barlow SemiBold, ASCII + middle dot
LV_FONT_DECLARE(font_text_32);    // Barlow SemiBold, ASCII + middle dot
LV_FONT_DECLARE(font_small_26);
LV_FONT_DECLARE(font_then_30);   // Material turn glyphs: left, right, straight, roundabout, U-turns

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_TEXT      lv_color_hex(0xFFFFFF)
#define COLOR_SOFT      lv_color_hex(0xE6E6E6)   // ETA line
#define COLOR_GREY      lv_color_hex(0xA8A8A8)   // road name, units
#define COLOR_AMBER     lv_color_hex(0xF5B83D)
#define COLOR_RED       lv_color_hex(0xE5484D)

#define DEGREE          "\xC2\xB0"
#define MIDDLE_DOT      "\xC2\xB7"

#define RING_SIZE       462
#define RING_WIDTH      8

// Header arrow: drawn in a 70x76 box with rounded strokes
#define HEAD_ARROW_W    80
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
static lv_obj_t *dist_label;
static lv_obj_t *unit_label;
static lv_obj_t *road_label;
static lv_obj_t *then_pill, *then_icon;   // "then" + the turn after next, when it comes close
static lv_obj_t *eta_label;       // "18:42 · 6.0 km", shown on turn and rerouting screens
static char eta_text[40] = "";    // ...what it says when no breaker warning is showing
static lv_obj_t *limit_sign, *limit_label;
static int limit_kmh = 0;
static uint32_t bump_m = 0;
static lv_obj_t *stale_veil;
static lv_obj_t *dbg_glow, *dbg_area, *dbg_marker;   // for serial test commands      // semi-transparent black over everything when the link goes quiet

static lv_obj_t *info_group;      // waiting / rerouting / arrived / cruise
static lv_obj_t *summary_grid;    // arrived: the ride's numbers
static lv_obj_t *sum_avg, *sum_top, *sum_lean, *sum_g;
static lv_obj_t *icon_app;        // the app's icon, on "Waiting for phone"
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

// Setting a label's text always redraws it (and whatever is under it, often the map), even
// when the text is the same: most updates repeat it, so only set it when it changes
static void Set_Text(lv_obj_t *label, const char *text)
{
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void Show_If(lv_obj_t *obj, bool show)
{
  if (show) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/******************************** Smooth progress ring ********************************
 * Updates arrive roughly once per GPS fix, at uneven intervals. Instead of jumping (or
 * restarting an animation each time), the ring keeps moving at the measured speed between
 * updates and eases towards each new value. Drawn by Smooth_Arc (anti-aliased, any angle).
 ***************************************************************************************/
#define RING_FRAME_MS     16                          // ~60 fps
#define RING_RADIUS       ((RING_SIZE - RING_WIDTH) / 2)  // centre line of the ring
#define RING_MAX_COAST_MS 1500                        // stop predicting if updates stop

static float ring_target = 0;     // last value received (0..1000)
static float ring_rate = 0;       // measured speed, units per ms
static float ring_shown = 0;      // value currently drawn
static uint32_t ring_target_ms = 0;

static void Ring_Draw(float value)
{
  Smooth_Arc_Set(ring, 0, value * 360.0f / 1000.0f);   // from 12 o'clock, clockwise
}

static void Ring_Frame(lv_timer_t *timer)
{
  if (!Pages_Nav_Visible()) return;         // covered by another page: redrawing it is wasted work
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
  Smooth_Arc_Set(spinner, start, 70);
}

/********************************* Waiting clock *********************************/
// The time on "Waiting for phone", under that line (the pod's own clock, so it works without the phone)
static void Wait_Clock_Tick(lv_timer_t *timer)
{
  if (mode != MODE_WAITING) return;
  char text[12];
  Clock_Text(text, sizeof(text));          // "" when the clock hasn't been set since power-up
  Set_Text(info_detail, text);
  Show_If(info_detail, text[0]);
}

/************************************* Icons *************************************/
static void Icons_Create(lv_obj_t *parent)
{
  icon_app = lv_img_create(parent);
  lv_img_set_src(icon_app, &app_icon);

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
  // Drawn clockwise from 340 deg round to 280 deg (just right of the top), where the arrowhead
  // carries on: the gap is ahead of the head, so it reads as a clockwise arrow
  lv_arc_set_angles(loop, 340, 280);
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

/*********************************** Turn icon ***********************************
 * The arrow (or roundabout) beside the distance, drawn here with smooth edges into a small image
 * whenever the turn changes: thick strokes with round ends, a quarter-circle bend, a filled
 * arrowhead. (LVGL's thick lines made blunt, lumpy arrows at this size.) Every pixel is sampled
 * 4x4 times; anything not covered is transparent.
 *
 * Roundabouts: a dim ring entered from the bottom, the way round drawn bright (clockwise in
 * India) to the exit, which leaves the ring at its real angle; the exit number sits inside.
 **********************************************************************************/
#define ICON_SS   4

static lv_obj_t *icon_canvas, *rb_num;

struct Icon_Shape {
  // Bright strokes: up to 3 capsules (thick lines with round ends) and one arc
  int caps;
  float cap[3][4], cap_hw;
  bool has_arc;
  float arc_cx, arc_cy, arc_r, arc_hw, arc_start, arc_sweep;   // degrees, LVGL convention (clockwise from 3 o'clock)
  bool has_tri;
  float tri[3][2];
  bool has_ring;                                // dim full ring (roundabouts), same centre/radius as the arc
  float ring_hw;
  bool mirror;                                  // evaluate at (W - x): left turns are mirrored right turns
};

static inline float Seg_Dist2(float px, float py, const float *s)
{
  float dx = s[2] - s[0], dy = s[3] - s[1], len2 = dx * dx + dy * dy;
  float t = len2 > 0 ? LV_CLAMP(0.0f, ((px - s[0]) * dx + (py - s[1]) * dy) / len2, 1.0f) : 0;
  float qx = s[0] + t * dx - px, qy = s[1] + t * dy - py;
  return qx * qx + qy * qy;
}

static inline bool In_Tri(float px, float py, const float (*t)[2])
{
  float d1 = (px - t[1][0]) * (t[0][1] - t[1][1]) - (t[0][0] - t[1][0]) * (py - t[1][1]);
  float d2 = (px - t[2][0]) * (t[1][1] - t[2][1]) - (t[1][0] - t[2][0]) * (py - t[2][1]);
  float d3 = (px - t[0][0]) * (t[2][1] - t[0][1]) - (t[2][0] - t[0][0]) * (py - t[0][1]);
  bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(neg && pos);
}

// 2 = bright, 1 = dim ring, 0 = nothing
static int Icon_Sample(const Icon_Shape &s, float x, float y)
{
  if (s.mirror) x = HEAD_ARROW_W - x;
  for (int i = 0; i < s.caps; i++)
    if (Seg_Dist2(x, y, s.cap[i]) <= s.cap_hw * s.cap_hw) return 2;
  if (s.has_tri && In_Tri(x, y, s.tri)) return 2;
  if (s.has_arc || s.has_ring) {
    float dx = x - s.arc_cx, dy = y - s.arc_cy, d = sqrtf(dx * dx + dy * dy);
    if (s.has_arc && fabsf(d - s.arc_r) <= s.arc_hw) {
      float ang = atan2f(dy, dx) * 180 / (float)M_PI;   // clockwise from 3 o'clock (y is down)
      float rel = s.arc_sweep >= 0 ? fmodf(ang - s.arc_start + 720, 360) : fmodf(s.arc_start - ang + 720, 360);
      if (rel <= fabsf(s.arc_sweep)) return 2;
    }
    if (s.has_ring && fabsf(d - s.arc_r) <= s.ring_hw) return 1;
  }
  return 0;
}

static void Icon_Render(const Icon_Shape &s)
{
  const lv_color_t bright = COLOR_TEXT, dim = lv_color_hex(0x505050);
  for (int py = 0; py < HEAD_ARROW_H; py++) {
    for (int px = 0; px < HEAD_ARROW_W; px++) {
      int n_bright = 0, n_dim = 0;
      for (int sy = 0; sy < ICON_SS; sy++)
        for (int sx = 0; sx < ICON_SS; sx++) {
          int v = Icon_Sample(s, px + (sx + 0.5f) / ICON_SS, py + (sy + 0.5f) / ICON_SS);
          if (v == 2) n_bright++;
          else if (v == 1) n_dim++;
        }
      int total = n_bright + n_dim;
      lv_color_t col = total ? lv_color_mix(bright, dim, (lv_opa_t)(n_bright * 255 / total)) : bright;
      lv_canvas_set_px_color(icon_canvas, px, py, col);
      lv_canvas_set_px_opa(icon_canvas, px, py, (lv_opa_t)(total * 255 / (ICON_SS * ICON_SS)));
    }
  }
  lv_obj_invalidate(icon_canvas);
}

// Arrowhead pointing along (ux, uy) with its tip at (tx, ty)
static void Icon_Head(Icon_Shape &s, float tx, float ty, float ux, float uy, float length, float half_width)
{
  float bx = tx - length * ux, by = ty - length * uy, nx = -uy, ny = ux;
  s.has_tri = true;
  s.tri[0][0] = tx; s.tri[0][1] = ty;
  s.tri[1][0] = bx + half_width * nx; s.tri[1][1] = by + half_width * ny;
  s.tri[2][0] = bx - half_width * nx; s.tri[2][1] = by - half_width * ny;
}

static void Icon_Set(Turn_Dir dir, int16_t degrees, uint8_t exit_number)
{
  Icon_Shape s = {};
  const float hw = HEAD_ARROW_LINE / 2.0f;
  if (dir == TURN_ROUNDABOUT) {
    const float cx = 40, cy = 38, r = 21;
    int travel = LV_CLAMP(-359, degrees, 359);
    if (travel == 0) travel = 180;                    // unknown: straight across
    s.has_ring = true; s.ring_hw = 3.5f;
    s.has_arc = true; s.arc_cx = cx; s.arc_cy = cy; s.arc_r = r; s.arc_hw = 4.5f;
    s.arc_start = 90; s.arc_sweep = travel;           // entry at the bottom
    float a = (90 + travel) * (float)M_PI / 180, ux = cosf(a), uy = sinf(a);
    s.cap_hw = 4.5f; s.caps = 2;
    float entry[4] = { cx, HEAD_ARROW_H - 5, cx, cy + r };
    float exit_stem[4] = { cx + r * ux, cy + r * uy, cx + 28 * ux, cy + 28 * uy };
    memcpy(s.cap[0], entry, sizeof(entry));
    memcpy(s.cap[1], exit_stem, sizeof(exit_stem));
    Icon_Head(s, cx + 38 * ux, cy + 38 * uy, ux, uy, 12, 9.5f);
  } else if (dir == TURN_STRAIGHT) {
    s.cap_hw = hw; s.caps = 1;
    float stem[4] = { 40, HEAD_ARROW_H - 6, 40, 26 };
    memcpy(s.cap[0], stem, sizeof(stem));
    Icon_Head(s, 40, 4, 0, -1, 22, 15);
  } else if (dir == TURN_UTURN_RIGHT || dir == TURN_UTURN_LEFT) {
    // U-turn to the right (left is its mirror): up, over a half-circle, back down to a head
    s.mirror = dir == TURN_UTURN_LEFT;
    s.cap_hw = hw; s.caps = 2;
    float up[4] = { 18, HEAD_ARROW_H - 6, 18, 30 }, down[4] = { 58, 30, 58, 40 };
    memcpy(s.cap[0], up, sizeof(up));
    memcpy(s.cap[1], down, sizeof(down));
    s.has_arc = true; s.arc_cx = 38; s.arc_cy = 30; s.arc_r = 20; s.arc_hw = hw;
    s.arc_start = 180; s.arc_sweep = 180;            // left point -> over the top -> right point
    Icon_Head(s, 58, 66, 0, 1, 22, 15);
  } else {
    // Right turn (left is its mirror): up, round a quarter-circle bend, right to a filled head
    s.mirror = dir == TURN_LEFT;
    s.cap_hw = hw; s.caps = 2;
    float stem[4] = { 23, HEAD_ARROW_H - 6, 23, 42 }, across[4] = { 39, 26, 47, 26 };
    memcpy(s.cap[0], stem, sizeof(stem));
    memcpy(s.cap[1], across, sizeof(across));
    s.has_arc = true; s.arc_cx = 39; s.arc_cy = 42; s.arc_r = 16; s.arc_hw = hw;
    s.arc_start = 180; s.arc_sweep = 90;             // left point -> top point
    Icon_Head(s, 71, 26, 1, 0, 22, 15);
  }
  Icon_Render(s);

  char num[4] = "";
  if (dir == TURN_ROUNDABOUT && exit_number > 0) snprintf(num, sizeof(num), "%u", exit_number);
  Set_Text(rb_num, num);
  Show_If(rb_num, num[0]);
  lv_obj_align(rb_num, LV_ALIGN_TOP_LEFT,
               40 - lv_txt_get_width(num, strlen(num), &font_small_26, 0, LV_TEXT_FLAG_NONE) / 2,
               38 - font_small_26.line_height / 2);
}

static void Icon_Create(lv_obj_t *box)
{
  void *buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(HEAD_ARROW_W, HEAD_ARROW_H), MALLOC_CAP_SPIRAM);
  icon_canvas = lv_canvas_create(box);
  lv_canvas_set_buffer(icon_canvas, buf, HEAD_ARROW_W, HEAD_ARROW_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_canvas_fill_bg(icon_canvas, lv_color_black(), LV_OPA_TRANSP);
  rb_num = Label_Create(box, &font_small_26, COLOR_TEXT);
  lv_obj_add_flag(rb_num, LV_OBJ_FLAG_HIDDEN);
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
  Show_If(icon_app, icon == icon_app);
  Show_If(icon_target, icon == icon_target);
  Show_If(icon_reroute, icon == icon_reroute);
  Set_Text(info_top, top);
  Set_Text(info_main, main);
  lv_obj_set_style_text_font(info_main, main_font, 0);
  Set_Text(info_sub, sub);
  Set_Text(info_detail, detail);
  Show_If(info_top, top[0]);
  Show_If(info_sub, sub[0]);
  Show_If(info_detail, detail[0]);
  Show_If(summary_grid, icon == icon_target);
  // The ride summary makes the column tall: nudge it down, clear of the status row's icons
  lv_obj_set_style_pad_top(info_group, icon == icon_target ? 22 : 0, 0);
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

  // Thin progress ring near the edge: starts at 12 o'clock and fills clockwise
  ring = Smooth_Arc_Create(scr, RING_RADIUS, RING_WIDTH, COLOR_BLUE);

  lv_timer_create(Ring_Frame, RING_FRAME_MS, NULL);

  spinner = Smooth_Arc_Create(scr, RING_RADIUS, RING_WIDTH, COLOR_AMBER);
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
  Icon_Create(arrow_box);

  dist_label = Label_Create(header, &font_big_104, COLOR_TEXT);
  unit_label = Label_Create(header, &font_text_32, COLOR_GREY);
  // The fonts leave different space under their baselines; shift the unit so both sit on one line
  lv_obj_set_style_translate_y(unit_label, font_text_32.base_line - font_big_104.base_line, 0);

  road_label = Label_Create(turn_group, &font_text_32, COLOR_GREY);
  lv_obj_set_width(road_label, 380);
  lv_label_set_long_mode(road_label, LV_LABEL_LONG_DOT);
  lv_obj_align(road_label, LV_ALIGN_TOP_MID, 0, 180);

  // "then ↰": the turn after this one, when it comes right after (under the road name)
  then_pill = Plain_Box(turn_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(then_pill, lv_color_hex(0x1E1E1E), 0);
  lv_obj_set_style_bg_opa(then_pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(then_pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(then_pill, 14, 0);
  lv_obj_set_style_pad_ver(then_pill, 2, 0);
  lv_obj_set_flex_flow(then_pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(then_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(then_pill, 6, 0);
  lv_obj_align(then_pill, LV_ALIGN_TOP_MID, 0, 222);
  lv_obj_t *then_text = Label_Create(then_pill, &font_small_26, COLOR_GREY);
  lv_label_set_text(then_text, "then");
  then_icon = Label_Create(then_pill, &font_then_30, COLOR_TEXT);
  lv_obj_add_flag(then_pill, LV_OBJ_FLAG_HIDDEN);

  /* The speed limit sign (bottom right), over the faded edge of the map. No speed: the bike's
     speedometer shows it */
  limit_sign = Plain_Box(turn_group, 62, 62);
  lv_obj_set_style_radius(limit_sign, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(limit_sign, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(limit_sign, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(limit_sign, COLOR_RED, 0);
  lv_obj_set_style_border_width(limit_sign, 6, 0);
  lv_obj_align(limit_sign, LV_ALIGN_TOP_MID, 136, 296);
  limit_label = Label_Create(limit_sign, &font_text_32, lv_color_black());
  lv_obj_center(limit_label);
  lv_obj_add_flag(limit_sign, LV_OBJ_FLAG_HIDDEN);

  /* Shared bottom line: arrival time and distance left */
  eta_label = Label_Create(scr, &font_text_32, COLOR_SOFT);
  lv_obj_align(eta_label, LV_ALIGN_TOP_MID, 0, 386);
  lv_obj_add_flag(eta_label, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(Wait_Clock_Tick, 1000, NULL);

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

  // Ride summary (Arrived): two rows of value over caption
  summary_grid = Plain_Box(info_group, 330, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(summary_grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(summary_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(summary_grid, 4, 0);
  lv_obj_set_style_pad_top(summary_grid, 8, 0);
  auto stat = [](const char *caption) {
    lv_obj_t *cell = Plain_Box(summary_grid, 160, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *value = Label_Create(cell, &font_text_32, COLOR_TEXT);
    lv_obj_t *cap = Label_Create(cell, &font_small_26, COLOR_GREY);
    lv_label_set_text(cap, caption);
    return value;
  };
  sum_avg = stat("AVG km/h");
  sum_top = stat("TOP km/h");
  sum_lean = stat("LEAN L " MIDDLE_DOT " R");
  sum_g = stat("PEAK g");
  lv_obj_add_flag(summary_grid, LV_OBJ_FLAG_HIDDEN);

  // The map band's edge fade would otherwise paint over the ring where they overlap
  lv_obj_move_foreground(ring);
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
  Map_Layer_Clear();                        // the phone is gone: its position and route are stale
  Set_Mode(MODE_WAITING);
  Nav_Screen_Set_Stale(false);
  Ring_Set_Fixed(0);
  Set_Info(icon_app, "", "BikePod", &font_title_44, "Waiting for phone", "");
  Wait_Clock_Tick(NULL);
}

void Nav_Screen_Show_Rerouting(const char *detail)
{
  if (mode != MODE_REROUTING) {             // repeats of the same screen only update the text
    Set_Mode(MODE_REROUTING);
    Map_Layer_Clear_Route();
  }
  Set_Info(icon_reroute, "", "Rerouting", &font_title_44, detail && detail[0] ? detail : "Finding a new route", "");
}

void Nav_Screen_Show_Arrived(float trip_km, uint32_t trip_min, float avg_kmh, float top_kmh, const char *place)
{
  char summary[40];
  if (trip_min < 60) snprintf(summary, sizeof(summary), "%.1f km " MIDDLE_DOT " %lu min", trip_km, (unsigned long)trip_min);
  else snprintf(summary, sizeof(summary), "%.1f km " MIDDLE_DOT " %lu h %lu min", trip_km,
                (unsigned long)(trip_min / 60), (unsigned long)(trip_min % 60));
  have_trip = false;
  if (mode != MODE_ARRIVED) {
    Set_Mode(MODE_ARRIVED);
    Ring_Set_Fixed(1000);
  }
  // The ride summary: speeds from the phone, lean and g from the pod's own sensor
  char text[24];
  float lean_l, lean_r, peak_g;
  Ride_Peaks_Get(&lean_l, &lean_r, &peak_g);
  if (avg_kmh > 0) snprintf(text, sizeof(text), "%d", (int)lroundf(avg_kmh)); else strcpy(text, "-");
  Set_Text(sum_avg, text);
  if (top_kmh > 0) snprintf(text, sizeof(text), "%d", (int)lroundf(top_kmh)); else strcpy(text, "-");
  Set_Text(sum_top, text);
  snprintf(text, sizeof(text), "%d" DEGREE " " MIDDLE_DOT " %d" DEGREE, (int)lroundf(lean_l), (int)lroundf(lean_r));
  Set_Text(sum_lean, text);
  snprintf(text, sizeof(text), "%.2f", peak_g);
  Set_Text(sum_g, text);
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
  char time[12];
  Settings_Format_Clock(clock, time, sizeof(time));
  Set_Info(NULL, time, speed, &font_big_104, "km/h", "");
}

void Nav_Screen_Set_Trip(const char *eta, float km_left)
{
  char clock[12];
  Settings_Format_Clock(eta, clock, sizeof(clock));
  snprintf(eta_text, sizeof(eta_text), "%s " MIDDLE_DOT " %.1f km", clock, km_left);
  if (bump_m == 0) Set_Text(eta_label, eta_text);       // a breaker warning has the line for now
  if (!have_trip) Ride_Peaks_Reset();                   // a trip is starting: its own summary
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

void Nav_Screen_Set_Limit(uint32_t kmh)
{
  if ((int)kmh == limit_kmh) return;
  limit_kmh = (int)kmh;
  if (kmh > 0) {
    char text[8];
    snprintf(text, sizeof(text), "%lu", (unsigned long)kmh);
    Set_Text(limit_label, text);
    // Three digits need the smaller font to fit the sign
    lv_obj_set_style_text_font(limit_label, kmh >= 100 ? &font_small_26 : &font_text_32, 0);
    lv_obj_center(limit_label);
  }
  Show_If(limit_sign, kmh > 0);
}

void Nav_Screen_Set_Then(char dir, uint32_t gap_m)
{
  // Material Icons Round: turn_left, turn_right, straight, roundabout, u_turn_right, u_turn_left
  const char *glyph = dir == 'L' ? "\xEE\xAE\xA6" : dir == 'R' ? "\xEE\xAE\xAB" :
                      dir == 'S' ? "\xEE\xAD\x95" : dir == 'O' ? "\xEE\xAE\xA3" :
                      dir == 'U' ? "\xEE\xAE\xA2" : dir == 'V' ? "\xEE\xAE\xA1" : NULL;
  if (glyph) Set_Text(then_icon, glyph);
  bool show = glyph != NULL;
  if (show == lv_obj_has_flag(then_pill, LV_OBJ_FLAG_HIDDEN)) Show_If(then_pill, show);
}

void Nav_Screen_Set_Bump(uint32_t metres)
{
  // Rounded so the line doesn't flicker every metre: 10 m steps, 5 m when close
  uint32_t step = metres > 50 ? 10 : 5;
  uint32_t shown = metres == 0 ? 0 : LV_MAX((metres + step / 2) / step * step, step);
  if (shown == bump_m) return;
  bump_m = shown;
  if (bump_m > 0) {
    char text[40];
    // Short: the bottom line is where the round screen is narrow ("Speed breaker" reached the ring)
    snprintf(text, sizeof(text), "Breaker " MIDDLE_DOT " %lu m", (unsigned long)bump_m);
    Set_Text(eta_label, text);
    lv_obj_set_style_text_color(eta_label, COLOR_AMBER, 0);
    Show_If(eta_label, mode == MODE_TURN);
  } else {
    Set_Text(eta_label, eta_text);
    lv_obj_set_style_text_color(eta_label, COLOR_SOFT, 0);
    Show_If(eta_label, have_trip && (mode == MODE_TURN || mode == MODE_REROUTING));
  }
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

void Nav_Screen_Update(Turn_Dir dir, uint32_t distance_m, uint16_t progress, const char *road,
                       uint8_t rb_exit, int16_t rb_degrees)
{
  if (mode != MODE_TURN) {
    Set_Mode(MODE_TURN);
    Ring_Set_Fixed(0);   // a fresh approach after another screen
  }
  turn_distance_m = distance_m;
  Map_Layer_Set_Turn_Distance(distance_m);   // the map zooms in as the turn gets closer

  // The icon is only redrawn when the turn changes (drawing it takes a few tens of ms)
  static int shown_key = -1;
  int key = dir == TURN_ROUNDABOUT ? (int)dir * 1000000 + (rb_degrees + 1000) * 256 + rb_exit : (int)dir * 1000000;
  if (key != shown_key) {
    shown_key = key;
    Icon_Set(dir, rb_degrees, rb_exit);
  }

  // Round like a car navigator: 1.2 km, 350 m (10 m steps), 45 m (5 m steps)
  char num[12];
  if (distance_m >= 1000) {
    snprintf(num, sizeof(num), "%.1f", distance_m / 1000.0f);
    Set_Text(unit_label, "km");
  } else {
    uint32_t step = distance_m >= 100 ? 10 : 5;
    snprintf(num, sizeof(num), "%lu", (unsigned long)((distance_m + step / 2) / step * step));
    Set_Text(unit_label, "m");
  }
  Set_Text(dist_label, num);

  Ring_Set_Target(progress);

  // Long names: try the smaller font before cutting the name off with "..."
  if (strcmp(lv_label_get_text(road_label), road) != 0) {
    lv_point_t size;
    lv_txt_get_size(&size, road, &font_text_32, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_style_text_font(road_label, size.x > lv_obj_get_width(road_label) ? &font_small_26 : &font_text_32, 0);
    lv_label_set_text(road_label, road);
  }
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
