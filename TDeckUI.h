// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Meshtastic-inspired user interface for the LilyGO T-Deck.
//
// The UI is organised around a home screen (app launcher) with a top status
// bar that is ALWAYS visible on every screen:
//
//   * time (RNS system clock, or uptime when no real-time clock is present)
//   * GPS status indicator (no GPS is fitted to the T-Deck, but the
//     indicator is driven by software state so future GPS support plugs
//     straight in via tdeck_ui_set_gps_fix())
//   * Reticulum status (transport on/off, radio state, peer count)
//   * battery percentage
//
// Four applications are provided, navigated with the trackball (scroll,
// press = select, left = back, long press = home) and the keyboard
// (printable keys type, Enter = confirm, Backspace = delete, Esc = back):
//
//   1. Messages   - send/receive point-to-point text messages to other nodes
//                   running this UI over the "tdeck_ui.messages" aspect
//   2. Contacts   - browse Reticulum contacts (from announces and the
//                   persisted known-destinations store), favourite them,
//                   give them a readable alias, save and export them
//   3. Reticulum  - ping and traceroute contacts, list direct contacts,
//                   view/edit the local identity
//   4. Settings   - device settings persisted to /settings.yaml on the
//                   T-Deck's microSD card
//
// Persistence lives on the microSD card in /settings.yaml. The SD slot shares
// the display/LoRa SPI bus (SCLK 40, MISO 38, MOSI 41) with its own
// chip-select on GPIO 39, so it is only mounted once the shared bus is up.
//
// Frames are composed in an off-screen RGB565 buffer in PSRAM and pushed to
// the ST7789 in one full-frame SPI burst, eliminating the flicker caused by
// incremental clears and redraws. The panel only repaints when an input
// event arrives or the periodic status refresh fires.

#pragma once

#include <Arduino.h>

#if BOARD_MODEL == BOARD_TDECK

#include <Adafruit_GFX.h>
#include <SD.h>
#ifdef ESP_PLATFORM
  #include <esp_heap_caps.h>
#endif

#include "TDeckKeyboard.h"
// The trackball implementation (global singleton + ISR pointer) is defined
// only here - TDeckUI.h is included from exactly one translation unit
// (RNode_Firmware.ino), so the symbols are defined exactly once.
#define TDECK_TRACKBALL_IMPLEMENTATION
#include "TDeckTrackball.h"

// The T-Deck's ST7789 panel is mounted in landscape; the drawing surface is
// 320x240 (matching display.setRotation(3) configured in Display.h).
#define TDECK_SCREEN_W 320
#define TDECK_SCREEN_H 240

// ---- Layout -----------------------------------------------------------------
#define TDECK_SBAR_H   24   // top status bar, always visible
#define TDECK_FOOT_H   18   // bottom hint bar
#define TDECK_LIST_X    6   // left padding for list content
#define TDECK_LIST_W   (TDECK_SCREEN_W - 2*TDECK_LIST_X)

// ---- RNS app aspects used by the UI -----------------------------------------
#define TDECK_UI_RNS_APP         "tdeck_ui"
#define TDECK_UI_RNS_ASPECT_MSGS "messages"
#define TDECK_UI_RNS_ASPECT_PING "ping"

// Simple line-based payload framing (robust against arbitrary message text).
#define TDECK_FRAME_MSG  "!mrmsg!"    // !mrmsg!<sender_hash>!<text>
#define TDECK_FRAME_PING "!mrping!"   // !mrping!<seq>!<sent_at>!<sender_pubkey>
#define TDECK_FRAME_PONG "!mrpong!"   // !mrpong!<seq>!<sent_at>

// ---- Storage / data limits ---------------------------------------------------
#define TDECK_SETTINGS_PATH "/settings.yaml"
#define TDECK_EXPORT_PATH   "/contacts.yaml"
#define TDECK_MAX_CONTACTS  32
#define TDECK_ALIAS_MAX     32
#define TDECK_APP_DATA_MAX  40
#define TDECK_MAX_MESSAGES  24
#define TDECK_MSG_MAX_LEN   220
#define TDECK_PING_TIMEOUT_MS 8000

// PSRAM-backed RGB565 off-screen frame buffer. Composing each frame here and
// then pushing it to the panel in a single burst avoids the visible flicker
// a direct-draw UI produces when it fillScreen()s and repaints. 320*240*2 =
// 150 KB, comfortably within the T-Deck's 8 MB PSRAM (falls back to internal
// heap if the PSRAM allocation fails).
class TDeckCanvas : public Adafruit_GFX {
  public:
    TDeckCanvas(uint16_t w, uint16_t h) : Adafruit_GFX(w, h), _w(w), _h(h), _buf(NULL) {}
    ~TDeckCanvas() {
        if (_buf) {
          #ifdef ESP_PLATFORM
            heap_caps_free(_buf);
          #else
            free(_buf);
          #endif
        }
    }

    void allocate() {
        if (_buf) return;
        size_t bytes = (size_t)_w * _h * 2;
      #ifdef ESP_PLATFORM
        _buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!_buf) _buf = (uint16_t *)malloc(bytes);
      #else
        _buf = (uint16_t *)malloc(bytes);
      #endif
    }

    bool valid() const { return _buf != NULL; }
    uint16_t *buf() const { return _buf; }

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (!_buf || x < 0 || y < 0 || x >= (int16_t)_w || y >= (int16_t)_h) return;
        _buf[(int32_t)y * _w + x] = color;
    }

  private:
    uint16_t _w;
    uint16_t _h;
    uint16_t *_buf;
};

static TDeckCanvas tdeck_canvas(TDECK_SCREEN_W, TDECK_SCREEN_H);
static TDeckKeyboard tdeck_kb;

// ---- Colours ----------------------------------------------------------------
#define TDECK_COL_BG     ST77XX_BLACK
#define TDECK_COL_FG     ST77XX_WHITE
#define TDECK_COL_ACCENT ST77XX_CYAN
#define TDECK_COL_SEL    ST77XX_BLUE
#define TDECK_COL_OK     ST77XX_GREEN
#define TDECK_COL_WARN   ST77XX_YELLOW
#define TDECK_COL_ERR    ST77XX_RED
#define TDECK_COL_SBAR   0x18E3   // dark navy status-bar background
#define TDECK_COL_DIM    0x7BEF   // grey for secondary text

// Symbols referenced from the firmware (radio_online, battery_*) are defined
// by Config.h/Display.h, both already included before this header in
// RNode_Firmware.ino. `reticulum` is defined later in that same translation
// unit, so it is declared extern here.
#ifdef HAS_RNS
extern RNS::Reticulum reticulum;
#endif

// ---- Contact store -----------------------------------------------------------
typedef struct {
  char  hash[40];              // identity hash (32 hex chars)
  char  dest_hash[40];         // last announced destination hash
  char  pubkey[140];           // 64-byte public key (128 hex chars)
  char  alias[TDECK_ALIAS_MAX+1];
  char  app_data[TDECK_APP_DATA_MAX+1];
  bool  favorite;
  bool  has_key;
  bool  persisted;             // present in settings.yaml
  double last_seen;            // unix time of last announce
} tdeck_contact_t;

static tdeck_contact_t tdeck_contacts[TDECK_MAX_CONTACTS];
static uint8_t tdeck_contact_count = 0;

// ---- Message store -----------------------------------------------------------
typedef struct {
  char  peer[40];              // peer identity hash (32 hex chars)
  char  text[TDECK_MSG_MAX_LEN+1];
  bool  inbound;
  double time;
} tdeck_message_t;
static tdeck_message_t tdeck_messages[TDECK_MAX_MESSAGES];
static uint8_t tdeck_msg_count = 0;

// ---- Device settings (persisted to /settings.yaml) ---------------------------
static char     tdeck_set_device_name[64]    = "microReticulum Node";
static uint8_t  tdeck_set_brightness         = 128;
static uint16_t tdeck_set_blank_timeout      = 30;   // seconds; 0 = never
static bool     tdeck_set_gps_enabled        = true;
static uint16_t tdeck_set_announce_interval  = 0;    // minutes; 0 = off
static bool     tdeck_set_confirm_send       = false;
static bool     tdeck_settings_dirty         = false;

// ---- Runtime state -----------------------------------------------------------
static bool     tdeck_ui_ready       = false;
static bool     tdeck_dirty          = true;
static bool     tdeck_sd_ready       = false;
static bool     tdeck_gps_fix        = false;  // updated via tdeck_ui_set_gps_fix()
static bool     tdeck_gps_present    = false;  // T-Deck has no GPS fitted
static uint32_t tdeck_last_tick      = 0;
static uint32_t tdeck_tick_ms        = 1000;

// Screens
enum {
  SCREEN_HOME = 0,
  SCREEN_MESSAGES,
  SCREEN_CONTACTS,
  SCREEN_RETICULUM,
  SCREEN_SETTINGS,
};

// Per-app views
enum { MSG_INBOX = 0, MSG_THREAD, MSG_COMPOSE, MSG_PICK };
enum { CT_LIST = 0, CT_DETAIL, CT_ALIAS };
enum { RET_MENU = 0, RET_PING_PICK, RET_PING_RUN, RET_TRACE_PICK,
       RET_TRACE_RUN, RET_DIRECT, RET_IDENTITY, RET_NAME_EDIT };
enum { SET_LIST = 0, SET_EDIT };

static uint8_t tdeck_screen = SCREEN_HOME;
static uint8_t tdeck_sub    = 0;
static uint8_t tdeck_cursor = 0;
static uint8_t tdeck_scroll = 0;

// Back-stack (small fixed depth; long-press also returns to home)
#define TDECK_NAV_DEPTH 8
static uint8_t tdeck_nav_screen[TDECK_NAV_DEPTH];
static uint8_t tdeck_nav_sub[TDECK_NAV_DEPTH];
static uint8_t tdeck_nav_depth = 0;

// Text input state
static char    tdeck_input_buf[TDECK_MSG_MAX_LEN+1];
static uint8_t tdeck_input_len  = 0;
static uint8_t tdeck_input_ctx  = 0;   // 0 = compose, 1 = alias, 2 = node name
static char    tdeck_input_peer[40];   // peer for compose context

// Contact detail scratch
static uint8_t tdeck_contact_detail_idx = 0;
static char    tdeck_thread_peer[40];   // conversation shown in thread view
static char    tdeck_toast[40];
static uint32_t tdeck_toast_until = 0;

// Settings app scratch
static uint8_t  tdeck_set_edit_idx = 0;   // row being edited in SET_EDIT

// ---- RNS integration state ---------------------------------------------------
#ifdef HAS_RNS
static RNS::Destination tdeck_rns_msg_dest({RNS::Type::NONE});
static RNS::Destination tdeck_rns_ping_dest({RNS::Type::NONE});
static RNS::HAnnounceHandler tdeck_rns_ann_handler;
static bool tdeck_rns_ready = false;
static double tdeck_last_announce_at = 0.0;
#endif

// ping/trace UI state (declared unconditionally: the draw code reads them even
// when RNS is compiled out, so the screens still render "RNS unavailable").
static bool     tdeck_ping_inflight = false;
static uint32_t tdeck_ping_seq      = 0;
static double   tdeck_ping_sent_at  = 0.0;
static char     tdeck_ping_peer[40];
static char     tdeck_ping_status[48];
static uint32_t tdeck_ping_deadline = 0;

