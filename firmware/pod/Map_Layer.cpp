/*****************************************************************************
  | File        :   Map_Layer.cpp
  | help        :   Streets, fuel stations and the route around the rider, drawn through a
                    smoothly moving camera (see Map_Layer.h).

  Areas: the Bluetooth task fills one buffer while the screen draws the other; they swap when
  an area is complete, so a new area never flickers in.

  Camera: GPS poses arrive about once a second. Each frame the camera is placed where the rider
  should be by now (last pose + measured velocity and turn rate), so it moves continuously. When
  a new pose arrives, the difference between where the camera was and the new prediction is kept
  as an offset that fades out over CAM_SETTLE_S, so corrections never snap and there is no lag.
  Zoom eases towards its target the same way.
******************************************************************************/
// This file's drawing loops run for every map pixel: compile it for speed, not size (Arduino's -Os)
#pragma GCC optimize("O2")
#include "Map_Layer.h"
#include <Arduino.h>
#include <math.h>

#define MAP_MAX_ROADS   400
#define MAP_MAX_POINTS  2400
#define MAP_MAX_FUEL    48
#define ROUTE_MAX       256       // route points after its corners are rounded

#define FRAME_MS        20        // camera update / redraw rate (~50 fps when moving)
#define CAM_SETTLE_S    0.35f     // how quickly a correction from a new pose fades out
#define ZOOM_EASE_S     0.6f      // how quickly the zoom follows its target
#define PREDICT_MAX_S   1.5f      // stop predicting if poses stop coming
#define ROUTE_BEND_PX   18.0f     // radius of the route's rounded corners on screen

struct Map_Road { uint16_t start; uint8_t count; uint8_t road_class; int16_t cx, cy, radius; };

struct Map_Area {
  uint16_t id;
  uint16_t road_count;
  uint16_t point_count;
  bool overflow;
  Map_Road roads[MAP_MAX_ROADS];
  int16_t points[MAP_MAX_POINTS][2];
  uint8_t fuel_count;
  int16_t fuel[MAP_MAX_FUEL][2];
};

static Map_Area areas[2];
static Map_Area *volatile active = NULL;      // being drawn (main loop only)
static Map_Area *volatile building = &areas[0];
static Map_Area *volatile ready = NULL;       // complete, waiting for the main loop to swap in
static portMUX_TYPE map_lock = portMUX_INITIALIZER_UNLOCKED;

// Line widths and colours by road class: big roads a little wider and lighter
static const lv_coord_t road_width[4] = { 0, 12, 8, 5 };
static const uint32_t road_color[4] = { 0, 0x4A4A4A, 0x3A3A3A, 0x2C2C2C };

// Route look (matches the turn screen)
#define ROUTE_WIDTH     10
#define ROUTE_CASING    3
#define COLOR_ROUTE     0xD6D6D6
#define COLOR_ROUTE_DONE 0x262626

LV_FONT_DECLARE(font_fuel_14);       // Material Icons Round "local_gas_station" only
#define FUEL_GLYPH  "\xEE\x95\x86"      // U+E546 in UTF-8
#define FUEL_BADGE  20                    // badge diameter, px

static lv_obj_t *layer;
static lv_coord_t center_x, center_y;   // where the rider's marker is, inside the layer

/* Each map area is in metres north-up around its own reference point (latitude/longitude, sent
   with MAPBEGIN). Knowing the references, a position in one area converts to any other, so the
   area on screen can always be drawn from the latest pose, whichever area that pose was sent for
   (an area that didn't arrive completely no longer makes the map vanish). */
#define REF_SLOTS 8
struct Area_Ref { bool valid; uint16_t id; double lat, lng; };
static Area_Ref refs[REF_SLOTS];
static uint8_t ref_next = 0;
static portMUX_TYPE ref_lock = portMUX_INITIALIZER_UNLOCKED;

static void Ref_Store(uint16_t id, double lat, double lng)
{
  portENTER_CRITICAL(&ref_lock);
  int slot = -1;
  for (int i = 0; i < REF_SLOTS; i++) if (refs[i].valid && refs[i].id == id) slot = i;
  if (slot < 0) { slot = ref_next; ref_next = (ref_next + 1) % REF_SLOTS; }
  refs[slot] = { true, id, lat, lng };
  portEXIT_CRITICAL(&ref_lock);
}

static bool Ref_Find(uint16_t id, double *lat, double *lng)
{
  bool found = false;
  portENTER_CRITICAL(&ref_lock);
  for (int i = 0; i < REF_SLOTS; i++)
    if (refs[i].valid && refs[i].id == id) { *lat = refs[i].lat; *lng = refs[i].lng; found = true; }
  portEXIT_CRITICAL(&ref_lock);
  return found;
}

