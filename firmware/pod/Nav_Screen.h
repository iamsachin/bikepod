#pragma once
#include <lvgl.h>

enum Turn_Dir { TURN_LEFT, TURN_RIGHT, TURN_STRAIGHT, TURN_ROUNDABOUT };
enum Screen_Mode { MODE_WAITING, MODE_TURN, MODE_REROUTING, MODE_ARRIVED, MODE_CRUISE };

void Nav_Screen_Create(void);
// Approaching-turn screen. distance_m: metres to the turn; progress: 0..1000 (how much of the approach is done).
// Roundabouts: rb_exit = exit number (0 unknown), rb_degrees = how far round the exit is, + clockwise
void Nav_Screen_Update(Turn_Dir dir, uint32_t distance_m, uint16_t progress, const char *road,
                       uint8_t rb_exit = 0, int16_t rb_degrees = 0);
// Rider's position and heading in the current map area's frame (see Map_Layer.h)
void Nav_Screen_Set_Pose(uint16_t area_id, float x, float y, float heading_deg);
// Route, whole metres in a map area's frame (drawn on the map)
void Nav_Screen_Set_Route(uint16_t area_id, const int16_t (*pts)[2], uint8_t count);
// Bottom line on the turn and rerouting screens: arrival time and kilometres left
void Nav_Screen_Set_Trip(const char *eta, float km_left);
// The speed limit sign (bottom right, hidden at 0) and a speed breaker warning in place of the
// arrival line (metres ahead, 0 = none)
void Nav_Screen_Set_Limit(uint32_t kmh);
void Nav_Screen_Set_Bump(uint32_t metres);
void Nav_Screen_Show_Waiting(void);
void Nav_Screen_Show_Rerouting(const char *detail);   // detail: why (empty = "Finding a new route")
void Nav_Screen_Show_Arrived(float trip_km, uint32_t trip_min, float avg_kmh, float top_kmh, const char *place);
void Nav_Screen_Show_Cruise(uint32_t speed_kmh, const char *clock);
void Nav_Screen_Set_Stale(bool stale);
Screen_Mode Nav_Screen_Mode(void);
void Nav_Screen_Debug(const char *what, bool hide);
void Nav_Screen_Bench(void);
