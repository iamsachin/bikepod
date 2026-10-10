/*****************************************************************************
  | File        :   Phone_Screens.cpp
  | help        :   Incoming call and now playing (see Phone_Screens.h).
                    The pod's fonts are Latin only: song titles in other scripts show what can
                    be drawn (the phone's app writes Latin for the navigation screens, but these
                    come straight from iOS).
******************************************************************************/
#include "Phone_Screens.h"
#include <Arduino.h>
#include <stdio.h>
#include "Phone_Notify.h"
#include "Pages.h"
#include "Clock.h"
#include "Map_Layer.h"
#include "Settings.h"
#include "BLE_Link.h"
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>

LV_FONT_DECLARE(font_title_44);
LV_FONT_DECLARE(font_text_32);
LV_FONT_DECLARE(font_small_26);
LV_FONT_DECLARE(font_media_40);
LV_FONT_DECLARE(font_playing_28);

#define COLOR_BLUE      lv_color_hex(0x3D7BF7)
#define COLOR_GREY      lv_color_hex(0xA8A8A8)
#define COLOR_DIM       lv_color_hex(0x6A6A6A)

#define ICON_CALL       "\xEE\x82\xB0"      // U+E0B0
#define ICON_PAUSE      "\xEE\x80\xB4"      // U+E034
#define ICON_PLAY       "\xEE\x80\xB7"      // U+E037
#define ICON_NEXT       "\xEE\x81\x84"      // U+E044
#define ICON_PREVIOUS   "\xEE\x81\x85"      // U+E045
#define ICON_PLAYING    "\xEE\x86\xB8"      // U+E1B8 sound bars

static lv_obj_t *call_root, *call_name;
static lv_obj_t *music_root, *music_top, *music_title, *music_artist, *music_none;
static lv_obj_t *music_row, *music_elapsed, *music_bar, *music_fill, *music_left;
static lv_obj_t *music_controls, *music_icon;
static bool music_visible = false;
static uint32_t shown_version = 0xFFFFFFFF;
static int speed_kmh = -1;

static lv_obj_t *Box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, h);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return box;
}

static lv_obj_t *Label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

