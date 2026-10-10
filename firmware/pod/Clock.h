#pragma once
#include <stdint.h>
#include <stddef.h>

// The board's PCF85063 real-time clock. The phone sets it (TIME message) whenever it's
// connected; it keeps running without the phone as long as the pod has power.

void Clock_Init(void);
// Local time from the phone
void Clock_Set(uint8_t hour, uint8_t minute, uint8_t second);
// Current local time; false if the clock hasn't been set since it last lost power
bool Clock_Get(uint8_t *hour, uint8_t *minute, uint8_t *second);
// "4:32 PM" / "16:32" (as chosen in settings), or "" if the time isn't known
void Clock_Text(char *out, size_t size);
