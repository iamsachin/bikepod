#pragma once
#include <Arduino.h>
#include "Nav_Screen.h"

// Phone -> pod over Bluetooth LE.
// The phone writes one UTF-8 line at a time to the "Nav" characteristic:
//   L|400|MG Rd               next turn: L = left, R = right, S = straight, metres, road name
//   TRIP|18:42|6.0            arrival time and kilometres left
//   RPATH|-2,-30;0,0;3,85;... route around the rider in metres, north-up (+x east, +y north),
//                             relative to the rider at the latest POSE (the rider is 0,0)
//   MAPBEGIN|7                start of map area 7 (roads around the rider, see Map_Layer.h)
//   MAP|7|2|-120,40;-80,35;.. one road of area 7: class (1 major, 2 medium, 3 minor), points in
//                             metres north-up from the area's reference point
//   FUEL|7|40,85;-120,300     fuel stations in area 7 (same frame as MAP)
//   MAPEND|7|96               area 7 complete with 96 roads: the pod switches to it
//   POSE|7|12,-40,85          rider in area 7's frame: x, y metres and heading in degrees
//   SPD|123                   GPS speed in tenths of a metre per second (for the lean estimate)
//   REROUTE                   off route, a new route is being fetched
//   ARRIVED|7.3|29|Cafe       trip finished: kilometres, minutes, destination
//   CRUISE|42|21:35           connected but not navigating: speed in km/h, local time
// The phone repeats its current lines every ~2 s so the pod can spot a silent link.
#define POD_BLE_NAME          "BikePod"
#define POD_SERVICE_UUID      "a30be272-f18f-4629-b9d2-b4dc872e5d05"
#define POD_NAV_CHAR_UUID     "50632787-1b5b-4788-a4ee-8ff5dc145962"

#define NAV_PATH_MAX_POINTS   24

enum Msg_Kind { MSG_TURN, MSG_TRIP, MSG_ROUTE, MSG_POSE, MSG_SPEED, MSG_REROUTE, MSG_ARRIVED, MSG_CRUISE };

struct Nav_Message {
  Msg_Kind kind;
  // MSG_TURN
  Turn_Dir dir;
  uint32_t distance_m;
  char text[64];            // MSG_TURN road name, MSG_ARRIVED destination
  // MSG_TRIP / MSG_CRUISE
  char clock[8];            // arrival time (TRIP) or local time (CRUISE)
  float km;                 // km left (TRIP) or trip km (ARRIVED)
  // MSG_ARRIVED
  uint32_t trip_min;
  // MSG_CRUISE
  uint32_t speed_kmh;
  // MSG_SPEED
  float speed_mps;
  // MSG_ROUTE
  uint8_t path_count;
  int16_t path[NAV_PATH_MAX_POINTS][2];
  // MSG_POSE
  uint16_t area_id;
  int16_t pose[3];                          // x, y, heading
};

void BLE_Link_Init(void);
bool BLE_Link_Connected(void);   // also true while test lines are being injected
// For testing from the Mac (serial "msg <line>"): handle a line as if the phone had sent it.
// While test lines are being injected, lines from the phone are ignored (until "test off").
void BLE_Link_Inject(const String &line);
void BLE_Link_End_Test(void);
// Takes the oldest waiting message; returns false when there is none
bool BLE_Link_Take_Message(Nav_Message *msg);