// Converts a point from area `from` to area `to` (same flat projection the phone uses)
static bool Convert(uint16_t from, float x, float y, uint16_t to, float *ox, float *oy)
{
  if (from == to) { *ox = x; *oy = y; return true; }
  double flat, flng, tlat, tlng;
  if (!Ref_Find(from, &flat, &flng) || !Ref_Find(to, &tlat, &tlng)) return false;
  *ox = x + (float)((flng - tlng) * 111320.0 * cos(tlat * M_PI / 180));
  *oy = y + (float)((flat - tlat) * 110540.0);
  return true;
}

struct Pose {
  bool valid;
  uint16_t area;                // the area this pose was sent in
  float x, y, heading;          // metres (that area's frame), degrees clockwise from north
  float vx, vy, turn;           // metres/s and degrees/s, measured from successive poses
  uint32_t at;                  // ms
};
static Pose last_pose;
static bool pose_ok = false;    // the latest pose converts into the area on screen
static float pose_x, pose_y;    // ...and there it is

// Route origin: where the rider was (and in which area) when the route arrived
static uint16_t route_area;
static float route_ox, route_oy;
static bool has_route = false;

// Camera (area frame) and zoom
static float cam_x, cam_y, cam_h;
static float off_x, off_y, off_h;   // correction left over from the last pose, fading out
static uint32_t off_at;
static float zoom = 0, zoom_target = 0;
static float drawn_x = 1e9, drawn_y, drawn_h, drawn_zoom;

// Route, north-up metres relative to its origin, corners already rounded
static float route[ROUTE_MAX][2];
static uint16_t route_count = 0;

static void Refresh_Pose(void);

/******************************* Bluetooth task side *******************************/
void Map_Data_Begin(uint16_t area_id, double ref_lat, double ref_lng)
{
  Ref_Store(area_id, ref_lat, ref_lng);
  portENTER_CRITICAL(&map_lock);
  Map_Area *a = building;
  if (a) {
    a->id = area_id;
    a->road_count = a->point_count = 0;
    a->fuel_count = 0;
    a->overflow = false;
  }
  portEXIT_CRITICAL(&map_lock);
}

void Map_Data_Add_Road(uint16_t area_id, uint8_t road_class, const int16_t (*pts)[2], uint8_t count)
{
  if (count < 2 || road_class < 1 || road_class > 3) return;
  portENTER_CRITICAL(&map_lock);
  Map_Area *a = building;
  if (a && a->id == area_id && a->road_count < MAP_MAX_ROADS && a->point_count + count <= MAP_MAX_POINTS) {
    Map_Road &r = a->roads[a->road_count++];
    r.start = a->point_count;
    r.count = count;
    r.road_class = road_class;
    // Bounding circle, so roads off screen can be skipped without looking at their points
    int16_t lx = pts[0][0], hx = pts[0][0], ly = pts[0][1], hy = pts[0][1];
    for (uint8_t i = 1; i < count; i++) {
      lx = LV_MIN(lx, pts[i][0]); hx = LV_MAX(hx, pts[i][0]);
      ly = LV_MIN(ly, pts[i][1]); hy = LV_MAX(hy, pts[i][1]);
    }
    r.cx = (lx + hx) / 2;
    r.cy = (ly + hy) / 2;
    r.radius = (int16_t)(sqrtf((float)(hx - lx) * (hx - lx) + (float)(hy - ly) * (hy - ly)) / 2 + 1);
    memcpy(a->points[a->point_count], pts, count * sizeof(a->points[0]));
    a->point_count += count;
  } else if (a) {
    a->overflow = true;                       // too much map: the rest is dropped, area still usable
  }
  portEXIT_CRITICAL(&map_lock);
}

void Map_Data_Add_Fuel(uint16_t area_id, const int16_t (*pts)[2], uint8_t count)
{
  portENTER_CRITICAL(&map_lock);
  Map_Area *a = building;
  if (a && a->id == area_id) {
    for (uint8_t i = 0; i < count && a->fuel_count < MAP_MAX_FUEL; i++) {
      a->fuel[a->fuel_count][0] = pts[i][0];
      a->fuel[a->fuel_count][1] = pts[i][1];
      a->fuel_count++;
    }
  }
  portEXIT_CRITICAL(&map_lock);
}

void Map_Data_End(uint16_t area_id, uint16_t road_count)
{
  portENTER_CRITICAL(&map_lock);
  Map_Area *a = building;
  // Only accept a complete area (all roads arrived, or the rest didn't fit)
  if (a && a->id == area_id && (a->road_count == road_count || a->overflow)) {
    ready = a;
    building = NULL;                          // nothing more is written until the main loop swaps
  }
  portEXIT_CRITICAL(&map_lock);
}