static bool  tdeck_trace_inflight = false;
static char  tdeck_trace_peer[40];
static char  tdeck_trace_out[400];

// ---- Forward declarations ----------------------------------------------------
static bool tdeck_sd_init();
static bool tdeck_settings_load();
static bool tdeck_settings_save();
static bool tdeck_contacts_export();
static void tdeck_settings_apply_contact(const char* hash, const char* alias,
                                         bool fav, const char* pubkey,
                                         const char* app_data);
static void tdeck_apply_brightness();
static void tdeck_toast_set(const char* fmt, ...);

static bool     tdeck_hex_decode(const char* hex, uint8_t* out, size_t n);
static void     tdeck_ui_time_str(char* out, size_t n);
static tdeck_contact_t* tdeck_contact_find(const char* hash_hex);
static void     tdeck_contact_upsert(const char* hash_hex, const char* dest_hex,
                                     const char* pubkey_hex, const char* app_data,
                                     double last_seen);
static const char* tdeck_contact_label(const tdeck_contact_t* c, char* buf, size_t n);
static void     tdeck_msg_add(const char* peer, const char* text, bool inbound);

static void     tdeck_input_start(uint8_t ctx, const char* initial);
static void     tdeck_input_add(char c);
static void     tdeck_input_del();
static void     tdeck_input_apply();
static bool     tdeck_in_text_input();

static void     tdeck_rns_setup();
static void     tdeck_rns_sync_contacts();
static void     tdeck_rns_announce_ui();
static bool     tdeck_rns_send_payload(const tdeck_contact_t* c, const char* aspect,
                                       const char* payload);
static void     tdeck_rns_ping_contact(uint8_t idx);
static void     tdeck_rns_ping_poll();
static void     tdeck_rns_trace_contact(uint8_t idx);

static void     tdeck_ui_draw_status_bar(TDeckCanvas& c);
static void     tdeck_ui_draw_footer(TDeckCanvas& c, const char* hint);
static void     tdeck_ui_draw_row(TDeckCanvas& c, uint8_t y, const char* text,
                                  bool selected);
static void     tdeck_ui_draw_home();
static void     tdeck_ui_draw_messages();
static void     tdeck_ui_draw_contacts();
static void     tdeck_ui_draw_reticulum();
static void     tdeck_ui_draw_reticulum_rest();
static void     tdeck_ui_draw_settings();
static void     tdeck_ui_draw_text_input(const char* title, const char* buf);
static void     tdeck_ui_draw();

static void     tdeck_ui_go_home();
static void     tdeck_ui_nav_push(uint8_t screen, uint8_t sub);
static void     tdeck_ui_nav_back();
static void     tdeck_ui_nav_select();
static void     tdeck_ui_nav_up();
static void     tdeck_ui_nav_down();

static void     tdeck_ui_handle_key(char key, uint8_t state);
static void     tdeck_ui_handle_trackball(tb_event_t event);

// ---- Small helpers -----------------------------------------------------------

static bool tdeck_hex_decode(const char* hex, uint8_t* out, size_t n) {
    if (!hex) return false;
    size_t hlen = strlen(hex);
    if (hlen != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        char hi = hex[i*2], lo = hex[i*2+1];
        uint8_t hv, lv;
        if (hi >= '0' && hi <= '9') hv = hi - '0';
        else if (hi >= 'a' && hi <= 'f') hv = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') hv = hi - 'A' + 10;
        else return false;
        if (lo >= '0' && lo <= '9') lv = lo - '0';
        else if (lo >= 'a' && lo <= 'f') lv = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') lv = lo - 'A' + 10;
        else return false;
        out[i] = (uint8_t)((hv << 4) | lv);
    }
    return true;
}

// Clock for the status bar: real unix time from RNS when available, otherwise
// uptime in the T+hh:mm format.
static void tdeck_ui_time_str(char* out, size_t n) {
  #ifdef HAS_RNS
    double now = RNS::Utilities::OS::time();
    if (now > 1600000000.0) {
        time_t t = (time_t)now;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        snprintf(out, n, "%02u:%02u", tmv.tm_hour, tmv.tm_min);
        return;
    }
  #endif
    uint32_t up = millis() / 1000;
    snprintf(out, n, "T+%02u:%02u", (up / 3600) % 100, (up / 60) % 60);
}

static tdeck_contact_t* tdeck_contact_find(const char* hash_hex) {
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        if (strcmp(tdeck_contacts[i].hash, hash_hex) == 0) return &tdeck_contacts[i];
    }
    return NULL;
}

// Insert or refresh a contact. Missing contacts are appended up to the store
// limit; existing entries keep their user alias/favourite state.
static void tdeck_contact_upsert(const char* hash_hex, const char* dest_hex,
                                 const char* pubkey_hex, const char* app_data,
                                 double last_seen) {
    if (!hash_hex || !hash_hex[0]) return;
    tdeck_contact_t* c = tdeck_contact_find(hash_hex);
    if (!c) {
        if (tdeck_contact_count >= TDECK_MAX_CONTACTS) return;
        c = &tdeck_contacts[tdeck_contact_count++];
        memset(c, 0, sizeof(*c));
        strncpy(c->hash, hash_hex, sizeof(c->hash)-1);
    }
    if (dest_hex && dest_hex[0]) strncpy(c->dest_hash, dest_hex, sizeof(c->dest_hash)-1);
    if (pubkey_hex && pubkey_hex[0]) {
        strncpy(c->pubkey, pubkey_hex, sizeof(c->pubkey)-1);
        c->has_key = true;
    }
    if (app_data && app_data[0]) strncpy(c->app_data, app_data, sizeof(c->app_data)-1);
    if (last_seen > c->last_seen) c->last_seen = last_seen;
}

// Best display name for a contact: alias, else announced name, else hash.
static const char* tdeck_contact_label(const tdeck_contact_t* c, char* buf, size_t n) {
    if (!c) { snprintf(buf, n, "(none)"); return buf; }
    if (c->alias[0]) { snprintf(buf, n, "%s", c->alias); return buf; }
    if (c->app_data[0]) { snprintf(buf, n, "%s", c->app_data); return buf; }
    snprintf(buf, n, "%s", c->hash);
    return buf;
}

static void tdeck_msg_add(const char* peer, const char* text, bool inbound) {
    if (!peer || !text) return;
    if (tdeck_msg_count >= TDECK_MAX_MESSAGES) {
        // Drop the oldest message to make room.
        memmove(&tdeck_messages[0], &tdeck_messages[1],
                sizeof(tdeck_message_t) * (TDECK_MAX_MESSAGES - 1));
        tdeck_msg_count--;
    }
    tdeck_message_t* m = &tdeck_messages[tdeck_msg_count++];
    memset(m, 0, sizeof(*m));
    strncpy(m->peer, peer, sizeof(m->peer)-1);
    strncpy(m->text, text, sizeof(m->text)-1);
    m->inbound = inbound;
  #ifdef HAS_RNS
    m->time = RNS::Utilities::OS::time();
  #else
    m->time = 0;
  #endif
}

static void tdeck_toast_set(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tdeck_toast, sizeof(tdeck_toast), fmt, ap);
    va_end(ap);
    tdeck_toast_until = millis() + 2200;
}

// ---- Text input --------------------------------------------------------------

static bool tdeck_in_text_input() {
    return (tdeck_screen == SCREEN_MESSAGES && tdeck_sub == MSG_COMPOSE) ||
           (tdeck_screen == SCREEN_CONTACTS && tdeck_sub == CT_ALIAS) ||
           (tdeck_screen == SCREEN_RETICULUM && tdeck_sub == RET_NAME_EDIT);
}

static void tdeck_input_start(uint8_t ctx, const char* initial) {
    tdeck_input_ctx = ctx;
    tdeck_input_len = 0;
    tdeck_input_buf[0] = '\0';
    if (initial) {
        strncpy(tdeck_input_buf, initial, sizeof(tdeck_input_buf)-1);
        tdeck_input_buf[sizeof(tdeck_input_buf)-1] = '\0';
        tdeck_input_len = strlen(tdeck_input_buf);
    }
}

static void tdeck_input_add(char c) {
    if (c < 0x20 || c > 0x7E) return;
    if (tdeck_input_len >= TDECK_MSG_MAX_LEN) return;
    tdeck_input_buf[tdeck_input_len++] = c;
    tdeck_input_buf[tdeck_input_len] = '\0';
    tdeck_dirty = true;
}

static void tdeck_input_del() {
    if (tdeck_input_len == 0) return;
    tdeck_input_buf[--tdeck_input_len] = '\0';
    tdeck_dirty = true;
}

// Commit the text entry buffer according to the context it was started in.
static void tdeck_input_apply() {
    if (tdeck_input_ctx == 0) {
        // Compose: send the message to tdeck_input_peer's contact.
        if (tdeck_input_len == 0) {
            tdeck_ui_nav_back();
            return;
        }
        tdeck_contact_t* c = tdeck_contact_find(tdeck_input_peer);
      #ifdef HAS_RNS
        if (!c || !c->has_key) {
            tdeck_toast_set("No key for contact");
            return;
        }
        char frame[TDECK_MSG_MAX_LEN + 64];
        snprintf(frame, sizeof(frame), "%s%s!%s", TDECK_FRAME_MSG, c->hash, tdeck_input_buf);
        if (tdeck_rns_send_payload(c, TDECK_UI_RNS_ASPECT_MSGS, frame)) {
            tdeck_msg_add(c->hash, tdeck_input_buf, false);
            tdeck_toast_set("Message sent");
        } else {
            tdeck_toast_set("Send failed");
            return;
        }
      #else
        (void)c;
        tdeck_toast_set("RNS unavailable");
        return;
      #endif
        tdeck_ui_nav_back();
    } else if (tdeck_input_ctx == 1) {
        // Alias for the contact detail currently viewed.
        tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
        if (c && c->hash[0]) {
            if (tdeck_input_len == 0) {
                c->alias[0] = '\0';
            } else {
                strncpy(c->alias, tdeck_input_buf, TDECK_ALIAS_MAX);
                c->alias[TDECK_ALIAS_MAX] = '\0';
            }
            c->persisted = true;
            tdeck_settings_dirty = true;
            tdeck_toast_set("Alias saved");
        }
        tdeck_ui_nav_back();
    } else if (tdeck_input_ctx == 2) {
        // Node name: used as the announced name and persisted.
        if (tdeck_input_len > 0) {
            strncpy(tdeck_set_device_name, tdeck_input_buf, sizeof(tdeck_set_device_name)-1);
            tdeck_settings_dirty = true;
          #ifdef HAS_RNS
            if (tdeck_rns_ready) tdeck_rns_announce_ui();
          #endif
            tdeck_toast_set("Name updated");
        }
        tdeck_ui_nav_back();
    }
}

// ---- SD card + settings.yaml persistence -------------------------------------

