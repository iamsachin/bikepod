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
  In a turn the map never rotates back: a guess that went slightly too far waits for the rider
  instead of unwinding (that unwinding made the map rotate, slip back, rotate... through turns).
******************************************************************************/
// This file's drawing loops run for every map pixel: compile it for speed, not size (Arduino's -Os)
// (no-math-errno lets sqrtf use the FPU's own instructions instead of calling into flash)
#pragma GCC optimize("O2", "no-math-errno")
#include "Map_Layer.h"
#include <Arduino.h>
#include <math.h>

#define MAP_MAX_ROADS   400
#define MAP_MAX_POINTS  2400
#define MAP_MAX_FUEL    48
#define ROUTE_MAX       256       // route points after its corners are rounded

#define FRAME_MS        20        // camera update / redraw rate (~50 fps when moving)
#define CAM_SETTLE_S    0.25f     // how quickly a correction from a new pose fades out (see Update_Camera)
#define ZOOM_EASE_S     0.6f      // how quickly the zoom follows its target
#define PREDICT_MAX_S   1.5f      // stop predicting if poses stop coming
#define ROUTE_BEND_M    12.0f     // radius of the route's rounded corners, metres
#define PREDICT_SPEED   0.95f     // predict a little slower than measured (riders slow into turns)
#define HOLD_BACK_DEG   8.0f      // in a turn, smaller corrections against it wait instead

struct Map_Road { uint16_t start; uint8_t count; uint8_t road_class; };

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

// Three buffers, so a new area can always start: one on screen, one complete and waiting to be
// swapped in, one being filled (with two, an area arriving right after another was dropped).
// In PSRAM: drawing reads them once per frame (Prepare_Screen_Points), and internal RAM is for Bluetooth.
#define AREA_BUFFERS 3
static Map_Area *areas = NULL;
static Map_Area *volatile active = NULL;      // being drawn (main loop only)
static Map_Area *volatile building = NULL;    // being filled by the Bluetooth task
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

LV_FONT_DECLARE(font_fuel_20);       // Material Icons Round "local_gas_station" only
#define FUEL_GLYPH  "\xEE\x95\x86"      // U+E546 in UTF-8
#define FUEL_BADGE  30                    // badge diameter, px (20 was hard to spot at a glance)

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
  float speed;                  // metres/s used for prediction (the lower of smoothed and latest)
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
static float turn_dir = 0;          // which way the camera has been rotating (+ clockwise)
static uint32_t cam_at = 0;
static float zoom = 0, zoom_target = 0, zoom_mid = 0;   // zoom eases to zoom_mid, which eases to the target
static float turn_dist = -1;        // metres to the next turn at turn_at
static uint32_t turn_at;
static float drawn_x = 1e9, drawn_y, drawn_h, drawn_zoom;

// Route, north-up metres relative to its origin, corners already rounded
static float route[ROUTE_MAX][2];
static float route_along[ROUTE_MAX];    // distance along the route to each point, metres
static uint16_t route_count = 0;
// The last route received (the phone repeats it with every update): forgotten when the route is
// cleared, or the same route sent again later would be ignored and never drawn
static int16_t last_route_in[48][2];
static uint8_t last_count = 0;
static uint16_t last_area = 0xFFFF;


static void Refresh_Pose(void);