/********************************* Camera *********************************/
static float Wrap180(float d)
{
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

// The latest pose, in the frame of the area on screen
static void Refresh_Pose(void)
{
  pose_ok = active && last_pose.valid && Convert(last_pose.area, last_pose.x, last_pose.y, active->id, &pose_x, &pose_y);
}

// Where the rider should be now (area on screen) according to the latest pose and its motion
static void Predict(uint32_t now, float *x, float *y, float *h)
{
  float dt = fminf((uint32_t)(now - last_pose.at) / 1000.0f, PREDICT_MAX_S);
  *x = pose_x + last_pose.vx * dt;
  *y = pose_y + last_pose.vy * dt;
  *h = last_pose.heading + last_pose.turn * dt;
}

static void Update_Camera(uint32_t now)
{
  if (!pose_ok) return;
  float px, py, ph;
  Predict(now, &px, &py, &ph);
  float fade = expf(-(float)(uint32_t)(now - off_at) / 1000.0f / CAM_SETTLE_S);
  cam_x = px + off_x * fade;
  cam_y = py + off_y * fade;
  cam_h = ph + off_h * fade;
}

void Map_Layer_Set_Pose(uint16_t area_id, int16_t x, int16_t y, int16_t heading_deg)
{
  uint32_t now = lv_tick_get();
  bool was_drawn = pose_ok && drawn_x < 1e8;
  float before_x = cam_x, before_y = cam_y, before_h = cam_h;   // camera as currently shown

  Pose p = last_pose;
  float px, py;
  // Motion since the previous pose (converted into this pose's area), smoothed against GPS jitter
  if (p.valid && Convert(p.area, p.x, p.y, area_id, &px, &py)) {
    float dt = (uint32_t)(now - p.at) / 1000.0f;
    if (dt > 0.05f && dt < 3) {
      float vx = (x - px) / dt, vy = (y - py) / dt;
      float turn = LV_CLAMP(-120.0f, Wrap180(heading_deg - p.heading) / dt, 120.0f);
      p.vx = 0.5f * p.vx + 0.5f * vx;
      p.vy = 0.5f * p.vy + 0.5f * vy;
      p.turn = 0.5f * p.turn + 0.5f * turn;
    } else if (dt >= 3) {
      p.vx = p.vy = p.turn = 0;
    }
    if (dt <= 0.05f) return;            // the same moment sent for another area: nothing new
  } else if (!p.valid) {
    p = Pose{};
  }
  p.valid = true;
  p.area = area_id;
  p.x = x;
  p.y = y;
  p.heading = p.heading + Wrap180(heading_deg - p.heading);   // continuous (no 359 -> 0 jumps)
  p.at = now;
  last_pose = p;

  Refresh_Pose();
  if (!pose_ok) return;
  float nx, ny, nh;
  Predict(now, &nx, &ny, &nh);
  if (was_drawn) {
    // Keep the camera where it is and let the difference fade out
    off_x = before_x - nx;
    off_y = before_y - ny;
    off_h = Wrap180(before_h - nh);
  } else {
    off_x = off_y = off_h = 0;
  }
  off_at = now;
  Update_Camera(now);
}

void Map_Layer_Set_Target_Scale(float pixels_per_metre)
{
  zoom_target = pixels_per_metre;
  if (zoom == 0) zoom = pixels_per_metre;
}

bool Map_Layer_Has_Map(void)
{
  return pose_ok;
}

void Map_Layer_Clear(void)
{
  last_pose.valid = false;
  pose_ok = false;
  has_route = false;
  route_count = 0;
  drawn_x = 1e9;
  if (layer) lv_obj_invalidate(layer);
}

/********************************* Route *********************************/
// Replaces each corner of an open polyline with an arc of radius r (metres); ends stay put
static uint16_t Round_Route(const float (*in)[2], uint8_t n, float r_target, float (*out)[2])
{
  uint16_t m = 0;
  auto add = [&](float x, float y) { if (m < ROUTE_MAX) { out[m][0] = x; out[m][1] = y; m++; } };
  if (n == 0) return 0;
  add(in[0][0], in[0][1]);
  for (uint8_t i = 1; i + 1 < n; i++) {
    float bx = in[i][0], by = in[i][1];
    float ux = in[i - 1][0] - bx, uy = in[i - 1][1] - by, lu = sqrtf(ux * ux + uy * uy);
    float vx = in[i + 1][0] - bx, vy = in[i + 1][1] - by, lv = sqrtf(vx * vx + vy * vy);
    if (lu < 0.5f || lv < 0.5f) { add(bx, by); continue; }
    ux /= lu; uy /= lu; vx /= lv; vy /= lv;
    float angle = acosf(LV_CLAMP(-1.0f, ux * vx + uy * vy, 1.0f));
    if (angle > 3.05f) { add(bx, by); continue; }           // practically straight
    float half = angle / 2;
    float t = fminf(r_target / tanf(half), 0.45f * fminf(lu, lv));
    float r = t * tanf(half);
    float bisx = ux + vx, bisy = uy + vy, lb = sqrtf(bisx * bisx + bisy * bisy);
    if (lb < 1e-3f || r < 0.2f) { add(bx, by); continue; }
    float d = r / sinf(half);
    float ox = bx + bisx / lb * d, oy = by + bisy / lb * d;
    float a1 = atan2f(by + uy * t - oy, bx + ux * t - ox), a2 = atan2f(by + vy * t - oy, bx + vx * t - ox);
    float sweep = a2 - a1;
    while (sweep > (float)M_PI) sweep -= 2 * (float)M_PI;
    while (sweep < -(float)M_PI) sweep += 2 * (float)M_PI;
    int steps = LV_MAX(1, (int)ceilf(fabsf(sweep) / 0.3f));   // ~17 degrees a piece: smooth at this size
    for (int k = 0; k <= steps; k++) {
      float a = a1 + sweep * k / steps;
      add(ox + r * cosf(a), oy + r * sinf(a));
    }
  }
  add(in[n - 1][0], in[n - 1][1]);
  return m;
}

void Map_Layer_Set_Route(const int16_t (*pts)[2], uint8_t count)
{
  // The route is relative to the rider at the latest pose: remember where that was
  if (!last_pose.valid) return;
  route_area = last_pose.area;
  route_ox = last_pose.x;
  route_oy = last_pose.y;
  has_route = true;
  static float in[32][2];
  if (count > 32) count = 32;
  for (uint8_t i = 0; i < count; i++) { in[i][0] = pts[i][0]; in[i][1] = pts[i][1]; }
  float r = ROUTE_BEND_PX / fmaxf(zoom_target > 0 ? zoom_target : 1, 0.3f);
  route_count = count >= 2 ? Round_Route(in, count, r, route) : 0;
  if (layer) lv_obj_invalidate(layer);
}

void Map_Layer_Clear_Route(void)
{
  route_count = 0;
  if (layer) lv_obj_invalidate(layer);
}

/********************************* Drawing *********************************
 * Roads and the route are rasterised here, straight into LVGL's draw buffer: each segment is a
 * capsule (a thick line with round ends) whose edge pixels are shaded by their distance to the
 * line, so edges are smooth and positions keep sub-pixel precision (smoother motion).
 * LVGL's own thick lines cost ~48 ms for a map frame; this is a few ms.
 * The edge fade is applied as pixels are written (the background is black, so only map pixels
 * ever need fading), instead of blending a fade image over the whole band every frame.
 ****************************************************************************/
uint32_t map_perf_roads_us = 0, map_perf_fuel_us = 0, map_perf_route_us = 0, map_perf_draws = 0;
static float cam_sin, cam_cos;
// Edge fade (towards the round screen's rim, and towards the top and bottom of the map band),
// from small tables in internal RAM: a 94 KB per-pixel table in PSRAM made every pixel wait on
// slow memory, and computing it per pixel (square roots) was slower still.
#define FADE_MAX_ROWS   256
#define RIM_LUT_SIZE    3700            // indexed by (dx^2 + dy^2) / 16, up to ~243 px from the centre
bool fade_on = false;                   // (also switched by the serial "hide fade" test command)
static uint8_t row_fade[FADE_MAX_ROWS];     // top/bottom fade per band row
static int16_t clear_half[FADE_MAX_ROWS];   // half-width of the unfaded middle of each row (-1: none)
static uint8_t rim_lut[RIM_LUT_SIZE];
static lv_coord_t fade_box_y1 = -1, fade_box_h = 0;
static float fade_top, fade_bottom, fade_r0, fade_r1;
static const int screen_cx = 240, screen_cy = 240;

void Map_Layer_Set_Fade(float top_px, float bottom_px, float rim_from, float rim_to)
{
  fade_top = top_px;
  fade_bottom = bottom_px;
  fade_r0 = rim_from;
  fade_r1 = rim_to;
  for (int i = 0; i < RIM_LUT_SIZE; i++) {
    float r = sqrtf(i * 16.0f + 8);
    float t = LV_CLAMP(0.0f, (r - rim_from) / (rim_to - rim_from), 1.0f);
    rim_lut[i] = (uint8_t)(t * t * (3 - 2 * t) * 255);
  }
  fade_box_y1 = -1;                     // row tables are filled once the band's position is known
  fade_on = true;
}

static void Fill_Row_Tables(const lv_area_t *box)
{
  fade_box_y1 = box->y1;
  fade_box_h = LV_MIN(lv_area_get_height(box), FADE_MAX_ROWS);
  for (int i = 0; i < fade_box_h; i++) {
    float in_band = i + 0.5f;
    float t1 = LV_CLAMP(0.0f, in_band / fade_top, 1.0f), t2 = LV_CLAMP(0.0f, (fade_box_h - in_band) / fade_bottom, 1.0f);
    float top = 1 - t1 * t1 * (3 - 2 * t1), bottom = 1 - t2 * t2 * (3 - 2 * t2);
    row_fade[i] = (uint8_t)(LV_MAX(top, bottom) * 255);
    float dy = box->y1 + i + 0.5f - screen_cy;
    clear_half[i] = fade_r0 * fade_r0 > dy * dy ? (int16_t)sqrtf(fade_r0 * fade_r0 - dy * dy) : -1;
  }
}

struct Fade_Row { uint32_t row; int clear_half; int dy2; };

static inline Fade_Row Row_Fade(int y)
{
  int i = LV_CLAMP(0, y - fade_box_y1, fade_box_h - 1);
  int dy = y - screen_cy;
  return { row_fade[i], clear_half[i], dy * dy };
}

static inline uint32_t Pixel_Fade(const Fade_Row &f, int x)
{
  int dx = x - screen_cx;
  if (dx <= f.clear_half && -dx <= f.clear_half) return f.row;     // inside the clear circle
  uint32_t rim = rim_lut[LV_MIN((dx * dx + f.dy2) >> 4, RIM_LUT_SIZE - 1)];
  return LV_MAX(rim, f.row);
}

struct Pt { float x, y; };

// Area metres -> screen pixels (floats: sub-pixel precision)
static inline Pt To_Screen(const lv_area_t *box, float x, float y)
{
  float dx = x - cam_x, dy = y - cam_y;
  float right = dx * cam_cos - dy * cam_sin;
  float ahead = dx * cam_sin + dy * cam_cos;
  return { box->x1 + center_x + right * zoom, box->y1 + center_y - ahead * zoom };
}

struct Raster {
  lv_color_t *buf;              // LVGL's draw buffer
  lv_area_t buf_area;           // its position on screen
  lv_coord_t buf_w;
  lv_area_t clip;               // part of it being drawn now
  lv_area_t box;                // the map layer on screen (for the fade table)
};

// Draws one capsule from a to b: `color` out to `core` px from the line, then `edge_color` out to
// `half` (a casing; pass core = half for a plain line), blending by coverage x fade
static void Capsule2(const Raster &r, Pt a, Pt b, float half, lv_color_t color, float core, lv_color_t edge_color);

static void Capsule(const Raster &r, Pt a, Pt b, float half, lv_color_t color)
{
  Capsule2(r, a, b, half, color, half, color);
}

uint32_t map_perf_capsules = 0, map_perf_pixels = 0;

// In IRAM: running from flash, the loop's code kept being evicted from the cache it shares with PSRAM
IRAM_ATTR static void Capsule2(const Raster &r, Pt a, Pt b, float half, lv_color_t color, float core, lv_color_t edge_color)
{
  map_perf_capsules++;
  const float reach = half + 0.6f;                  // how far from the line a pixel centre can be touched
  float minx = fminf(a.x, b.x) - reach, maxx = fmaxf(a.x, b.x) + reach;
  float miny = fminf(a.y, b.y) - reach, maxy = fmaxf(a.y, b.y) + reach;
  int x1 = LV_MAX((int)floorf(minx), r.clip.x1), x2 = LV_MIN((int)ceilf(maxx), r.clip.x2);
  int y1 = LV_MAX((int)floorf(miny), r.clip.y1), y2 = LV_MIN((int)ceilf(maxy), r.clip.y2);
  if (x1 > x2 || y1 > y2) return;

  float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
  if (len < 1e-3f) { dx = 1e-3f; dy = 0; len = 1e-3f; }
  float ux = dx / len, uy = dy / len;               // along the segment
  const int32_t len_q = (int32_t)(len * 256);
  const int32_t in_q = (int32_t)(LV_MAX(half - 0.5f, 0.0f) * 256), out_q = (int32_t)((half + 0.5f) * 256);
  const bool casing = core < half;
  const bool darken = color.full == 0;              // black casing darkens; lines lighten
  const int32_t core_lo_q = (int32_t)((core - 0.5f) * 256), core_hi_q = (int32_t)((core + 0.5f) * 256);

  for (int y = y1; y <= y2; y++) {
    float py = y + 0.5f - a.y;
    // Only the stretch of this row the capsule can touch (not the whole bounding box, which
    // for a long diagonal is most of the screen)
    float t0 = 0, t1 = 1;
    if (fabsf(dy) > 1e-4f) {
      t0 = (py - reach) / dy;
      t1 = (py + reach) / dy;
      if (t0 > t1) { float s = t0; t0 = t1; t1 = s; }
      t0 = LV_MAX(t0, 0.0f);
      t1 = LV_MIN(t1, 1.0f);
      if (t0 > t1) continue;
    } else if (fabsf(py) > reach) {
      continue;
    }
    float sx0 = a.x + dx * t0, sx1 = a.x + dx * t1;
    int rx1 = LV_MAX(x1, (int)floorf(fminf(sx0, sx1) - reach));
    int rx2 = LV_MIN(x2, (int)ceilf(fmaxf(sx0, sx1) + reach));
    if (rx1 > rx2) continue;

    lv_color_t *row = r.buf + (y - r.buf_area.y1) * r.buf_w - r.buf_area.x1;
    Fade_Row fr;
    if (fade_on) {
      fr = Row_Fade(y);
      if (fr.row >= 250) continue;                  // this whole row is faded to black
    }
    map_perf_pixels += rx2 - rx1 + 1;
    // Distance across the line and position along it, in 1/256 px, change by a constant per
    // pixel: integer adds only (floating point here cost ~70 cycles a pixel)
    float px = rx1 + 0.5f - a.x;
    int32_t across = (int32_t)((px * -uy + py * ux) * 256), along = (int32_t)((px * ux + py * uy) * 256);
    const int32_t step_across = (int32_t)(-uy * 256), step_along = (int32_t)(ux * 256);
    for (int x = rx1; x <= rx2; x++, across += step_across, along += step_along) {
      int32_t d;
      if (along >= 0 && along <= len_q) {
        d = across < 0 ? -across : across;          // beside the segment
      } else {
        float ex = (along < 0 ? along : along - len_q) / 256.0f, ey = across / 256.0f;
        d = (int32_t)(sqrtf(ex * ex + ey * ey) * 256);  // beyond an end: distance to that end
      }
      if (d >= out_q) continue;
      uint32_t cover = d <= in_q ? 255 : (uint32_t)((out_q - d) * 255) >> 8;
      lv_color_t col = color;
      if (casing && d > core_lo_q) {
        int32_t inside = core_hi_q - d;             // 1 px blend from line colour to casing
        col = inside <= 0 ? edge_color : lv_color_mix(color, edge_color, (lv_opa_t)((inside * 255) >> 8));
      }
      if (fade_on) {
        uint32_t f = Pixel_Fade(fr, x);
        if (f >= 250) continue;
        cover = (cover * (256 - f)) >> 8;
      }
      if (cover <= 4) continue;
      // Pieces of a line overlap at every joint: plain blending would add their soft edges
      // twice (bright rings). Light lines keep the brighter of old and new, the black casing
      // keeps the darker, so overlapping pieces merge into one line.
      lv_color_t old = row[x];
      if (darken) {
        uint32_t keep = 255 - cover;
        lv_color_t dim;
        dim.ch.red = (old.ch.red * keep) >> 8;
        dim.ch.green = (old.ch.green * keep) >> 8;
        dim.ch.blue = (old.ch.blue * keep) >> 8;
        row[x] = dim;
      } else {
        lv_color_t mixed = cover >= 250 ? col : lv_color_mix(col, old, (lv_opa_t)cover);
        if (mixed.ch.red < old.ch.red) mixed.ch.red = old.ch.red;
        if (mixed.ch.green < old.ch.green) mixed.ch.green = old.ch.green;
        if (mixed.ch.blue < old.ch.blue) mixed.ch.blue = old.ch.blue;
        row[x] = mixed;
      }
    }
  }
}

// Draws a polyline (area metres, offset by ox, oy) as capsules; `from` replaces the first point
static void Polyline(const Raster &r, const float (*pts)[2], uint16_t n, float ox, float oy,
                     const Pt *from, float half, lv_color_t color, float core, lv_color_t edge_color)
{
  if (n == 0) return;
  Pt prev = from ? *from : To_Screen(&r.box, ox + pts[0][0], oy + pts[0][1]);
  for (uint16_t i = from ? 0 : 1; i < n; i++) {
    Pt next = To_Screen(&r.box, ox + pts[i][0], oy + pts[i][1]);
    Capsule2(r, prev, next, half, color, core, edge_color);
    prev = next;
  }
}

static void Draw_Route(const Raster &r)
{
  float ox, oy;
  if (route_count < 2 || !has_route || !Convert(route_area, route_ox, route_oy, active->id, &ox, &oy)) return;

  // Split where the camera is: nearest point on the route
  float best = 1e18f, px = 0, py = 0;
  uint16_t split = 0;
  for (uint16_t i = 0; i + 1 < route_count; i++) {
    float ax = ox + route[i][0], ay = oy + route[i][1];
    float bx = ox + route[i + 1][0], by = oy + route[i + 1][1];
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? LV_CLAMP(0.0f, ((cam_x - ax) * dx + (cam_y - ay) * dy) / len2, 1.0f) : 0;
    float qx = ax + t * dx, qy = ay + t * dy;
    float d2 = (qx - cam_x) * (qx - cam_x) + (qy - cam_y) * (qy - cam_y);
    if (d2 < best) { best = d2; split = i; px = qx; py = qy; }
  }
  Pt here = To_Screen(&r.box, px, py);

  // Casing first for the whole route, then the lines on top: drawing each piece's casing and
  // line together let the next piece's casing paint black stripes across the previous line
  const float behind = (ROUTE_WIDTH - 2) / 2.0f, ahead = ROUTE_WIDTH / 2.0f;
  lv_color_t black = lv_color_black(), done = lv_color_hex(COLOR_ROUTE_DONE), bright = lv_color_hex(COLOR_ROUTE);
  Pt split_pt = To_Screen(&r.box, ox + route[split][0], oy + route[split][1]);
  for (int pass = 0; pass < 2; pass++) {
    float extra = pass == 0 ? ROUTE_CASING : 0;
    Polyline(r, route, split + 1, ox, oy, NULL, behind + extra, pass == 0 ? black : done, behind + extra, pass == 0 ? black : done);
    Capsule2(r, split_pt, here, behind + extra, pass == 0 ? black : done, behind + extra, pass == 0 ? black : done);
  }
  for (int pass = 0; pass < 2; pass++) {
    float extra = pass == 0 ? ROUTE_CASING : 0;
    Polyline(r, route + split + 1, route_count - split - 1, ox, oy, &here, ahead + extra,
             pass == 0 ? black : bright, ahead + extra, pass == 0 ? black : bright);
  }
}

static void Draw_Event(lv_event_t *e)
{
  if (!pose_ok || zoom <= 0) return;
  uint32_t t0 = micros();
  map_perf_draws++;
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  Raster r;
  r.buf = (lv_color_t *)ctx->buf;
  r.buf_area = *ctx->buf_area;
  r.buf_w = lv_area_get_width(ctx->buf_area);
  r.clip = *ctx->clip_area;
  lv_obj_get_coords(layer, &r.box);
  if (fade_box_y1 != r.box.y1) Fill_Row_Tables(&r.box);
  float h = cam_h * (float)M_PI / 180;
  cam_sin = sinf(h);
  cam_cos = cosf(h);

  if (active) {
    // Minor roads first, so bigger roads are drawn over them
    for (int cls = 3; cls >= 1; cls--) {
      float half = road_width[cls] / 2.0f;
      lv_color_t color = lv_color_hex(road_color[cls]);
      for (uint16_t i = 0; i < active->road_count; i++) {
        const Map_Road &road = active->roads[i];
        if (road.road_class != cls) continue;
        Pt c = To_Screen(&r.box, road.cx, road.cy);
        float rr = road.radius * zoom + half + 1;
        if (c.x + rr < r.clip.x1 || c.x - rr > r.clip.x2 || c.y + rr < r.clip.y1 || c.y - rr > r.clip.y2) continue;
        const int16_t *p0 = active->points[road.start];
        Pt prev = To_Screen(&r.box, p0[0], p0[1]);
        for (uint8_t k = 1; k < road.count; k++) {
          const int16_t *p = active->points[road.start + k];
          Pt next = To_Screen(&r.box, p[0], p[1]);
          Capsule(r, prev, next, half, color);
          prev = next;
        }
      }
    }
    uint32_t t1 = micros();
    map_perf_roads_us += t1 - t0;
    t0 = t1;

    // Fuel stations: a small dark round badge with an amber pump, kept upright as the map turns
    // (skipped where the edge fade would hide it, since LVGL draws these without the fade)
    lv_draw_rect_dsc_t badge;
    lv_draw_rect_dsc_init(&badge);
    badge.radius = LV_RADIUS_CIRCLE;
    badge.bg_color = lv_color_hex(0x1E1E1E);
    badge.bg_opa = LV_OPA_COVER;
    badge.border_color = lv_color_hex(0x4A4A4A);
    badge.border_width = 1;
    badge.border_opa = LV_OPA_COVER;
    lv_draw_label_dsc_t icon;
    lv_draw_label_dsc_init(&icon);
    icon.font = &font_fuel_14;
    icon.color = lv_color_hex(0xF2B544);
    const lv_coord_t br = FUEL_BADGE / 2;
    for (uint8_t i = 0; i < active->fuel_count; i++) {
      Pt f = To_Screen(&r.box, active->fuel[i][0], active->fuel[i][1]);
      lv_coord_t sx = (lv_coord_t)lroundf(f.x), sy = (lv_coord_t)lroundf(f.y);
      lv_area_t area = { (lv_coord_t)(sx - br), (lv_coord_t)(sy - br), (lv_coord_t)(sx + br - 1), (lv_coord_t)(sy + br - 1) };
      if (area.x2 < r.clip.x1 || area.x1 > r.clip.x2 || area.y2 < r.clip.y1 || area.y1 > r.clip.y2) continue;
      if (fade_on && (sx < r.box.x1 || sx > r.box.x2 || sy < r.box.y1 || sy > r.box.y2 ||
                      Pixel_Fade(Row_Fade(sy), sx) > 100)) continue;
      lv_draw_rect(ctx, &badge, &area);
      lv_area_t glyph = { (lv_coord_t)(sx - 7), (lv_coord_t)(sy - font_fuel_14.line_height / 2),
                          (lv_coord_t)(sx + 7), (lv_coord_t)(sy + font_fuel_14.line_height / 2) };
      lv_draw_label(ctx, &icon, &glyph, FUEL_GLYPH, NULL);
    }
    map_perf_fuel_us += micros() - t0;
  }

  t0 = micros();
  Draw_Route(r);
  map_perf_route_us += micros() - t0;
}

/********************************* Main loop side *********************************/
void Map_Layer_Poll(void)
{
  portENTER_CRITICAL(&map_lock);
  Map_Area *next = ready;
  if (next) {
    ready = NULL;
    Map_Area *old = active;
    active = next;
    building = old ? old : (next == &areas[0] ? &areas[1] : &areas[0]);
    building->id = 0xFFFF;                    // so the next MAP line starts a fresh area
  }
  portEXIT_CRITICAL(&map_lock);
  if (next) {
    // New area on screen: the latest pose converts into it; the camera carries on smoothly
    // (corrections are differences, the same in any area's frame)
    Refresh_Pose();
    if (layer) lv_obj_invalidate(layer);
  }
}

// Every frame: move the camera and zoom, and redraw only if the picture actually changes
static void Frame(lv_timer_t *timer)
{
  static uint32_t last = 0;
  uint32_t now = lv_tick_get();
  float dt = last ? (uint32_t)(now - last) / 1000.0f : 0;
  last = now;
  if (zoom_target > 0) zoom += (zoom_target - zoom) * (1 - expf(-dt / ZOOM_EASE_S));
  if (!pose_ok || !layer) return;
  Update_Camera(now);
  float moved_px = sqrtf((cam_x - drawn_x) * (cam_x - drawn_x) + (cam_y - drawn_y) * (cam_y - drawn_y)) * zoom;
  if (moved_px > 0.3f || fabsf(cam_h - drawn_h) > 0.15f || fabsf(zoom - drawn_zoom) > 0.002f * zoom) {
    drawn_x = cam_x; drawn_y = cam_y; drawn_h = cam_h; drawn_zoom = zoom;
    lv_obj_invalidate(layer);
  }
}

lv_obj_t *Map_Layer_Create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t marker_x, lv_coord_t marker_y)
{
  layer = lv_obj_create(parent);
  lv_obj_remove_style_all(layer);
  lv_obj_set_size(layer, w, h);
  lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(layer, Draw_Event, LV_EVENT_DRAW_MAIN, NULL);
  center_x = marker_x;
  center_y = marker_y;
  areas[0].id = areas[1].id = 0xFFFF;
  lv_timer_create(Frame, FRAME_MS, NULL);
  return layer;
}

