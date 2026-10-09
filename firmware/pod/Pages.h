#pragma once

// Screens you swipe between: 0 = navigation, 1 = lean. Swipe right for the next screen, left to
// go back; the screen's name shows briefly at the bottom when it changes.
void Pages_Init(void);
void Pages_Show(int page);
int Pages_Current(void);