// Mount the T-Deck's microSD card. The SD slot shares the display/LoRa SPI bus
// (SCLK 40, MISO 38, MOSI 41) with its own chip-select on GPIO 39; the shared
// bus is already running at this point (radio + display init precede the UI).
static bool tdeck_sd_init() {
  #ifdef ESP_PLATFORM
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    if (SD.begin(SD_CS, SPI, 4000000)) {
        tdeck_sd_ready = true;
        Serial.println("[TDeck] SD card ready");
        return true;
    }
    Serial.println("[TDeck] SD card not found (settings will not persist)");
  #endif
    return false;
}

static void tdeck_settings_apply_contact(const char* hash, const char* alias,
                                         bool fav, const char* pubkey,
                                         const char* app_data) {
    tdeck_contact_t* c = tdeck_contact_find(hash);
    if (!c) {
        tdeck_contact_upsert(hash, NULL, pubkey, app_data, 0);
        c = tdeck_contact_find(hash);
    }
    if (!c) return;
    if (alias && alias[0]) {
        strncpy(c->alias, alias, TDECK_ALIAS_MAX);
        c->alias[TDECK_ALIAS_MAX] = '\0';
    }
    c->favorite = fav;
    c->persisted = true;
    if (pubkey && pubkey[0]) {
        strncpy(c->pubkey, pubkey, sizeof(c->pubkey)-1);
        c->has_key = true;
    }
    if (app_data && app_data[0]) strncpy(c->app_data, app_data, sizeof(c->app_data)-1);
}

// Strip surrounding quotes from a YAML scalar value (in place at *start).
static void tdeck_yaml_strip_quotes(const char** start) {
    const char* s = *start;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len-1] == '"') || (s[0] == '\'' && s[len-1] == '\''))) {
        char* tmp = (char*)s;
        tmp[len-1] = '\0';
        s++;
    }
    *start = s;
}

static bool tdeck_settings_load() {
    if (!tdeck_sd_ready) return false;
    if (!SD.exists(TDECK_SETTINGS_PATH)) return false;
    File f = SD.open(TDECK_SETTINGS_PATH, FILE_READ);
    if (!f) return false;

    bool in_contacts = false;
    char hash[40] = {0}, alias[TDECK_ALIAS_MAX+1] = {0}, pub[140] = {0};
    char appd[TDECK_APP_DATA_MAX+1] = {0};
    bool fav = false;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();   // strips \r\n and surrounding whitespace
        if (line.length() == 0 || line.startsWith("#")) continue;

        if (line == "contacts:") { in_contacts = true; continue; }

        if (!in_contacts) {
            int ci = line.indexOf(':');
            if (ci <= 0) continue;
            String k = line.substring(0, ci);
            k.trim();
            String v = line.substring(ci + 1);
            v.trim();
            const char* vs = v.c_str();
            if (k == "device_name") {
                strncpy(tdeck_set_device_name, vs, sizeof(tdeck_set_device_name)-1);
            } else if (k == "brightness") {
                tdeck_set_brightness = (uint8_t)constrain(atoi(vs), 0, 255);
            } else if (k == "blank_timeout") {
                tdeck_set_blank_timeout = (uint16_t)constrain(atoi(vs), 0, 3600);
            } else if (k == "gps_enabled") {
                tdeck_set_gps_enabled = (v == "true");
            } else if (k == "announce_interval") {
                tdeck_set_announce_interval = (uint16_t)constrain(atoi(vs), 0, 1440);
            } else if (k == "confirm_send") {
                tdeck_set_confirm_send = (v == "true");
            }
        } else {
            // Contacts section: "- hash:" starts an entry, indented keys follow.
            if (line.startsWith("- hash:")) {
                if (hash[0]) tdeck_settings_apply_contact(hash, alias, fav, pub, appd);
                memset(hash, 0, sizeof(hash));
                memset(alias, 0, sizeof(alias));
                memset(pub, 0, sizeof(pub));
                memset(appd, 0, sizeof(appd));
                fav = false;
                const char* p = line.c_str() + 7;
                tdeck_yaml_strip_quotes(&p);
                strncpy(hash, p, sizeof(hash)-1);
            } else {
                int ci = line.indexOf(':');
                if (ci <= 0) continue;
                String k = line.substring(0, ci);
                k.trim();
                String v = line.substring(ci + 1);
                v.trim();
                const char* vs = v.c_str();
                tdeck_yaml_strip_quotes(&vs);
                if (k == "alias") {
                    strncpy(alias, vs, sizeof(alias)-1);
                } else if (k == "favorite") {
                    fav = (v == "true");
                } else if (k == "public_key") {
                    strncpy(pub, vs, sizeof(pub)-1);
                } else if (k == "app_data") {
                    strncpy(appd, vs, sizeof(appd)-1);
                }
            }
        }
    }
    if (hash[0]) tdeck_settings_apply_contact(hash, alias, fav, pub, appd);
    f.close();
    tdeck_settings_dirty = false;
    return true;
}

static bool tdeck_settings_save() {
    if (!tdeck_sd_ready) return false;
    File f = SD.open(TDECK_SETTINGS_PATH, FILE_WRITE);
    if (!f) return false;

    f.println("# microReticulum T-Deck settings");
    f.printf("device_name: \"%s\"\n", tdeck_set_device_name);
    f.printf("brightness: %u\n", tdeck_set_brightness);
    f.printf("blank_timeout: %u\n", tdeck_set_blank_timeout);
    f.printf("gps_enabled: %s\n", tdeck_set_gps_enabled ? "true" : "false");
    f.printf("announce_interval: %u\n", tdeck_set_announce_interval);
    f.printf("confirm_send: %s\n", tdeck_set_confirm_send ? "true" : "false");
    f.println("contacts:");
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        tdeck_contact_t& c = tdeck_contacts[i];
        if (!c.persisted && !c.favorite && !c.alias[0]) continue;
        f.printf("  - hash: \"%s\"\n", c.hash);
        if (c.alias[0]) f.printf("    alias: \"%s\"\n", c.alias);
        if (c.favorite) f.printf("    favorite: true\n");
        if (c.has_key && c.pubkey[0]) f.printf("    public_key: \"%s\"\n", c.pubkey);
        if (c.app_data[0]) f.printf("    app_data: \"%s\"\n", c.app_data);
        if (c.last_seen > 0) f.printf("    last_seen: %.0f\n", c.last_seen);
    }
    f.close();
    tdeck_settings_dirty = false;
    return true;
}

// Write a human-readable YAML export of all known contacts to the SD card.
static bool tdeck_contacts_export() {
    if (!tdeck_sd_ready) return false;
    File f = SD.open(TDECK_EXPORT_PATH, FILE_WRITE);
    if (!f) return false;

    f.println("# microReticulum T-Deck contact export");
    f.printf("exported: %.0f\n", (double)(millis() / 1000));
    f.printf("count: %u\n", tdeck_contact_count);
    f.println("contacts:");
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        tdeck_contact_t& c = tdeck_contacts[i];
        f.printf("  - hash: \"%s\"\n", c.hash);
        if (c.alias[0]) f.printf("    alias: \"%s\"\n", c.alias);
        if (c.app_data[0]) f.printf("    name: \"%s\"\n", c.app_data);
        if (c.favorite) f.printf("    favorite: true\n");
        if (c.has_key && c.pubkey[0]) f.printf("    public_key: \"%s\"\n", c.pubkey);
        if (c.dest_hash[0]) f.printf("    destination: \"%s\"\n", c.dest_hash);
        if (c.last_seen > 0) f.printf("    last_seen: %.0f\n", c.last_seen);
    }
    f.close();
    return true;
}

// Apply the configured brightness: display backlight on/off plus keyboard
// backlight level, kept in sync with Display.h's contrast globals so the two
// never fight over the panel.
static void tdeck_apply_brightness() {
    uint8_t v = tdeck_set_brightness;
    display_intensity = v;
    display_contrast  = v;
    set_contrast(&display, v);
    tdeck_kb.setBacklight(v);
}

// ---- RNS integration ---------------------------------------------------------

#ifdef HAS_RNS

// Fetches the (idx)th '!'-separated field after `start`. Returns a pointer to
// the first byte following the field (or NULL when the field was the last one),
// and copies the field into `out`.
static const char* tdeck_rns_field(const char* start, uint8_t idx, char* out, size_t n) {
    const char* p = start;
    for (uint8_t i = 0; i < idx; i++) {
        p = strchr(p, '!');
        if (!p) return NULL;
        p++;
    }
    const char* end = strchr(p, '!');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return end ? end + 1 : NULL;
}

// Fired by the Transport for every announce heard on the mesh. Populates the
// contact store with the announced identity (hash + public key + name).
static void tdeck_rns_on_announce(const RNS::Bytes& dst_hash, const RNS::Identity& id,
                                  const RNS::Bytes& app_data) {
    if (dst_hash.size() != RNS::Type::Reticulum::DESTINATION_LENGTH) return;
    char hash[40] = {0}, dest[40] = {0}, pub[140] = {0}, appd[TDECK_APP_DATA_MAX+1] = {0};

    if (id) {
        std::string h = id.hash().toHex();
        strncpy(hash, h.c_str(), sizeof(hash)-1);
        std::string pk = id.get_public_key().toHex();
        strncpy(pub, pk.c_str(), sizeof(pub)-1);
    }
    std::string dh = dst_hash.toHex();
    strncpy(dest, dh.c_str(), sizeof(dest)-1);
    if (app_data.size() > 0) {
        std::string ad = app_data.toString();
        strncpy(appd, ad.c_str(), sizeof(appd)-1);
    }
    tdeck_contact_upsert(hash, dest, pub[0] ? pub : NULL, appd, RNS::Utilities::OS::time());
    tdeck_dirty = true;
}

// Packet callback for the local "tdeck_ui.messages" IN destination.
static void tdeck_rns_on_message(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();
    if (s.rfind(TDECK_FRAME_MSG, 0) != 0) return;
    const char* p = s.c_str() + strlen(TDECK_FRAME_MSG);

    char sender[40];
    p = tdeck_rns_field(p, 0, sender, sizeof(sender));
    if (!p || !sender[0]) return;

    char text[TDECK_MSG_MAX_LEN+1];
    strncpy(text, p, sizeof(text)-1);
    text[sizeof(text)-1] = '\0';

    tdeck_contact_upsert(sender, NULL, NULL, NULL, 0);
    tdeck_msg_add(sender, text, true);
    tdeck_dirty = true;
}