static void Set_Text(lv_obj_t *label, const char *text)
{
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void Show_If(lv_obj_t *obj, bool show)
{
  if (show) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// "1:14"
static void Format_Time(char *out, size_t size, float seconds)
{
  int s = (int)LV_MAX(seconds, 0.0f);
  snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

static void Control_Cb(lv_event_t *e)
{
  Phone_Notify_Media_Command((Media_Command)(intptr_t)lv_event_get_user_data(e));
}

// A round button with an icon; returns the icon label. The middle one is filled blue.
static lv_obj_t *Control(lv_obj_t *row, const char *icon, bool main, Media_Command command)
{
  lv_coord_t size = main ? 76 : 64;
  lv_obj_t *btn = Box(row, size, size);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, main ? COLOR_BLUE : lv_color_hex(0x1E1E1E), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(btn, main ? lv_color_hex(0x2C5CC0) : lv_color_hex(0x333333), LV_STATE_PRESSED);
  lv_obj_add_event_cb(btn, Control_Cb, LV_EVENT_CLICKED, (void *)(intptr_t)command);
  lv_obj_t *label = Label(btn, &font_media_40, lv_color_white());
  lv_label_set_text(label, icon);
  lv_obj_center(label);
  return label;
}

/*********************************** Album art ***********************************
 * The phone looks the cover up from the song's title and artist (iOS doesn't share artwork)
 * and sends it as 120x120 RGB565 in base64 parts. Drawn full screen behind the page, darkened so the
 * text stays readable and fading to black towards the round screen's edge.
 **********************************************************************************/
#define ART_SIZE        120
#define ART_PART        360                          // raw bytes per ART line
#define ART_PARTS       (ART_SIZE * ART_SIZE * 2 / ART_PART)
#define ART_SHOWN       480                          // the whole screen (4x)

static uint8_t *art_raw;                             // PSRAM, filled by the Bluetooth task
static uint8_t art_have[(ART_PARTS + 7) / 8];
static volatile uint16_t art_id = 0;                 // the song we want a cover for
static volatile uint16_t art_complete = 0;           // the song whose cover has fully arrived
static uint16_t art_drawn = 0;
static lv_obj_t *art_canvas;

void Album_Art_Line(const char *line)
{
  // ART|id|part|parts|base64
  unsigned id, part, parts;
  if (sscanf(line, "ART|%u|%u|%u|", &id, &part, &parts) != 3 || !art_raw) return;
  if (id != art_id || parts != ART_PARTS || part >= ART_PARTS) return;
  const char *b64 = line;
  for (int i = 0; i < 4 && b64; i++) { b64 = strchr(b64, '|'); if (b64) b64++; }
  if (!b64) return;
  if (part == 0) memset(art_have, 0, sizeof(art_have));
  size_t out_len = 0;
  if (mbedtls_base64_decode(art_raw + part * ART_PART, ART_PART, &out_len, (const unsigned char *)b64, strlen(b64)) != 0) return;
  art_have[part / 8] |= 1 << (part % 8);
  for (int i = 0; i < ART_PARTS; i++) if (!(art_have[i / 8] & (1 << (i % 8)))) return;
  art_complete = id;
}

static inline uint16_t Art_Px(int x, int y)
{
  x = LV_CLAMP(0, x, ART_SIZE - 1);
  y = LV_CLAMP(0, y, ART_SIZE - 1);
  const uint8_t *p = art_raw + (y * ART_SIZE + x) * 2;
  return p[0] | p[1] << 8;
}

// Enlarged with smoothing, darkened, and faded to black outside a circle
// Serial "art": what the album art is waiting for
void Album_Art_Debug(void)
{
  int have = 0;
  for (int i = 0; i < ART_PARTS; i++) if (art_have[i / 8] & (1 << (i % 8))) have++;
  printf("ART want %u, complete %u, drawn %u, parts %d/%d, buffer %s\r\n", art_id, art_complete, art_drawn,
         have, ART_PARTS, art_raw ? "ok" : "MISSING");
}

// Drawn a few rows at a time (whole-number maths, straight into the canvas buffer) so the
// screen keeps updating: the whole cover in one go froze the page for two seconds
static lv_color_t *art_buf;
static lv_timer_t *art_timer;
static int art_row;

static void Art_Draw_Rows(lv_timer_t *timer)
{
  const int half = ART_SHOWN / 2, r_full = 150, r_zero = 238;   // fade from r_full out to r_zero
  const int brightness = 141;                                    // of 256 (0.55)
  uint32_t start = millis();
  while (art_row < ART_SHOWN && millis() - start < 12) {
    int y = art_row++;
    // Source position in eighths of a pixel (4x: each output pixel is a quarter of a source one)
    int v8 = 2 * y - 3, y0 = v8 >> 3, fy = v8 & 7;
    int dy = 2 * y + 1 - ART_SHOWN;                              // in half pixels
    lv_color_t *out = art_buf + y * ART_SHOWN;
    for (int x = 0; x < ART_SHOWN; x++) {
      int dx = 2 * x + 1 - ART_SHOWN;
      int d2 = (dx * dx + dy * dy) >> 2;                         // squared distance in pixels
      int k;
      if (d2 <= r_full * r_full) k = brightness;
      else if (d2 >= r_zero * r_zero) { out[x] = lv_color_black(); continue; }
      else {
        float t = (sqrtf((float)d2) - r_full) / (r_zero - r_full);
        k = (int)(brightness * (1 - t * t * (3 - 2 * t)));
      }
      int u8 = 2 * x - 3, x0 = u8 >> 3, fx = u8 & 7;
      int w00 = (8 - fx) * (8 - fy), w10 = fx * (8 - fy), w01 = (8 - fx) * fy, w11 = fx * fy;   // sum 64
      uint16_t a = Art_Px(x0, y0), b = Art_Px(x0 + 1, y0), cc = Art_Px(x0, y0 + 1), d = Art_Px(x0 + 1, y0 + 1);
      int r = w00 * (a >> 11) + w10 * (b >> 11) + w01 * (cc >> 11) + w11 * (d >> 11);
      int g = w00 * ((a >> 5) & 0x3F) + w10 * ((b >> 5) & 0x3F) + w01 * ((cc >> 5) & 0x3F) + w11 * ((d >> 5) & 0x3F);
      int bl = w00 * (a & 0x1F) + w10 * (b & 0x1F) + w01 * (cc & 0x1F) + w11 * (d & 0x1F);
      // 5/6/5 bits x64 weights x k/256 -> 8 bits per channel
      out[x] = lv_color_make((r * k) >> 11, (g * k) >> 12, (bl * k) >> 11);
    }
  }
  if (art_row >= ART_SHOWN) {
    lv_timer_del(art_timer);
    art_timer = NULL;
    lv_obj_clear_flag(art_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(art_canvas);
  }
}

static void Art_Draw(void)
{
  art_row = 0;
  if (!art_timer) art_timer = lv_timer_create(Art_Draw_Rows, 5, NULL);
}

static void Art_Create(lv_obj_t *parent)
{
  art_raw = (uint8_t *)heap_caps_malloc(ART_SIZE * ART_SIZE * 2, MALLOC_CAP_SPIRAM);
  art_buf = (lv_color_t *)heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(ART_SHOWN, ART_SHOWN), MALLOC_CAP_SPIRAM);
  void *buf = art_buf;
  art_canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(art_canvas, buf, ART_SHOWN, ART_SHOWN, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(art_canvas, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(art_canvas, LV_OBJ_FLAG_HIDDEN);
}

// The song changed: ask the app for its cover
static void Art_Request(const Phone_State &s)
{
  art_id = art_id % 60000 + 1;
  if (art_timer) { lv_timer_del(art_timer); art_timer = NULL; }   // an older cover half drawn
  lv_obj_add_flag(art_canvas, LV_OBJ_FLAG_HIDDEN);
  char line[160];
  snprintf(line, sizeof(line), "NOWPLAYING|%u|%s|%s", art_id, s.title, s.artist);
  BLE_Link_Notify(line);
  printf("ART: asking for %u (%s)\r\n", art_id, s.title);
}

/********************************* Incoming call *********************************/
static void Call_Create(void)
{
  call_root = Box(lv_layer_top(), LV_PCT(100), LV_PCT(100));   // over every page and panel
  lv_obj_set_style_bg_color(call_root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(call_root, LV_OPA_COVER, 0);

  // Blue phone in a soft halo
  for (int i = 0; i < 2; i++) {
    lv_obj_t *halo = Box(call_root, i ? 168 : 130, i ? 168 : 130);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(halo, COLOR_BLUE, 0);
    lv_obj_set_style_border_width(halo, 2, 0);
    lv_obj_set_style_border_opa(halo, i ? LV_OPA_20 : LV_OPA_40, 0);
    lv_obj_align(halo, LV_ALIGN_CENTER, 0, -50);
  }
  lv_obj_t *disc = Box(call_root, 96, 96);
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(disc, COLOR_BLUE, 0);
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
  lv_obj_align(disc, LV_ALIGN_CENTER, 0, -50);
  lv_obj_t *icon = Label(disc, &font_media_40, lv_color_white());
  lv_label_set_text(icon, ICON_CALL);
  lv_obj_center(icon);

  call_name = Label(call_root, &font_title_44, lv_color_white());
  lv_obj_set_width(call_name, 400);
  lv_label_set_long_mode(call_name, LV_LABEL_LONG_DOT);
  lv_obj_align(call_name, LV_ALIGN_CENTER, 0, 64);

  lv_obj_t *caption = Label(call_root, &font_small_26, COLOR_DIM);
  lv_obj_set_style_text_letter_space(caption, 3, 0);
  lv_label_set_text(caption, "INCOMING CALL");
  lv_obj_align(caption, LV_ALIGN_CENTER, 0, 112);

  lv_obj_add_flag(call_root, LV_OBJ_FLAG_HIDDEN);
}

bool Call_Screen_Showing(void)
{
  return !lv_obj_has_flag(call_root, LV_OBJ_FLAG_HIDDEN);
}

/********************************** Now playing **********************************/
static void Music_Create(lv_obj_t *parent)
{
  music_root = Box(parent, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(music_root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(music_root, LV_OPA_COVER, 0);
  Art_Create(music_root);                           // first: behind everything else

  music_top = Label(music_root, &font_small_26, COLOR_GREY);
  lv_obj_align(music_top, LV_ALIGN_TOP_MID, 0, 92);

  // Title (up to two lines) and artist as one block, centred between the top line and the controls
  lv_obj_t *middle = Box(music_root, 390, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(middle, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(middle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(middle, 6, 0);
  lv_obj_align(middle, LV_ALIGN_CENTER, 0, -42);
  music_title = Label(middle, &font_title_44, lv_color_white());
  lv_obj_set_width(music_title, 380);
  lv_label_set_long_mode(music_title, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_max_height(music_title, 2 * font_title_44.line_height, 0);   // two lines at most
  music_artist = Label(middle, &font_text_32, COLOR_GREY);
  lv_obj_set_width(music_artist, 360);
  lv_label_set_long_mode(music_artist, LV_LABEL_LONG_DOT);

  // 1:14 ━━━━━── -2:52
  music_row = Box(music_root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(music_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(music_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(music_row, 12, 0);
  lv_obj_align(music_row, LV_ALIGN_TOP_MID, 0, 296);
  music_elapsed = Label(music_row, &font_small_26, COLOR_GREY);
  music_bar = Box(music_row, 150, 4);
  lv_obj_set_style_radius(music_bar, 2, 0);
  lv_obj_set_style_bg_color(music_bar, lv_color_hex(0x333333), 0);
  lv_obj_set_style_bg_opa(music_bar, LV_OPA_COVER, 0);
  music_fill = Box(music_bar, 0, 4);
  lv_obj_set_style_radius(music_fill, 2, 0);
  lv_obj_set_style_bg_color(music_fill, COLOR_BLUE, 0);
  lv_obj_set_style_bg_opa(music_fill, LV_OPA_COVER, 0);
  music_left = Label(music_row, &font_small_26, COLOR_DIM);

  // ⏮  ⏯  ⏭  (big targets for gloves)
  music_controls = Box(music_root, 280, 76);
  lv_obj_set_flex_flow(music_controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(music_controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(music_controls, LV_ALIGN_TOP_MID, 0, 338);
  Control(music_controls, ICON_PREVIOUS, false, MEDIA_PREVIOUS);
  music_icon = Control(music_controls, ICON_PAUSE, true, MEDIA_TOGGLE);
  Control(music_controls, ICON_NEXT, false, MEDIA_NEXT);

  music_none = Label(music_root, &font_text_32, COLOR_DIM);
  lv_label_set_text(music_none, "Nothing playing");
  lv_obj_center(music_none);

  Pages_Add_Dots(music_root, 3);
  lv_obj_add_flag(music_root, LV_OBJ_FLAG_HIDDEN);
}

void Music_Set_Speed(float metres_per_second)
{
  speed_kmh = (int)lroundf(LV_MAX(metres_per_second, 0.0f) * 3.6f);
}

static void Music_Refresh(const Phone_State &s)
{
  // Top line: time · speed
  char top[40], clock[12];
  Clock_Text(clock, sizeof(clock));
  if (clock[0] && speed_kmh >= 0) snprintf(top, sizeof(top), "%s  \xC2\xB7  %d km/h", clock, speed_kmh);
  else if (clock[0]) snprintf(top, sizeof(top), "%s", clock);
  else top[0] = 0;
  Set_Text(music_top, top);

  bool have = s.title[0];
  Show_If(music_none, !have);
  Show_If(music_title, have);
  Show_If(music_artist, have);
  Show_If(music_row, have);
  Show_If(music_controls, have);
  if (!have) return;
  // Film songs often come as "Tum Hi Ho (From \"Aashiqui 2\") - Unplugged Version": keep the name
  char title[64];
  strlcpy(title, s.title, sizeof(title));
  for (const char *cut : { " (From", " (from", " [From", " - ", " (feat", " (Feat" }) {
    char *p = strstr(title, cut);
    if (p && p != title) *p = 0;
  }
  Set_Text(music_title, title);
  Set_Text(music_artist, s.artist);

  float elapsed = s.elapsed_s + s.rate * (millis() - s.elapsed_ms) / 1000.0f;
  if (s.duration_s > 0) elapsed = LV_MIN(elapsed, s.duration_s);
  char text[12];
  Format_Time(text, sizeof(text), elapsed);
  Set_Text(music_elapsed, text);
  if (s.duration_s > 0) {
    text[0] = '-';
    Format_Time(text + 1, sizeof(text) - 1, s.duration_s - elapsed);
  } else {
    text[0] = 0;
  }
  Set_Text(music_left, text);
  lv_coord_t fill = s.duration_s > 0 ? (lv_coord_t)(150 * LV_CLAMP(0.0f, elapsed / s.duration_s, 1.0f)) : 0;
  if (lv_obj_get_width(music_fill) != fill) lv_obj_set_width(music_fill, fill);
  Set_Text(music_icon, s.media_playing ? ICON_PAUSE : ICON_PLAY);
}

void Music_Screen_Show(bool show)
{
  music_visible = show;
  Show_If(music_root, show);
  if (show) shown_version = 0xFFFFFFFF;             // refresh now
}

/************************************ Updates ************************************/
// While music plays: small sound bars at the top of the other pages; tap it for Now playing
static lv_obj_t *note_btn;

static void Note_Click(lv_event_t *e)
{
  Pages_Show(3);
}

static void Note_Create(void)
{
  note_btn = lv_label_create(lv_layer_top());   // created before the call overlay, so it sits under it
  lv_obj_set_style_text_font(note_btn, &font_playing_28, 0);
  lv_obj_set_style_text_color(note_btn, COLOR_BLUE, 0);
  lv_label_set_text(note_btn, ICON_PLAYING);
  lv_obj_align(note_btn, LV_ALIGN_TOP_MID, 0, 56);   // between the rim and each page's first line
  lv_obj_add_flag(note_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(note_btn, 20);       // easier to hit with gloves
  lv_obj_add_event_cb(note_btn, Note_Click, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(note_btn, LV_OBJ_FLAG_HIDDEN);
}

static void Note_Update(const Phone_State &s)
{
  bool show = s.media_playing && s.title[0] && Pages_Current() != 3 && !Settings_Panel_Open() && !s.call_ringing;
  if (show == lv_obj_has_flag(note_btn, LV_OBJ_FLAG_HIDDEN)) Show_If(note_btn, show);
}

static void Tick(lv_timer_t *timer)
{
  Phone_State s;
  Phone_Notify_Get(&s);

  // The phone says it's ringing first and gives the caller's name a moment later: wait for the
  // name (up to 1.5 s) rather than flash "Unknown caller"
  static uint32_t ring_ms = 0;
  if (!s.call_ringing) ring_ms = 0;
  else if (!ring_ms) ring_ms = millis() | 1;
  bool ringing = s.call_ringing && (s.caller[0] || millis() - ring_ms > 1500);
  if (ringing != Call_Screen_Showing()) {
    Show_If(call_root, ringing);
    // The map underneath stops redrawing while the call covers it
    Map_Layer_Set_Visible(Pages_Nav_Visible());
  }
  if (ringing) Set_Text(call_name, s.caller[0] ? s.caller : "Unknown caller");
  Note_Update(s);

  // Album art: ask for it when the song changes (again after 15 s if nothing came), draw it when complete
  static char last_song[112] = "";
  static uint32_t asked_ms = 0;
  static uint8_t asks = 0;
  char song[112];
  snprintf(song, sizeof(song), "%s|%s", s.title, s.artist);
  if (s.title[0] && strcmp(song, last_song) != 0) {
    strlcpy(last_song, song, sizeof(last_song));
    Art_Request(s);
    asked_ms = millis();
    asks = 1;
  } else if (s.title[0] && art_complete != art_id && asks < 3 && millis() - asked_ms > 15000) {
    art_id--;                                        // same song: ask again under the same id
    Art_Request(s);
    asked_ms = millis();
    asks++;
  }
  if (art_complete == art_id && art_drawn != art_id) {
    art_drawn = art_id;
    Art_Draw();
  }
  if (!s.title[0]) lv_obj_add_flag(art_canvas, LV_OBJ_FLAG_HIDDEN);

  // The music page: the clock and position tick every second, the rest on changes
  if (music_visible) {
    static uint32_t last_refresh = 0;
    if (s.version != shown_version || millis() - last_refresh >= 1000) {
      shown_version = s.version;
      last_refresh = millis();
      Music_Refresh(s);
    }
  }
}

void Phone_Screens_Create(lv_obj_t *parent)
{
  Music_Create(parent);
  Note_Create();
  Call_Create();
  lv_timer_create(Tick, 200, NULL);
}
