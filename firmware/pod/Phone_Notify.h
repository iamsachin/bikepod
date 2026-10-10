#pragma once
#include <Arduino.h>
#include <esp_gatts_api.h>
#include <esp_gap_ble_api.h>

// Calls and music from the iPhone itself, over the same Bluetooth link the BikePod app uses:
//   ANCS (Apple Notification Center Service): incoming calls and the caller's name
//   AMS  (Apple Media Service): the song playing, its artist, playing or paused
// iOS only shares these with a paired accessory: the first time, the phone asks to pair and
// to allow notifications. The pod reads them as a GATT client of the phone.

struct Phone_State {
  uint32_t version;         // goes up whenever anything below changes
  bool call_ringing;
  char caller[48];          // UTF-8, as iOS gives it (contact name or number)
  bool media_playing;
  char title[64];
  char artist[48];
  float duration_s;         // the track's length (0 = unknown)
  float elapsed_s;          // position when last reported...
  float rate;               // ...and playback rate (1 = normal), at...
  uint32_t elapsed_ms;      // ...this time (millis)
};

// Hooks from BLE_Link (Bluetooth task)
void Phone_Notify_Init(void);                                  // security settings; call before advertising
void Phone_Notify_Connected(const esp_bd_addr_t remote);
void Phone_Notify_Disconnected(void);

// Music controls (AMS remote commands)
enum Media_Command { MEDIA_TOGGLE = 2, MEDIA_NEXT = 3, MEDIA_PREVIOUS = 4 };
void Phone_Notify_Media_Command(Media_Command command);

// Testing without a phone (serial "fakecall" / "fakemusic")
void Phone_Notify_Fake_Call(const char *name);               // "" = call over
void Phone_Notify_Fake_Music(const char *title, const char *artist, bool playing);

// Main loop: the latest state (copied)
void Phone_Notify_Get(Phone_State *out);