// Packet callback for the local "tdeck_ui.ping" IN destination: answers ping
// requests and collects pong replies for the in-flight ping.
static void tdeck_rns_on_ping(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();

    if (s.rfind(TDECK_FRAME_PING, 0) == 0) {
        char seq[16], sent[24], pub[140];
        const char* p = s.c_str() + strlen(TDECK_FRAME_PING);
        p = tdeck_rns_field(p, 0, seq, sizeof(seq));
        if (!p) return;
        p = tdeck_rns_field(p, 0, sent, sizeof(sent));
        if (!p) return;
        if (!tdeck_rns_field(p, 0, pub, sizeof(pub)) || strlen(pub) != 128) return;

        // Reply to whoever pinged us using the public key carried in the frame.
        uint8_t key[64];
        if (!tdeck_hex_decode(pub, key, 64)) return;
        RNS::Identity rid(false);
        rid.load_public_key(RNS::Bytes(key, 64));
        RNS::Destination out(rid, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                             TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_PING);
        char pong[64];
        snprintf(pong, sizeof(pong), "%s%s!%s", TDECK_FRAME_PONG, seq, sent);
        RNS::Packet(out, RNS::Bytes(pong)).send();
        return;
    }

    if (s.rfind(TDECK_FRAME_PONG, 0) == 0) {
        if (!tdeck_ping_inflight) return;
        char seq[16], sent[24];
        const char* p = s.c_str() + strlen(TDECK_FRAME_PONG);
        p = tdeck_rns_field(p, 0, seq, sizeof(seq));
        if (!p) return;
        tdeck_rns_field(p, 0, sent, sizeof(sent));
        if ((uint32_t)strtoul(seq, NULL, 16) != tdeck_ping_seq) return;

        double sent_at = strtod(sent, NULL);
        double rtt = (RNS::Utilities::OS::time() - sent_at) * 1000.0;
        if (rtt < 0) rtt = 0;
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "PONG in %.0f ms", rtt);
        tdeck_ping_inflight = false;
        tdeck_dirty = true;
        return;
    }
}

class TDeckAnnounceHandler : public RNS::AnnounceHandler {
  public:
    TDeckAnnounceHandler() : RNS::AnnounceHandler() {}
    virtual void received_announce(const RNS::Bytes& destination_hash,
                                   const RNS::Identity& announced_identity,
                                   const RNS::Bytes& app_data) override {
        tdeck_rns_on_announce(destination_hash, announced_identity, app_data);
    }
};

// One-time setup once RNS has started (deferred from tdeck_ui_init because the
// Reticulum instance comes up later in setup()).
static void tdeck_rns_setup() {
    if (tdeck_rns_ready) return;
    const RNS::Identity& id = RNS::Transport::identity();
    if (!id) return;

    tdeck_rns_ready = true;

    tdeck_rns_msg_dest = RNS::Destination(id, RNS::Type::Destination::IN,
                                          RNS::Type::Destination::SINGLE,
                                          TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_MSGS);
    tdeck_rns_msg_dest.set_packet_callback(tdeck_rns_on_message);
    tdeck_rns_msg_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    tdeck_rns_ping_dest = RNS::Destination(id, RNS::Type::Destination::IN,
                                           RNS::Type::Destination::SINGLE,
                                           TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_PING);
    tdeck_rns_ping_dest.set_packet_callback(tdeck_rns_on_ping);
    tdeck_rns_ping_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    tdeck_rns_ann_handler = RNS::HAnnounceHandler(new TDeckAnnounceHandler());
    RNS::Transport::register_announce_handler(tdeck_rns_ann_handler);

    tdeck_rns_announce_ui();
    tdeck_rns_sync_contacts();
    tdeck_toast_set("RNS ready");
    tdeck_dirty = true;
}

// Re-announce the UI destinations (used at startup and when the name changes).
static void tdeck_rns_announce_ui() {
    if (!tdeck_rns_ready) return;
    RNS::Bytes name(tdeck_set_device_name);
    if (tdeck_rns_msg_dest)  tdeck_rns_msg_dest.announce(name);
    if (tdeck_rns_ping_dest) tdeck_rns_ping_dest.announce(name);
    tdeck_last_announce_at = RNS::Utilities::OS::time();
}

// Import identities RNS has persisted into the contact store.
static void tdeck_rns_sync_contacts() {
    // known_destinations() exposes a const store but the TypedStore iterator
    // API is non-const; const_cast is safe here because we only read.
    auto& known = const_cast<RNS::Persistence::KnownDestinations&>(RNS::Identity::known_destinations());
    for (auto it = known.begin(); it != known.end(); ++it) {
        const auto& entry = *it;
        std::string h = entry.key.toHex();
        const RNS::Persistence::IdentityEntry& ie = entry.value;
        char hash[40] = {0}, pub[140] = {0}, appd[TDECK_APP_DATA_MAX+1] = {0};
        strncpy(hash, h.c_str(), sizeof(hash)-1);
        if (ie._public_key.size() > 0) {
            std::string pk = ie._public_key.toHex();
            strncpy(pub, pk.c_str(), sizeof(pub)-1);
        }
        if (ie._app_data.size() > 0) {
            std::string ad = ie._app_data.toString();
            strncpy(appd, ad.c_str(), sizeof(appd)-1);
        }
        tdeck_contact_upsert(hash, NULL, pub[0] ? pub : NULL, appd, ie._timestamp);
    }
    tdeck_dirty = true;
}

// Send an arbitrary framed payload to a contact's destination for `aspect`.
static bool tdeck_rns_send_payload(const tdeck_contact_t* c, const char* aspect,
                                   const char* payload) {
    if (!tdeck_rns_ready) return false;
    if (!c || !c->has_key || !c->pubkey[0]) return false;
    uint8_t key[64];
    if (!tdeck_hex_decode(c->pubkey, key, 64)) return false;

    RNS::Identity rid(false);
    rid.load_public_key(RNS::Bytes(key, 64));
    RNS::Destination out(rid, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                         TDECK_UI_RNS_APP, aspect);
    RNS::Packet(out, RNS::Bytes(payload)).send();
    return true;
}

static void tdeck_rns_ping_contact(uint8_t idx) {
    if (!tdeck_rns_ready) return;
    if (idx >= tdeck_contact_count) return;
    tdeck_contact_t* c = &tdeck_contacts[idx];
    if (!c->has_key) {
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "No key for contact");
        return;
    }
    strncpy(tdeck_ping_peer, c->hash, sizeof(tdeck_ping_peer)-1);
    tdeck_ping_seq++;

    char frame[256];
    snprintf(frame, sizeof(frame), "%s%08x!%.3f!%s", TDECK_FRAME_PING, tdeck_ping_seq,
             RNS::Utilities::OS::time(), c->pubkey);
    if (!tdeck_rns_send_payload(c, TDECK_UI_RNS_ASPECT_PING, frame)) {
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "Send failed");
        return;
    }
    tdeck_ping_inflight  = true;
    tdeck_ping_sent_at   = RNS::Utilities::OS::time();
    tdeck_ping_deadline  = millis() + TDECK_PING_TIMEOUT_MS;
    snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "Pinging...");
    tdeck_dirty = true;
}

static void tdeck_rns_ping_poll() {
    if (!tdeck_ping_inflight) return;
    if (millis() >= tdeck_ping_deadline) {
        tdeck_ping_inflight = false;
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "Timeout");
        tdeck_dirty = true;
    }
}

// Inspect the transport state for the route to a contact and display it.
static void tdeck_rns_trace_contact(uint8_t idx) {
    if (!tdeck_rns_ready) return;
    if (idx >= tdeck_contact_count) return;
    tdeck_contact_t* c = &tdeck_contacts[idx];
    strncpy(tdeck_trace_peer, c->hash, sizeof(tdeck_trace_peer)-1);

    char label[48];
    tdeck_contact_label(c, label, sizeof(label));

    // Prefer the announced destination hash; fall back to the identity hash.
    RNS::Bytes probe = {RNS::Type::NONE};
    uint8_t raw[16];
    if (c->dest_hash[0] && tdeck_hex_decode(c->dest_hash, raw, 16)) {
        probe = RNS::Bytes(raw, 16);
    } else if (tdeck_hex_decode(c->hash, raw, 16)) {
        probe = RNS::Bytes(raw, 16);
    }

    char out[400];
    int off = snprintf(out, sizeof(out), "Trace to %s\n", label);
    if (probe && RNS::Transport::has_path(probe)) {
        off += snprintf(out + off, sizeof(out) - off, "Hops: %u\n", RNS::Transport::hops_to(probe));
        RNS::Bytes nh = RNS::Transport::next_hop(probe);
        if (nh) off += snprintf(out + off, sizeof(out) - off, "Next hop: %s\n", nh.toHex().c_str());
        const auto& states = RNS::Transport::path_states();
        auto sit = states.find(probe);
        if (sit != states.end()) {
            const char* st = (sit->second == RNS::Type::Transport::STATE_RESPONSIVE)   ? "responsive" :
                             (sit->second == RNS::Type::Transport::STATE_UNRESPONSIVE) ? "unresponsive" :
                                                                                         "unknown";
            off += snprintf(out + off, sizeof(out) - off, "Path state: %s\n", st);
        }
        RNS::Interface inf = RNS::Transport::next_hop_interface(probe);
        if (inf) off += snprintf(out + off, sizeof(out) - off, "Interface: %s\n", inf.name().c_str());
        off += snprintf(out + off, sizeof(out) - off, "Route known; pinging...");
    } else {
        off += snprintf(out + off, sizeof(out) - off, "No route yet; pinging to establish one...");
    }
    strncpy(tdeck_trace_out, out, sizeof(tdeck_trace_out)-1);
    tdeck_trace_out[sizeof(tdeck_trace_out)-1] = '\0';
    tdeck_trace_inflight = true;
    tdeck_rns_ping_contact(idx);
    tdeck_dirty = true;
}

#endif // HAS_RNS

// ---- Drawing helpers ---------------------------------------------------------

// Top status bar: node name, GPS indicator, Reticulum indicator, clock and
// battery. Drawn on every screen.
static void tdeck_ui_draw_status_bar(TDeckCanvas& c) {
    c.fillRect(0, 0, TDECK_SCREEN_W, TDECK_SBAR_H, TDECK_COL_SBAR);
    c.drawFastHLine(0, TDECK_SBAR_H - 1, TDECK_SCREEN_W, TDECK_COL_DIM);
    c.setTextSize(1);
    c.setTextColor(TDECK_COL_FG);

    // Node name (left)
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H / 2 - 4);
    char name[20];
    strncpy(name, tdeck_set_device_name, sizeof(name)-1);
    name[sizeof(name)-1] = '\0';
    c.print(name);

    // Reticulum status block
    int x = TDECK_LIST_X + 16 * 6;
    uint16_t rns_col = TDECK_COL_ERR;
    const char* rns_txt = "RNS off";
  #ifdef HAS_RNS
    if (tdeck_rns_ready) {
        rns_col = TDECK_COL_OK;
        rns_txt = "RNS on";
    }
  #else
    (void)rns_col;
    (void)rns_txt;
  #endif
    c.fillCircle(x + 3, TDECK_SBAR_H / 2, 3, rns_col);
    c.setCursor(x + 9, TDECK_SBAR_H / 2 - 4);
    c.print(rns_txt);

    // GPS status block
    x += 9 + strlen(rns_txt) * 6;
    const char* gps_txt = "GPS n/a";
    uint16_t gps_col = TDECK_COL_DIM;
    if (!tdeck_set_gps_enabled) {
        gps_txt = "GPS off";
        gps_col = TDECK_COL_DIM;
    } else if (tdeck_gps_present) {
        if (tdeck_gps_fix) { gps_txt = "GPS 3D"; gps_col = TDECK_COL_OK; }
        else               { gps_txt = "GPS --"; gps_col = TDECK_COL_WARN; }
    }
    c.setTextColor(gps_col);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(gps_txt);

    // Clock (right of GPS)
    char timestr[16];
    tdeck_ui_time_str(timestr, sizeof(timestr));
    x += strlen(gps_txt) * 6 + 8;
    c.setTextColor(TDECK_COL_FG);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(timestr);

    // Battery (right-aligned)
    char bat[8];
    if (battery_ready) {
        snprintf(bat, sizeof(bat), "%u%%", (uint8_t)(battery_percent + 0.5f));
    } else {
        snprintf(bat, sizeof(bat), "--%%");
    }
    int bat_x = TDECK_SCREEN_W - TDECK_LIST_X - strlen(bat) * 6;
    c.setCursor(bat_x, TDECK_SBAR_H / 2 - 4);
    c.print(bat);
    c.drawRect(TDECK_SCREEN_W - TDECK_LIST_X - 4, TDECK_SBAR_H / 2 - 6, 9, 12, TDECK_COL_FG);
    c.fillRect(TDECK_SCREEN_W - TDECK_LIST_X - 4, TDECK_SBAR_H / 2 - 8, 9, 2, TDECK_COL_FG);
}

