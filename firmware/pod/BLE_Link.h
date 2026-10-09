#pragma once
#include <Arduino.h>
#include "Nav_Screen.h"

// Phone -> pod over Bluetooth LE.
// The phone writes one UTF-8 line to the "Nav" characteristic:
//   L|400|MG Rd          next turn: L = left, R = right, S = straight, metres, road name
//   REROUTE              off route, a new route is being fetched
//   ARRIVED|7.3|29       trip finished: kilometres, minutes
//   CRUISE|42|21:35      connected but not navigating: speed in km/h, local time
// The phone repeats the current line every ~2 s so the pod can spot a silent link.
#define POD_BLE_NAME          "BikePod"
#define POD_SERVICE_UUID      "a30be272-f18f-4629-b9d2-b4dc872e5d05"
#define POD_NAV_CHAR_UUID     "50632787-1b5b-4788-a4ee-8ff5dc145962"

enum Msg_Kind { MSG_TURN, MSG_REROUTE, MSG_ARRIVED, MSG_CRUISE };

struct Nav_Message {
  Msg_Kind kind;
  // MSG_TURN
  Turn_Dir dir;
  uint32_t distance_m;
  char road[64];
  // MSG_ARRIVED
  float trip_km;
  uint32_t trip_min;
  // MSG_CRUISE
  uint32_t speed_kmh;
  char clock[8];
};

void BLE_Link_Init(void);
bool BLE_Link_Connected(void);
// Copies the newest message into msg and returns true if one arrived since the last call
bool BLE_Link_Take_Message(Nav_Message *msg);
