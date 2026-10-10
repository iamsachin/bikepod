/*****************************************************************************
  | File        :   Phone_Notify.cpp
  | help        :   ANCS + AMS client (see Phone_Notify.h).
                    Formats from Apple's specifications:
                    ANCS Notification Source: EventID, EventFlags, CategoryID, CategoryCount, UID (4)
                         Control Point (write): 0 = Get Notification Attributes, UID, attribute
                         IDs (Title = 1, followed by a 2 byte max length)
                         Data Source: 0, UID, then attribute ID, length (2), value
                    AMS  Entity Update (write to subscribe): EntityID, attribute IDs
                         notifications: EntityID, AttributeID, Flags, value
                         Player (0): PlaybackInfo (1) = "state,rate,elapsed"; Track (2): Artist (0), Title (2)
                    Bluetooth callbacks can't make requests that wait for an answer (they'd wait
                    on themselves), so a small task does the connecting and the caller lookups.
******************************************************************************/
#include "Phone_Notify.h"
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLESecurity.h>
#include "Screenshot.h"

#define LOG(...) do { if (!screenshot_sending) printf(__VA_ARGS__); } while (0)

static BLEUUID ANCS_SERVICE("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static BLEUUID ANCS_NOTIFICATION_SOURCE("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static BLEUUID ANCS_CONTROL_POINT("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static BLEUUID ANCS_DATA_SOURCE("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
static BLEUUID AMS_SERVICE("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static BLEUUID AMS_ENTITY_UPDATE("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static BLEUUID AMS_REMOTE_COMMAND("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
#define REQUEST_COMMAND 0xFFFFFF00u           // queue items at or above this are media commands

#define CATEGORY_INCOMING_CALL  1
#define EVENT_ADDED             0
#define EVENT_REMOVED           2
#define FLAG_PRE_EXISTING       0x04

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static Phone_State state;

static esp_bd_addr_t peer;
static volatile bool linked = false;          // the phone is connected
static volatile bool secured = false;         // ...and the link is encrypted (paired)
static TaskHandle_t worker = NULL;
static QueueHandle_t requests;                // notification UIDs to look up (callers)
static BLERemoteCharacteristic *control_point = NULL;
static BLERemoteCharacteristic *remote_command = NULL;
static volatile uint32_t call_uid = 0xFFFFFFFF;

static void Changed(void (*edit)(Phone_State &))
{
  portENTER_CRITICAL(&state_lock);
  edit(state);
  state.version++;
  portEXIT_CRITICAL(&state_lock);
}

void Phone_Notify_Get(Phone_State *out)
{
  portENTER_CRITICAL(&state_lock);
  *out = state;
  portEXIT_CRITICAL(&state_lock);
}

static void Copy_Value(char *dst, size_t size, const uint8_t *src, size_t len)
{
  size_t n = len < size - 1 ? len : size - 1;
  while (n > 0 && n < len && (src[n] & 0xC0) == 0x80) n--;   // don't cut a UTF-8 character
  memcpy(dst, src, n);
  dst[n] = 0;
}

/******************************** Notifications (Bluetooth task) ********************************/
static void Notification_Source(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
  if (len < 8) return;
  uint8_t event = data[0], flags = data[1], category = data[2];
  uint32_t uid = data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24;
  if (category != CATEGORY_INCOMING_CALL || (flags & FLAG_PRE_EXISTING)) return;
  if (event == EVENT_ADDED) {
    call_uid = uid;
    Changed([](Phone_State &s) { s.call_ringing = true; s.caller[0] = 0; });
    xQueueSend(requests, &uid, 0);             // the worker asks for the caller's name
    LOG("PHONE: incoming call (uid %lu)\r\n", (unsigned long)uid);
  } else if (event == EVENT_REMOVED && uid == call_uid) {
    call_uid = 0xFFFFFFFF;
    Changed([](Phone_State &s) { s.call_ringing = false; });
    LOG("PHONE: call over\r\n");
  }
}

static void Data_Source(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
  // 0, UID (4), then attribute ID, length (2), value
  if (len < 8 || data[0] != 0) return;
  uint32_t uid = data[1] | data[2] << 8 | data[3] << 16 | (uint32_t)data[4] << 24;
  if (uid != call_uid || data[5] != 1) return;
  size_t value_len = data[6] | data[7] << 8;
  if (value_len > len - 8) value_len = len - 8;
  static char name[48];
  Copy_Value(name, sizeof(name), data + 8, value_len);
  Changed([](Phone_State &s) { strlcpy(s.caller, name, sizeof(s.caller)); });
  LOG("PHONE: caller \"%s\"\r\n", name);
}

static void Entity_Update(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
  if (len < 3) return;
  uint8_t entity = data[0], attribute = data[1];
  static char value[64];
  Copy_Value(value, sizeof(value), data + 3, len - 3);
  if (entity == 2 && attribute == 2) {
    Changed([](Phone_State &s) { strlcpy(s.title, value, sizeof(s.title)); });
  } else if (entity == 2 && attribute == 0) {
    Changed([](Phone_State &s) { strlcpy(s.artist, value, sizeof(s.artist)); });
  } else if (entity == 2 && attribute == 3) {
    static float duration;
    duration = atof(value);
    Changed([](Phone_State &s) { s.duration_s = duration; });
  } else if (entity == 0 && attribute == 1) {
    // "state,rate,elapsed": state 0 paused, 1 playing (2/3 seeking)
    static float rate, elapsed;
    static bool playing;
    playing = value[0] == '1';
    const char *c1 = strchr(value, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    rate = c1 ? atof(c1 + 1) : 0;
    elapsed = c2 ? atof(c2 + 1) : 0;
    Changed([](Phone_State &s) {
      s.media_playing = playing;
      s.rate = playing ? rate : 0;
      s.elapsed_s = elapsed;
      s.elapsed_ms = millis();
    });
  } else {
    return;
  }
  LOG("PHONE: media %u/%u \"%s\"\r\n", entity, attribute, value);
}

/*********************************** Security ***********************************/
class Security_Callbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }       // "Just Works": the phone asks Pair / Cancel
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    secured = result.success;
    LOG("PHONE: pairing %s\r\n", result.success ? "done" : "failed");
  }
};

void Phone_Notify_Init(void)
{
  requests = xQueueCreate(4, sizeof(uint32_t));
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(new Security_Callbacks());
  BLESecurity *security = new BLESecurity();
  security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);      // bond: remember the pairing
  security->setCapability(ESP_IO_CAP_NONE);                      // no screen PIN
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
}

/************************************ Worker ************************************/
static void Worker(void *)
{
  // Wait for the link to be encrypted (the first time, the rider has to tap Pair)
  for (int i = 0; i < 600 && linked && !secured; i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (linked && secured) {
    static BLEClient *client = NULL;
    if (!client) client = BLEDevice::createClient();
    if (client->connect(BLEAddress(peer))) {           // the existing link, as a GATT client
      BLERemoteService *ancs = client->getService(ANCS_SERVICE);
      if (ancs) {
        BLERemoteCharacteristic *ds = ancs->getCharacteristic(ANCS_DATA_SOURCE);
        BLERemoteCharacteristic *ns = ancs->getCharacteristic(ANCS_NOTIFICATION_SOURCE);
        control_point = ancs->getCharacteristic(ANCS_CONTROL_POINT);
        if (ds) ds->registerForNotify(Data_Source);
        if (ns) ns->registerForNotify(Notification_Source);
        LOG("PHONE: notifications %s\r\n", ns && ds && control_point ? "ready" : "incomplete");
      } else {
        LOG("PHONE: no notification service (not allowed on the phone?)\r\n");
      }
      BLERemoteService *ams = client->getService(AMS_SERVICE);
      BLERemoteCharacteristic *eu = ams ? ams->getCharacteristic(AMS_ENTITY_UPDATE) : NULL;
      remote_command = ams ? ams->getCharacteristic(AMS_REMOTE_COMMAND) : NULL;
      if (eu) {
        eu->registerForNotify(Entity_Update);
        uint8_t track[] = { 2, 0, 2, 3 };               // Track: Artist, Title, Duration
        uint8_t player[] = { 0, 1 };                    // Player: PlaybackInfo
        eu->writeValue(track, sizeof(track), true);
        eu->writeValue(player, sizeof(player), true);
        LOG("PHONE: media ready\r\n");
      }
    } else {
      LOG("PHONE: could not open the phone's services\r\n");
    }
  }
  // Look up callers' names while connected
  uint32_t uid;
  while (linked) {
    if (xQueueReceive(requests, &uid, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    if (uid >= REQUEST_COMMAND) {
      uint8_t command = (uint8_t)(uid - REQUEST_COMMAND);
      if (remote_command) remote_command->writeValue(&command, 1, true);
      LOG("PHONE: media command %u\r\n", command);
    } else if (control_point) {
      uint8_t cmd[] = { 0, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24), 1, 40, 0 };
      control_point->writeValue(cmd, sizeof(cmd), true);   // Title, up to 40 bytes
    }
  }
  control_point = NULL;
  remote_command = NULL;
  worker = NULL;
  vTaskDelete(NULL);
}

void Phone_Notify_Connected(const esp_bd_addr_t remote)
{
  memcpy(peer, remote, sizeof(peer));
  linked = true;
  secured = false;
  esp_ble_set_encryption((uint8_t *)peer, ESP_BLE_SEC_ENCRYPT);  // pair (or re-use the pairing)
  if (!worker) xTaskCreatePinnedToCore(Worker, "phone", 6144, NULL, 1, &worker, 0);
}

void Phone_Notify_Disconnected(void)
{
  linked = false;
  secured = false;
  call_uid = 0xFFFFFFFF;
  Changed([](Phone_State &s) { s.call_ringing = false; s.media_playing = false; });
}

void Phone_Notify_Media_Command(Media_Command command)
{
  uint32_t item = REQUEST_COMMAND + command;
  if (requests) xQueueSend(requests, &item, 0);
}

/************************************ Testing ************************************/
void Phone_Notify_Fake_Call(const char *name)
{
  static char copy[48];
  strlcpy(copy, name, sizeof(copy));
  Changed([](Phone_State &s) { s.call_ringing = copy[0]; strlcpy(s.caller, copy, sizeof(s.caller)); });
}

void Phone_Notify_Fake_Music(const char *title, const char *artist, bool playing)
{
  static char t[64], a[48];
  static bool p;
  strlcpy(t, title, sizeof(t));
  strlcpy(a, artist, sizeof(a));
  p = playing;
  Changed([](Phone_State &s) {
    strlcpy(s.title, t, sizeof(s.title));
    strlcpy(s.artist, a, sizeof(s.artist));
    s.media_playing = p;
    s.duration_s = 246;
    s.elapsed_s = 74;
    s.rate = p ? 1 : 0;
    s.elapsed_ms = millis();
  });
}
