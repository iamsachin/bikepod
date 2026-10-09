/*****************************************************************************
  | File        :   Lean_Screen.cpp
  | help        :   Lean angle from the QMI8658 motion sensor (see Lean_Screen.h).

  How the lean is estimated:
  - At start-up the pod sits still for a second: the average accelerometer reading is "up",
    and the average gyro reading is its bias (subtracted from then on).
  - The gyro's rotation about the bike's forward axis is integrated: fast and smooth, but it drifts.
  - It is pulled gently towards a reference that doesn't drift:
      moving:  lean = atan(speed x turn rate / g)  (what a balanced bike in a turn must lean)
      stopped: the tilt of gravity across the pod (the accelerometer alone; on the move it reads
               near zero in a steady turn, because cornering force and gravity add up along
               the bike's own axis)
  - Lateral g is speed x turn rate / g.
******************************************************************************/
#include "Lean_Screen.h"
#include <Arduino.h>
#include <math.h>
#include "Gyro_QMI8658.h"

LV_FONT_DECLARE(font_big_104);
LV_FONT_DECLARE(font_title_44);
LV_FONT_DECLARE(font_text_32);
LV_FONT_DECLARE(font_small_26);

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_DASH      lv_color_hex(0x2E2E2E)
#define COLOR_GREY      lv_color_hex(0xA8A8A8)
#define COLOR_DIM       lv_color_hex(0x6A6A6A)
#define DEGREE          "\xC2\xB0"
#define MIDDLE_DOT      "\xC2\xB7"

// Which sensor axis points to the right of the screen (as the rider sees it), and its sign.
// Checked on the bench by tilting the pod (serial "imu" prints the readings).
#define LATERAL_AXIS    { 0.0f, 1.0f, 0.0f }     // X turned out to run up the screen (it measured pitch)

#define SAMPLE_MS       10        // 100 readings a second
#define CAL_SAMPLES     100       // one second still at start-up
#define MOVING_MPS      3.0f      // above this, trust speed x turn rate; below, trust gravity
#define PULL_PER_S      2.0f      // how quickly the gyro estimate is pulled to the reference (drift only:
                                  // the gyro itself follows the lean instantly)
#define GAUGE_MAX_DEG   60.0f     // lean at each end of the top scale
#define GAUGE_HALF_SPAN 100       // degrees of arc either side of 12 o'clock
#define HISTORY         100       // graph points
#define HISTORY_MS      200       // one point every 0.2 s: 20 s of history

