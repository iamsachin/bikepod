#pragma once
#include <lvgl.h>

enum Turn_Dir { TURN_LEFT, TURN_RIGHT, TURN_STRAIGHT };

void Nav_Screen_Create(void);
// distance_m: metres to the turn; progress: 0..1000 (how much of the approach is done)
void Nav_Screen_Update(Turn_Dir dir, uint32_t distance_m, uint16_t progress, const char *road);
void Nav_Fake_Data_Start(void);
void Nav_Fake_Data_Stop(void);