// Bottom hint bar with an optional transient toast message.
static void tdeck_ui_draw_footer(TDeckCanvas& c, const char* hint) {
    c.fillRect(0, TDECK_SCREEN_H - TDECK_FOOT_H, TDECK_SCREEN_W, TDECK_FOOT_H, TDECK_COL_SBAR);
    c.drawFastHLine(0, TDECK_SCREEN_H - TDECK_FOOT_H, TDECK_SCREEN_W, TDECK_COL_DIM);
    c.setTextSize(1);
    c.setTextColor(TDECK_COL_DIM);
    c.setCursor(TDECK_LIST_X, TDECK_SCREEN_H - TDECK_FOOT_H + 5);
    if (tdeck_toast[0] && millis() < tdeck_toast_until) {
        c.print(tdeck_toast);
    } else if (hint) {
        c.print(hint);
    }
}

// Simple 12x12 app glyphs for the home screen menu.
static void tdeck_ui_draw_icon(TDeckCanvas& c, uint8_t x, uint8_t y, uint8_t icon, uint16_t color) {
    c.fillRect(x, y, 12, 12, color);
    switch (icon) {
        case 0: // envelope
            c.fillRect(x, y + 3, 12, 6, TDECK_COL_SEL);
            c.drawLine(x, y + 3, x + 6, y + 9, TDECK_COL_SEL);
            c.drawLine(x + 12, y + 3, x + 6, y + 9, TDECK_COL_SEL);
            break;
        case 1: // contacts (two heads)
            c.fillCircle(x + 4, y + 4, 3, TDECK_COL_SEL);
            c.fillCircle(x + 9, y + 4, 3, TDECK_COL_SEL);
            c.fillRect(x + 1, y + 8, 6, 4, TDECK_COL_SEL);
            c.fillRect(x + 7, y + 8, 6, 4, TDECK_COL_SEL);
            break;
        case 2: // network (circle + dot)
            c.drawCircle(x + 6, y + 6, 5, TDECK_COL_SEL);
            c.fillCircle(x + 6, y + 6, 2, TDECK_COL_SEL);
            break;
        case 3: // settings (gear = circle + spokes)
            c.drawCircle(x + 6, y + 6, 4, TDECK_COL_SEL);
            c.fillCircle(x + 6, y + 6, 2, TDECK_COL_SEL);
            c.drawFastHLine(x + 2, y + 6, 8, TDECK_COL_SEL);
            c.drawFastVLine(x + 6, y + 2, 8, TDECK_COL_SEL);
            break;
    }
}

// Highlighted list row with a selection bar.
static void tdeck_ui_draw_row(TDeckCanvas& c, uint8_t y, const char* text, bool selected) {
    if (selected) {
        c.fillRect(0, y, TDECK_SCREEN_W, 14, TDECK_COL_SEL);
    }
    c.setTextSize(1);
    c.setTextColor(selected ? ST77XX_WHITE : TDECK_COL_FG);
    c.setCursor(TDECK_LIST_X, y + 3);
    c.print(text);
}

// Draws a scrollable list of up to `n` items with a title header. `cursor` is
// the selected index, `scroll` the first visible index. Returns the number of
// visible rows.
static uint8_t tdeck_ui_list_frame(TDeckCanvas& c, const char* title, uint8_t n,
                                   uint8_t cursor, uint8_t* scroll) {
    c.setTextSize(2);
    c.setTextColor(TDECK_COL_ACCENT);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
    c.print(title);
    c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

    uint8_t rows = (TDECK_SCREEN_H - TDECK_SBAR_H - 24 - TDECK_FOOT_H) / 16;
    if (rows < 1) rows = 1;
    if (*scroll > cursor) *scroll = cursor;
    if (cursor >= *scroll + rows) *scroll = cursor - rows + 1;
    return rows;
}

static void tdeck_ui_draw_home() {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);

    c.setTextSize(2);
    c.setTextColor(TDECK_COL_ACCENT);
    c.setCursor(10, TDECK_SBAR_H + 8);
    c.print("microReticulum");
    c.drawFastHLine(0, TDECK_SBAR_H + 32, TDECK_SCREEN_W, TDECK_COL_DIM);

    static const char* apps[] = { "Messages", "Contacts", "Reticulum", "Settings" };
    uint8_t napps = 4;
    for (uint8_t i = 0; i < napps; i++) {
        uint8_t y = TDECK_SBAR_H + 44 + i * 40;
        bool sel = (i == tdeck_cursor);
        if (sel) c.fillRect(0, y - 2, TDECK_SCREEN_W, 34, TDECK_COL_SEL);
        tdeck_ui_draw_icon(c, 14, y + 6, i, sel ? ST77XX_WHITE : TDECK_COL_ACCENT);
        c.setTextSize(2);
        c.setTextColor(sel ? ST77XX_WHITE : TDECK_COL_FG);
        c.setCursor(40, y + 7);
        c.print(apps[i]);
    }

    tdeck_ui_draw_footer(c, "TB: scroll/select  L: back  long press: home");
}

// Number of distinct peers that have messages (conversations).
static uint8_t tdeck_inbox_peer_count() {
    uint8_t count = 0;
    for (int i = tdeck_msg_count - 1; i >= 0; i--) {
        bool dup = false;
        for (int j = i + 1; j < (int)tdeck_msg_count; j++) {
            if (strcmp(tdeck_messages[i].peer, tdeck_messages[j].peer) == 0) { dup = true; break; }
        }
        if (!dup) count++;
    }
    return count;
}

// Fill `out` with the peer hash of the idx-th conversation (newest first).
static bool tdeck_inbox_peer_at(uint8_t idx, char* out, size_t n) {
    char seen[TDECK_MAX_MESSAGES][40];
    uint8_t scount = 0;
    for (int i = tdeck_msg_count - 1; i >= 0; i--) {
        const char* p = tdeck_messages[i].peer;
        bool dup = false;
        for (uint8_t j = 0; j < scount; j++) {
            if (strcmp(seen[j], p) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strncpy(seen[scount], p, 40);
        seen[scount][39] = '\0';
        if (scount == idx) {
            strncpy(out, p, n);
            out[n-1] = '\0';
            return true;
        }
        scount++;
    }
    return false;
}

static void tdeck_ui_draw_messages() {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);

    if (tdeck_sub == MSG_INBOX) {
        uint8_t nconv = tdeck_inbox_peer_count();
        uint8_t total = nconv + 1;                       // "+ New message"
        uint8_t rows = tdeck_ui_list_frame(c, "Messages", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[80];
            if (idx == 0) {
                snprintf(line, sizeof(line), "+ New message");
            } else {
                char peer[40];
                if (tdeck_inbox_peer_at(idx - 1, peer, sizeof(peer))) {
                    tdeck_contact_t* cnt = tdeck_contact_find(peer);
                    char label[40];
                    tdeck_contact_label(cnt, label, sizeof(label));
                    snprintf(line, sizeof(line), "%s%s", (cnt && cnt->favorite) ? "* " : "", label);
                } else {
                    snprintf(line, sizeof(line), "?");
                }
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "TB: scroll/select  L: back");
    } else if (tdeck_sub == MSG_THREAD) {
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
        tdeck_contact_t* cnt = tdeck_contact_find(tdeck_thread_peer);
        char label[48];
        tdeck_contact_label(cnt, label, sizeof(label));
        c.print(label);
        c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

        // Collect indices of the conversation's messages, newest at the bottom.
        uint8_t idxs[TDECK_MAX_MESSAGES];
        uint8_t n = 0;
        for (uint8_t i = 0; i < tdeck_msg_count; i++) {
            if (strcmp(tdeck_messages[i].peer, tdeck_thread_peer) == 0) {
                idxs[n++] = i;
            }
        }
        uint8_t rows = (TDECK_SCREEN_H - TDECK_SBAR_H - 24 - TDECK_FOOT_H) / 12;
        if (rows < 1) rows = 1;
        if (tdeck_scroll > n) tdeck_scroll = n;
        uint8_t end = n - tdeck_scroll;              // index just past last shown
        uint8_t start = end > rows ? end - rows : 0; // scroll back to fill the view
        for (uint8_t r = 0; r < rows && start + r < n; r++) {
            uint8_t mi = idxs[start + r];
            bool inbound = tdeck_messages[mi].inbound;
            uint8_t y = TDECK_SBAR_H + 28 + r * 12;
            if (inbound) {
                c.fillRect(0, y, TDECK_SCREEN_W - 80, 11, TDECK_COL_SEL);
                c.setTextColor(ST77XX_WHITE);
            } else {
                c.fillRect(TDECK_SCREEN_W - 80, y, 80, 11, TDECK_COL_SBAR);
                c.setTextColor(TDECK_COL_DIM);
            }
            c.setTextSize(1);
            c.setCursor(inbound ? TDECK_LIST_X : TDECK_SCREEN_W - 78, y + 2);
            c.print(tdeck_messages[mi].text);
        }
        tdeck_ui_draw_footer(c, "press: reply  L: back");
    } else if (tdeck_sub == MSG_PICK) {
        uint8_t total = tdeck_contact_count;
        uint8_t rows = tdeck_ui_list_frame(c, "To", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char label[48];
            tdeck_contact_label(&tdeck_contacts[idx], label, sizeof(label));
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, label, idx == tdeck_cursor);
        }
        if (total == 0) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts yet - wait for announces");
        }
        tdeck_ui_draw_footer(c, "select contact  L: back");
    } else if (tdeck_sub == MSG_COMPOSE) {
        tdeck_ui_draw_text_input("Message", tdeck_input_buf);
    }
}

// Full-screen text entry used by compose / alias / name editing.
static void tdeck_ui_draw_text_input(const char* title, const char* buf) {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);

    c.setTextSize(2);
    c.setTextColor(TDECK_COL_ACCENT);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 8);
    c.print(title);
    c.drawFastHLine(0, TDECK_SBAR_H + 32, TDECK_SCREEN_W, TDECK_COL_DIM);

    // Input box (wraps the buffer at 25 chars per textSize-2 line)
    c.fillRect(6, TDECK_SBAR_H + 42, TDECK_SCREEN_W - 12, 42, TDECK_COL_SBAR);
    c.drawRect(6, TDECK_SBAR_H + 42, TDECK_SCREEN_W - 12, 42, TDECK_COL_DIM);
    c.setTextSize(2);
    c.setTextColor(TDECK_COL_FG);
    uint8_t start_line = strlen(buf) > 50 ? strlen(buf) - 50 : 0;
    const char* p = buf + start_line;
    c.setCursor(12, TDECK_SBAR_H + 48);
    uint8_t col = 0;
    for (; *p; p++) {
        c.print(*p);
        if (++col >= 25) {
            col = 0;
            c.setCursor(12, TDECK_SBAR_H + 66);
        }
    }
    c.drawFastHLine(0, TDECK_SBAR_H + 96, TDECK_SCREEN_W, TDECK_COL_DIM);

    c.setTextSize(1);
    c.setTextColor(TDECK_COL_DIM);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 104);
    c.print("Enter: ok   Backspace: delete   Esc: cancel");
    tdeck_ui_draw_footer(c, "type on keyboard  L: back");
}