/******************************* Bluetooth task side *******************************/
void Map_Data_Begin(uint16_t area_id, double ref_lat, double ref_lng)
{
  Ref_Store(area_id, ref_lat, ref_lng);
  if (!areas) return;
  portENTER_CRITICAL(&map_lock);
  // Any buffer that's neither on screen nor waiting to be (an unfinished area is abandoned)
  Map_Area *a = NULL;
  for (int i = 0; i < AREA_BUFFERS && !a; i++)
    if (&areas[i] != active && &areas[i] != ready) a = &areas[i];
  building = a;
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

// A point `s` metres along the route (route-relative coordinates)
static void Route_Point(float s, float *x, float *y)
{
  s = LV_CLAMP(0.0f, s, route_along[route_count - 1]);
  uint16_t i = 0;
  while (i + 2 < route_count && route_along[i + 1] < s) i++;
  float seg = route_along[i + 1] - route_along[i];
  float t = seg > 0 ? (s - route_along[i]) / seg : 0;
  *x = route[i][0] + t * (route[i + 1][0] - route[i][0]);
  *y = route[i][1] + t * (route[i + 1][1] - route[i][1]);
}

// Nearest point of the route to (px, py) (route-relative): distance along it, returns distance squared
static float Nearest_On_Route(float px, float py, float *s)
{
  float best = 1e18f;
  *s = 0;
  for (uint16_t i = 0; i + 1 < route_count; i++) {
    float ax = route[i][0], ay = route[i][1], dx = route[i + 1][0] - ax, dy = route[i + 1][1] - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? LV_CLAMP(0.0f, ((px - ax) * dx + (py - ay) * dy) / len2, 1.0f) : 0;
    float qx = ax + t * dx - px, qy = ay + t * dy - py, d2 = qx * qx + qy * qy;
    if (d2 < best) { best = d2; *s = route_along[i] + t * sqrtf(len2); }
  }
  return best;
}

// Where the rider should be now (area on screen). On the route, the prediction moves along the
// route at the rider's speed and takes the route's direction (a straight-line guess left the
// route on every bend, so the arrow drifted off the line). Off the route: straight line.
static void Predict(uint32_t now, float *x, float *y, float *h)
{
  float dt = fminf((uint32_t)(now - last_pose.at) / 1000.0f, PREDICT_MAX_S);
  float ox, oy;
  if (has_route && route_count >= 2 && Convert(route_area, route_ox, route_oy, active->id, &ox, &oy)) {
    // Where the latest pose sits on the route
    float s0, best = Nearest_On_Route(pose_x - ox, pose_y - oy, &s0);
    if (best < 25.0f * 25.0f) {
      float s = s0 + last_pose.speed * dt;
      float rx, ry, ax, ay, bx, by;
      Route_Point(s, &rx, &ry);
      Route_Point(s - 4, &ax, &ay);           // direction of the route around that point
      Route_Point(s + 4, &bx, &by);
      *x = ox + rx;
      *y = oy + ry;
      float route_h = atan2f(bx - ax, by - ay) * 180 / (float)M_PI;   // degrees clockwise from north
      *h = last_pose.heading + Wrap180(route_h - last_pose.heading);
      return;
    }
  }
  // Diagnostics: why the prediction isn't following the route (at most once a second)
  static uint32_t logged_ms = 0;
  if (has_route && route_count >= 2 && lv_tick_elaps(logged_ms) > 1000) {
    logged_ms = lv_tick_get();
    float cx, cy, s0;
    if (!Convert(route_area, route_ox, route_oy, active->id, &cx, &cy)) {
      printf("MAP: off-route prediction: route area %u, screen area %u (no conversion)\r\n", route_area, active->id);
    } else {
      float d2 = Nearest_On_Route(pose_x - cx, pose_y - cy, &s0);
      printf("MAP: off-route prediction: pose %.0f m from the route (pose area %u, route area %u, screen %u, %u points)\r\n",
             sqrtf(d2), last_pose.area, route_area, active->id, route_count);
      // The route and pose themselves, to plot (area frame, metres)
      printf("MAP: pose %.1f,%.1f heading %.0f; route", pose_x, pose_y, last_pose.heading);
      for (uint16_t i = 0; i < route_count; i++) printf(" %.0f,%.0f", cx + route[i][0], cy + route[i][1]);
      printf("\r\n");
    }
  }
  float k = PREDICT_SPEED;
  *x = pose_x + last_pose.vx * k * dt;
  *y = pose_y + last_pose.vy * k * dt;
  *h = last_pose.heading + last_pose.turn * dt;
}

static void Update_Camera(uint32_t now)
{
  if (!pose_ok) return;
  float px, py, ph;
  Predict(now, &px, &py, &ph);
  // Critically damped: the correction starts fading at zero speed (a plain exponential started
  // at full speed, a small push with every pose that showed as a jump when zoomed in at turns)
  float t = (uint32_t)(now - off_at) / 1000.0f / CAM_SETTLE_S;
  float fade = (1 + t) * expf(-t);
  cam_x = px + off_x * fade;
  cam_y = py + off_y * fade;
  // On the route, the arrow stays on the line: a fading correction slides straight, which cut
  // across bends and pulled the arrow off the line for a moment
  float ox, oy;
  if (has_route && route_count >= 2 && Convert(route_area, route_ox, route_oy, active->id, &ox, &oy)) {
    float s, d2 = Nearest_On_Route(cam_x - ox, cam_y - oy, &s);
    if (d2 < 25.0f * 25.0f) {
      float rx, ry;
      Route_Point(s, &rx, &ry);
      cam_x = ox + rx;
      cam_y = oy + ry;
    }
  }

  // Heading: follow the target, but don't turn back against the turn for a small correction
  float dt = cam_at ? fminf((uint32_t)(now - cam_at) / 1000.0f, 0.2f) : 0;
  cam_at = now;
  float err = Wrap180(ph + off_h * fade - cam_h);
  if (err * turn_dir < 0 && fabsf(err) < HOLD_BACK_DEG) err = 0;
  float step = err * (1 - expf(-dt / 0.12f));
  cam_h += step;
  // Remember the turning direction (forgotten after about half a second of going straight)
  float rate = dt > 0 ? step / dt : 0;
  if (fabsf(rate) > 4) turn_dir = rate > 0 ? 1 : -1;
  else turn_dir *= expf(-dt / 0.5f);
  if (fabsf(turn_dir) < 0.2f) turn_dir = 0;
}

void Map_Layer_Set_Pose(uint16_t area_id, float x, float y, float heading_deg)
{
  // A pose for an area whose reference hasn't arrived yet can't be placed: keep the last one
  // (taking it lost the route-following prediction, and the arrow left the route at bends)
  double ref_lat, ref_lng;
  if (!Ref_Find(area_id, &ref_lat, &ref_lng)) {
    static uint16_t logged = 0;
    if (logged != area_id) printf("MAP: pose for area %u before its reference, kept the last\r\n", logged = area_id);
    return;
  }
  uint32_t now = lv_tick_get();
  bool was_drawn = pose_ok && drawn_x < 1e8;
  // Camera as it should be shown right now (not as of the last frame, up to ~100 ms ago: that
  // made the map stall and then surge with every pose, very visible zoomed in near turns)
  if (was_drawn) Update_Camera(now);
  float before_x = cam_x, before_y = cam_y, before_h = cam_h;

  Pose p = last_pose;
  float px, py;
  // Motion since the previous pose (converted into this pose's area), smoothed against GPS jitter
  if (p.valid && Convert(p.area, p.x, p.y, area_id, &px, &py)) {
    float dt = (uint32_t)(now - p.at) / 1000.0f;
    if (dt > 0.05f && dt < 3) {
      float vx = (x - px) / dt, vy = (y - py) / dt;
      float latest = hypotf(vx, vy);
      float turn = LV_CLAMP(-120.0f, Wrap180(heading_deg - p.heading) / dt, 120.0f);
      p.vx = 0.5f * p.vx + 0.5f * vx;
      p.vy = 0.5f * p.vy + 0.5f * vy;
      p.turn = 0.5f * p.turn + 0.5f * turn;
      p.speed = PREDICT_SPEED * fminf(hypotf(p.vx, p.vy), latest);
    } else if (dt >= 3) {
      p.vx = p.vy = p.turn = p.speed = 0;
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
    cam_h = nh;                         // first picture: start facing the right way
    turn_dir = 0;
  }
  off_at = now;
  Update_Camera(now);
}

void Map_Layer_Set_Turn_Distance(float metres)
{
  turn_dist = metres;
  turn_at = lv_tick_get();
}

// Zoom so the turn sits about 78 px ahead of the rider (room above it for the road beyond); not
// further out than 0.55 px/m, or the map area would end on screen
static float Zoom_For(float metres)
{
  return LV_CLAMP(0.55f, 78.0f / LV_MAX(metres, 30.0f), 2.6f);
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
  last_area = 0xFFFF;                   // the same route sent again must be drawn
  turn_dist = -1;
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

void Map_Layer_Set_Route(uint16_t area_id, const int16_t (*pts)[2], uint8_t count)
{
  double ref_lat, ref_lng;
  if (!Ref_Find(area_id, &ref_lat, &ref_lng)) return;     // as for poses: keep the current route
  int16_t (*last)[2] = last_route_in;
  if (count > 48) count = 48;
  if (area_id == last_area && count == last_count && memcmp(last, pts, count * sizeof(last[0])) == 0) return;
  memcpy(last, pts, count * sizeof(last[0]));
  last_count = count;
  last_area = area_id;

  route_area = area_id;
  route_ox = route_oy = 0;                  // points are already in that area's frame
  has_route = true;
  static float in[48][2];
  for (uint8_t i = 0; i < count; i++) { in[i][0] = pts[i][0]; in[i][1] = pts[i][1]; }
  // Corners rounded with a fixed radius in metres, so the route keeps its shape as the zoom changes
  route_count = count >= 2 ? Round_Route(in, count, ROUTE_BEND_M, route) : 0;
  if (route_count) route_along[0] = 0;
  for (uint16_t i = 1; i < route_count; i++)
    route_along[i] = route_along[i - 1] + hypotf(route[i][0] - route[i - 1][0], route[i][1] - route[i - 1][1]);
  if (layer) lv_obj_invalidate(layer);
}

void Map_Layer_Clear_Route(void)
{
  route_count = 0;
  has_route = false;
  last_area = 0xFFFF;
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
static int16_t vis_half[FADE_MAX_ROWS];     // half-width of the part of each row not faded to black
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
    vis_half[i] = fade_r1 * fade_r1 > dy * dy ? (int16_t)sqrtf(fade_r1 * fade_r1 - dy * dy) + 1 : -1;
  }
}

struct Fade_Row { uint32_t row; int clear_half; int dy2; int vis_half; };

static inline Fade_Row Row_Fade(int y)
{
  int i = LV_CLAMP(0, y - fade_box_y1, fade_box_h - 1);
  int dy = y - screen_cy;
  return { row_fade[i], clear_half[i], dy * dy, vis_half[i] };
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

// Casing coverage for the strip being drawn (LVGL draws the band in strips of up to MASK_ROWS rows)
#define MASK_ROWS 20
static uint8_t casing_mask[480 * MASK_ROWS];     // always all zero outside a casing pass
static int mask_x1 = INT16_MAX, mask_x2 = INT16_MIN, mask_y1 = INT16_MAX, mask_y2 = INT16_MIN;   // touched (screen)

// Darkens the picture under the recorded casing, once per pixel, and clears what it used (only
// the part the casing touched: going over the whole strip cost ~4 ms a frame)
static void Apply_Casing_Mask(const Raster &r)
{
  int x1 = LV_MAX(mask_x1, r.clip.x1), x2 = LV_MIN(mask_x2, r.clip.x2);
  int y1 = LV_MAX(mask_y1, r.clip.y1), y2 = LV_MIN(mask_y2, r.clip.y2);
  for (int y = y1; y <= y2 && x1 <= x2; y++) {
    lv_color_t *row = r.buf + (y - r.buf_area.y1) * r.buf_w - r.buf_area.x1;
    uint8_t *m = &casing_mask[(y - r.buf_area.y1) * r.buf_w - r.buf_area.x1];
    for (int x = x1; x <= x2; x++) {
      if (!m[x]) continue;
      uint32_t keep = 255 - m[x];
      m[x] = 0;
      lv_color_t c = row[x];
      c.ch.red = (c.ch.red * keep) >> 8;
      c.ch.green = (c.ch.green * keep) >> 8;
      c.ch.blue = (c.ch.blue * keep) >> 8;
      row[x] = c;
    }
  }
  mask_x1 = mask_y1 = INT16_MAX;
  mask_x2 = mask_y2 = INT16_MIN;
}

static void Capsule(const Raster &r, Pt a, Pt b, float half, lv_color_t color)
{
  Capsule2(r, a, b, half, color, half, color);
}

uint32_t map_perf_capsules = 0, map_perf_pixels = 0;

// Small helpers kept inline: the library floorf/ceilf/fminf/fmaxf live in flash, and calling
// them from this loop (several per row) stalled on the cache shared with PSRAM
static inline int Floor_I(float v) { int i = (int)v; return i - (v < i); }
static inline int Ceil_I(float v) { int i = (int)v; return i + (v > i); }
static inline float Min_F(float a, float b) { return a < b ? a : b; }
static inline float Max_F(float a, float b) { return a > b ? a : b; }
// Square root without the library call: bit-trick estimate + two Newton steps (~1e-6 relative)
static inline float Sqrt_F(float v)
{
  if (v <= 0) return 0;
  union { float f; uint32_t i; } u = { v };
  u.i = (u.i >> 1) + 0x1FC00000;
  float s = u.f;
  s = 0.5f * (s + v / s);
  return 0.5f * (s + v / s);
}

// Colour scaled by coverage (as if over black)
static inline lv_color_t Scale(lv_color_t c, uint32_t cover)
{
  lv_color_t s;
  s.ch.red = (c.ch.red * cover) >> 8;
  s.ch.green = (c.ch.green * cover) >> 8;
  s.ch.blue = (c.ch.blue * cover) >> 8;
  return s;
}

// The brighter of two colours, channel by channel
static inline lv_color_t Lighten(lv_color_t old, lv_color_t lit)
{
  if (lit.ch.red < old.ch.red) lit.ch.red = old.ch.red;
  if (lit.ch.green < old.ch.green) lit.ch.green = old.ch.green;
  if (lit.ch.blue < old.ch.blue) lit.ch.blue = old.ch.blue;
  return lit;
}

// Narrows [k1, k2] to the k where lo <= start + step * k <= hi, one step inside the exact bounds
static inline void Solid_Range(int32_t start, int32_t step, float inv_step, int32_t lo, int32_t hi, int *k1, int *k2)
{
  if (step == 0) {
    if (start < lo || start > hi) *k2 = *k1 - 1;
    return;
  }
  float ka = (lo - start) * inv_step, kb = (hi - start) * inv_step;
  if (ka > kb) { float s = ka; ka = kb; kb = s; }
  *k1 = LV_MAX(*k1, Ceil_I(ka) + 1);
  *k2 = LV_MIN(*k2, Floor_I(kb) - 1);
}

// In IRAM: running from flash, the loop's code kept being evicted from the cache it shares with PSRAM
// One copy of the loop per kind of line (casing blend or not, darkening mask or lightening, edge
// fade or not): with every case in one loop it ran out of registers and spilled to memory
template <bool CASING, bool DARKEN, bool FADE, bool OVER = false>
IRAM_ATTR static void Capsule_Kind(const Raster &r, Pt a, Pt b, float half, lv_color_t color, float core, lv_color_t edge_color);

IRAM_ATTR static void Capsule2(const Raster &r, Pt a, Pt b, float half, lv_color_t color, float core, lv_color_t edge_color)
{
  map_perf_capsules++;
  const bool casing = core < half, darken = color.full == 0;
  if (fade_on) {
    if (darken) Capsule_Kind<false, true, true>(r, a, b, half, color, core, edge_color);
    else if (casing) Capsule_Kind<true, false, true>(r, a, b, half, color, core, edge_color);
    else Capsule_Kind<false, false, true>(r, a, b, half, color, core, edge_color);
  } else {
    if (darken) Capsule_Kind<false, true, false>(r, a, b, half, color, core, edge_color);
    else if (casing) Capsule_Kind<true, false, false>(r, a, b, half, color, core, edge_color);
    else Capsule_Kind<false, false, false>(r, a, b, half, color, core, edge_color);
  }
}

template <bool CASING, bool DARKEN, bool FADE, bool OVER>
IRAM_ATTR static void Capsule_Kind(const Raster &r, Pt a, Pt b, float half, lv_color_t color, float core, lv_color_t edge_color)
{
  const float reach = half + 0.6f;                  // how far from the line a pixel centre can be touched
  // Rows first: most segments don't touch the strip being drawn, so leave before anything else
  int y1 = LV_MAX(Floor_I(Min_F(a.y, b.y) - reach), r.clip.y1), y2 = LV_MIN(Ceil_I(Max_F(a.y, b.y) + reach), r.clip.y2);
  if (y1 > y2) return;
  int x1 = LV_MAX(Floor_I(Min_F(a.x, b.x) - reach), r.clip.x1), x2 = LV_MIN(Ceil_I(Max_F(a.x, b.x) + reach), r.clip.x2);
  if (x1 > x2) return;

  float dx = b.x - a.x, dy = b.y - a.y, len = Sqrt_F(dx * dx + dy * dy);
  if (len < 1e-3f) { dx = 1e-3f; dy = 0; len = 1e-3f; }
  const float inv_len = 1.0f / len;                 // divisions are slow (no FPU divide): one each
  float ux = dx * inv_len, uy = dy * inv_len;       // along the segment
  const bool sloped = dy > 1e-4f || dy < -1e-4f;
  const float inv_dy = sloped ? 1.0f / dy : 0;
  const int32_t len_q = (int32_t)(len * 256);
  const int32_t in_q = (int32_t)(LV_MAX(half - 0.5f, 0.0f) * 256), out_q = (int32_t)((half + 0.5f) * 256);
  const bool casing = CASING;                       // line colour blends into a casing colour
  const bool darken = DARKEN;                       // black casing darkens; lines lighten
  const int32_t core_lo_q = (int32_t)((core - 0.5f) * 256), core_hi_q = (int32_t)((core + 0.5f) * 256);
  // Beyond the ends (round caps) the distance comes from distance squared: inside in_q or outside
  // out_q needs no root, and across the 1 px edge between them a straight-line estimate is within
  // 1/20 px (a square root per cap pixel was the loop's slowest part). Fits 32 bits: half <= ~40 px
  const int32_t in2_q = in_q * in_q, out2_q = out_q * out_q;
  const float edge_k = 1.0f / (in_q + out_q);
  const bool fade = FADE;
  const int32_t step_across = (int32_t)(-uy * 256), step_along = (int32_t)(ux * 256);
  const float inv_step_across = step_across ? 1.0f / step_across : 0, inv_step_along = step_along ? 1.0f / step_along : 0;
  // Solid: full coverage and (with a casing) still the line's own colour
  const int32_t solid_q = casing ? LV_MIN(in_q, core_lo_q) : in_q;

  // Only the stretch of each row the capsule can touch (not the whole bounding box, which for a
  // long diagonal is most of the screen): where the line crosses the band reach above and below
  // the row's centre, kept to the segment's own x extent. Both move by `slope` per row.
  const float seg_x_lo = Min_F(a.x, b.x), seg_x_hi = Max_F(a.x, b.x);
  const float slope = sloped ? dx * inv_dy : 0;
  float cross_a = a.x + (y1 + 0.5f - a.y - reach) * slope;   // line's x at (row centre - reach)
  float cross_b = cross_a + 2 * reach * slope;                // ... and at (row centre + reach)
  for (int y = y1; y <= y2; y++, cross_a += slope, cross_b += slope) {
    float py = y + 0.5f - a.y;
    if (!sloped && (py > reach || -py > reach)) continue;
    float lo = sloped ? Max_F(Min_F(cross_a, cross_b), seg_x_lo) : seg_x_lo;
    float hi = sloped ? Min_F(Max_F(cross_a, cross_b), seg_x_hi) : seg_x_hi;
    if (lo > hi) continue;
    int rx1 = LV_MAX(x1, Floor_I(lo - reach));
    int rx2 = LV_MIN(x2, Ceil_I(hi + reach));
    if (rx1 > rx2) continue;

    lv_color_t *row = r.buf + (y - r.buf_area.y1) * r.buf_w - r.buf_area.x1;
    uint8_t *mask_row = &casing_mask[(y - r.buf_area.y1) * r.buf_w - r.buf_area.x1];
    Fade_Row fr = { 0, 10000, 0, 10000 };
    if (fade) {
      fr = Row_Fade(y);
      if (fr.row >= 250 || fr.vis_half < 0) continue;     // this whole row is faded to black
      // Nothing to draw beyond the round screen's faded rim
      rx1 = LV_MAX(rx1, screen_cx - fr.vis_half);
      rx2 = LV_MIN(rx2, screen_cx + fr.vis_half);
      if (rx1 > rx2) continue;
    }
    map_perf_pixels += rx2 - rx1 + 1;
    if (DARKEN) {
      mask_x1 = LV_MIN(mask_x1, rx1); mask_x2 = LV_MAX(mask_x2, rx2);
      mask_y1 = LV_MIN(mask_y1, y); mask_y2 = LV_MAX(mask_y2, y);
    }
    // Distance across the line and position along it, in 1/256 px, change by a constant per
    // pixel: integer adds only (floating point here cost ~70 cycles a pixel)
    float px = rx1 + 0.5f - a.x;
    int32_t across0 = (int32_t)((px * -uy + py * ux) * 256), along0 = (int32_t)((px * ux + py * uy) * 256);

    // The solid middle of the line (full coverage, line colour, the row's own fade) gets one
    // colour, written by a tight loop; only the soft edges and caps need the per-pixel work.
    // Kept one pixel inside the exact bounds, so rounding never puts an edge pixel in it.
    int k1 = 0, k2 = rx2 - rx1;
    if (k2 < 3 || solid_q < 256) {
      k1 = k2 + 1;                                  // too short to have a middle
    } else {
      Solid_Range(across0, step_across, inv_step_across, -solid_q, solid_q, &k1, &k2);
      Solid_Range(along0, step_along, inv_step_along, 0, len_q, &k1, &k2);
    }

    auto edge_pixel = [&](int x, int32_t across, int32_t along) __attribute__((always_inline)) {
      int32_t d;
      if (along >= 0 && along <= len_q) {
        d = across < 0 ? -across : across;          // beside the segment
      } else {
        int32_t ex = along < 0 ? along : along - len_q;  // beyond an end: distance to that end
        int32_t d2 = ex * ex + across * across;
        if (d2 >= out2_q) return;
        d = d2 <= in2_q ? 0 : (int32_t)((d2 - in2_q) * edge_k) + in_q;
      }
      if (d >= out_q) return;
      uint32_t cover = d <= in_q ? 255 : (uint32_t)((out_q - d) * 255) >> 8;
      lv_color_t col = color;
      if (casing && d > core_lo_q) {
        int32_t inside = core_hi_q - d;             // 1 px blend from line colour to casing
        col = inside <= 0 ? edge_color : lv_color_mix(color, edge_color, (lv_opa_t)((inside * 255) >> 8));
      }
      if (fade) {
        uint32_t f = Pixel_Fade(fr, x);
        if (f >= 250) return;
        cover = (cover * (256 - f)) >> 8;
      }
      if (cover <= 4) return;
      // Pieces of a line overlap at every joint, so a pixel can be drawn several times. Each
      // write must give the same result however often it is repeated:
      //  - black casing: record the strongest coverage in a mask; it darkens the picture once,
      //    after all casing pieces (Apply_Casing_Mask)
      //  - light lines: the line's colour scaled by coverage (as if over black), keeping the
      //    brighter of that and what is there (max is the same however often it's applied)
      if (darken) {
        if (cover > mask_row[x]) mask_row[x] = (uint8_t)LV_MIN(cover, 255u);
        return;
      }
      // Overlays (the breaker line) are painted over what's there, so they keep their colour on
      // the light route; they are thin and their few joints don't show the double blend
      if (OVER) { row[x] = cover >= 250 ? col : lv_color_mix(col, row[x], (lv_opa_t)cover); return; }
      row[x] = Lighten(row[x], cover >= 250 ? col : Scale(col, cover));
    };

    int x = rx1;
    int32_t across = across0, along = along0;
    for (; x <= rx2 && x < rx1 + k1; x++, across += step_across, along += step_along) edge_pixel(x, across, along);
    if (k1 <= k2) {
      // Solid middle: full coverage, so only the edge fade varies (and only towards the rim)
      int xe = rx1 + k2;
      uint32_t last_f = 256;
      uint32_t cover = 0;
      lv_color_t lit = color;
      for (; x <= xe; x++) {
        uint32_t f = FADE ? Pixel_Fade(fr, x) : 0;
        if (f != last_f) {
          last_f = f;
          cover = f >= 250 ? 0 : (255 * (256 - f)) >> 8;
          lit = cover >= 250 ? color : Scale(color, cover);
        }
        if (cover <= 4) continue;
        if (DARKEN) {
          if (cover > mask_row[x]) mask_row[x] = (uint8_t)cover;
        } else if (OVER) {
          row[x] = cover >= 250 ? color : lv_color_mix(color, row[x], (lv_opa_t)cover);
        } else {
          row[x] = row[x].full == 0 ? lit : Lighten(row[x], lit);
        }
      }
      across = across0 + step_across * (x - rx1);
      along = along0 + step_along * (x - rx1);
    }
    for (; x <= rx2; x++, across += step_across, along += step_along) edge_pixel(x, across, along);
  }
}

// A plain line painted over the picture (see OVER in Capsule_Kind)
static void Capsule_Over(const Raster &r, Pt a, Pt b, float half, lv_color_t color)
{
  if (fade_on) Capsule_Kind<false, false, true, true>(r, a, b, half, color, half, color);
  else Capsule_Kind<false, false, false, true>(r, a, b, half, color, half, color);
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

  // All the casing first (behind and ahead), then the lines on top. Drawing a part's casing
  // after another part's line cut dark rings into it (at every joint, and where the dim part
  // behind the rider meets the bright part ahead)
  const float behind = (ROUTE_WIDTH - 2) / 2.0f, ahead = ROUTE_WIDTH / 2.0f;
  lv_color_t black = lv_color_black(), done = lv_color_hex(COLOR_ROUTE_DONE), bright = lv_color_hex(COLOR_ROUTE);
  Pt split_pt = To_Screen(&r.box, ox + route[split][0], oy + route[split][1]);
  const float (*rest)[2] = route + split + 1;
  uint16_t rest_n = route_count - split - 1;
  // The strip being drawn must fit the casing mask; LVGL draws in strips of at most MASK_ROWS
  bool fits = r.buf_w <= 480 && lv_area_get_height(&r.buf_area) <= MASK_ROWS;
  for (int pass = 0; pass < 2; pass++) {
    float extra = pass == 0 ? ROUTE_CASING : 0;
    lv_color_t back = pass == 0 ? black : done, front = pass == 0 ? black : bright;
    if (pass == 0 && !fits) continue;
    Polyline(r, route, split + 1, ox, oy, NULL, behind + extra, back, behind + extra, back);
    Capsule2(r, split_pt, here, behind + extra, back, behind + extra, back);
    Polyline(r, rest, rest_n, ox, oy, &here, ahead + extra, front, ahead + extra, front);
    if (pass == 0) Apply_Casing_Mask(r);
  }
}

/* The next speed breaker (within 200 m): a thin amber line along the route from the rider to it,
   and a short bar across the route at it, each with a dark hairline edge so they stand out on
   the light route. The phone sends the breaker's spot in an area's frame. */
static bool bump_on = false;
static uint16_t bump_area;
static float bump_x, bump_y;

void Map_Layer_Set_Bump(bool on, uint16_t area_id, float x, float y)
{
  bool changed = on != bump_on || (on && (area_id != bump_area || x != bump_x || y != bump_y));
  bump_on = on;
  bump_area = area_id;
  bump_x = x;
  bump_y = y;
  if (changed && layer) lv_obj_invalidate(layer);
}

static void Draw_Bump(const Raster &r)
{
  float ox, oy, bx, by;
  if (!bump_on || route_count < 2 || !has_route || !Convert(route_area, route_ox, route_oy, active->id, &ox, &oy) ||
      !Convert(bump_area, bump_x, bump_y, active->id, &bx, &by)) return;
  float s_cam, s_bump;
  Nearest_On_Route(cam_x - ox, cam_y - oy, &s_cam);
  if (Nearest_On_Route(bx - ox, by - oy, &s_bump) > 20.0f * 20.0f || s_bump <= s_cam) return;

  // The stretch from the rider to the breaker, in screen pixels
  static Pt pts[ROUTE_MAX];                             // static: 2 KB is a lot for the draw task's stack
  uint16_t n = 0;
  float x, y;
  Route_Point(s_cam, &x, &y);
  pts[n++] = To_Screen(&r.box, ox + x, oy + y);
  for (uint16_t i = 0; i < route_count && n < ROUTE_MAX - 1; i++)
    if (route_along[i] > s_cam && route_along[i] < s_bump) pts[n++] = To_Screen(&r.box, ox + route[i][0], oy + route[i][1]);
  Route_Point(s_bump, &x, &y);
  Pt end = To_Screen(&r.box, ox + x, oy + y);
  pts[n++] = end;

  // The bar: across the route's direction just before the breaker
  float ax, ay;
  Route_Point(s_bump - 3, &ax, &ay);
  Pt before = To_Screen(&r.box, ox + ax, oy + ay);
  float dx = end.x - before.x, dy = end.y - before.y, len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) { dx = 0; dy = -1; len = 1; }
  const float bar = 13;                                // px either side of the route
  Pt b1 = { end.x - dy / len * bar, end.y + dx / len * bar }, b2 = { end.x + dy / len * bar, end.y - dx / len * bar };

  lv_color_t dark = lv_color_hex(0x1A1200), amber = lv_color_hex(0xF5B83D);
  for (int pass = 0; pass < 2; pass++) {               // dark edge first, then amber on top
    float half = pass == 0 ? 3.0f : 1.8f, bar_half = pass == 0 ? 3.6f : 2.4f;
    lv_color_t col = pass == 0 ? dark : amber;
    for (uint16_t i = 0; i + 1 < n; i++) Capsule_Over(r, pts[i], pts[i + 1], half, col);
    Capsule_Over(r, b1, b2, bar_half, col);
  }
}

// Road points in screen pixels for the current camera, worked out once per frame (LVGL draws the
// band in ~11 strips, and transforming every point for every strip cost ~12 ms a frame), with
// each road's screen bounds so strips can skip roads quickly. In PSRAM (internal RAM is for Bluetooth).
struct Road_Box { float x1, y1, x2, y2; };
static Pt *screen_pts;
static Road_Box *screen_box;
static struct { const Map_Area *area; uint16_t roads; float x, y, h, zoom; lv_coord_t bx, by; } screen_key;

static void Prepare_Screen_Points(const Raster &r)
{
  if (screen_key.area == active && screen_key.roads == active->road_count && screen_key.x == cam_x &&
      screen_key.y == cam_y && screen_key.h == cam_h && screen_key.zoom == zoom &&
      screen_key.bx == r.box.x1 && screen_key.by == r.box.y1) return;
  screen_key = { active, active->road_count, cam_x, cam_y, cam_h, zoom, r.box.x1, r.box.y1 };
  for (uint16_t i = 0; i < active->road_count; i++) {
    const Map_Road &road = active->roads[i];
    Road_Box b = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (uint8_t k = 0; k < road.count; k++) {
      const int16_t *p = active->points[road.start + k];
      Pt s = To_Screen(&r.box, p[0], p[1]);
      screen_pts[road.start + k] = s;
      b.x1 = Min_F(b.x1, s.x); b.x2 = Max_F(b.x2, s.x);
      b.y1 = Min_F(b.y1, s.y); b.y2 = Max_F(b.y2, s.y);
    }
    screen_box[i] = b;
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

  if (active && screen_pts) {
    Prepare_Screen_Points(r);
    // Minor roads first, so bigger roads are drawn over them
    for (int cls = 3; cls >= 1; cls--) {
      float half = road_width[cls] / 2.0f;
      float reach = half + 1;
      float cy1 = r.clip.y1 - reach, cy2 = r.clip.y2 + 1 + reach, cx1 = r.clip.x1 - reach, cx2 = r.clip.x2 + 1 + reach;
      lv_color_t color = lv_color_hex(road_color[cls]);
      for (uint16_t i = 0; i < active->road_count; i++) {
        const Map_Road &road = active->roads[i];
        if (road.road_class != cls) continue;
        const Road_Box &b = screen_box[i];
        if (b.y2 < cy1 || b.y1 > cy2 || b.x2 < cx1 || b.x1 > cx2) continue;
        const Pt *s = &screen_pts[road.start];
        for (uint8_t k = 1; k < road.count; k++) {
          // Pieces entirely above or below this strip: skip without a call
          if ((s[k - 1].y < cy1 && s[k].y < cy1) || (s[k - 1].y > cy2 && s[k].y > cy2)) continue;
          Capsule(r, s[k - 1], s[k], half, color);
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
    icon.font = &font_fuel_20;
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
      lv_area_t glyph = { (lv_coord_t)(sx - 10), (lv_coord_t)(sy - font_fuel_20.line_height / 2),
                          (lv_coord_t)(sx + 10), (lv_coord_t)(sy + font_fuel_20.line_height / 2) };
      lv_draw_label(ctx, &icon, &glyph, FUEL_GLYPH, NULL);
    }
    map_perf_fuel_us += micros() - t0;
  }

  t0 = micros();
  Draw_Route(r);
  Draw_Bump(r);
  map_perf_route_us += micros() - t0;
}

/********************************* Main loop side *********************************/
void Map_Layer_Poll(void)
{
  portENTER_CRITICAL(&map_lock);
  Map_Area *next = ready;
  if (next) {
    ready = NULL;
    active = next;                            // the old one is free for the next area
    screen_key.area = NULL;                   // screen points belong to the old area
  }
  portEXIT_CRITICAL(&map_lock);
  if (next) {
    // New area on screen: the latest pose converts into it; the camera carries on smoothly
    // (corrections are differences, the same in any area's frame)
    Refresh_Pose();
    if (layer) lv_obj_invalidate(layer);
  }
}

bool map_cam_log = false;   // serial "camlog": print every new map frame's camera
static bool map_visible = true;

void Map_Layer_Set_Visible(bool visible)
{
  map_visible = visible;
  if (visible && layer) lv_obj_invalidate(layer);
}

// Every frame: move the camera and zoom, and redraw only if the picture actually changes
static void Frame(lv_timer_t *timer)
{
  static uint32_t last = 0;
  uint32_t now = lv_tick_get();
  float dt = last ? (uint32_t)(now - last) / 1000.0f : 0;
  last = now;
  if (turn_dist >= 0) {
    // Distance to the turn now, counting down at the rider's speed since the last update
    float coast = fminf((uint32_t)(now - turn_at) / 1000.0f, PREDICT_MAX_S);
    zoom_target = Zoom_For(turn_dist - last_pose.speed * coast);
    if (zoom == 0) zoom = zoom_mid = zoom_target;
  }
  if (zoom_target > 0) {
    // Two easing stages: the zoom speeds up and slows down gently instead of lurching
    float k = 1 - expf(-dt / (ZOOM_EASE_S / 2));
    zoom_mid += (zoom_target - zoom_mid) * k;
    zoom += (zoom_mid - zoom) * k;
  }
  // Covered by another page (the lean screen): its redraws would only repaint that page
  if (!pose_ok || !layer || !map_visible) return;
  Update_Camera(now);
  float moved_px = sqrtf((cam_x - drawn_x) * (cam_x - drawn_x) + (cam_y - drawn_y) * (cam_y - drawn_y)) * zoom;
  if (moved_px > 0.3f || fabsf(cam_h - drawn_h) > 0.15f || fabsf(zoom - drawn_zoom) > 0.002f * zoom) {
    drawn_x = cam_x; drawn_y = cam_y; drawn_h = cam_h; drawn_zoom = zoom;
    lv_obj_invalidate(layer);
    if (map_cam_log) printf("F %lu %.2f %.2f %.2f %.3f\r\n", (unsigned long)now, cam_x, cam_y, cam_h, zoom);
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
  screen_pts = (Pt *)heap_caps_malloc(MAP_MAX_POINTS * sizeof(Pt), MALLOC_CAP_SPIRAM);
  screen_box = (Road_Box *)heap_caps_malloc(MAP_MAX_ROADS * sizeof(Road_Box), MALLOC_CAP_SPIRAM);
  areas = (Map_Area *)heap_caps_calloc(AREA_BUFFERS, sizeof(Map_Area), MALLOC_CAP_SPIRAM);
  for (int i = 0; areas && i < AREA_BUFFERS; i++) areas[i].id = 0xFFFF;
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
