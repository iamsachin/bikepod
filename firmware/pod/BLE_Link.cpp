/*****************************************************************************
  | File        :   BLE_Link.cpp
  | help        :   BLE GATT server: one service with one writable "Nav" characteristic.
                    BLE callbacks run in the Bluetooth task, and LVGL must only be touched
                    from the main loop, so messages are handed over through a small locked queue
                    (several lines can arrive between two screen updates).
******************************************************************************/
#include "BLE_Link.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2901.h>
#include "Screenshot.h"
#include "Map_Layer.h"
#include "Phone_Notify.h"
#include <BLE2902.h>
#include "Phone_Screens.h"

// Log lines would cut into screenshot data on the same serial port
#define LOG(...) do { if (!screenshot_sending) printf(__VA_ARGS__); } while (0)

#define QUEUE_SIZE 8

static portMUX_TYPE msg_lock = portMUX_INITIALIZER_UNLOCKED;
static Nav_Message queue[QUEUE_SIZE];
static uint8_t queue_head = 0;     // next to take
static uint8_t queue_count = 0;
static volatile bool connected = false;
static volatile bool testing = false;    // lines come from the Mac over serial, not the phone
static BLECharacteristic *events = NULL; // pod -> phone

// Returns the text between the n-th and (n+1)-th '|' (n = 0 is the first field)
static String Field(const String &text, int n)
{
  int start = 0;
  for (int i = 0; i < n; i++) {
    start = text.indexOf('|', start) + 1;
    if (start == 0) return "";
  }
  int end = text.indexOf('|', start);
  return end < 0 ? text.substring(start) : text.substring(start, end);
}

// Everything after the n-th '|' (for free text that is always the last field)
static String Rest(const String &text, int n)
{
  int start = 0;
  for (int i = 0; i < n; i++) {
    start = text.indexOf('|', start) + 1;
    if (start == 0) return "";
  }
  return text.substring(start);
}

// Everything after the n-th '|', without copying
static const char *Rest_Ptr(const String &text, int n)
{
  const char *p = text.c_str();
  for (int i = 0; i < n; i++) {
    p = strchr(p, '|');
    if (!p) return "";
    p++;
  }
  return p;
}

// "x,y;x,y;..." -> points. Parsed in place: a String per number made thousands of small
// allocations per map area, in the Bluetooth task's (scarce) internal RAM
static uint8_t Parse_Points(const char *s, int16_t (*pts)[2], uint8_t max)
{
  uint8_t count = 0;
  while (*s && count < max) {
    char *end;
    long x = strtol(s, &end, 10);
    if (end == s || *end != ',') break;
    s = end + 1;
    long y = strtol(s, &end, 10);
    if (end == s) break;
    pts[count][0] = (int16_t)x;
    pts[count][1] = (int16_t)y;
    count++;
    if (*end != ';') break;
    s = end + 1;
  }
  return count;
}