static void tdeck_ui_draw_contacts() {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);

    if (tdeck_sub == CT_LIST) {
        uint8_t total = tdeck_contact_count;
        uint8_t rows = tdeck_ui_list_frame(c, "Contacts", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[80];
            tdeck_contact_t& ct = tdeck_contacts[idx];
            char label[48];
            tdeck_contact_label(&ct, label, sizeof(label));
            snprintf(line, sizeof(line), "%s%s", ct.favorite ? "* " : "", label);
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        if (total == 0) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts yet - wait for announces");
        }
        tdeck_ui_draw_footer(c, "select: detail  L: back");
    } else if (tdeck_sub == CT_DETAIL) {
        tdeck_contact_t* ct = (tdeck_contact_detail_idx < tdeck_contact_count)
                              ? &tdeck_contacts[tdeck_contact_detail_idx] : NULL;
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
        c.print("Contact");
        c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(1);
        int y = TDECK_SBAR_H + 34;
        if (ct) {
            char label[48];
            tdeck_contact_label(ct, label, sizeof(label));
            c.setTextColor(TDECK_COL_FG);
            c.setCursor(TDECK_LIST_X, y); c.print("Name: "); c.print(label); y += 12;
            c.setTextColor(TDECK_COL_DIM);
            c.setCursor(TDECK_LIST_X, y); c.print("Hash: "); c.print(ct->hash); y += 12;
            if (ct->app_data[0]) { c.setCursor(TDECK_LIST_X, y); c.print("Ann: "); c.print(ct->app_data); y += 12; }
            if (ct->has_key)  { c.setCursor(TDECK_LIST_X, y); c.print("Key: yes"); }
            else              { c.setCursor(TDECK_LIST_X, y); c.print("Key: no announce yet"); }
            y += 12;
        } else {
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, y); c.print("Contact lost"); y += 12;
        }
        c.drawFastHLine(0, y + 4, TDECK_SCREEN_W, TDECK_COL_DIM);

        // Actions (drawn below the info block; no second list title)
        static const char* actions[] = {
            "Toggle favorite", "Edit alias", "Send message",
            "Export all", "Delete contact", "Back"
        };
        uint8_t ay = y + 12;
        if (ay < TDECK_SBAR_H + 30) ay = TDECK_SBAR_H + 30;
        uint8_t n = 6;
        uint8_t rows = (TDECK_SCREEN_H - TDECK_FOOT_H - ay) / 16;
        if (rows > n) rows = n;
        if (tdeck_cursor >= n) tdeck_cursor = n - 1;
        for (uint8_t r = 0; r < rows; r++) {
            uint8_t idx = r;
            char line[48];
            if (idx == 0 && ct) snprintf(line, sizeof(line), "%s", ct->favorite ? "Unfavorite" : "Favorite");
            else if (idx < n) snprintf(line, sizeof(line), "%s", actions[idx]);
            else snprintf(line, sizeof(line), "");
            tdeck_ui_draw_row(c, ay + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: run  L: back");
    } else if (tdeck_sub == CT_ALIAS) {
        tdeck_ui_draw_text_input("Alias", tdeck_input_buf);
    }
}

#ifdef HAS_RNS
// Count directly reachable destinations (1 hop) from the transport path table.
static uint8_t tdeck_direct_count() {
    uint8_t n = 0;
    for (auto it = RNS::Transport::path_table().begin(); it != RNS::Transport::path_table().end(); ++it) {
        if (it->second._hops <= 1) n++;
    }
    return n;
}

static bool tdeck_direct_at(uint8_t idx, char* out, size_t n, uint8_t* hops) {
    uint8_t seen = 0;
    for (auto it = RNS::Transport::path_table().begin(); it != RNS::Transport::path_table().end(); ++it) {
        if (it->second._hops <= 1) {
            if (seen == idx) {
                std::string h = it->first.toHex();
                strncpy(out, h.c_str(), n - 1);
                out[n-1] = '\0';
                if (hops) *hops = it->second._hops;
                return true;
            }
            seen++;
        }
    }
    return false;
}
#endif

static void tdeck_ui_draw_reticulum() {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);

    if (tdeck_sub == RET_MENU) {
        static const char* items[] = {
            "Ping contact", "Traceroute contact", "Direct contacts", "My identity"
        };
        uint8_t n = 4;
        uint8_t rows = tdeck_ui_list_frame(c, "Reticulum", n, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < n; r++) {
            uint8_t idx = tdeck_scroll + r;
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, items[idx], idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: run  L: back");
    } else if (tdeck_sub == RET_PING_PICK || tdeck_sub == RET_TRACE_PICK) {
        uint8_t total = tdeck_contact_count;
        uint8_t rows = tdeck_ui_list_frame(c, "Pick contact", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char label[48];
            tdeck_contact_label(&tdeck_contacts[idx], label, sizeof(label));
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, label, idx == tdeck_cursor);
        }
        if (total == 0) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts yet");
        }
        tdeck_ui_draw_footer(c, "select contact  L: back");
    } else if (tdeck_sub == RET_PING_RUN || tdeck_sub == RET_TRACE_RUN) {
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 6);
        c.print(tdeck_sub == RET_PING_RUN ? "Ping" : "Traceroute");
        c.drawFastHLine(0, TDECK_SBAR_H + 26, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(1);
        int y = TDECK_SBAR_H + 36;
        c.setTextColor(TDECK_COL_FG);
        c.setCursor(TDECK_LIST_X, y);
        c.print("Peer: ");
        c.print(tdeck_sub == RET_PING_RUN ? tdeck_ping_peer : tdeck_trace_peer);
        y += 12;
        c.setTextColor(TDECK_COL_WARN);
        c.setCursor(TDECK_LIST_X, y);
        c.print(tdeck_ping_status);
        y += 14;

        if (tdeck_sub == RET_TRACE_RUN && tdeck_trace_out[0]) {
            c.drawFastHLine(0, y, TDECK_SCREEN_W, TDECK_COL_DIM);
            y += 6;
            // Split the trace text into lines for display.
            const char* p = tdeck_trace_out;
            while (*p && y < TDECK_SCREEN_H - TDECK_FOOT_H - 6) {
                char line[60];
                uint8_t li = 0;
                while (*p && *p != '\n' && li < sizeof(line)-1) line[li++] = *p++;
                line[li] = '\0';
                c.setTextColor(TDECK_COL_FG);
                c.setCursor(TDECK_LIST_X, y);
                c.print(line);
                y += 12;
                if (*p == '\n') p++;
            }
        }
        tdeck_ui_draw_footer(c, "press: retry  L: back");
    }
}

static void tdeck_ui_draw_reticulum_rest() {
    TDeckCanvas &c = tdeck_canvas;
    if (tdeck_sub == RET_DIRECT) {
        uint8_t total;
      #ifdef HAS_RNS
        total = tdeck_direct_count();
      #else
        total = 0;
      #endif
        uint8_t rows = tdeck_ui_list_frame(c, "Direct contacts", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[80];
          #ifdef HAS_RNS
            uint8_t hops = 0;
            char h[40];
            if (tdeck_direct_at(idx, h, sizeof(h), &hops)) {
                snprintf(line, sizeof(line), "%s  (%u hop)", h, hops);
            } else {
                snprintf(line, sizeof(line), "?");
            }
          #else
            snprintf(line, sizeof(line), "RNS unavailable");
          #endif
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "L: back");
    } else if (tdeck_sub == RET_IDENTITY) {
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
        c.print("My identity");
        c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(1);
        int y = TDECK_SBAR_H + 34;
      #ifdef HAS_RNS
        if (tdeck_rns_ready) {
            const RNS::Identity& id = RNS::Transport::identity();
            c.setTextColor(TDECK_COL_FG);
            c.setCursor(TDECK_LIST_X, y); c.print("Hash: "); c.print(id.hash().toHex().c_str()); y += 12;
            c.setCursor(TDECK_LIST_X, y); c.print("Name: "); c.print(tdeck_set_device_name); y += 12;
            c.setTextColor(TDECK_COL_DIM);
            c.setCursor(TDECK_LIST_X, y);
            c.print("Transport: ");
            c.print(RNS::Reticulum::transport_enabled() ? "enabled" : "disabled");
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            c.printf("Paths: %u  Ann: %u", (unsigned)RNS::Transport::path_table().size(),
                     (unsigned)RNS::Transport::announces_received());
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            c.printf("TX: %u  RX: %u", (unsigned)RNS::Transport::packets_sent(),
                     (unsigned)RNS::Transport::packets_received());
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            c.printf("Interfaces: %u", (unsigned)RNS::Transport::get_interfaces().size());
            y += 12;
        } else {
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, y); c.print("RNS not ready yet"); y += 12;
        }
      #else
        c.setTextColor(TDECK_COL_WARN);
        c.setCursor(TDECK_LIST_X, y); c.print("RNS not compiled in"); y += 12;
      #endif
        c.drawFastHLine(0, y + 4, TDECK_SCREEN_W, TDECK_COL_DIM);

        // Actions (below the identity info block)
        uint8_t ay = y + 12;
        if (ay < TDECK_SBAR_H + 30) ay = TDECK_SBAR_H + 30;
        static const char* acts[] = { "Edit name", "Back" };
        for (uint8_t r = 0; r < 2; r++) {
            tdeck_ui_draw_row(c, ay + r * 16, acts[r], r == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: run  L: back");
    } else if (tdeck_sub == RET_NAME_EDIT) {
        tdeck_ui_draw_text_input("Node name", tdeck_input_buf);
    }
}

// Settings list: fills `label`/`value` for a given row; `action` marks rows
// that execute immediately instead of being edited numerically.
#define TDECK_SET_ROWS 9
static void tdeck_set_row(uint8_t idx, char* label, size_t n, char* value, size_t vn, bool* action) {
    switch (idx) {
        case 0: snprintf(label, n, "Node name");   snprintf(value, vn, "%s", tdeck_set_device_name); *action = true; break;
        case 1: snprintf(label, n, "Brightness");  snprintf(value, vn, "%u", tdeck_set_brightness);   *action = false; break;
        case 2: snprintf(label, n, "Blank timeout"); snprintf(value, vn, "%us", tdeck_set_blank_timeout); *action = false; break;
        case 3: snprintf(label, n, "Show GPS");    snprintf(value, vn, "%s", tdeck_set_gps_enabled ? "yes" : "no"); *action = false; break;
        case 4: snprintf(label, n, "Announce min"); snprintf(value, vn, "%u", tdeck_set_announce_interval); *action = false; break;
        case 5: snprintf(label, n, "Confirm send"); snprintf(value, vn, "%s", tdeck_set_confirm_send ? "yes" : "no"); *action = false; break;
        case 6: snprintf(label, n, "Save to SD");  snprintf(value, vn, ""); *action = true; break;
        case 7: snprintf(label, n, "Reload from SD"); snprintf(value, vn, ""); *action = true; break;
        default: snprintf(label, n, "Export contacts"); snprintf(value, vn, ""); *action = true; break;
    }
}

static void tdeck_ui_draw_settings() {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);

    if (tdeck_sub == SET_LIST) {
        uint8_t rows = tdeck_ui_list_frame(c, "Settings", TDECK_SET_ROWS, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < TDECK_SET_ROWS; r++) {
            uint8_t idx = tdeck_scroll + r;
            char label[32], value[40];
            bool action = false;
            tdeck_set_row(idx, label, sizeof(label), value, sizeof(value), &action);
            char line[72];
            if (action && !value[0]) snprintf(line, sizeof(line), "%s", label);
            else snprintf(line, sizeof(line), "%s: %s", label, value);
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: edit  L: back");
    } else if (tdeck_sub == SET_EDIT) {
        char label[32], value[40];
        bool action = false;
        tdeck_set_row(tdeck_set_edit_idx, label, sizeof(label), value, sizeof(value), &action);
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 8);
        c.print(label);
        c.drawFastHLine(0, TDECK_SBAR_H + 32, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(2);
        c.setTextColor(TDECK_COL_FG);
        c.setCursor(20, TDECK_SBAR_H + 60);
        c.print(value);
        c.setTextSize(1);
        c.setTextColor(TDECK_COL_DIM);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 100);
        c.print("UP/DOWN: change   Enter: save   L: back");
        tdeck_ui_draw_footer(c, "changes are saved on 'Save to SD'");
    }
}

