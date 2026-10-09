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
#define LATERAL_AXIS    { 1.0f, 0.0f, 0.0f }

#define SAMPLE_MS       10        // 100 readings a second
#define CAL_SAMPLES     100       // one second still at start-up
#define MOVING_MPS      3.0f      // above this, trust speed x turn rate; below, trust gravity
#define PULL_PER_S      1.0f      // how quickly the gyro estimate is pulled to the reference
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
static uint8_t history_head = 0;

// UI
static lv_obj_t *root;
static lv_obj_t *gauge;
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
  const float dt = SAMPLE_MS / 1000.0f;
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

  static uint32_t last_history_ms = 0;
  if (lv_tick_elaps(last_history_ms) >= HISTORY_MS) {
    last_history_ms = lv_tick_get();
    history[history_head] = lean;
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
}

/*********************************** Drawing ***********************************/
// Top scale: dashes from 60 deg left to 60 deg right over the top; blue from upright to the
// current lean, a white tick at the lean, grey ticks at the deepest lean each side
static void Gauge_Draw(lv_event_t *e)
{
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t box;
  lv_obj_get_coords(gauge, &box);
  lv_point_t c = { (lv_coord_t)((box.x1 + box.x2) / 2), (lv_coord_t)((box.y1 + box.y2) / 2) };

  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.width = 10;
  const int outer = 220;
  float lean_arc = fmaxf(-GAUGE_HALF_SPAN, fminf(GAUGE_HALF_SPAN, lean * GAUGE_HALF_SPAN / GAUGE_MAX_DEG));
  for (int a = 270 - GAUGE_HALF_SPAN; a < 270 + GAUGE_HALF_SPAN; a += 5) {
    float mid = a + 1.5f - 270;                         // dash centre, relative to 12 o'clock
    bool lit = (lean_arc > 0 && mid > 0 && mid < lean_arc) || (lean_arc < 0 && mid < 0 && mid > lean_arc);
    arc.color = lit ? COLOR_BLUE : COLOR_DASH;
    lv_draw_arc(ctx, &arc, &c, outer, a % 360, (a + 3) % 360);
  }

  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.round_start = line.round_end = 1;
  auto tick = [&](float rel_deg, int r1, int r2, lv_coord_t width, lv_color_t color) {
    float t = (270 + rel_deg) * (float)M_PI / 180;
    lv_point_t p1 = { (lv_coord_t)lroundf(c.x + r1 * cosf(t)), (lv_coord_t)lroundf(c.y + r1 * sinf(t)) };
    lv_point_t p2 = { (lv_coord_t)lroundf(c.x + r2 * cosf(t)), (lv_coord_t)lroundf(c.y + r2 * sinf(t)) };
    line.width = width;
    line.color = color;
    lv_draw_line(ctx, &line, &p1, &p2);
  };
  if (max_left > 1) tick(-fminf(max_left, GAUGE_MAX_DEG) * GAUGE_HALF_SPAN / GAUGE_MAX_DEG, 203, 222, 3, COLOR_GREY);
  if (max_right > 1) tick(fminf(max_right, GAUGE_MAX_DEG) * GAUGE_HALF_SPAN / GAUGE_MAX_DEG, 203, 222, 3, COLOR_GREY);
  tick(lean_arc, 198, 226, 5, lv_color_white());
}

static void Refresh(lv_timer_t *timer)
{
  if (!visible) return;
  char text[24];
  int shown = (int)lroundf(fabsf(lean));
  snprintf(text, sizeof(text), "%d" DEGREE, shown);
  lv_label_set_text(value_label, text);
  lv_label_set_text(side_label, shown == 0 ? " " : (lean > 0 ? "R" : "L"));
  snprintf(text, sizeof(text), "%d" DEGREE, (int)lroundf(max_left));
  lv_label_set_text(max_l_label, text);
  snprintf(text, sizeof(text), "%.2f", fabsf(lat_g));
  lv_label_set_text(lat_label, text);
  snprintf(text, sizeof(text), "%d" DEGREE, (int)lroundf(max_right));
  lv_label_set_text(max_r_label, text);

  // Graph: oldest on the left; 45 deg of lean = 24 px either side of the middle
  for (int i = 0; i < HISTORY; i++) {
    float v = history[(history_head + i) % HISTORY];
    graph_pts[i].x = i * 3;
    graph_pts[i].y = (lv_coord_t)lroundf(25 - fmaxf(-45, fminf(45, v)) * 24 / 45);
  }
  lv_line_set_points(graph, graph_pts, HISTORY);

  if (calibrated) {
    snprintf(text, sizeof(text), "Calibrated " MIDDLE_DOT " tilt %d" DEGREE, (int)lroundf(tilt_deg));
    lv_label_set_text(cal_label, text);
  } else {
    lv_label_set_text(cal_label, "Calibrating, keep still");
  }
  lv_obj_invalidate(gauge);
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

  gauge = Box(root, LV_PCT(100), LV_PCT(100));
  lv_obj_add_event_cb(gauge, Gauge_Draw, LV_EVENT_DRAW_MAIN, NULL);

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
  lv_obj_set_style_line_width(graph, 4, 0);
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
  lv_timer_create(Refresh, 100, NULL);
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