// Copies text, cut at a whole UTF-8 character if it doesn't fit (a cut through a character
// shows as a broken glyph, e.g. in Kannada road names)
static void Copy_Text(const char *src, char *dst, size_t size)
{
  size_t n = strlen(src);
  if (n >= size) {
    n = size - 1;
    while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;   // back to the start of a character
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

// Map lines come in bursts of ~100, too many for the message queue: they go straight into
// the map's own buffer. Returns true if the line was a map line.
static bool Handle_Map_Line(const String &text)
{
  if (text.startsWith("MAP|")) {
    static int16_t pts[NAV_PATH_MAX_POINTS * 2][2];
    uint8_t count = Parse_Points(Rest_Ptr(text, 3), pts, NAV_PATH_MAX_POINTS * 2);
    Map_Data_Add_Road(Field(text, 1).toInt(), Field(text, 2).toInt(), pts, count);
    return true;
  }
  if (text.startsWith("FUEL|")) {
    static int16_t pts[16][2];
    uint8_t count = Parse_Points(Rest_Ptr(text, 2), pts, 16);
    Map_Data_Add_Fuel(Field(text, 1).toInt(), pts, count);
    return true;
  }
  if (text.startsWith("MAPBEGIN|")) {
    Map_Data_Begin(Field(text, 1).toInt(), Field(text, 2).toDouble(), Field(text, 3).toDouble());
    return true;
  }
  if (text.startsWith("MAPEND|")) {
    LOG("BLE: %s\r\n", text.c_str());
    Map_Data_End(Field(text, 1).toInt(), Field(text, 2).toInt());
    return true;
  }
  return false;
}

static bool Parse_Message(const String &text, Nav_Message *msg)
{
  String kind = Field(text, 0);
  memset(msg, 0, sizeof(*msg));

  if (kind == "TIME") {
    msg->kind = MSG_TIME;
    msg->elapsed_s = Field(text, 1).toInt() * 3600 + Field(text, 2).toInt() * 60 + Field(text, 3).toInt();
    return true;
  }
  if (kind == "LIMIT") {
    msg->kind = MSG_LIMIT;
    msg->speed_kmh = Field(text, 1).toInt();
    return true;
  }
  if (kind == "THEN") {
    msg->kind = MSG_THEN;
    String dir = Field(text, 1);
    msg->text[0] = dir == "0" ? 0 : dir[0];
    msg->text[1] = 0;
    msg->distance_m = Field(text, 2).toInt();
    return true;
  }
  if (kind == "PHONEBAT") {
    msg->kind = MSG_PHONEBAT;
    msg->distance_m = Field(text, 1).toInt();
    msg->speed_kmh = Field(text, 2).toInt();
    return true;
  }
  if (kind == "FUELNEXT") {
    msg->kind = MSG_FUELNEXT;
    msg->distance_m = Field(text, 1).toInt();
    return true;
  }
  if (kind == "RAIN") {
    msg->kind = MSG_RAIN;
    msg->rb_degrees = Field(text, 1).toInt();
    return true;
  }
  if (kind == "NIGHT") {
    msg->kind = MSG_NIGHT;
    msg->distance_m = Field(text, 1).toInt();
    return true;
  }
  if (kind == "BUMP") {
    msg->kind = MSG_BUMP;
    msg->distance_m = Field(text, 1).toInt();
    // BUMP|85|area|x|y: also where it is on the map (pose[2] = 1 when given)
    bool spot = Field(text, 4).length() > 0;
    msg->area_id = spot ? Field(text, 2).toInt() : 0;
    msg->pose[0] = spot ? Field(text, 3).toFloat() : 0;
    msg->pose[1] = spot ? Field(text, 4).toFloat() : 0;
    msg->pose[2] = spot ? 1 : 0;
    return true;
  }
  if (kind == "SPD") {
    msg->kind = MSG_SPEED;
    msg->speed_mps = Field(text, 1).toInt() / 10.0f;
    return true;
  }
  if (kind == "POSE") {
    msg->kind = MSG_POSE;
    msg->area_id = Field(text, 1).toInt();
    String p = Rest(text, 2);
    int c1 = p.indexOf(','), c2 = p.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) return false;
    msg->pose[0] = p.substring(0, c1).toInt() / 10.0f;      // sent in tenths
    msg->pose[1] = p.substring(c1 + 1, c2).toInt() / 10.0f;
    msg->pose[2] = p.substring(c2 + 1).toInt() / 10.0f;
    return true;
  }

  if (kind == "TRIP") {
    msg->kind = MSG_TRIP;
    Field(text, 1).toCharArray(msg->clock, sizeof(msg->clock));
    msg->km = Field(text, 2).toFloat();
    msg->km_total = Field(text, 3).toFloat();
    msg->elapsed_s = Field(text, 4).toInt();
    return true;
  }
  if (kind == "RPATH") {
    msg->kind = MSG_ROUTE;
    msg->area_id = Field(text, 1).toInt();
    msg->path_count = Parse_Points(Rest_Ptr(text, 2), msg->path, NAV_PATH_MAX_POINTS);
    return true;
  }
  if (kind == "REROUTE") {
    msg->kind = MSG_REROUTE;
    Copy_Text(Rest_Ptr(text, 1), msg->text, sizeof(msg->text));   // why, e.g. "No internet" (may be empty)
    return true;
  }
  if (kind == "ARRIVED") {
    msg->kind = MSG_ARRIVED;
    msg->km = Field(text, 1).toFloat();
    msg->trip_min = Field(text, 2).toInt();
    msg->avg_kmh = Field(text, 3).toFloat();
    msg->top_kmh = Field(text, 4).toFloat();
    Copy_Text(Rest_Ptr(text, 5), msg->text, sizeof(msg->text));
    return true;
  }
  if (kind == "CRUISE") {
    msg->kind = MSG_CRUISE;
    msg->speed_kmh = Field(text, 1).toInt();
    Field(text, 2).toCharArray(msg->clock, sizeof(msg->clock));
    return true;
  }

  // Roundabout: O|250|4|271|Hosur Rd
  if (kind == "O") {
    msg->kind = MSG_TURN;
    msg->dir = TURN_ROUNDABOUT;
    msg->distance_m = Field(text, 1).toInt();
    msg->rb_exit = (uint8_t)Field(text, 2).toInt();
    msg->rb_degrees = (int16_t)Field(text, 3).toInt();
    Copy_Text(Rest_Ptr(text, 4), msg->text, sizeof(msg->text));
    return true;
  }

  // Turn: L|400|MG Rd (L, R, S, or U / V for a U-turn to the right / left)
  if (kind.length() != 1) return false;
  switch (toupper(kind[0])) {
    case 'L': msg->dir = TURN_LEFT; break;
    case 'R': msg->dir = TURN_RIGHT; break;
    case 'S': msg->dir = TURN_STRAIGHT; break;
    case 'U': msg->dir = TURN_UTURN_RIGHT; break;   // U-turn swinging right (keep-left countries)
    case 'V': msg->dir = TURN_UTURN_LEFT; break;
    default:  return false;
  }
  msg->kind = MSG_TURN;
  msg->distance_m = Field(text, 1).toInt();
  Copy_Text(Rest_Ptr(text, 2), msg->text, sizeof(msg->text));
  return true;
}

static void Handle_Line(const String &text)
{
  if (Handle_Map_Line(text)) return;
  if (text.startsWith("ART|")) { Album_Art_Line(text.c_str()); return; }
  Nav_Message msg;
  if (!Parse_Message(text, &msg)) {
    LOG("BLE: ignored \"%s\"\r\n", text.c_str());
    return;
  }
  if (msg.kind != MSG_ROUTE && msg.kind != MSG_POSE && msg.kind != MSG_SPEED && msg.kind != MSG_BUMP && msg.kind != MSG_TIME && msg.kind != MSG_NIGHT && msg.kind != MSG_PHONEBAT) LOG("BLE: %s\r\n", text.c_str());   // paths are too chatty to log
  portENTER_CRITICAL(&msg_lock);
  if (queue_count == QUEUE_SIZE) {          // full: drop the oldest, the newest matters most
    queue_head = (queue_head + 1) % QUEUE_SIZE;
    queue_count--;
  }
  queue[(queue_head + queue_count) % QUEUE_SIZE] = msg;
  queue_count++;
  portEXIT_CRITICAL(&msg_lock);
}

class Nav_Write_Callbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    if (!testing) Handle_Line(characteristic->getValue());
  }
};

