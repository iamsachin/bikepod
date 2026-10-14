/*****************************************************************************
  | File        :   Smooth_Arc.cpp
  | help        :   Anti-aliased ring arcs (see Smooth_Arc.h).
******************************************************************************/
#include "Smooth_Arc.h"
#include <math.h>

#define CX 240.0f
#define CY 240.0f
#define MAX_ARCS 4

struct Arc_Data {
  float radius, half;           // centre line radius, half the width
  lv_color_t color;
  float start, sweep;           // degrees clockwise from 12 o'clock
};
static Arc_Data arcs[MAX_ARCS];
static int arc_count = 0;

static void Point_At(const Arc_Data &d, float deg, float *x, float *y)
{
  float a = deg * (float)M_PI / 180;
  *x = CX + d.radius * sinf(a);
  *y = CY - d.radius * cosf(a);
}

static void Draw_Cb(lv_event_t *e)
{
  const Arc_Data &d = *(const Arc_Data *)lv_event_get_user_data(e);
  if (d.sweep <= 0) return;
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  const lv_area_t &clip = *ctx->clip_area, &buf_area = *ctx->buf_area;
  lv_color_t *buf = (lv_color_t *)ctx->buf;
  const int buf_w = lv_area_get_width(&buf_area);

  const float r_in = d.radius - d.half - 0.6f, r_out = d.radius + d.half + 0.6f;
  const bool full = d.sweep >= 360;
  float sx, sy, ex, ey;                                 // centres of the round ends
  Point_At(d, d.start, &sx, &sy);
  Point_At(d, d.start + d.sweep, &ex, &ey);

  for (int y = clip.y1; y <= clip.y2; y++) {
    float dy = y + 0.5f - CY;
    if (fabsf(dy) > r_out) continue;
    // The band crosses this row in up to two spans, left and right of the centre
    float outer = sqrtf(r_out * r_out - dy * dy);
    float inner = fabsf(dy) < r_in ? sqrtf(r_in * r_in - dy * dy) : 0;
    for (int side = -1; side <= 1; side += 2) {
      // Left span [-outer, -inner], right span [inner, outer]; with no hole (top and bottom of
      // the ring) one span [-outer, outer]
      int xa, xb;
      if (inner == 0) { xa = (int)floorf(CX - outer); xb = (int)ceilf(CX + outer); }
      else if (side < 0) { xa = (int)floorf(CX - outer); xb = (int)ceilf(CX - inner); }
      else { xa = (int)floorf(CX + inner); xb = (int)ceilf(CX + outer); }
      xa = LV_MAX(xa, clip.x1);
      xb = LV_MIN(xb, clip.x2);
      lv_color_t *row = buf + (y - buf_area.y1) * buf_w - buf_area.x1;
      for (int x = xa; x <= xb; x++) {
        float dx = x + 0.5f - CX;
        float dist = sqrtf(dx * dx + dy * dy);
        float cover = d.half + 0.5f - fabsf(dist - d.radius);     // 1 px soft edge across the band
        if (cover <= 0) continue;
        if (!full) {
          float a = atan2f(dx, -dy) * 180 / (float)M_PI;      // clockwise from 12 o'clock
          float rel = fmodf(a - d.start + 720, 360);
          if (rel > d.sweep) {
            // Beyond the arc: only its round ends
            float ds = sqrtf((x + 0.5f - sx) * (x + 0.5f - sx) + (y + 0.5f - sy) * (y + 0.5f - sy));
            float de = sqrtf((x + 0.5f - ex) * (x + 0.5f - ex) + (y + 0.5f - ey) * (y + 0.5f - ey));
            cover = d.half + 0.5f - fminf(ds, de);
            if (cover <= 0) continue;
          }
        }
        if (cover >= 1) row[x] = d.color;
        else row[x] = lv_color_mix(d.color, row[x], (lv_opa_t)(cover * 255));
      }
      if (inner == 0) break;                          // one span covers the whole row
    }
  }
}

lv_obj_t *Smooth_Arc_Create(lv_obj_t *parent, float radius, float width, lv_color_t color)
{
  if (arc_count >= MAX_ARCS) return NULL;
  Arc_Data *d = &arcs[arc_count++];
  *d = { radius, width / 2, color, 0, 0 };
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_coord_t size = (lv_coord_t)ceilf(2 * (radius + width / 2) + 4);
  lv_obj_set_size(obj, size, size);
  lv_obj_center(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(obj, Draw_Cb, LV_EVENT_DRAW_MAIN, d);
  lv_obj_set_user_data(obj, d);
  return obj;
}

// Marks the part of the ring between two angles for redrawing (sampled every 10 degrees)
static void Invalidate_Span(lv_obj_t *obj, const Arc_Data &d, float from, float to)
{
  if (to < from) { float t = from; from = to; to = t; }
  if (to - from >= 350) { lv_obj_invalidate(obj); return; }
  float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
  for (float a = from; ; a += 10) {
    float x, y;
    Point_At(d, fminf(a, to), &x, &y);
    x1 = fminf(x1, x); y1 = fminf(y1, y); x2 = fmaxf(x2, x); y2 = fmaxf(y2, y);
    if (a >= to) break;
  }
  lv_coord_t pad = (lv_coord_t)ceilf(d.half + 2);
  lv_area_t area = { (lv_coord_t)(x1 - pad), (lv_coord_t)(y1 - pad), (lv_coord_t)(x2 + pad), (lv_coord_t)(y2 + pad) };
  lv_obj_invalidate_area(lv_obj_get_parent(obj), &area);
}

void Smooth_Arc_Set(lv_obj_t *obj, float start_deg, float sweep_deg)
{
  Arc_Data &d = *(Arc_Data *)lv_obj_get_user_data(obj);
  start_deg = fmodf(start_deg + 360, 360);
  sweep_deg = LV_CLAMP(0.0f, sweep_deg, 360.0f);
  if (start_deg == d.start && sweep_deg == d.sweep) return;
  float old_start = d.start, old_end = d.start + d.sweep;
  bool was_empty = d.sweep <= 0;
  d.start = start_deg;
  d.sweep = sweep_deg;
  if (was_empty || sweep_deg <= 0 || sweep_deg >= 360) { lv_obj_invalidate(obj); return; }
  // Only what changed: around the old and new start, and the old and new end (unwrapped so the
  // shorter way round is used)
  auto near = [](float a, float b) { while (b - a > 180) b -= 360; while (a - b > 180) b += 360; return b; };
  float new_end = d.start + d.sweep;
  Invalidate_Span(obj, d, old_start - 1, near(old_start, d.start) + 1);
  Invalidate_Span(obj, d, old_end - 1, near(old_end, new_end) + 1);
}
