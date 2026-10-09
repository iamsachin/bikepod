#pragma once
#include <lvgl.h>

// Street map under the route drawing.
//
// The phone sends the roads around the rider once per "area" (every ~150 m of riding), in
// metres north-up from the area's reference point, then the rider's pose in that frame with
// every GPS update. The pod turns the roads heading-up around the rider itself, so Bluetooth
// carries a few bytes per update instead of the whole map.

enum Map_Road_Class { MAP_MAJOR = 1, MAP_MEDIUM = 2, MAP_MINOR = 3 };

// Called from the Bluetooth task while an area arrives (MAPBEGIN / MAP / MAPEND lines)
void Map_Data_Begin(uint16_t area_id);
void Map_Data_Add_Road(uint16_t area_id, uint8_t road_class, const int16_t (*pts)[2], uint8_t count);
void Map_Data_End(uint16_t area_id, uint16_t road_count);

// LVGL side (main loop only)
lv_obj_t *Map_Layer_Create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t marker_x, lv_coord_t marker_y);
void Map_Layer_Poll(void);                                   // picks up a newly completed area
void Map_Layer_Set_Pose(uint16_t area_id, int16_t x, int16_t y, int16_t heading_deg);
void Map_Layer_Set_Scale(float pixels_per_metre);
bool Map_Layer_Has_Map(void);
void Map_Layer_Clear(void);
