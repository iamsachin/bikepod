/*****************************************************************************
  | File        :   Map_Layer.cpp
  | help        :   Draws the streets around the rider (see Map_Layer.h).
                    Two buffers: the Bluetooth task fills one while the screen draws the other,
                    and they swap when an area is complete, so a new area never flickers in.
******************************************************************************/
#include "Map_Layer.h"
#include <Arduino.h>
#include <math.h>

#define MAP_MAX_ROADS   400
#define MAP_MAX_POINTS  2400

struct Map_Road { uint16_t start; uint8_t count; uint8_t road_class; };

struct Map_Area {
  uint16_t id;
  uint16_t road_count;
  uint16_t point_count;
  bool overflow;
  Map_Road roads[MAP_MAX_ROADS];
  int16_t points[MAP_MAX_POINTS][2];
};

static Map_Area areas[2];
static Map_Area *volatile active = NULL;      // being drawn (main loop only)
static Map_Area *volatile building = &areas[0];
static Map_Area *volatile ready = NULL;       // complete, waiting for the main loop to swap in
static portMUX_TYPE map_lock = portMUX_INITIALIZER_UNLOCKED;

// Line widths and colours by road class: big roads a little wider and lighter
static const lv_coord_t road_width[4] = { 0, 12, 9, 6 };
static const uint32_t road_color[4] = { 0, 0x4A4A4A, 0x3A3A3A, 0x2C2C2C };

static lv_obj_t *layer;
static lv_coord_t center_x, center_y;
static float scale = 0;               // pixels per metre (set with the route drawing's zoom)
static bool have_pose = false;
static uint16_t pose_area = 0;
static float pose_x, pose_y, pose_sin, pose_cos;

/******************************* Bluetooth task side *******************************/
void Map_Data_Begin(uint16_t area_id)
{
  portENTER_CRITICAL(&map_lock);
  Map_Area *a = building;
  if (a) {
    a->id = area_id;
    a->road_count = a->point_count = 0;
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
  if (next && layer) lv_obj_invalidate(layer);
}

void Map_Layer_Set_Pose(uint16_t area_id, int16_t x, int16_t y, int16_t heading_deg)
{
  float h = heading_deg * (float)M_PI / 180;
  pose_area = area_id;
  pose_x = x;
  pose_y = y;
  pose_sin = sinf(h);
  pose_cos = cosf(h);
  have_pose = true;
  if (layer) lv_obj_invalidate(layer);
}

void Map_Layer_Set_Scale(float pixels_per_metre)
{
  if (fabsf(pixels_per_metre - scale) < 0.0005f) return;
  scale = pixels_per_metre;
  if (layer) lv_obj_invalidate(layer);
}

bool Map_Layer_Has_Map(void)
{
  return active && have_pose && pose_area == active->id;
}

void Map_Layer_Clear(void)
{
  have_pose = false;
  if (layer) lv_obj_invalidate(layer);
}

// Rotate the area's north-up metres into the rider's heading-up view, in screen pixels
static inline lv_point_t To_Screen(const lv_area_t *box, const int16_t *p)
{
  float dx = p[0] - pose_x, dy = p[1] - pose_y;
  float right = dx * pose_cos - dy * pose_sin;
  float ahead = dx * pose_sin + dy * pose_cos;
  lv_point_t s = { (lv_coord_t)lroundf(box->x1 + center_x + right * scale),
                   (lv_coord_t)lroundf(box->y1 + center_y - ahead * scale) };
  return s;
}

static void Draw_Event(lv_event_t *e)
{
  if (!Map_Layer_Has_Map() || scale <= 0) return;
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t box;
  lv_obj_get_coords(layer, &box);
  const lv_area_t *clip = ctx->clip_area;

  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.round_start = 1;
  dsc.round_end = 1;

  // Minor roads first, so bigger roads are drawn over them
  for (int cls = 3; cls >= 1; cls--) {
    dsc.width = road_width[cls];
    dsc.color = lv_color_hex(road_color[cls]);
    lv_coord_t pad = dsc.width;
    for (uint16_t r = 0; r < active->road_count; r++) {
      const Map_Road &road = active->roads[r];
      if (road.road_class != cls) continue;
      lv_point_t prev = To_Screen(&box, active->points[road.start]);
      for (uint8_t i = 1; i < road.count; i++) {
        lv_point_t next = To_Screen(&box, active->points[road.start + i]);
        // Skip segments wholly outside the area being redrawn
        bool outside = (prev.x < clip->x1 - pad && next.x < clip->x1 - pad) ||
                       (prev.x > clip->x2 + pad && next.x > clip->x2 + pad) ||
                       (prev.y < clip->y1 - pad && next.y < clip->y1 - pad) ||
                       (prev.y > clip->y2 + pad && next.y > clip->y2 + pad);
        if (!outside) lv_draw_line(ctx, &dsc, &prev, &next);
        prev = next;
      }
    }
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
  return layer;
}
