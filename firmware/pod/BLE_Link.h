#pragma once
#include <Arduino.h>
#include "Nav_Screen.h"

// Phone -> pod over Bluetooth LE.
// The phone writes one UTF-8 line at a time to the "Nav" characteristic:
//   L|400|MG Rd               next turn: L = left, R = right, S = straight, metres, road name
//   O|250|4|271|Hosur Rd      roundabout: metres, exit number (0 = unknown), degrees round to the
//                             exit (+ clockwise, as in India; - anticlockwise), road name
//   TRIP|18:42|6.0|7.3|1440   arrival time, kilometres left, the route's total km, seconds riding
//   RPATH|7|-2,-30;3,85;...   route around the rider, whole metres in map area 7's frame
//   MAPBEGIN|7|12.9|77.6      start of map area 7 (roads around the rider, see Map_Layer.h),
//                             with its reference point (latitude, longitude)
//   MAP|7|2|-120,40;-80,35;.. one road of area 7: class (1 major, 2 medium, 3 minor), points in
//                             metres north-up from the area's reference point
//   FUEL|7|40,85;-120,300     fuel stations in area 7 (same frame as MAP)
//   MAPEND|7|96               area 7 complete with 96 roads: the pod switches to it
//   POSE|7|120,-405,853       rider in area 7's frame: x, y and heading in tenths (metres, degrees)
//   SPD|123                   GPS speed in tenths of a metre per second (lean estimate, speed shown)
//   LIMIT|50                  speed limit where the rider is, km/h (0 = not known)
//   BUMP|85|7|120|-40         metres to the next mapped speed breaker (0 = none close); optionally
//                             where it is: area 7, x 120, y -40 metres (the map draws a line to it)
//   TIME|16|32|05             the phone's local time (sets the pod's clock)
//   REROUTE|detail            off route, a new route is being fetched ("Finding a new route" / "No internet")
//   ARRIVED|7.3|29|Cafe       trip finished: kilometres, minutes, destination
//   CRUISE|42|21:35           connected but not navigating: speed in km/h, local time
// The phone repeats its current lines every ~2 s so the pod can spot a silent link.
#define POD_BLE_NAME          "BikePod"
#define POD_SERVICE_UUID      "a30be272-f18f-4629-b9d2-b4dc872e5d05"
#define POD_NAV_CHAR_UUID     "50632787-1b5b-4788-a4ee-8ff5dc145962"
// Pod -> phone: notifications the app subscribes to, one UTF-8 line each:
//   NOWPLAYING|7|Tum Hi Ho|Arijit Singh   the song changed (id 7): the app looks up its cover
//   LEAN|31|27                 deepest lean left / right (degrees) in the last 2 s, while moving
// Phone -> pod, in reply (on the Nav characteristic):
//   ART|7|0|80|<base64>       part 0 of 80 of song 7's cover: 120x120 RGB565, 360 bytes a part
#define POD_EVENT_CHAR_UUID   "6e0a3a5c-6f2b-4c1d-9c9a-2b6c5d7e8f10"

#define NAV_PATH_MAX_POINTS   40

enum Msg_Kind { MSG_TURN, MSG_TRIP, MSG_ROUTE, MSG_POSE, MSG_SPEED, MSG_REROUTE, MSG_ARRIVED, MSG_CRUISE,
                MSG_LIMIT, MSG_BUMP, MSG_TIME };

struct Nav_Message {
  Msg_Kind kind;
  // MSG_TURN
  Turn_Dir dir;
  uint32_t distance_m;
  uint8_t rb_exit;          // roundabouts only
  int16_t rb_degrees;
  char text[64];            // MSG_TURN road name, MSG_ARRIVED destination, MSG_REROUTE detail
  // MSG_TRIP / MSG_CRUISE
  char clock[8];            // arrival time (TRIP) or local time (CRUISE)
  float km;                 // km left (TRIP) or trip km (ARRIVED)
  float km_total;           // TRIP: the route's total
  uint32_t elapsed_s;       // TRIP: seconds since the start
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
  float pose[3];                            // x, y (metres), heading (degrees)
};

void BLE_Link_Init(void);
bool BLE_Link_Connected(void);   // also true while test lines are being injected
// For testing from the Mac (serial "msg <line>"): handle a line as if the phone had sent it.
// While test lines are being injected, lines from the phone are ignored (until "test off").
void BLE_Link_Inject(const String &line);
void BLE_Link_End_Test(void);
// Takes the oldest waiting message; returns false when there is none
bool BLE_Link_Take_Message(Nav_Message *msg);
// Sends a line to the phone app (if it's listening)
void BLE_Link_Notify(const char *line);