// Serial "cam": camera and data state, for debugging
void Map_Layer_Debug_Print(void)
{
  printf("CAM pose_ok=%d pose_area=%d active=%d roads=%u route=%u cam=%.1f,%.1f h=%.1f zoom=%.3f->%.3f\r\n",
         pose_ok, last_pose.valid ? (int)last_pose.area : -1, active ? (int)active->id : -1,
         active ? active->road_count : 0, route_count, cam_x, cam_y, cam_h, zoom, zoom_target);
}

// Serial "rastbench": the capsule loop alone, into an internal-RAM buffer
void Map_Layer_Raster_Bench(void)
{
  static lv_color_t test_buf[480 * 20];
  Raster r;
  r.buf = test_buf;
  r.buf_area = { 0, 0, 479, 19 };
  r.buf_w = 480;
  r.clip = r.buf_area;
  r.box = { 0, 0, 479, 195 };
  bool was = fade_on;
  fade_on = false;
  map_perf_pixels = 0;
  uint32_t t0 = micros();
  for (int i = 0; i < 500; i++) {
    Pt a = { 10.0f + (i % 40) * 11, 2.0f }, b = { 60.0f + (i % 40) * 11, 17.0f };
    Capsule2(r, a, b, 4.0f, lv_color_white(), 4.0f, lv_color_white());
  }
  uint32_t us = micros() - t0;
  fade_on = was;
  printf("RAST 500 capsules: %lu us, %lu pixels checked, %lu ns per pixel\r\n",
         (unsigned long)us, (unsigned long)map_perf_pixels, (unsigned long)(us * 1000ULL / LV_MAX(map_perf_pixels, 1u)));
}