struct Vec { float x, y, z; };
static inline float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec Cross(Vec a, Vec b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline Vec Scale(Vec a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static inline Vec Sub(Vec a, Vec b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline Vec Unit(Vec a) { float l = sqrtf(Dot(a, a)); return l > 1e-6f ? Scale(a, 1 / l) : a; }

// Estimate
static int cal_count = 0;
static Vec acc_sum, gyro_sum;
static Vec up, lateral, forward, gyro_bias;
static bool calibrated = false;
static float tilt_deg = 0;
static float lean = 0;            // degrees, + = right
static float lat_g = 0;           // + = to the right
static float max_left = 0, max_right = 0;
static float speed_mps = 0;
static uint32_t speed_at_ms = 0;
static float history[HISTORY];
static float graph_lean = 0;      // lean filtered for the graph (~0.5 s), so it draws as a smooth curve
static uint8_t history_head = 0;

// UI
static lv_obj_t *root;
// Top scale, split so a change only redraws a small strip: static grey dashes, a blue arc for
// the lean, a static layer of black gaps over it (turning the arc into dashes), and small tick lines
static lv_obj_t *dashes;
static lv_obj_t *lit_arc;
static lv_obj_t *gaps;
static lv_obj_t *tick_now, *tick_max_l, *tick_max_r;
static lv_point_t tick_pts[3][2];
static lv_obj_t *value_label, *side_label;
static lv_obj_t *max_l_label, *lat_label, *max_r_label;
static lv_obj_t *graph;
static lv_point_t graph_pts[HISTORY];
static lv_obj_t *cal_label;
static bool visible = false;

/********************************* Sensor *********************************/
static void Sample(lv_timer_t *timer)
{
  getAccelerometer();
  getGyroscope();
  Vec a = { Accel.x, Accel.y, Accel.z };    // g
  Vec w = { Gyro.x, Gyro.y, Gyro.z };       // degrees per second

  if (!calibrated) {
    acc_sum = { acc_sum.x + a.x, acc_sum.y + a.y, acc_sum.z + a.z };
    gyro_sum = { gyro_sum.x + w.x, gyro_sum.y + w.y, gyro_sum.z + w.z };
    if (++cal_count < CAL_SAMPLES) return;
    up = Unit(acc_sum);
    gyro_bias = Scale(gyro_sum, 1.0f / CAL_SAMPLES);
    Vec l = LATERAL_AXIS;
    lateral = Unit(Sub(l, Scale(up, Dot(l, up))));     // horizontal, to the right
    forward = Cross(up, lateral);                      // right-handed: up x right = forward
    tilt_deg = acosf(fminf(1, fabsf(up.z))) * 180 / (float)M_PI;   // screen face vs. horizontal
    lean = 0;
    calibrated = true;
    printf("LEAN: calibrated, tilt %.0f deg\r\n", tilt_deg);
    return;
  }

  w = Sub(w, gyro_bias);
  // Real time since the last reading: this runs in the screen's loop, which can be late while a
  // frame is being drawn (assuming a fixed 10 ms made rotations count up to 10x too small)
  static uint32_t last_us = 0;
  uint32_t now_us = micros();
  float dt = last_us ? fminf((now_us - last_us) / 1e6f, 0.05f) : SAMPLE_MS / 1000.0f;
  last_us = now_us;
  lean += Dot(w, forward) * dt;                        // roll rate, + = leaning further right

  float v = lv_tick_elaps(speed_at_ms) < 3000 ? speed_mps : 0;
  float yaw_rad = Dot(w, up) * (float)M_PI / 180;      // + = turning left (anticlockwise from above)
  float reference;
  if (v > MOVING_MPS) {
    lat_g = -v * yaw_rad / 9.81f;
    reference = atanf(lat_g) * 180 / (float)M_PI;
  } else {
    float across = Dot(a, lateral) / fmaxf(0.2f, sqrtf(Dot(a, a)));
    reference = -asinf(fmaxf(-1, fminf(1, across))) * 180 / (float)M_PI;
    lat_g = -Dot(a, lateral);
  }
  lean += (reference - lean) * fminf(1, PULL_PER_S * dt);

  if (lean > max_right) max_right = lean;
  if (-lean > max_left) max_left = -lean;

  graph_lean += (lean - graph_lean) * dt / 0.5f;
  static uint32_t last_history_ms = 0;
  if (lv_tick_elaps(last_history_ms) >= HISTORY_MS) {
    last_history_ms = lv_tick_get();
    history[history_head] = graph_lean;
    history_head = (history_head + 1) % HISTORY;
  }
}

void Lean_Set_Speed(float metres_per_second)
{
  speed_mps = metres_per_second;
  speed_at_ms = lv_tick_get();
}

void Lean_Calibrate(void)
{
  calibrated = false;
  cal_count = 0;
  acc_sum = gyro_sum = { 0, 0, 0 };
  max_left = max_right = 0;
  memset(history, 0, sizeof(history));
  graph_lean = 0;
}

/*********************************** Drawing ***********************************/
// Scale geometry: dashes of 3 degrees every 5 degrees, from 100 degrees left of 12 o'clock to
// 100 right (60 deg of lean at each end). LVGL angles: 0 = 3 o'clock, clockwise, so 270 = top.
#define SCALE_OUTER   220
#define SCALE_WIDTH   10
#define SCALE_START   (270 - GAUGE_HALF_SPAN)
#define SCALE_END     (270 + GAUGE_HALF_SPAN)

// grow: extra pixels each side (the black gaps are drawn slightly wider so they fully cut the blue)
static void Scale_Arcs(lv_event_t *e, lv_color_t color, int from, int len, int grow)
{
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_obj_t *obj = lv_event_get_target(e);
  lv_area_t box;
  lv_obj_get_coords(obj, &box);
  lv_point_t c = { (lv_coord_t)((box.x1 + box.x2) / 2), (lv_coord_t)((box.y1 + box.y2) / 2) };
  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.width = SCALE_WIDTH + 2 * grow;
  arc.color = color;
  for (int a = SCALE_START + from; a < SCALE_END; a += 5) lv_draw_arc(ctx, &arc, &c, SCALE_OUTER + grow, a % 360, (a + len) % 360);
}

static void Dashes_Draw(lv_event_t *e) { Scale_Arcs(e, COLOR_DASH, 0, 3, 0); }          // grey dashes
static void Gaps_Draw(lv_event_t *e) { Scale_Arcs(e, lv_color_black(), 3, 2, 1); }      // black gaps

static lv_obj_t *Scale_Layer(lv_obj_t *parent, lv_event_cb_t draw)
{
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_size(obj, 2 * SCALE_OUTER + 4, 2 * SCALE_OUTER + 4);
  lv_obj_center(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(obj, draw, LV_EVENT_DRAW_MAIN, NULL);
  return obj;
}

static lv_obj_t *Tick_Line(lv_obj_t *parent, lv_coord_t width, lv_color_t color)
{
  lv_obj_t *line = lv_line_create(parent);
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_color(line, color, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  return line;
}

// Points a tick line radially at `rel_deg` from 12 o'clock (screen coordinates; the root is full screen)
static void Place_Tick(lv_obj_t *line, lv_point_t *pts, float rel_deg, int r1, int r2)
{
  float t = (270 + rel_deg) * (float)M_PI / 180;
  lv_point_t a = { (lv_coord_t)lroundf(240 + r1 * cosf(t)), (lv_coord_t)lroundf(240 + r1 * sinf(t)) };
  lv_point_t b = { (lv_coord_t)lroundf(240 + r2 * cosf(t)), (lv_coord_t)lroundf(240 + r2 * sinf(t)) };
  if (a.x == pts[0].x && a.y == pts[0].y && b.x == pts[1].x && b.y == pts[1].y) return;   // unchanged
  pts[0] = a;
  pts[1] = b;
  lv_line_set_points(line, pts, 2);
}

static void Update_Scale(void)
{
  float lean_arc = fmaxf(-GAUGE_HALF_SPAN, fminf(GAUGE_HALF_SPAN, lean * GAUGE_HALF_SPAN / GAUGE_MAX_DEG));
  int end = 270 + (int)lroundf(lean_arc);
  if (end == 270) {
    lv_obj_add_flag(lit_arc, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(lit_arc, LV_OBJ_FLAG_HIDDEN);
    if (end > 270) lv_arc_set_angles(lit_arc, 270, end % 360);
    else lv_arc_set_angles(lit_arc, end, 270);
  }
  Place_Tick(tick_now, tick_pts[0], lean_arc, 198, 226);
  Place_Tick(tick_max_l, tick_pts[1], -fminf(max_left, GAUGE_MAX_DEG) * GAUGE_HALF_SPAN / GAUGE_MAX_DEG, 203, 222);
  Place_Tick(tick_max_r, tick_pts[2], fminf(max_right, GAUGE_MAX_DEG) * GAUGE_HALF_SPAN / GAUGE_MAX_DEG, 203, 222);
  if (max_left > 1) lv_obj_clear_flag(tick_max_l, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(tick_max_l, LV_OBJ_FLAG_HIDDEN);
  if (max_right > 1) lv_obj_clear_flag(tick_max_r, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(tick_max_r, LV_OBJ_FLAG_HIDDEN);
}

// Only touch a label when its text actually changes (setting it redraws it)
static void Set_Text(lv_obj_t *label, const char *text)
{
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void Refresh(lv_timer_t *timer)
{
  if (!visible) return;
  char text[24];
  int shown = (int)lroundf(fabsf(lean));
  snprintf(text, sizeof(text), "%d" DEGREE, shown);
  Set_Text(value_label, text);
  Set_Text(side_label, shown == 0 ? " " : (lean > 0 ? "R" : "L"));
  snprintf(text, sizeof(text), "%d" DEGREE, (int)lroundf(max_left));
  Set_Text(max_l_label, text);
  snprintf(text, sizeof(text), "%.2f", fabsf(lat_g));
  Set_Text(lat_label, text);
  snprintf(text, sizeof(text), "%d" DEGREE, (int)lroundf(max_right));
  Set_Text(max_r_label, text);
  Update_Scale();

  static uint8_t drawn_head = 0xFF;            // the graph only changes when a sample is added
  if (drawn_head != history_head) {
    drawn_head = history_head;

  // Graph: oldest on the left; 45 deg of lean = 24 px either side of the middle. A 5-point
  // moving average on top of the filtered samples keeps the line a smooth curve.
  for (int i = 0; i < HISTORY; i++) {
    float sum = 0;
    int n = 0;
    for (int k = -2; k <= 2; k++) {
      int j = i + k;
      if (j < 0 || j >= HISTORY) continue;
      sum += history[(history_head + j) % HISTORY];
      n++;
    }
    float v = sum / n;
    graph_pts[i].x = i * 3;
    graph_pts[i].y = (lv_coord_t)lroundf(25 - fmaxf(-45, fminf(45, v)) * 24 / 45);
  }
  lv_line_set_points(graph, graph_pts, HISTORY);
  }

  if (calibrated) {
    snprintf(text, sizeof(text), "Calibrated " MIDDLE_DOT " tilt %d" DEGREE, (int)lroundf(tilt_deg));
    Set_Text(cal_label, text);
  } else {
    Set_Text(cal_label, "Calibrating, keep still");
  }
}

static lv_obj_t *Label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

static lv_obj_t *Box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, h);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return box;
}

// One stat column: value over a small caption
static lv_obj_t *Stat(lv_obj_t *row, const char *caption, lv_color_t value_color)
{
  lv_obj_t *col = Box(row, 100, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *value = Label(col, &font_text_32, value_color);
  lv_obj_t *cap = Label(col, &font_small_26, COLOR_DIM);
  lv_label_set_text(cap, caption);
  return value;
}

void Lean_Screen_Create(lv_obj_t *parent)
{
  root = Box(parent, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  dashes = Scale_Layer(root, Dashes_Draw);
  lit_arc = lv_arc_create(root);
  lv_obj_set_size(lit_arc, 2 * SCALE_OUTER, 2 * SCALE_OUTER);
  lv_obj_center(lit_arc);
  lv_obj_remove_style(lit_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(lit_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(lit_arc, 0, 0);
  lv_obj_set_style_arc_opa(lit_arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(lit_arc, SCALE_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(lit_arc, COLOR_BLUE, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(lit_arc, false, LV_PART_INDICATOR);
  lv_arc_set_bg_angles(lit_arc, 0, 360);
  lv_obj_add_flag(lit_arc, LV_OBJ_FLAG_HIDDEN);
  gaps = Scale_Layer(root, Gaps_Draw);
  tick_max_l = Tick_Line(root, 3, COLOR_GREY);
  tick_max_r = Tick_Line(root, 3, COLOR_GREY);
  tick_now = Tick_Line(root, 5, lv_color_white());

  lv_obj_t *title = Label(root, &font_small_26, COLOR_GREY);
  lv_label_set_text(title, "LEAN " MIDDLE_DOT " ESTIMATED");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 104);

  // Big lean with the side beside it, sharing a baseline
  lv_obj_t *big = Box(root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(big, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(big, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(big, 10, 0);
  lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 142);
  value_label = Label(big, &font_big_104, lv_color_white());
  side_label = Label(big, &font_title_44, COLOR_GREY);
  lv_obj_set_style_translate_y(side_label, font_title_44.base_line - font_big_104.base_line, 0);

  lv_obj_t *row = Box(root, 330, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 250);
  max_l_label = Stat(row, "MAX L", lv_color_white());
  lat_label = Stat(row, "LAT g", COLOR_GREY);
  max_r_label = Stat(row, "MAX R", lv_color_white());

  // History graph with a faint upright line through the middle
  lv_obj_t *graph_box = Box(root, 300, 50);
  lv_obj_align(graph_box, LV_ALIGN_TOP_MID, 0, 334);
  static lv_point_t mid[] = { {0, 25}, {297, 25} };
  lv_obj_t *mid_line = lv_line_create(graph_box);
  lv_line_set_points(mid_line, mid, 2);
  lv_obj_set_style_line_color(mid_line, COLOR_DASH, 0);
  lv_obj_set_style_line_width(mid_line, 2, 0);
  graph = lv_line_create(graph_box);
  lv_obj_set_style_line_color(graph, COLOR_BLUE, 0);
  lv_obj_set_style_line_width(graph, 3, 0);
  lv_obj_set_style_line_rounded(graph, true, 0);

  cal_label = Label(root, &font_small_26, COLOR_DIM);
  lv_obj_align(cal_label, LV_ALIGN_TOP_MID, 0, 392);

  // Page dots: navigation, lean (this one)
  lv_obj_t *dots = Box(root, LV_SIZE_CONTENT, 4);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(dots, 6, 0);
  lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, 432);
  for (int i = 0; i < 2; i++) {
    lv_obj_t *dot = Box(dots, i == 1 ? 18 : 6, 4);
    lv_obj_set_style_radius(dot, 2, 0);
    lv_obj_set_style_bg_color(dot, i == 1 ? lv_color_white() : COLOR_DIM, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  }

  lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

  QMI8658_Init();                     // gyro range 256 deg/s: see gyro_scale in Gyro_QMI8658.cpp
  Lean_Calibrate();
  lv_timer_create(Sample, SAMPLE_MS, NULL);
  lv_timer_create(Refresh, 33, NULL);   // ~30 updates a second
}

void Lean_Screen_Show(bool show)
{
  visible = show;
  if (show) {
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    Refresh(NULL);
  } else {
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
  }
}
