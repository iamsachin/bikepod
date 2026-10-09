#pragma once
#include <lvgl.h>

enum Turn_Dir { TURN_LEFT, TURN_RIGHT, TURN_STRAIGHT };
enum Screen_Mode { MODE_WAITING, MODE_TURN, MODE_REROUTING, MODE_ARRIVED, MODE_CRUISE };

void Nav_Screen_Create(void);
// Approaching-turn screen. distance_m: metres to the turn; progress: 0..1000 (how much of the approach is done)
void Nav_Screen_Update(Turn_Dir dir, uint32_t distance_m, uint16_t progress, const char *road);
// Route around the rider in metres, heading-up (+x right, +y ahead); the rider is the (0,0) point
void Nav_Screen_Set_Path(const int16_t (*pts)[2], uint8_t count);
// Bottom line on the turn and rerouting screens: arrival time and kilometres left
void Nav_Screen_Set_Trip(const char *eta, float km_left);
void Nav_Screen_Show_Waiting(void);
void Nav_Screen_Show_Rerouting(void);
void Nav_Screen_Show_Arrived(float trip_km, uint32_t trip_min, const char *place);
void Nav_Screen_Show_Cruise(uint32_t speed_kmh, const char *clock);
void Nav_Screen_Set_Stale(bool stale);
Screen_Mode Nav_Screen_Mode(void);
