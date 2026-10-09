#pragma once

// Screens you swipe between: 0 = navigation, 1 = lean. Swipe right for the next screen, left to
// go back.
void Pages_Init(void);
void Pages_Show(int page);
int Pages_Current(void);