// Master draw: the status bar is drawn by every screen, then the active app.
static void tdeck_ui_draw() {
    if (!tdeck_canvas.valid()) return;
    switch (tdeck_screen) {
        case SCREEN_HOME:      tdeck_ui_draw_home(); break;
        case SCREEN_MESSAGES:  tdeck_ui_draw_messages(); break;
        case SCREEN_CONTACTS:  tdeck_ui_draw_contacts(); break;
        case SCREEN_RETICULUM:
            if (tdeck_sub == RET_DIRECT || tdeck_sub == RET_IDENTITY ||
                tdeck_sub == RET_NAME_EDIT) {
                tdeck_ui_draw_reticulum_rest();
            } else {
                tdeck_ui_draw_reticulum();
            }
            break;
        case SCREEN_SETTINGS:  tdeck_ui_draw_settings(); break;
        default:               tdeck_ui_draw_home(); break;
    }
    display.drawRGBBitmap(0, 0, tdeck_canvas.buf(), TDECK_SCREEN_W, TDECK_SCREEN_H);
}

// ---- Navigation ---------------------------------------------------------------

static void tdeck_ui_go_home() {
    tdeck_nav_depth = 0;
    tdeck_screen = SCREEN_HOME;
    tdeck_sub = 0;
    tdeck_cursor = 0;
    tdeck_scroll = 0;
    tdeck_dirty = true;
}

static void tdeck_ui_nav_push(uint8_t screen, uint8_t sub) {
    if (tdeck_nav_depth < TDECK_NAV_DEPTH) {
        tdeck_nav_screen[tdeck_nav_depth] = tdeck_screen;
        tdeck_nav_sub[tdeck_nav_depth]    = tdeck_sub;
        tdeck_nav_depth++;
    }
    tdeck_screen = screen;
    tdeck_sub = sub;
    tdeck_cursor = 0;
    tdeck_scroll = 0;
    tdeck_dirty = true;
}

static void tdeck_ui_nav_back() {
    if (tdeck_nav_depth > 0) {
        tdeck_nav_depth--;
        tdeck_screen = tdeck_nav_screen[tdeck_nav_depth];
        tdeck_sub    = tdeck_nav_sub[tdeck_nav_depth];
    } else {
        tdeck_screen = SCREEN_HOME;
        tdeck_sub = 0;
    }
    tdeck_cursor = 0;
    tdeck_scroll = 0;
    tdeck_dirty = true;
}

// Change the value of the setting row currently being edited.
static void tdeck_set_edit_change(int dir) {
    switch (tdeck_set_edit_idx) {
        case 1: {
            int v = (int)tdeck_set_brightness + dir * 16;
            tdeck_set_brightness = (uint8_t)constrain(v, 0, 255);
            tdeck_apply_brightness();
            tdeck_settings_dirty = true;
            break;
        }
        case 2: {
            int v = (int)tdeck_set_blank_timeout + dir * 15;
            tdeck_set_blank_timeout = (uint16_t)constrain(v, 0, 600);
            tdeck_settings_dirty = true;
            break;
        }
        case 3: tdeck_set_gps_enabled = !tdeck_set_gps_enabled; tdeck_settings_dirty = true; break;
        case 4: {
            int v = (int)tdeck_set_announce_interval + dir * 5;
            tdeck_set_announce_interval = (uint16_t)constrain(v, 0, 240);
            tdeck_settings_dirty = true;
            break;
        }
        case 5: tdeck_set_confirm_send = !tdeck_set_confirm_send; tdeck_settings_dirty = true; break;
        default: break;
    }
    tdeck_dirty = true;
}

static void tdeck_ui_nav_select() {
    switch (tdeck_screen) {
      case SCREEN_HOME:
        if (tdeck_cursor < 4) {
            static const uint8_t targets[4][2] = {
                { SCREEN_MESSAGES, MSG_INBOX },
                { SCREEN_CONTACTS, CT_LIST },
                { SCREEN_RETICULUM, RET_MENU },
                { SCREEN_SETTINGS, SET_LIST },
            };
            tdeck_ui_nav_push(targets[tdeck_cursor][0], targets[tdeck_cursor][1]);
        }
        break;

      case SCREEN_MESSAGES:
        if (tdeck_sub == MSG_INBOX) {
            uint8_t nconv = tdeck_inbox_peer_count();
            if (tdeck_cursor == 0) {
                tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_PICK);
            } else if (tdeck_cursor <= nconv) {
                char peer[40];
                if (tdeck_inbox_peer_at(tdeck_cursor - 1, peer, sizeof(peer))) {
                    strncpy(tdeck_thread_peer, peer, sizeof(tdeck_thread_peer)-1);
                    tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_THREAD);
                }
            }
        } else if (tdeck_sub == MSG_THREAD) {
            // press = quick reply to this contact
            strncpy(tdeck_input_peer, tdeck_thread_peer, sizeof(tdeck_input_peer)-1);
            tdeck_input_start(0, "");
            tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
        } else if (tdeck_sub == MSG_PICK) {
            if (tdeck_cursor < tdeck_contact_count) {
                strncpy(tdeck_input_peer, tdeck_contacts[tdeck_cursor].hash, sizeof(tdeck_input_peer)-1);
                tdeck_input_start(0, "");
                tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
            }
        } else if (tdeck_sub == MSG_COMPOSE) {
            tdeck_input_apply();
        }
        break;

      case SCREEN_CONTACTS:
        if (tdeck_sub == CT_LIST) {
            if (tdeck_cursor < tdeck_contact_count) {
                tdeck_contact_detail_idx = tdeck_cursor;
                tdeck_ui_nav_push(SCREEN_CONTACTS, CT_DETAIL);
            }
        } else if (tdeck_sub == CT_DETAIL) {
            switch (tdeck_cursor) {
                case 0: { // toggle favorite
                    if (tdeck_contact_detail_idx < tdeck_contact_count) {
                        tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
                        c->favorite = !c->favorite;
                        c->persisted = true;
                        tdeck_settings_dirty = true;
                        tdeck_toast_set(c->favorite ? "Favorited" : "Unfavorited");
                    }
                    break;
                }
                case 1: { // edit alias
                    if (tdeck_contact_detail_idx < tdeck_contact_count) {
                        tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
                        tdeck_input_start(1, c->alias);
                        tdeck_ui_nav_push(SCREEN_CONTACTS, CT_ALIAS);
                    }
                    break;
                }
                case 2: { // send message
                    if (tdeck_contact_detail_idx < tdeck_contact_count) {
                        tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
                        strncpy(tdeck_input_peer, c->hash, sizeof(tdeck_input_peer)-1);
                        tdeck_input_start(0, "");
                        tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
                    }
                    break;
                }
                case 3: // export all
                    if (tdeck_contacts_export()) tdeck_toast_set("Exported");
                    else tdeck_toast_set("Export failed");
                    break;
                case 4: { // delete contact
                    if (tdeck_contact_detail_idx < tdeck_contact_count) {
                        memmove(&tdeck_contacts[tdeck_contact_detail_idx],
                                &tdeck_contacts[tdeck_contact_detail_idx + 1],
                                sizeof(tdeck_contact_t) *
                                    (tdeck_contact_count - tdeck_contact_detail_idx - 1));
                        tdeck_contact_count--;
                        tdeck_settings_dirty = true;
                    }
                    tdeck_ui_nav_back();
                    break;
                }
                default:
                    tdeck_ui_nav_back();
                    break;
            }
        } else if (tdeck_sub == CT_ALIAS) {
            tdeck_input_apply();
        }
        break;

      case SCREEN_RETICULUM:
        if (tdeck_sub == RET_MENU) {
            switch (tdeck_cursor) {
                case 0: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_PING_PICK); break;
                case 1: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_TRACE_PICK); break;
                case 2: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_DIRECT); break;
                case 3: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_IDENTITY); break;
                default: break;
            }
        } else if (tdeck_sub == RET_PING_PICK) {
          #ifdef HAS_RNS
            if (tdeck_cursor < tdeck_contact_count) {
                tdeck_rns_ping_contact(tdeck_cursor);
                tdeck_ui_nav_push(SCREEN_RETICULUM, RET_PING_RUN);
            }
          #else
            tdeck_toast_set("RNS unavailable");
          #endif
        } else if (tdeck_sub == RET_TRACE_PICK) {
          #ifdef HAS_RNS
            if (tdeck_cursor < tdeck_contact_count) {
                tdeck_rns_trace_contact(tdeck_cursor);
                tdeck_ui_nav_push(SCREEN_RETICULUM, RET_TRACE_RUN);
            }
          #else
            tdeck_toast_set("RNS unavailable");
          #endif
        } else if (tdeck_sub == RET_PING_RUN) {
            // press = retry the ping against the stored peer
          #ifdef HAS_RNS
            tdeck_contact_t* c = tdeck_contact_find(tdeck_ping_peer);
            if (c) {
                for (uint8_t i = 0; i < tdeck_contact_count; i++) {
                    if (&tdeck_contacts[i] == c) { tdeck_rns_ping_contact(i); break; }
                }
            }
          #endif
        } else if (tdeck_sub == RET_TRACE_RUN) {
          #ifdef HAS_RNS
            tdeck_contact_t* c = tdeck_contact_find(tdeck_trace_peer);
            if (c) {
                for (uint8_t i = 0; i < tdeck_contact_count; i++) {
                    if (&tdeck_contacts[i] == c) { tdeck_rns_trace_contact(i); break; }
                }
            }
          #endif
        } else if (tdeck_sub == RET_IDENTITY) {
            if (tdeck_cursor == 0) {
                tdeck_input_start(2, tdeck_set_device_name);
                tdeck_ui_nav_push(SCREEN_RETICULUM, RET_NAME_EDIT);
            } else {
                tdeck_ui_nav_back();
            }
        } else if (tdeck_sub == RET_NAME_EDIT) {
            tdeck_input_apply();
        }
        break;

      case SCREEN_SETTINGS:
        if (tdeck_sub == SET_LIST) {
            switch (tdeck_cursor) {
                case 0: // node name -> text input
                    tdeck_input_start(2, tdeck_set_device_name);
                    tdeck_ui_nav_push(SCREEN_RETICULUM, RET_NAME_EDIT);
                    break;
                case 1: case 2: case 3: case 4: case 5:
                    tdeck_set_edit_idx = tdeck_cursor;
                    tdeck_ui_nav_push(SCREEN_SETTINGS, SET_EDIT);
                    break;
                case 6:
                    if (tdeck_settings_save()) tdeck_toast_set("Saved to SD");
                    else tdeck_toast_set("SD save failed");
                    break;
                case 7:
                    tdeck_settings_load();
                    tdeck_apply_brightness();
                    tdeck_toast_set("Reloaded");
                    break;
                case 8:
                    if (tdeck_contacts_export()) tdeck_toast_set("Exported");
                    else tdeck_toast_set("Export failed");
                    break;
                default: break;
            }
        } else if (tdeck_sub == SET_EDIT) {
            tdeck_settings_dirty = true;
            tdeck_ui_nav_back();
        }
        break;
    }
}

