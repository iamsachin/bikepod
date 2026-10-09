/*****************************************************************************
  | File        :   BLE_Link.cpp
  | help        :   BLE GATT server: one service with one writable "Nav" characteristic.
                    BLE callbacks run in the Bluetooth task, and LVGL must only be touched
                    from the main loop, so messages are handed over through a locked buffer.
******************************************************************************/
#include "BLE_Link.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2901.h>

static portMUX_TYPE msg_lock = portMUX_INITIALIZER_UNLOCKED;
static Nav_Message pending_msg;
static bool msg_waiting = false;
static volatile bool connected = false;

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

static bool Parse_Message(const String &text, Nav_Message *msg)
{
  String kind = Field(text, 0);
  memset(msg, 0, sizeof(*msg));

  if (kind == "REROUTE") {
    msg->kind = MSG_REROUTE;
    return true;
  }
  if (kind == "ARRIVED") {
    msg->kind = MSG_ARRIVED;
    msg->trip_km = Field(text, 1).toFloat();
    msg->trip_min = Field(text, 2).toInt();
    return true;
  }
  if (kind == "CRUISE") {
    msg->kind = MSG_CRUISE;
    msg->speed_kmh = Field(text, 1).toInt();
    Field(text, 2).toCharArray(msg->clock, sizeof(msg->clock));
    return true;
  }

  // Turn: L|400|MG Rd (the road name may itself contain '|', so take everything after the 2nd one)
  if (kind.length() != 1 || text.indexOf('|', 2) < 0) return false;
  switch (toupper(kind[0])) {
    case 'L': msg->dir = TURN_LEFT; break;
    case 'R': msg->dir = TURN_RIGHT; break;
    case 'S': msg->dir = TURN_STRAIGHT; break;
    default:  return false;
  }
  msg->kind = MSG_TURN;
  msg->distance_m = Field(text, 1).toInt();
  text.substring(text.indexOf('|', 2) + 1).toCharArray(msg->road, sizeof(msg->road));
  return true;
}

class Nav_Write_Callbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String text = characteristic->getValue();
    Nav_Message msg;
    if (!Parse_Message(text, &msg)) {
      printf("BLE: ignored \"%s\"\r\n", text.c_str());
      return;
    }
    printf("BLE: %s\r\n", text.c_str());
    portENTER_CRITICAL(&msg_lock);
    pending_msg = msg;
    msg_waiting = true;
    portEXIT_CRITICAL(&msg_lock);
  }
};

class Server_Callbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    connected = true;
    printf("BLE: phone connected\r\n");
  }
  void onDisconnect(BLEServer *server) override {
    connected = false;
    printf("BLE: phone disconnected, advertising again\r\n");
    BLEDevice::startAdvertising();
  }
};

void BLE_Link_Init(void)
{
  BLEDevice::init(POD_BLE_NAME);
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
  printf("BLE: advertising as \"%s\"\r\n", POD_BLE_NAME);
}

bool BLE_Link_Connected(void)
{
  return connected;
}

bool BLE_Link_Take_Message(Nav_Message *msg)
{
  bool got = false;
  portENTER_CRITICAL(&msg_lock);
  if (msg_waiting) {
    *msg = pending_msg;
    msg_waiting = false;
    got = true;
  }
  portEXIT_CRITICAL(&msg_lock);
  return got;
}
