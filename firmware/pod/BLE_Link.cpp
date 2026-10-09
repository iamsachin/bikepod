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

// Log lines would cut into screenshot data on the same serial port
#define LOG(...) do { if (!screenshot_sending) printf(__VA_ARGS__); } while (0)

#define QUEUE_SIZE 8

static portMUX_TYPE msg_lock = portMUX_INITIALIZER_UNLOCKED;
static Nav_Message queue[QUEUE_SIZE];
static uint8_t queue_head = 0;     // next to take
static uint8_t queue_count = 0;
static volatile bool connected = false;
static volatile bool testing = false;    // lines come from the Mac over serial, not the phone

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

// "x,y;x,y;..." -> points
static uint8_t Parse_Path(const String &list, int16_t (*pts)[2])
{
  uint8_t count = 0;
  int pos = 0;
  while (pos < (int)list.length() && count < NAV_PATH_MAX_POINTS) {
    int semi = list.indexOf(';', pos);
    if (semi < 0) semi = list.length();
    int comma = list.indexOf(',', pos);
    if (comma > pos && comma < semi) {
      pts[count][0] = list.substring(pos, comma).toInt();
      pts[count][1] = list.substring(comma + 1, semi).toInt();
      count++;
    }
    pos = semi + 1;
  }
  return count;
}

// "x,y,a;x,y,a;..." -> stubs
static uint8_t Parse_Stubs(const String &list, int16_t (*stubs)[3])
{
  uint8_t count = 0;
  int pos = 0;
  while (pos < (int)list.length() && count < NAV_MAX_STUBS) {
    int semi = list.indexOf(';', pos);
    if (semi < 0) semi = list.length();
    int c1 = list.indexOf(',', pos);
    int c2 = c1 < 0 ? -1 : list.indexOf(',', c1 + 1);
    if (c1 > pos && c2 > c1 && c2 < semi) {
      stubs[count][0] = list.substring(pos, c1).toInt();
      stubs[count][1] = list.substring(c1 + 1, c2).toInt();
      stubs[count][2] = list.substring(c2 + 1, semi).toInt();
      count++;
    }
    pos = semi + 1;
  }
  return count;
}

static bool Parse_Message(const String &text, Nav_Message *msg)
{
  String kind = Field(text, 0);
  memset(msg, 0, sizeof(*msg));

  if (kind == "TRIP") {
    msg->kind = MSG_TRIP;
    Field(text, 1).toCharArray(msg->clock, sizeof(msg->clock));
    msg->km = Field(text, 2).toFloat();
    return true;
  }
  if (kind == "PATH") {
    msg->kind = MSG_PATH;
    msg->path_count = Parse_Path(Rest(text, 1), msg->path);
    return true;
  }
  if (kind == "STUB") {
    msg->kind = MSG_STUB;
    msg->stub_count = Parse_Stubs(Rest(text, 1), msg->stubs);
    return true;
  }
  if (kind == "REROUTE") {
    msg->kind = MSG_REROUTE;
    return true;
  }
  if (kind == "ARRIVED") {
    msg->kind = MSG_ARRIVED;
    msg->km = Field(text, 1).toFloat();
    msg->trip_min = Field(text, 2).toInt();
    Rest(text, 3).toCharArray(msg->text, sizeof(msg->text));
    return true;
  }
  if (kind == "CRUISE") {
    msg->kind = MSG_CRUISE;
    msg->speed_kmh = Field(text, 1).toInt();
    Field(text, 2).toCharArray(msg->clock, sizeof(msg->clock));
    return true;
  }

  // Turn: L|400|MG Rd
  if (kind.length() != 1) return false;
  switch (toupper(kind[0])) {
    case 'L': msg->dir = TURN_LEFT; break;
    case 'R': msg->dir = TURN_RIGHT; break;
    case 'S': msg->dir = TURN_STRAIGHT; break;
    default:  return false;
  }
  msg->kind = MSG_TURN;
  msg->distance_m = Field(text, 1).toInt();
  Rest(text, 2).toCharArray(msg->text, sizeof(msg->text));
  return true;
}

static void Handle_Line(const String &text)
{
  Nav_Message msg;
  if (!Parse_Message(text, &msg)) {
    LOG("BLE: ignored \"%s\"\r\n", text.c_str());
    return;
  }
  if (msg.kind != MSG_PATH && msg.kind != MSG_STUB) LOG("BLE: %s\r\n", text.c_str());   // paths are too chatty to log
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
  void onDisconnect(BLEServer *server) override {
    connected = false;
    LOG("BLE: phone disconnected, advertising again\r\n");
    BLEDevice::startAdvertising();
  }
};

void BLE_Link_Init(void)
{
  BLEDevice::init(POD_BLE_NAME);
  BLEDevice::setMTU(517);                  // let the phone send a whole PATH line in one packet
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