static void tdeck_ui_nav_up() {
    switch (tdeck_screen) {
      case SCREEN_HOME:
        tdeck_cursor = (tdeck_cursor + 3) % 4;      // wrap around 4 apps
        break;
      case SCREEN_MESSAGES:
        if (tdeck_sub == MSG_INBOX) {
            if (tdeck_cursor > 0) tdeck_cursor--;
        } else if (tdeck_sub == MSG_THREAD) {
            tdeck_scroll++;                          // view older messages
        } else if (tdeck_sub == MSG_PICK) {
            if (tdeck_cursor > 0) tdeck_cursor--;
        }
        break;
      case SCREEN_CONTACTS:
        if (tdeck_sub == CT_LIST || tdeck_sub == CT_DETAIL) {
            if (tdeck_cursor > 0) tdeck_cursor--;
        }
        break;
      case SCREEN_RETICULUM:
        if (tdeck_sub == RET_MENU || tdeck_sub == RET_PING_PICK ||
            tdeck_sub == RET_TRACE_PICK || tdeck_sub == RET_DIRECT ||
            tdeck_sub == RET_IDENTITY) {
            if (tdeck_cursor > 0) tdeck_cursor--;
        }
        break;
      case SCREEN_SETTINGS:
        if (tdeck_sub == SET_LIST) {
            if (tdeck_cursor > 0) tdeck_cursor--;
        } else if (tdeck_sub == SET_EDIT) {
            tdeck_set_edit_change(+1);
        }
        break;
      default: break;
    }
    tdeck_dirty = true;
}

static void tdeck_ui_nav_down() {
    switch (tdeck_screen) {
      case SCREEN_HOME:
        tdeck_cursor = (tdeck_cursor + 1) % 4;
        break;
      case SCREEN_MESSAGES:
        if (tdeck_sub == MSG_INBOX) {
            uint8_t total = tdeck_inbox_peer_count() + 1;
            if (tdeck_cursor + 1 < total) tdeck_cursor++;
        } else if (tdeck_sub == MSG_THREAD) {
            if (tdeck_scroll > 0) tdeck_scroll--;   // view newer messages
        } else if (tdeck_sub == MSG_PICK) {
            if (tdeck_cursor + 1 < tdeck_contact_count) tdeck_cursor++;
        }
        break;
      case SCREEN_CONTACTS:
        if (tdeck_sub == CT_LIST) {
            if (tdeck_cursor + 1 < tdeck_contact_count) tdeck_cursor++;
        } else if (tdeck_sub == CT_DETAIL) {
            if (tdeck_cursor + 1 < 6) tdeck_cursor++;
        }
        break;
      case SCREEN_RETICULUM:
        if (tdeck_sub == RET_MENU) {
            if (tdeck_cursor + 1 < 4) tdeck_cursor++;
        } else if (tdeck_sub == RET_PING_PICK || tdeck_sub == RET_TRACE_PICK) {
            if (tdeck_cursor + 1 < tdeck_contact_count) tdeck_cursor++;
        } else if (tdeck_sub == RET_DIRECT) {
          #ifdef HAS_RNS
            uint8_t total = tdeck_direct_count();
            if (tdeck_cursor + 1 < total) tdeck_cursor++;
          #endif
        } else if (tdeck_sub == RET_IDENTITY) {
            if (tdeck_cursor + 1 < 2) tdeck_cursor++;
        }
        break;
      case SCREEN_SETTINGS:
        if (tdeck_sub == SET_LIST) {
            if (tdeck_cursor + 1 < TDECK_SET_ROWS) tdeck_cursor++;
        } else if (tdeck_sub == SET_EDIT) {
            tdeck_set_edit_change(-1);
        }
        break;
      default: break;
    }
    tdeck_dirty = true;
}

// ---- Input handling ------------------------------------------------------------

static void tdeck_ui_handle_key(char key, uint8_t state) {
    // Only act on press events; ignore release/auto-repeat noise.
    if (state != KB_KEY_STATE_PRESS && state != KB_KEY_STATE_LONG_PRESS) return;

    switch (key) {
        case 0x0A:   // LF / Enter
        case 0x0D:   // CR / Enter
            if (tdeck_in_text_input()) tdeck_input_apply();
            else tdeck_ui_nav_select();
            break;
        case 0x08:   // Backspace
            if (tdeck_in_text_input()) tdeck_input_del();
            else tdeck_ui_nav_back();
            break;
        case 0x1B:   // Esc
            tdeck_ui_nav_back();
            break;
        default:
            if (key >= 0x20 && key <= 0x7E) {
                if (tdeck_in_text_input()) {
                    tdeck_input_add(key);
                } else if (tdeck_screen == SCREEN_HOME && key >= '1' && key <= '4') {
                    tdeck_cursor = (uint8_t)(key - '1');
                    tdeck_ui_nav_select();
                }
            }
            break;
    }
    tdeck_dirty = true;
}

static void tdeck_ui_handle_trackball(tb_event_t event) {
    switch (event) {
        case TB_EVENT_UP:         tdeck_ui_nav_up(); break;
        case TB_EVENT_DOWN:       tdeck_ui_nav_down(); break;
        case TB_EVENT_LEFT:       tdeck_ui_nav_back(); break;
        case TB_EVENT_RIGHT:      tdeck_ui_nav_select(); break;
        case TB_EVENT_PRESS:      tdeck_ui_nav_select(); break;
        case TB_EVENT_PRESS_LONG: tdeck_ui_go_home(); break;
        default: break;
    }
    tdeck_dirty = true;
}

// ---- Public API ---------------------------------------------------------------

// Called by Display.h's update_display() to determine whether the T-Deck UI
// owns the screen. When true, the legacy 64x64 canvas renderer yields.
bool tdeck_ui_active() {
    return tdeck_ui_ready;
}

// External hook for GPS code: sets whether a 3D fix is currently held. Calling
// this also marks a GPS as present so the status bar shows fix/no-fix instead
// of "n/a".
void tdeck_ui_set_gps_fix(bool fix) {
    if (tdeck_gps_fix != fix) {
        tdeck_gps_fix = fix;
        tdeck_gps_present = true;
        tdeck_dirty = true;
    }
}

void tdeck_ui_init() {
    // Power on the keyboard rail
    pinMode(KB_POWERON, OUTPUT);
    digitalWrite(KB_POWERON, HIGH);
    delay(50);

    // The T-Deck's keyboard and PMU share the I2C bus
    Wire.begin(I2C_SDA, I2C_SCL);

    // Start the keyboard. TDeckKeyboard auto-detects whether the unit has
    // the ESP32-C3 "T-Keyboard" byte-stream controller or a direct BBQ10.
    tdeck_kb.begin(&Wire);
    Serial.print("[TDeck] keyboard: ");
    switch (tdeck_kb.protocol()) {
        case 1: Serial.println("BBQ10 registers"); break;
        case 2: Serial.println("ESP32-C3 byte-stream"); break;
        default: Serial.println("not detected"); break;
    }

    // Start the trackball GPIO interrupts
    tdeck_trackball.begin();

    // Allocate the off-screen frame buffer (PSRAM-backed)
    tdeck_canvas.allocate();
    if (!tdeck_canvas.valid()) {
        Serial.println("[TDeck] warning: could not allocate frame buffer");
    }

    // Mount the microSD card and load /settings.yaml (device settings plus
    // contact aliases/favourites). A missing card simply leaves defaults.
    tdeck_sd_init();
    tdeck_settings_load();
    tdeck_apply_brightness();

    tdeck_ui_ready = true;
    tdeck_dirty = true;
    tdeck_last_tick = millis();
    tdeck_ui_go_home();
}

void tdeck_ui_loop() {
    if (!tdeck_ui_ready) return;

  #ifdef HAS_RNS
    // Deferred RNS setup: the Reticulum instance starts later in setup(),
    // so the UI's destinations + announce handler are registered once it does.
    if (!tdeck_rns_ready && reticulum && RNS::Transport::identity()) {
        tdeck_rns_setup();
    }
    tdeck_rns_ping_poll();
    if (tdeck_rns_ready && tdeck_set_announce_interval > 0) {
        double now = RNS::Utilities::OS::time();
        if ((now - tdeck_last_announce_at) >= tdeck_set_announce_interval * 60.0) {
            tdeck_rns_announce_ui();
        }
    }
  #endif

    // Poll keyboard (the BBQ10/C3 latch key events until read)
    TDeckKeyboard::KeyEvent key_event = tdeck_kb.keyEvent();
    if (key_event.key != '\0' || key_event.state != KB_KEY_STATE_IDLE) {
        tdeck_ui_handle_key(key_event.key, key_event.state);
        tdeck_kb.clearInterruptStatus();
        tdeck_dirty = true;
    }

    // Poll trackball
    tb_event_t tb_events = tdeck_trackball.readEvents();
    if (tb_events != TB_EVENT_NONE) {
        tdeck_ui_handle_trackball(tb_events);
        tdeck_dirty = true;
    }

    // Repaint on input events, or periodically so the status bar clock and
    // live screens (ping, battery) stay fresh. Full-frame pushes mean no
    // flicker.
    bool periodic = (millis() - tdeck_last_tick >= tdeck_tick_ms);
    if (periodic) {
        tdeck_last_tick = millis();
    }
    if (tdeck_dirty || periodic) {
        tdeck_dirty = false;
        tdeck_ui_draw();
    }
}

#endif // BOARD_MODEL == BOARD_TDECK
