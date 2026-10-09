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

static bool Parse_Message(const String &text, Nav_Message *msg)
{
  int bar1 = text.indexOf('|');
  int bar2 = text.indexOf('|', bar1 + 1);
  if (bar1 < 1 || bar2 < 0) return false;

  switch (toupper(text[0])) {
    case 'L': msg->dir = TURN_LEFT; break;
    case 'R': msg->dir = TURN_RIGHT; break;
    case 'S': msg->dir = TURN_STRAIGHT; break;
    default:  return false;
  }
  msg->distance_m = text.substring(bar1 + 1, bar2).toInt();
  text.substring(bar2 + 1).toCharArray(msg->road, sizeof(msg->road));
  return true;
}

class Nav_Write_Callbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String text = characteristic->getValue();
    Nav_Message msg;
    if (!Parse_Message(text, &msg)) {
      printf("BLE: ignored \"%s\" (expected like R|400|MG Road)\r\n", text.c_str());
      return;
    }
    printf("BLE: dir=%d distance=%lu road=\"%s\"\r\n", msg.dir, (unsigned long)msg.distance_m, msg.road);
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