void BLE_Link_Inject(const String &line)
{
  testing = true;
  Handle_Line(line);
}

void BLE_Link_End_Test(void)
{
  testing = false;
}

class Server_Callbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    connected = true;
    LOG("BLE: phone connected\r\n");
  }
  void onConnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override {
    Phone_Notify_Connected(param->connect.remote_bda);   // calls and music from the phone
    // Keep advertising: iOS connects by itself once paired (calls, music), and the BikePod app,
    // scanning in the background, can only find the pod while it advertises
    BLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer *server) override {
    connected = false;
    Phone_Notify_Disconnected();
    LOG("BLE: phone disconnected, advertising again\r\n");
    BLEDevice::startAdvertising();
  }
};

void BLE_Link_Init(void)
{
  BLEDevice::init(POD_BLE_NAME);
  BLEDevice::setMTU(517);                  // let the phone send a whole PATH line in one packet
  Phone_Notify_Init();                     // pairing settings (for calls and music)
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new Server_Callbacks());

  BLEService *service = server->createService(POD_SERVICE_UUID);
  BLECharacteristic *nav = service->createCharacteristic(
    POD_NAV_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  nav->setCallbacks(new Nav_Write_Callbacks());

  BLE2901 *label = new BLE2901();          // shows "Nav" as the characteristic name in nRF Connect
  label->setDescription("Nav");
  nav->addDescriptor(label);

  events = service->createCharacteristic(POD_EVENT_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  events->addDescriptor(new BLE2902());    // lets the app subscribe

  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(POD_SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  LOG("BLE: advertising as \"%s\"\r\n", POD_BLE_NAME);
}

bool BLE_Link_Connected(void)
{
  return connected || testing;
}

void BLE_Link_Notify(const char *line)
{
  if (!events || !connected) return;
  events->setValue((uint8_t *)line, strlen(line));
  events->notify();
}

bool BLE_Link_Take_Message(Nav_Message *msg)
{
  bool got = false;
  portENTER_CRITICAL(&msg_lock);
  if (queue_count > 0) {
    *msg = queue[queue_head];
    queue_head = (queue_head + 1) % QUEUE_SIZE;
    queue_count--;
    got = true;
  }
  portEXIT_CRITICAL(&msg_lock);
  return got;
}
