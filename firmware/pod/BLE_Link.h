#pragma once
#include <Arduino.h>
#include "Nav_Screen.h"

// Phone -> pod over Bluetooth LE.
// The phone writes one UTF-8 line to the "Nav" characteristic:  <dir>|<metres>|<road>
//   dir: L = left, R = right, S = straight      e.g.  R|400|MG Road
#define POD_BLE_NAME          "BikePod"
#define POD_SERVICE_UUID      "a30be272-f18f-4629-b9d2-b4dc872e5d05"
#define POD_NAV_CHAR_UUID     "50632787-1b5b-4788-a4ee-8ff5dc145962"

struct Nav_Message {
  Turn_Dir dir;
  uint32_t distance_m;
  char road[64];
};

void BLE_Link_Init(void);
bool BLE_Link_Connected(void);
// Copies the newest message into msg and returns true if one arrived since the last call
bool BLE_Link_Take_Message(Nav_Message *msg);
