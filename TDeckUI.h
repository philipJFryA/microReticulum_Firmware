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
//   * node name (truncated to 16 characters, drawn on the left)
//   * right-justified cluster:
//       - battery percentage
//       - time (RNS system clock, GPS-disciplined when a fix is held,
//         otherwise uptime when no real-time clock is present)
//       - GPS satellite icon (green = lock held, yellow = hardware
//         responding but no lock, red = hardware not responding/init failure)
//       - Reticulum cell tower (green = any traffic from another node
//         within the last 5 minutes, yellow = radio online but mesh quiet,
//         red = hardware failure)
//       - direct (1-hop) connection count, immediately right of the cell tower
//
// The GNSS is an L76K or other NMEA-0183 receiver on UART1 (GPIO 43/44)
// parsed by TDeckGPS.h; the fix is published to peers through the LXMF
// announce payload and, once its UTC epoch reaches RNS, drives the status
// bar clock.
//
// Four applications are provided, navigated with the trackball (scroll,
// press = enter, left = back, long press = home) and the keyboard
// (W/A/S/D = scroll up/left/down/right, Enter = confirm, Backspace =
// back/delete). Key-based navigation never exits a text field; only the
// trackball-left event leaves text input.
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
  // ROM miniz/tinfl raw-deflate inflater used by the SAR PNG tile decoder.
  // The ROM symbols are mapped at link time via esp32s3.rom.ld - no external
  // PNG/zlib library is needed.
  #include "rom/miniz.h"
#endif

#include "TDeckKeyboard.h"
// The trackball implementation (global singleton + ISR pointer) is defined
// only here - TDeckUI.h is included from exactly one translation unit
// (RNode_Firmware.ino), so the symbols are defined exactly once.
#define TDECK_TRACKBALL_IMPLEMENTATION
#include "TDeckTrackball.h"
// The GPS driver implementation (global singleton) is likewise defined
// exactly once here.
#define TDECK_GPS_IMPLEMENTATION
#include "TDeckGPS.h"
#include "LXMF.h"

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
// The messages app registers the standard LXMF delivery destination so it is
// interoperable with Sideband, NomadNet, MeshChat and all other LXMF clients.
// See https://github.com/markqvist/lxmf
#define TDECK_UI_RNS_APP          LXMF_APP_NAME
#define TDECK_UI_RNS_ASPECT_MSGS  LXMF_DELIVERY_ASPECT
#define TDECK_UI_RNS_ASPECT_PING "ping"
#define TDECK_UI_RNS_ASPECT_BCAST "broadcast"

// Simple line-based payload framing for the ping/trace helpers (not part of
// the LXMF protocol - these are UI-internal diagnostics).
#define TDECK_FRAME_PING "!mrping!"   // !mrping!<seq>!<sent_at>!<sender_pubkey>!<sender_dest_hash>
#define TDECK_FRAME_PONG "!mrpong!"   // !mrpong!<seq>!<sent_at>
#define TDECK_FRAME_BCAST "!mrbcast!" // !mrbcast!<sender_hash>!<text>

// ---- SAR (Situational Awareness) app ------------------------------------------
// The SAR app shares periodic encrypted position beacons with a configured set
// of peer nodes over the "tdeck_ui.sar.position" aspect, and renders a map of
// the local mesh on the T-Deck screen.
//
// Map tiles are stored on the microSD card using the standard XYZ (slippy map)
// scheme under /tiles/{zoom}/{x}/{y}.png. Standard XYZ tile PNGs are decoded
// directly on the ESP32-S3 using the ROM's miniz/tinfl inflate (no external
// PNG library dependency); the x/y coordinates are standard Web Mercator
// (OSM-style) tile indices. Tiles can be provisioned over USB serial or the
// WebSocket console, or fetched over WiFi (when connected) from any
// XYZ-compatible tile server.
#define TDECK_SAR_RNS_ASPECT    "sar.position"
#define TDECK_SAR_FRAME_POS     "!mrsar!"   // !mrsar!<sender_hash>!<lat>!<lon>!<alt>!<sat>!<age>
#define TDECK_SAR_MAX_PEERS     8
#define TDECK_SAR_MAX_TRACKS    24
#define TDECK_SAR_TRACK_TIMEOUT (60*30)     // seconds; drop tracks not updated
#define TDECK_SAR_DEFAULT_INTERVAL 60       // seconds between beacons
#define TDECK_SAR_TILE_ROOT     "/tiles"
#define TDECK_SAR_TILE_EXT      ".png"
#define TDECK_TILE_SIZE         256
#define TDECK_SAR_MIN_ZOOM      3
#define TDECK_SAR_MAX_ZOOM      18
#define TDECK_SAR_MAP_X         2           // map viewport origin (below status bar)
#define TDECK_SAR_MAP_Y         (TDECK_SBAR_H + 2)
#define TDECK_SAR_MAP_W         (TDECK_SCREEN_W - 2*TDECK_SAR_MAP_X)
#define TDECK_SAR_MAP_H         (TDECK_SCREEN_H - TDECK_SAR_MAP_Y - TDECK_FOOT_H - 26 - 2)

// Broadcast conversation sentinel peer (not a contact hash: "bcast" is not
// a 32-char hex string, so it can never collide with a real identity hash).
#define TDECK_BCAST_PEER "bcast"

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

// Defined in RNode_Firmware.ino after this header is included; forward
// declared here so tdeck_ui_debug() can consult it.
extern bool kiss_framed_logs;

// Route diagnostic text through the same output path as firmware logs so it
// never interleaves as unframed bytes into a KISS-aware host stream when
// kiss_framed_logs is enabled (it defaults to false on embedded builds).
static void tdeck_ui_debug(const char* msg) {
  if (kiss_framed_logs) {
    kiss_indicate_log(msg, strlen(msg));
  } else {
    Serial.println(msg);
  }
  Serial.flush();
}

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
// Forward declarations for the radio bootstrap in tdeck_ui_loop().
// startRadio(), update_radio_lock() and tx_queue_handler() are defined in
// RNode_Firmware.ino after this header is included, so they must be
// declared here.
bool startRadio();
void update_radio_lock();
void tx_queue_handler();
// packet queue depth (defined in RNode_Firmware.ino after this header).
extern volatile uint8_t queue_height;
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
  uint32_t last_seen_ms;       // millis() of last announce heard (direct-count)
  float lat;                   // last announced position (0 = unknown)
  float lon;
  float alt;
  uint8_t sat;
  float age_s;
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
static int16_t  tdeck_set_tz_offset_min      = 0;    // minutes from UTC (e.g. -480 EST)
static uint16_t tdeck_set_announce_interval  = 0;    // minutes; 0 = off
static bool     tdeck_set_confirm_send       = false;
static bool     tdeck_settings_dirty         = false;

// Valid SX126x signal bandwidth options (Hz), used by the radio BW editor.
static const uint32_t tdeck_bw_options[] = {
    7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000
};
#define TDECK_BW_OPTIONS (sizeof(tdeck_bw_options) / sizeof(tdeck_bw_options[0]))

// ---- Runtime state -----------------------------------------------------------
static bool     tdeck_ui_ready       = false;
static bool     tdeck_dirty          = true;
static bool     tdeck_sd_ready       = false;
static bool     tdeck_gps_fix        = false;  // updated from tdeck_gps in the loop
static bool     tdeck_gps_present    = false;  // set when a GNSS is detected on UART1
static bool     tdeck_gps_time_set   = false;  // GNSS UTC epoch applied to RNS clock
static bool     tdeck_gps_announced  = false;  // first-fix announce already sent
static uint32_t tdeck_last_tick      = 0;
static uint32_t tdeck_tick_ms        = 1000;
static uint32_t tdeck_ui_start_ms    = 0;      // set once in tdeck_ui_init()

// Screens
enum {
  SCREEN_HOME = 0,
  SCREEN_MESSAGES,
  SCREEN_CONTACTS,
  SCREEN_RETICULUM,
  SCREEN_SETTINGS,
  SCREEN_SAR,
};

// Per-app views
enum { MSG_INBOX = 0, MSG_THREAD, MSG_COMPOSE, MSG_PICK };
enum { CT_LIST = 0, CT_DETAIL, CT_ALIAS };
enum { RET_MENU = 0, RET_PING_PICK, RET_PING_RUN, RET_TRACE_PICK,
       RET_TRACE_RUN, RET_DIRECT, RET_IDENTITY, RET_NAME_EDIT };
enum { SET_LIST = 0, SET_EDIT };
// SAR: map view, peer list, contact picker, SAR config list, value editor
enum { SAR_MAP = 0, SAR_PEERS, SAR_PICK, SAR_CFG, SAR_CFG_EDIT };

static uint8_t tdeck_screen = SCREEN_HOME;
static uint8_t tdeck_sub    = 0;
static uint8_t tdeck_cursor = 0;
static uint8_t tdeck_scroll = 0;

// ---- SAR (Situational Awareness) state ---------------------------------------
// Beacon peers are contacts the user has explicitly selected to share
// encrypted position beacons with. The list is persisted to settings.yaml
// as the "sar_peers:" section.
static char     tdeck_sar_peers[TDECK_SAR_MAX_PEERS][40];
static uint8_t  tdeck_sar_peer_count   = 0;
static uint16_t tdeck_sar_interval     = TDECK_SAR_DEFAULT_INTERVAL; // seconds
static uint8_t  tdeck_sar_send_enabled = 1;

// Map view state (Web Mercator / XYZ).
static int64_t  tdeck_sar_center_lat_e7 = 0;  // centre position, 1e7 fixed point
static int64_t  tdeck_sar_center_lon_e7 = 0;
static uint8_t  tdeck_sar_zoom          = 14;
static int32_t  tdeck_sar_pan_px_x      = 0;  // pan offset from the tile origin
static int32_t  tdeck_sar_pan_px_y      = 0;
static bool     tdeck_sar_has_centre    = false;

// Position tracks (per beacon peer, most recent position). The beacon frame
// carries an age so the renderer can grey stale nodes.
typedef struct {
  char     hash[40];
  float    lat;
  float    lon;
  double   last_heard;       // RNS unix time
  uint32_t last_heard_ms;    // millis() of last beacon
  uint32_t seq;              // sender's beacon sequence
} tdeck_sar_track_t;
static tdeck_sar_track_t tdeck_sar_tracks[TDECK_SAR_MAX_PEERS];
static uint8_t tdeck_sar_track_count = 0;

// SAR beacon pacing (millis-based so it works before GPS-disciplined clock).
static uint32_t tdeck_sar_last_beacon_ms = 0;
static uint32_t tdeck_sar_last_ui_action_ms = 0; // pan/zoom pacing

// SAR debug: last tile path attempted and whether it loaded. Shown on the
// map view (bottom strip) and logged over serial to help diagnose SD-card
// layout / tile provisioning issues.
static char    tdeck_sar_debug_path[80] = "";
static bool    tdeck_sar_debug_ok       = false;
static uint8_t tdeck_sar_debug_miss     = 0;   // consecutive cache-miss count

// Inbox scratch for the SAR peer picker (contact indices to choose from).
static uint8_t tdeck_sar_pick_contact_idx = 0;

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

// SAR contact picker scratch (index of the currently browsed contact)
static uint8_t tdeck_sar_picker_cursor = 0;
static uint8_t tdeck_sar_picker_scroll = 0;
// SAR "Add peer" contact picker (separate from the browse list above)
static uint8_t tdeck_sar_pick_contact_cursor = 0;
static uint8_t tdeck_sar_pick_contact_scroll = 0;

// ---- RNS integration state ---------------------------------------------------
#ifdef HAS_RNS
static RNS::Destination tdeck_rns_msg_dest({RNS::Type::NONE});
static RNS::Destination tdeck_rns_ping_dest({RNS::Type::NONE});
static RNS::Destination tdeck_rns_bcast_dest({RNS::Type::NONE});
static RNS::Destination tdeck_rns_sar_dest({RNS::Type::NONE});
static RNS::HAnnounceHandler tdeck_rns_ann_handler;
static bool tdeck_rns_ready = false;
static double tdeck_last_announce_at = 0.0;
static uint32_t tdeck_last_rx_count = 0;    // last seen RNS::Transport::packets_received()
static uint32_t tdeck_last_rx_ms    = 0;    // millis() of last packet received from another node
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
                                       const RNS::Bytes& payload);
static bool     tdeck_rns_send_broadcast(const char* text);
static void     tdeck_rns_ping_contact(uint8_t idx);
static void     tdeck_rns_ping_poll();
static void     tdeck_rns_trace_contact(uint8_t idx);
#ifdef HAS_RNS
static uint8_t  tdeck_direct_count();
#endif

static void     tdeck_ui_draw_status_bar(TDeckCanvas& c);
static void     tdeck_ui_draw_footer(TDeckCanvas& c, const char* hint);
static void     tdeck_ui_draw_row(TDeckCanvas& c, uint8_t y, const char* text,
                                  bool selected);
static uint8_t  tdeck_ui_list_frame(TDeckCanvas& c, const char* title, uint8_t n,
                                    uint8_t cursor, uint8_t* scroll);
static void     tdeck_ui_draw_home();
static void     tdeck_ui_draw_messages();
static void     tdeck_ui_draw_contacts();
static void     tdeck_ui_draw_reticulum();
static void     tdeck_ui_draw_reticulum_rest();
static void     tdeck_ui_draw_settings();
static void     tdeck_ui_draw_text_input(const char* title, const char* buf);
static void     tdeck_ui_draw_sar();
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

// Announce app_data from Reticulum peers is not always a plain display name:
// Sideband-style announces can carry binary metadata/control bytes, a newline,
// and then the actual human-readable name. Extract a clean printable name:
// the content of the last line (after any embedded newlines), with control
// characters stripped and surrounding whitespace trimmed.
static void tdeck_name_clean(const char* raw, char* out, size_t n) {
    if (!raw) { if (n > 0) out[0] = '\0'; return; }
    // The human-readable name follows the final newline; everything before it
    // is metadata/garbage (matches "garbage on first line, name on second").
    const char* last = raw;
    for (const char* p = raw; *p; p++) {
        if (*p == '\n') last = p + 1;
    }
    size_t o = 0;
    for (const char* p = last; *p && o < n - 1; p++) {
        char ch = *p;
        if (ch == '\r' || ch == '\n') continue;
        if (ch >= 0x20 && ch <= 0x7E) out[o++] = ch;  // printable ASCII only
        // control / high bytes are dropped
    }
    while (o > 0 && out[o-1] == ' ') o--;
    out[o] = '\0';
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
    if (app_data && app_data[0]) {
        char clean[TDECK_APP_DATA_MAX+1];
        tdeck_name_clean(app_data, clean, sizeof(clean));
        if (clean[0]) strncpy(c->app_data, clean, sizeof(c->app_data)-1);
    }
    if (last_seen > c->last_seen) c->last_seen = last_seen;
}

// Best display name for a contact: alias, else announced name, else hash.
// The announced name is cleaned at display time so pre-existing persisted
// names (saved before the ingest sanitizer existed) still render correctly.
static const char* tdeck_contact_label(const tdeck_contact_t* c, char* buf, size_t n) {
    if (!c) { snprintf(buf, n, "(none)"); return buf; }
    if (c->alias[0]) { snprintf(buf, n, "%s", c->alias); return buf; }
    if (c->app_data[0]) {
        char clean[TDECK_APP_DATA_MAX+1];
        tdeck_name_clean(c->app_data, clean, sizeof(clean));
        if (clean[0]) { snprintf(buf, n, "%s", clean); return buf; }
    }
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
        // Compose: broadcast to all nodes, or send to tdeck_input_peer.
        if (tdeck_input_len == 0) {
            tdeck_ui_nav_back();
            return;
        }
        bool is_bcast = (strcmp(tdeck_input_peer, TDECK_BCAST_PEER) == 0);
        tdeck_contact_t* c = is_bcast ? NULL : tdeck_contact_find(tdeck_input_peer);
      #ifdef HAS_RNS
        if (is_bcast) {
            if (tdeck_rns_send_broadcast(tdeck_input_buf)) {
                tdeck_msg_add(TDECK_BCAST_PEER, tdeck_input_buf, false);
                tdeck_toast_set("Broadcast sent");
            } else {
                tdeck_toast_set("Send failed");
                return;
            }
        } else if (!c || !c->has_key) {
            tdeck_toast_set("No key for contact");
            return;
        } else {
            const RNS::Identity& sender = RNS::Transport::identity();
            const RNS::Destination& msg_local_dest = tdeck_rns_msg_dest;
            if (!sender || !msg_local_dest) {
                tdeck_toast_set("No identity");
                return;
            }
            // Build a standard LXMF message addressed to the peer's
            // "lxmf.delivery" destination. The destination hash in the LXMF
            // header is the peer's announced "lxmf.delivery" destination
            // (computed from their public key), and the source hash is our
            // own "lxmf.delivery" destination hash. The payload is the
            // mandatory msgpack [timestamp, title, content, fields] list.
            RNS::Bytes dest_raw;
            uint8_t raw_dest[16];
            if (c->dest_hash[0] && tdeck_hex_decode(c->dest_hash, raw_dest, 16)) {
                dest_raw = RNS::Bytes(raw_dest, 16);
            } else {
                // Fall back to the deterministic lxmf.delivery hash computed
                // from the contact's public key — this is what Sideband
                // addresses when the announce destination is unknown.
                uint8_t key[64];
                if (!tdeck_hex_decode(c->pubkey, key, 64)) {
                    tdeck_toast_set("No key for contact");
                    return;
                }
                RNS::Identity rid(false);
                rid.load_public_key(RNS::Bytes(key, 64));
                dest_raw = RNS::Destination::hash(rid, LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);
            }

            // The source hash is our own lxmf.delivery destination hash.
            RNS::Bytes source_raw = msg_local_dest.hash();

            RNS::Bytes lxmf_packed;
            if (!LXMF::LXMessage::pack(lxmf_packed, dest_raw, source_raw,
                                        sender, tdeck_input_buf)) {
                tdeck_toast_set("Pack failed");
                return;
            }
            // LXMF messages are delivered to a SINGLE (opportunistic)
            // destination. The Python reference implementation's
            // LXMessage.__as_packet() sends self.packed[DESTINATION_LENGTH:]
            // for opportunistic delivery — the destination hash is NOT
            // included in the packet data, because the receiver's
            // delivery_packet() handler prepends packet.destination.hash
            // when reconstructing the full LXMF wire format. Our own
            // receive path (tdeck_rns_on_message) models the same rule.
            // Strip the 16-byte destination hash prefix so the remote
            // LXMF client doesn't double-prepend it (which misaligns the
            // msgpack payload and breaks unpacking with "object of type
            // 'int' has no len()").
            const size_t lxmf_dest_len = RNS::Type::Reticulum::DESTINATION_LENGTH;
            RNS::Bytes lxmf_opportunistic;
            if (lxmf_packed.size() >= lxmf_dest_len) {
                lxmf_opportunistic = lxmf_packed.mid(lxmf_dest_len);
            } else {
                lxmf_opportunistic = lxmf_packed;
            }
            if (tdeck_rns_send_payload(c, TDECK_UI_RNS_ASPECT_MSGS,
                                       lxmf_opportunistic)) {
                tdeck_msg_add(c->hash, tdeck_input_buf, false);
                tdeck_toast_set("Message sent");
            } else {
                tdeck_toast_set("Send failed");
                return;
            }
        }
      #else
        (void)c;
        (void)is_bcast;
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
        tdeck_ui_debug("[TDeck] SD card ready");
        return true;
    }
    tdeck_ui_debug("[TDeck] SD card not found (settings will not persist)");
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
    if (app_data && app_data[0]) {
        char clean[TDECK_APP_DATA_MAX+1];
        tdeck_name_clean(app_data, clean, sizeof(clean));
        if (clean[0]) strncpy(c->app_data, clean, sizeof(c->app_data)-1);
    }
}

// Strip surrounding quotes from a YAML scalar value (in place at *start).
// Loops so already-corrupted values with multiple accumulated quote pairs
// (""""MyNode"""") are fully unwrapped rather than one pair per load.
static void tdeck_yaml_strip_quotes(const char** start) {
    const char* s = *start;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len >= 2 && ((s[0] == '"' && s[len-1] == '"') || (s[0] == '\'' && s[len-1] == '\''))) {
        char* tmp = (char*)s;
        tmp[len-1] = '\0';
        s++;
        len -= 2;
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
                tdeck_yaml_strip_quotes(&vs);
                strncpy(tdeck_set_device_name, vs, sizeof(tdeck_set_device_name)-1);
                tdeck_set_device_name[sizeof(tdeck_set_device_name)-1] = '\0';
            } else if (k == "brightness") {
                tdeck_set_brightness = (uint8_t)constrain(atoi(vs), 0, 255);
            } else if (k == "blank_timeout") {
                tdeck_set_blank_timeout = (uint16_t)constrain(atoi(vs), 0, 3600);
            } else if (k == "gps_enabled") {
                tdeck_set_gps_enabled = (v == "true");
            } else if (k == "timezone_offset") {
                tdeck_set_tz_offset_min = (int16_t)constrain(atoi(vs), -1440, 1440);
            } else if (k == "announce_interval") {
                tdeck_set_announce_interval = (uint16_t)constrain(atoi(vs), 0, 1440);
            } else if (k == "confirm_send") {
                tdeck_set_confirm_send = (v == "true");
            } else if (k == "radio_freq") {
                lora_freq = (uint32_t)constrain(atol(vs), 100000000L, 1000000000L);
            } else if (k == "radio_bw") {
                lora_bw = (uint32_t)constrain(atol(vs), 7800L, 500000L);
            } else if (k == "radio_sf") {
                lora_sf = constrain(atoi(vs), 5, 12);
            } else if (k == "radio_cr") {
                lora_cr = constrain(atoi(vs), 5, 8);
            } else if (k == "radio_txp") {
                lora_txp = constrain(atoi(vs), -9, 22);
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
    f.printf("timezone_offset: %d\n", tdeck_set_tz_offset_min);
    f.printf("announce_interval: %u\n", tdeck_set_announce_interval);
    f.printf("confirm_send: %s\n", tdeck_set_confirm_send ? "true" : "false");
    f.printf("radio_freq: %u\n", lora_freq);
    f.printf("radio_bw: %u\n", lora_bw);
    f.printf("radio_sf: %d\n", lora_sf);
    f.printf("radio_cr: %d\n", lora_cr);
    f.printf("radio_txp: %d\n", lora_txp);
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

// Decode the optional location element appended by other T-Deck UIs to
// their LXMF announce app_data:
//   [display_name, stamp_cost, [supported_features], [lat, lon, alt_m, sat, age_s]]
// The location element is a msgpack fixarray5 of 3 floats, a positive int
// (satellites), and a float (age seconds). Returns true and fills the outputs
// when a well-formed location element is found.
static bool tdeck_announce_location(const uint8_t* data, size_t size,
                                    float& lat, float& lon, float& alt,
                                    uint8_t& sat, float& age_s) {
    if (!data || size < 3) return false;
    LXMF::MsgPackReader reader(data, size);

    size_t count = 0;
    if (!reader.read_array_header(count) || count < 5) return false;

    // Element 0: display name (bin/str) — skip.
    std::vector<uint8_t> name;
    if (!reader.read_bin(name)) return false;

    // Element 1: stamp_cost (nil) — skip.
    if (!reader.skip_object()) return false;

    // Element 2: [supported_functionality] — skip.
    if (!reader.skip_object()) return false;

    // Element 3: [lat, lon, alt_m, sat, age_s].
    size_t loc_count = 0;
    if (!reader.read_array_header(loc_count) || loc_count < 5) return false;

    double lat_d = 0, lon_d = 0, alt_d = 0, age_d = 0;
    if (!reader.read_double(lat_d) || !reader.read_double(lon_d) ||
        !reader.read_double(alt_d)) return false;

    // Satellites: MsgPackReader.read_int handles both fixint and int8/16/32.
    int64_t sat_i = 0;
    if (!reader.read_int(sat_i)) return false;
    if (sat_i < 0 || sat_i > 255) return false;

    if (!reader.read_double(age_d)) return false;

    lat = (float)lat_d;
    lon = (float)lon_d;
    alt = (float)alt_d;
    sat = (uint8_t)sat_i;
    age_s = (float)age_d;
    return true;
}

// Fired by the Transport for every announce heard on the mesh. Populates the
// contact store with the announced identity (hash + public key + name).
//
// Sideband (and Reticulum generally) treats the announced *destination* hash
// as the peer's addressable identity; the recalled Identity hash is the
// cryptographic identity behind it. When Transport cannot recall the identity
// for a destination (e.g. a non-T-Deck peer whose announce arrives before our
// persisted identity store has been seeded, or a peer that announces a
// destination not backed by a recallable identity), fall back to registering
// the contact under the destination hash itself so announces always add a
// contact regardless.
static void tdeck_rns_on_announce(const RNS::Bytes& dst_hash, const RNS::Identity& id,
                                  const RNS::Bytes& app_data) {
    if (dst_hash.size() != RNS::Type::Reticulum::DESTINATION_LENGTH) return;
    char hash[40] = {0}, dest[40] = {0}, pub[140] = {0}, appd[TDECK_APP_DATA_MAX+1] = {0};

    std::string dh = dst_hash.toHex();
    strncpy(dest, dh.c_str(), sizeof(dest)-1);

    if (id) {
        std::string h = id.hash().toHex();
        strncpy(hash, h.c_str(), sizeof(hash)-1);
        std::string pk = id.get_public_key().toHex();
        strncpy(pub, pk.c_str(), sizeof(pub)-1);
    } else {
        // No recallable identity: register the contact under the announced
        // destination hash (what Sideband shows as the peer address).
        strncpy(hash, dest, sizeof(hash)-1);
    }
    // Peer location (if the announce carried our optional 4th element):
    //   [name, stamp, sf_list, [lat, lon, alt_m, sat, age_s]]
    float peer_lat = 0.0f, peer_lon = 0.0f, peer_alt = 0.0f, peer_age = 0.0f;
    uint8_t peer_sat = 0;
    bool has_loc = false;

    if (app_data.size() > 0) {
        // LXMF announces encode the display name as the first element of a
        // msgpack array: [display_name, stamp_cost, [supported_features]].
        // Decode it - if the data isn't a valid LXMF announce array, fall
        // back to the legacy raw-text handling for plain Reticulum peers.
        std::vector<uint8_t> lxmf_name;
        bool lxmf_decoded = LXMF::announce_display_name(app_data.data(),
                                                        app_data.size(),
                                                        lxmf_name);
        if (lxmf_decoded) {
            size_t n = lxmf_name.size();
            if (n > TDECK_APP_DATA_MAX) n = TDECK_APP_DATA_MAX;
            if (n > 0) {
                memcpy(appd, lxmf_name.data(), n);
                appd[n] = '\0';
            }
        } else {
            std::string ad = app_data.toString();
            strncpy(appd, ad.c_str(), sizeof(appd)-1);
        }

        // Decode the optional 4th announce element (location array).
        has_loc = tdeck_announce_location(app_data.data(), app_data.size(),
                                          peer_lat, peer_lon, peer_alt,
                                          peer_sat, peer_age);
    }
    tdeck_contact_upsert(hash, dest, pub[0] ? pub : NULL, appd, RNS::Utilities::OS::time());
    tdeck_contact_t* up = tdeck_contact_find(hash);
    if (up) up->last_seen_ms = millis();

    // Store the decoded position on the contact.
    if (has_loc) {
        tdeck_contact_t* c = tdeck_contact_find(hash);
        if (c) {
            c->lat  = peer_lat;
            c->lon  = peer_lon;
            c->alt  = peer_alt;
            c->sat  = peer_sat;
            c->age_s = peer_age;
        }
    }
    tdeck_dirty = true;
}

// Packet callback for the local "lxmf.delivery" IN destination.
//
// Per the LXMF specification, an opportunistic single-packet message carries
// the following wire layout (the destination hash is prepended from the
// packet header by the receiver, matching LXMRouter.delivery_packet):
//   dest_hash(16) || source_hash(16) || signature(64) ||
//   msgpack([timestamp, title, content, fields])
//
// The signature is verified with the sender's public key recalled via RNS
// from the source hash. Unverifiable messages are accepted but flagged as
// unverified (source identity unknown), mirroring the LXMF reference
// implementation's SOURCE_UNKNOWN / SIGNATURE_INVALID handling.
//
// The same handler is registered both as the destination's packet callback
// (for opportunistic single-packet delivery) and as the packet callback for
// incoming links established to this destination (for link-delivered LXMF,
// which is how Sideband / NomadNet / standard LXMF clients send messages
// when a path and stable link are available). Link-delivered payloads have
// the same wire layout minus the destination hash prefix (the link already
// addresses the peer), so the same reconstruction applies.
static void tdeck_rns_on_message(const RNS::Bytes& data, const RNS::Packet& packet) {
    // Acknowledge the packet to the sender. This sends a cryptographic proof
    // back through RNS, which the sender's delivery callback interprets as a
    // delivery confirmation (mirrors LXMRouter.delivery_packet which calls
    // packet.prove() on every LXMF delivery). The Packet callback receives a
    // const Packet, so we use the const Identity::prove() entry point.
    const RNS::Identity& local_id = RNS::Transport::identity();
    if (local_id) local_id.prove(packet);

    // The LXMF reference implementation prepends the packet's destination
    // hash to the payload for opportunistic delivery, because the receiver
    // knows the destination from the packet header (and the sender saves
    // bandwidth by not duplicating it). For a packet delivered to our
    // SINGLE lxmf.delivery destination, prepend our destination hash to
    // reconstruct the full LXMF wire format.
    //
    // Link-delivered messages (Sideband / NomadNet style) already carry the
    // full LXMF wire format including the destination hash, because the link
    // itself carries no addressing. Detect that case and skip the prefix.
    RNS::Bytes lxmf_data;
    const size_t dest_len = RNS::Type::Reticulum::DESTINATION_LENGTH;
    if (data.size() >= 2*dest_len + LXMF::SIGNATURE_LENGTH + 2 &&
        data.left(dest_len) == tdeck_rns_msg_dest.hash()) {
        // Already has the destination hash prefix (link-delivered message).
        lxmf_data.append(data);
    } else {
        // Opportunistic single-packet delivery: prepend the destination hash.
        lxmf_data.append(tdeck_rns_msg_dest.hash());
        lxmf_data.append(data);
    }

    LXMF::LXMessage msg;
    if (!LXMF::LXMessage::unpack(msg, lxmf_data)) return;

    // Extract the sender identity hash (hex) from the LXMF source hash.
    std::string source_hex = msg.source_hash.toHex();

    // Attempt to recall the sender identity for signature verification.
    // The source hash is the sender's lxmf.delivery destination hash;
    // RNS can recall the identity from the announce that carried it.
    char sender[40] = {0};
    strncpy(sender, source_hex.c_str(), sizeof(sender)-1);

    RNS::Identity source_identity = {RNS::Type::NONE};
    // We may not be able to recall the identity if the announce was heard
    // but the identity store isn't seeded yet. In that case, accept the
    // message without signature verification (matches LXMF's tolerance of
    // SOURCE_UNKNOWN messages when the destination identity is unknown).
    bool signature_valid = false;
    if (msg.source_hash) {
        source_identity = RNS::Identity::recall(msg.source_hash);
        if (source_identity) {
            signature_valid = LXMF::LXMessage::verify(msg, source_identity);
        }
    }

    // Upsert the contact under the signed identity if we could verify it,
    // otherwise under the source hash.
    char contact_hash[40] = {0};
    if (signature_valid && source_identity) {
        std::string ih = source_identity.hash().toHex();
        strncpy(contact_hash, ih.c_str(), sizeof(contact_hash)-1);
    } else {
        strncpy(contact_hash, sender, sizeof(contact_hash)-1);
    }

    // The sender's announced destination hash is the LXMF source hash
    // (their lxmf.delivery destination). Store it for reply routing.
    tdeck_contact_upsert(contact_hash, sender, NULL, NULL, 0);

    // If we recalled the identity, we have the public key - persist it so
    // replies can be encrypted.
    if (source_identity) {
        std::string pk = source_identity.get_public_key().toHex();
        tdeck_contact_t* cnt = tdeck_contact_find(contact_hash);
        if (cnt) {
            strncpy(cnt->pubkey, pk.c_str(), sizeof(cnt->pubkey)-1);
            cnt->has_key = true;
        }
    }

    // Extract message content as text.
    char text[TDECK_MSG_MAX_LEN+1];
    size_t content_len = msg.content.size();
    if (content_len > TDECK_MSG_MAX_LEN) content_len = TDECK_MSG_MAX_LEN;
    if (content_len > 0) {
        memcpy(text, msg.content.data(), content_len);
    }
    text[content_len] = '\0';

    tdeck_msg_add(contact_hash, text, true);

    // Surface the incoming message in the UI with a toast so the user gets
    // immediate feedback even when they are not on the Messages screen.
    tdeck_contact_t* cnt = tdeck_contact_find(contact_hash);
    if (cnt) {
        char label[48];
        tdeck_contact_label(cnt, label, sizeof(label));
        tdeck_toast_set("Msg from %s", label);
    } else {
        tdeck_toast_set("Message received");
    }

    tdeck_dirty = true;
}

// Packet callback for the local "tdeck_ui.broadcast" PLAIN destination.
// Broadcasts are unencrypted; the sender announces itself via the
// "!mrbcast!" frame's sender hash field. Messages land in a single shared
// "Broadcast" conversation (TDECK_BCAST_PEER) so the inbox stays clean.
static void tdeck_rns_on_broadcast(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();
    if (s.rfind(TDECK_FRAME_BCAST, 0) != 0) return;
    const char* p = s.c_str() + strlen(TDECK_FRAME_BCAST);

    char sender[40];
    p = tdeck_rns_field(p, 0, sender, sizeof(sender));
    if (!p || !sender[0]) return;

    // Don't echo our own broadcasts back into the thread (the outbound copy
    // is already added locally by tdeck_input_apply).
    const RNS::Identity& id = RNS::Transport::identity();
    if (id && strcmp(sender, id.hash().toHex().c_str()) == 0) return;

    char text[TDECK_MSG_MAX_LEN+1];
    strncpy(text, p, sizeof(text)-1);
    text[sizeof(text)-1] = '\0';

    tdeck_contact_upsert(sender, NULL, NULL, NULL, 0);
    tdeck_msg_add(TDECK_BCAST_PEER, text, true);
    tdeck_dirty = true;
}

// Packet callback for the local "tdeck_ui.ping" IN destination: answers ping
// requests and collects pong replies for the in-flight ping.
static void tdeck_rns_on_ping(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();

    if (s.rfind(TDECK_FRAME_PING, 0) == 0) {
        char seq[16], sent[24], pub[140], sender_dest[40];
        const char* p = s.c_str() + strlen(TDECK_FRAME_PING);
        p = tdeck_rns_field(p, 0, seq, sizeof(seq));
        if (!p) return;
        p = tdeck_rns_field(p, 0, sent, sizeof(sent));
        if (!p) return;
        p = tdeck_rns_field(p, 0, pub, sizeof(pub));
        if (!p || strlen(pub) != 128) return;
        // sender_dest is the final field: tdeck_rns_field returns NULL for the
        // last field (no trailing '!') but still copies the bytes, so we must
        // NOT treat a NULL return as failure here.
        tdeck_rns_field(p, 0, sender_dest, sizeof(sender_dest));
        if (strlen(sender_dest) != 32) return;

        // Reply to whoever pinged us: build a destination addressed to the
        // announced destination the sender listens on (carried in frame) and
        // encrypted with the sender's public key (also carried in frame) so
        // the sender's SINGLE destination can decrypt the PONG.
        uint8_t key[64], dest_raw[16];
        if (!tdeck_hex_decode(pub, key, 64)) return;
        if (!tdeck_hex_decode(sender_dest, dest_raw, 16)) return;
        RNS::Identity rid(false);
        rid.load_public_key(RNS::Bytes(key, 64));
        RNS::Destination out(rid, RNS::Type::Destination::OUT,
                             RNS::Type::Destination::SINGLE, RNS::Bytes(dest_raw, 16));
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

// --- SAR (Situational Awareness) RNS integration -----

// True when `send_enabled` and at least one configured peer exists.
static bool tdeck_sar_active() {
    return tdeck_sar_send_enabled && tdeck_sar_peer_count > 0;
}

// Find the index of a configured SAR beacon peer by hash, or -1.
static int8_t tdeck_sar_peer_index(const char* hash) {
    if (!hash) return -1;
    for (uint8_t i = 0; i < tdeck_sar_peer_count; i++) {
        if (strcmp(tdeck_sar_peers[i], hash) == 0) return (int8_t)i;
    }
    return -1;
}

// Packet callback for the local "tdeck_ui.sar.position" IN destination.
// Receives encrypted position beacons from configured peers:
//   !mrsar!<sender_hash>!<lat>!<lon>!<alt>!<sat>!<age_s>
// Frame is line-based text (encrypted by RNS for the SINGLE destination),
// so data.toString() is safe.
static void tdeck_rns_on_sar_position(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();
    if (s.rfind(TDECK_SAR_FRAME_POS, 0) != 0) return;
    const char* p = s.c_str() + strlen(TDECK_SAR_FRAME_POS);

    char sender[40];
    p = tdeck_rns_field(p, 0, sender, sizeof(sender));
    if (!p || strlen(sender) != 32) return;

    char lat_s[24], lon_s[24], alt_s[24], sat_s[16], age_s[24];
    p = tdeck_rns_field(p, 0, lat_s, sizeof(lat_s));
    if (!p) return;
    p = tdeck_rns_field(p, 0, lon_s, sizeof(lon_s));
    if (!p) return;
    p = tdeck_rns_field(p, 0, alt_s, sizeof(alt_s));
    if (!p) return;
    p = tdeck_rns_field(p, 0, sat_s, sizeof(sat_s));
    if (!p) return;
    tdeck_rns_field(p, 0, age_s, sizeof(age_s));   // last field (may be empty)

    float lat = strtof(lat_s, NULL);
    float lon = strtof(lon_s, NULL);
    if (lat == 0.0f && lon == 0.0f) return;         // not a real position

    // Upsert the sender into the contact store (beacons also teach peers).
    tdeck_contact_upsert(sender, NULL, NULL, NULL, 0);

    // Update (or create) the track for this sender.
    tdeck_sar_track_t* tr = NULL;
    for (uint8_t i = 0; i < tdeck_sar_track_count; i++) {
        if (strcmp(tdeck_sar_tracks[i].hash, sender) == 0) { tr = &tdeck_sar_tracks[i]; break; }
    }
    if (!tr) {
        if (tdeck_sar_track_count >= TDECK_SAR_MAX_PEERS) return;
        tr = &tdeck_sar_tracks[tdeck_sar_track_count++];
        memset(tr, 0, sizeof(*tr));
        strncpy(tr->hash, sender, sizeof(tr->hash)-1);
    }
    tr->lat = lat;
    tr->lon = lon;
    tr->last_heard = RNS::Utilities::OS::time();
    tr->last_heard_ms = millis();

    // Centre the map on the first position received.
    if (!tdeck_sar_has_centre) {
        tdeck_sar_has_centre = true;
        tdeck_sar_center_lat_e7 = (int64_t)(lat * 10000000.0);
        tdeck_sar_center_lon_e7 = (int64_t)(lon * 10000000.0);
        tdeck_sar_pan_px_x = 0;
        tdeck_sar_pan_px_y = 0;
    }
    tdeck_dirty = true;
}

// Drop stale tracks whose beacon age exceeds TDECK_SAR_TRACK_TIMEOUT.
static void tdeck_sar_expire_tracks() {
    for (uint8_t i = 0; i < tdeck_sar_track_count; ) {
        if (millis() - tdeck_sar_tracks[i].last_heard_ms > (uint32_t)TDECK_SAR_TRACK_TIMEOUT * 1000) {
            memmove(&tdeck_sar_tracks[i], &tdeck_sar_tracks[i+1],
                    sizeof(tdeck_sar_track_t) * (tdeck_sar_track_count - i - 1));
            tdeck_sar_track_count--;
        } else {
            i++;
        }
    }
}

// Send our current GPS fix to every configured SAR peer (encrypted via the
// deterministic per-peer, per-aspect destination). Does nothing without a
// fresh fix, configured peers, or when transmissions are disabled.
static void tdeck_rns_sar_beacon() {
    if (!tdeck_rns_ready) return;
    if (!tdeck_sar_active()) return;
    if (!tdeck_gps_present || !tdeck_gps_fix) return;
    float lat = tdeck_gps.latitude();
    float lon = tdeck_gps.longitude();
    if (lat == 0.0f && lon == 0.0f) return;

    const RNS::Identity& id = RNS::Transport::identity();
    if (!id) return;

    char frame[160];
    uint32_t age = (millis() - tdeck_gps.lastFixMs()) / 1000;
    snprintf(frame, sizeof(frame), "%s%s!%.5f!%.5f!%.0f!%u!%lu",
             TDECK_SAR_FRAME_POS, id.hash().toHex().c_str(),
             (double)lat, (double)lon, (double)tdeck_gps.altitude(),
             (unsigned)tdeck_gps.satellites(), (unsigned long)age);

    RNS::Bytes payload(frame);
    uint8_t sent = 0;
    for (uint8_t i = 0; i < tdeck_sar_peer_count; i++) {
        tdeck_contact_t* c = tdeck_contact_find(tdeck_sar_peers[i]);
        if (c && tdeck_rns_send_payload(c, TDECK_SAR_RNS_ASPECT, payload)) sent++;
    }
    if (sent > 0) tdeck_sar_last_beacon_ms = millis();
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

// Link-established callback for the local "lxmf.delivery" IN destination.
// The reference LXMF implementation (Sideband, NomadNet, rnsh, …) delivers
// messages over an established Link once a path is known, rather than as
// opportunistic single packets. Without a packet callback on the incoming
// link, Link::receive() drops the decrypted DATA payload (its
// CONTEXT_NONE handler only fires callbacks registered on the link). Hook
// the same LXMF delivery handler that the destination's packet callback
// uses, so link-delivered messages reach the UI identically.
static void tdeck_rns_on_link_established(RNS::Link& link) {
    link.set_packet_callback(tdeck_rns_on_message);
}

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
    tdeck_rns_msg_dest.set_link_established_callback(tdeck_rns_on_link_established);
    tdeck_rns_msg_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    tdeck_rns_ping_dest = RNS::Destination(id, RNS::Type::Destination::IN,
                                           RNS::Type::Destination::SINGLE,
                                           TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_PING);
    tdeck_rns_ping_dest.set_packet_callback(tdeck_rns_on_ping);
    tdeck_rns_ping_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    // Shared PLAIN broadcast destination: any node running this UI can
    // transmit unencrypted broadcasts to every other UI node in range
    // without needing their public key first. PLAIN destinations cannot
    // hold an identity (Destination.cpp throws std::invalid_argument),
    // so register with a null identity — the destination hash is then
    // derived from the app+aspect name alone, giving every UI node the
    // same unencrypted channel. PLAIN destinations also cannot be
    // announced (Destination::announce throws), so listeners discover
    // the channel implicitly by registering the same name.
    tdeck_rns_bcast_dest = RNS::Destination(RNS::Identity({RNS::Type::NONE}),
                                            RNS::Type::Destination::IN,
                                            RNS::Type::Destination::PLAIN,
                                            TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_BCAST);
    tdeck_rns_bcast_dest.set_packet_callback(tdeck_rns_on_broadcast);
    tdeck_rns_bcast_dest.set_proof_strategy(RNS::Type::Destination::PROVE_NONE);

    // Encrypted SAR position-beacon destination (per-node identity). Peers
    // that have this node's public key (from the announce) can send us
    // encrypted position beacons via the deterministic per-peer aspect hash.
    tdeck_rns_sar_dest = RNS::Destination(id, RNS::Type::Destination::IN,
                                          RNS::Type::Destination::SINGLE,
                                          TDECK_UI_RNS_APP, TDECK_SAR_RNS_ASPECT);
    tdeck_rns_sar_dest.set_packet_callback(tdeck_rns_on_sar_position);
    tdeck_rns_sar_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    tdeck_rns_ann_handler = RNS::HAnnounceHandler(new TDeckAnnounceHandler());
    RNS::Transport::register_announce_handler(tdeck_rns_ann_handler);

    tdeck_rns_announce_ui();
    tdeck_rns_sync_contacts();
    tdeck_toast_set("RNS ready");
    tdeck_dirty = true;
}

// Send an unencrypted broadcast to the shared "tdeck_ui.broadcast" PLAIN
// destination. The sender's identity hash is embedded in the frame so
// receiving UIs can upsert the caller into their contact store.
static bool tdeck_rns_send_broadcast(const char* text) {
    if (!tdeck_rns_ready || !tdeck_rns_bcast_dest) return false;
    const RNS::Identity& id = RNS::Transport::identity();
    if (!id) return false;

    char frame[TDECK_MSG_MAX_LEN + 64];
    snprintf(frame, sizeof(frame), "%s%s!%s", TDECK_FRAME_BCAST,
             id.hash().toHex().c_str(), text);
    RNS::Destination out({RNS::Type::NONE}, RNS::Type::Destination::OUT,
                         RNS::Type::Destination::PLAIN,
                         TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_BCAST);
    RNS::Packet(out, RNS::Bytes(frame)).send();
    return true;
}

// Re-announce the UI destinations (used at startup and when the name changes).
// PLAIN broadcast destinations cannot be announced, so only the SINGLE
// destinations are re-announced here.
//
// The LXMF announce app_data is msgpack-encoded as:
//   [display_name, stamp_cost, [supported_functionality], location]
// matching LXMRouter.get_announce_app_data() in the reference implementation
// (first three elements). The optional location element is appended only when
// a fresh GPS fix is held, so standard LXMF clients (Sideband, NomadNet …)
// still parse the announce unchanged — they read the first three elements and
// ignore the rest — while other T-Deck UIs decode position from element 3.
// The location payload is a compact non-fixed array: [lat, lon, alt_m, sat, age_s].
static void tdeck_rns_announce_ui() {
    if (!tdeck_rns_ready) return;

    // Build LXMF-compatible announce app_data:
    //   [display_name(bin/str), stamp_cost(nil), [SF_COMPRESSION]]
    arduino::msgpack::Packer packer;
    packer.reserve_buffer(128);
    std::vector<uint8_t> name(tdeck_set_device_name,
                              tdeck_set_device_name + strlen(tdeck_set_device_name));
    // stamp_cost is nil (no stamping required); supported functionality
    // advertises SF_COMPRESSION like the reference implementation.
    arduino::msgpack::object::nil_t stamp_cost;
    std::vector<uint8_t> sf_list = {LXMF::SF_COMPRESSION};
    packer.to_array(name, stamp_cost, sf_list);
    RNS::Bytes app_data(packer.data(), packer.size());

    // Append a location element when a fresh fix is held: the final announce
    // array becomes [name, stamp, sf_list, loc], where loc is the compact
    // [lat, lon, alt_m, sat, age_s] array. Standard LXMF clients read only
    // the first three elements and ignore the rest, so interoperability is
    // preserved while other T-Deck UIs decode position from element 3.
    // The msgpack fixarray3 header (0x93) produced by to_array() is simply
    // replaced with a fixarray4 header (0x94) and the loc element bytes are
    // concatenated — msgpack arrays are flat element lists, so this is the
    // exact wire representation the reference implementation would emit.
    if (tdeck_gps_present && tdeck_gps_fix) {
        float lat = tdeck_gps.latitude();
        float lon = tdeck_gps.longitude();
        float alt = tdeck_gps.altitude();
        uint8_t sat = tdeck_gps.satellites();
        if (lat != 0.0f || lon != 0.0f) {
            // age = seconds since the fix was last updated.
            uint32_t age = (uint32_t)((millis() - tdeck_gps.lastFixMs()) / 1000);

            // location element bytes: fixarray5 [lat, lon, alt, sat, age_s].
            // Values are cast to double so the MsgPack library emits float64
            // (packFloat64), which the receiver's MsgPackReader::read_double
            // decodes — read_double has no float32 branch.
            arduino::msgpack::Packer loc;
            loc.reserve_buffer(32);
            loc.to_array((double)lat, (double)lon, (double)alt,
                         sat, (double)age);

            std::vector<uint8_t> out;
            out.reserve(app_data.size() + loc.size() + 1);
            out.push_back(0x94);                                    // fixarray4
            // Copy the three base element bytes (skip the 0x93 header).
            if (app_data.size() > 0) {
                out.insert(out.end(), app_data.data() + 1, app_data.data() + app_data.size());
            }
            // Append the location array element bytes.
            out.insert(out.end(), loc.data(), loc.data() + loc.size());

            app_data = RNS::Bytes(out.data(), out.size());
        }
    }

    if (tdeck_rns_msg_dest)  tdeck_rns_msg_dest.announce(app_data);
    if (tdeck_rns_ping_dest) tdeck_rns_ping_dest.announce(app_data);
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

// Send an arbitrary payload to a contact's destination for `aspect`.
// The destination is the deterministic per-identity hash of `<app>.<aspect>`
// — the exact hash the peer registered and announced. For messages this is
// the standard LXMF "lxmf.delivery" destination, making the T-Deck
// interoperable with all LXMF clients. The ping aspect is a UI-internal
// diagnostic destination.
//
// The payload is passed as RNS::Bytes (not const char*) because LXMF wire
// data is binary and may contain embedded NUL bytes; the old str-based
// overload would truncate at the first zero byte.
static bool tdeck_rns_send_payload(const tdeck_contact_t* c, const char* aspect,
                                   const RNS::Bytes& payload) {
    if (!tdeck_rns_ready) return false;
    if (!c || !c->has_key || !c->pubkey[0]) return false;
    uint8_t key[64];
    if (!tdeck_hex_decode(c->pubkey, key, 64)) return false;

    RNS::Identity rid(false);
    rid.load_public_key(RNS::Bytes(key, 64));
    // Deterministic per-peer, per-aspect destination hash. Both ends
    // compute the same value from the same public key and aspect, so frames
    // address exactly the destination the peer registered and announced.
    RNS::Bytes aspect_hash = RNS::Destination::hash(rid, TDECK_UI_RNS_APP, aspect);
    if (aspect_hash.size() != RNS::Type::Reticulum::DESTINATION_LENGTH) return false;

    // Encrypt with the contact's public key (their SINGLE destination requires
    // it) and address to the aspect hash.
    RNS::Destination out(rid, RNS::Type::Destination::OUT,
                         RNS::Type::Destination::SINGLE, aspect_hash);
    RNS::Packet(out, payload).send();
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

    const RNS::Identity& self = RNS::Transport::identity();
    const RNS::Destination& local_dest = tdeck_rns_ping_dest;
    if (!self || !local_dest) {
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "No identity");
        return;
    }
    // Carry our announced destination hash so the responder can route the
    // PONG back to the exact destination we are listening on.
    char frame[256 + 40];
    snprintf(frame, sizeof(frame), "%s%08x!%.3f!%s!%s", TDECK_FRAME_PING, tdeck_ping_seq,
             RNS::Utilities::OS::time(), self.get_public_key().toHex().c_str(),
             local_dest.hash().toHex().c_str());
    if (!tdeck_rns_send_payload(c, TDECK_UI_RNS_ASPECT_PING, RNS::Bytes(frame))) {
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

// ---- SAR map rendering -------------------------------------------------------
// Web Mercator (EPSG:3857) projection helpers for XYZ/slippy map tiles.
// The world fits in one 256x256 tile at zoom 0. Coordinates use the standard
// OSM formula; lat/lon are in degrees.
static int64_t tdeck_sar_lon_to_tile_x(double lon, uint8_t zoom) {
    if (lon <= -180.0) lon = -180.0;
    if (lon >= 180.0)  lon = 180.0;
    double n = pow(2.0, (double)zoom);
    return (int64_t)floor((lon + 180.0) / 360.0 * n);
}
static int64_t tdeck_sar_lat_to_tile_y(double lat, uint8_t zoom) {
    if (lat <= -85.05112878) lat = -85.05112878;
    if (lat >=  85.05112878) lat =  85.05112878;
    double n = pow(2.0, (double)zoom);
    double lat_rad = lat * (PI / 180.0);
    return (int64_t)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) / 2.0 * n);
}
static double tdeck_sar_tile_x_to_lon(int64_t x, uint8_t zoom) {
    double n = pow(2.0, (double)zoom);
    return x / n * 360.0 - 180.0;
}
static double tdeck_sar_tile_y_to_lat(int64_t y, uint8_t zoom) {
    double n = pow(2.0, (double)zoom);
    double lat_rad = atan(sinh(PI * (1.0 - 2.0 * y / n)));
    return lat_rad * (180.0 / PI);
}

// Convert a lat/lon to a pixel offset within the whole Web Mercator world at
// the given zoom (0,0 = top-left of the world tile).
static void tdeck_sar_latlon_to_world_px(double lat, double lon, uint8_t zoom,
                                         double& px, double& py) {
    double n = pow(2.0, (double)zoom);
    double tx = (lon + 180.0) / 360.0 * n;
    double lat_rad = lat * (PI / 180.0);
    double ty = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) / 2.0 * n;
    px = tx * TDECK_TILE_SIZE;
    py = ty * TDECK_TILE_SIZE;
}

// Convert a world pixel back to lat/lon.
static void tdeck_sar_world_px_to_latlon(double px, double py, uint8_t zoom,
                                         double& lat, double& lon) {
    double n = pow(2.0, (double)zoom);
    double tx = px / (double)TDECK_TILE_SIZE;
    double ty = py / (double)TDECK_TILE_SIZE;
    lon = tx / n * 360.0 - 180.0;
    double lat_rad = atan(sinh(PI * (1.0 - 2.0 * ty / n)));
    lat = lat_rad * (180.0 / PI);
}

// Decode a 24-bit RGB BMP tile (legacy layout; used as a fallback for SD
// cards provisioned before the PNG switch). Rows are bottom-up and padded to
// 4 bytes, each pixel is 3 bytes BGR.
static bool tdeck_sar_decode_bmp(const char* path, uint16_t* out) {
    if (!tdeck_sd_ready) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    // BMP header: 2 (magic) + 12 (DIB header fields we need) ...
    uint8_t hdr[18];
    if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { f.close(); return false; }
    uint32_t data_offset = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
                           ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    if (data_offset < sizeof(hdr)) data_offset = sizeof(hdr);
    f.seek(data_offset);

    const uint16_t pad = (4 - (TDECK_TILE_SIZE * 3) % 4) % 4;
    uint8_t* row = (uint8_t*)malloc(TDECK_TILE_SIZE * 3 + 3);
    if (!row) { f.close(); return false; }
    for (int32_t yy = TDECK_TILE_SIZE - 1; yy >= 0; yy--) {
        size_t got = f.read(row, TDECK_TILE_SIZE * 3);
        if (got != TDECK_TILE_SIZE * 3) { free(row); f.close(); return false; }
        if (pad) f.read(row + TDECK_TILE_SIZE * 3, pad);
        uint16_t* dst = out + ((int32_t)yy * TDECK_TILE_SIZE);
        for (uint16_t xx = 0; xx < TDECK_TILE_SIZE; xx++) {
            uint8_t b = row[xx * 3 + 0];
            uint8_t g = row[xx * 3 + 1];
            uint8_t r = row[xx * 3 + 2];
            uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            dst[xx] = (rgb565 >> 8) | (rgb565 << 8);   // big-endian for GFX
        }
    }
    free(row);
    f.close();
    return true;
}

// Decode a PNG tile (256x256) from the SD card into the given 256x256 RGB565
// buffer. Returns true on success. The canvas is RGB565 big-endian (Adafruit
// drawRGBBitmap order), so each pixel is byte-swapped before storing.
//
// PNG decoding uses the ESP32-S3 ROM's miniz/tinfl raw-deflate inflater
// (tinfl_decompress_mem_to_mem, mapped from the ROM at runtime via
// esp32s3.rom.ld) - no external PNG library is needed. Standard XYZ tile
// PNGs (8-bit RGB, RGBA, palette or grayscale) are handled directly.
static bool tdeck_sar_decode_png(const char* path, uint16_t* out) {
    if (!tdeck_sd_ready) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    // The whole PNG file is read into a heap buffer (usually a few KB for a
    // 256x256 OSM tile). The buffer is heap-allocated (not a big stack array)
    // so the RNS low-memory watchdog never trips on deep call chains (LoRa RX
    // interrupt path → map renderer).
    uint32_t fsize = f.size();
    if (fsize < 64 || fsize > (512 * 1024)) { f.close(); return false; }
    uint8_t* file = (uint8_t*)malloc(fsize);
    if (!file) { f.close(); return false; }
    if (f.read(file, fsize) != fsize) { free(file); f.close(); return false; }
    f.close();

    // ---- PNG signature + IHDR ----
    static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (fsize < 24 || memcmp(file, png_magic, 8) != 0) { free(file); return false; }

    // IHDR: 4-byte length + "IHDR" + 13 bytes payload
    uint32_t ihdr_len = ((uint32_t)file[8] << 24) | ((uint32_t)file[9] << 16) |
                        ((uint32_t)file[10] << 8) | (uint32_t)file[11];
    if (ihdr_len < 13 || file[12] != 'I' || file[13] != 'H' ||
        file[14] != 'D' || file[15] != 'R') { free(file); return false; }
    uint32_t png_w = ((uint32_t)file[16] << 24) | ((uint32_t)file[17] << 16) |
                     ((uint32_t)file[18] << 8) | (uint32_t)file[19];
    uint32_t png_h = ((uint32_t)file[20] << 24) | ((uint32_t)file[21] << 16) |
                     ((uint32_t)file[22] << 8) | (uint32_t)file[23];
    uint8_t bit_depth = file[24];
    uint8_t color_type = file[25];
    uint8_t compression = file[26];
    uint8_t filter_method = file[27];
    uint8_t interlace = file[28];

    if (png_w != TDECK_TILE_SIZE || png_h != TDECK_TILE_SIZE) { free(file); return false; }
    if (bit_depth != 8) { free(file); return false; }
    // OSM/XYZ tiles: 0 = grey, 2 = RGB, 3 = palette, 4 = grey+alpha, 6 = RGBA
    if (color_type != 0 && color_type != 2 && color_type != 3 &&
        color_type != 4 && color_type != 6) { free(file); return false; }
    if (compression != 0 || filter_method != 0 || interlace != 0) { free(file); return false; }

    // ---- Parse chunks to collect PLTE/tRNS and concatenated IDAT ----
    uint8_t palette[768] = {0};
    uint8_t palette_alpha[256] = {0};
    bool has_plte = false;
    bool has_trans = false;
    uint8_t* idat = NULL;         // concatenated IDAT payloads (zlib stream)
    uint32_t idat_total = 0;      // bytes currently in `idat`
    uint32_t idat_cap = 0;        // allocated size of `idat`
    uint32_t pos = 8;

    while (pos + 8 <= fsize) {
        uint32_t clen = ((uint32_t)file[pos] << 24) | ((uint32_t)file[pos + 1] << 16) |
                        ((uint32_t)file[pos + 2] << 8) | (uint32_t)file[pos + 3];
        uint32_t ctype = ((uint32_t)file[pos + 4] << 24) | ((uint32_t)file[pos + 5] << 16) |
                         ((uint32_t)file[pos + 6] << 8) | (uint32_t)file[pos + 7];
        if (pos + 12 + clen > fsize) { free(file); free(idat); return false; }

        if (ctype == 0x504C5445) {  // "PLTE"
            if (clen > 768) { free(file); free(idat); return false; }
            memcpy(palette, file + pos + 8, clen);
            has_plte = true;
        } else if (ctype == 0x74524E53) {  // "tRNS"
            if (clen > 256) { free(file); free(idat); return false; }
            memcpy(palette_alpha, file + pos + 8, clen);
            has_trans = true;
        } else if (ctype == 0x49444154) {  // "IDAT" - append payload
            // All IDAT chunks are consecutive by spec; concatenate them all
            // into one zlib stream before inflating.
            if (idat_total + clen < idat_total) { free(file); free(idat); return false; } // overflow
            if (idat_total + clen > idat_cap) {
                uint32_t new_cap = idat_cap ? idat_cap * 2 : 4096;
                while (new_cap < idat_total + clen) new_cap *= 2;
                uint8_t* nidat = (uint8_t*)realloc(idat, new_cap);
                if (!nidat) { free(file); free(idat); return false; }
                idat = nidat;
                idat_cap = new_cap;
            }
            memcpy(idat + idat_total, file + pos + 8, clen);
            idat_total += clen;
        }
        pos += 12 + clen;
    }

    if (idat_total == 0) { free(file); free(idat); return false; }

    // ---- zlib header validation only (tinfl parses it itself) ----
    if (idat_total < 2) { free(file); free(idat); return false; }
    // Verify the zlib header: CMF & FLG (0x78 0x01/0x5E/0x9C/0xDA)
    if ((idat[0] & 0x0F) != 8 || (idat[0] >> 4) > 7) { free(file); free(idat); return false; }
    if (((idat[0] * 256 + idat[1]) % 31) != 0) { free(file); free(idat); return false; }
    const uint8_t* inflate_src = idat;
    uint32_t inflate_len = idat_total;

    // Raw scanline size (pre-filter) for 8-bit channels.
    uint8_t channels = 1;
    if (color_type == 2) channels = 3;       // RGB
    else if (color_type == 4) channels = 2;  // grayscale + alpha
    else if (color_type == 6) channels = 4;  // RGBA
    uint16_t row_bpp = (uint16_t)(TDECK_TILE_SIZE * channels);
    uint32_t out_bytes = (uint32_t)(row_bpp + 1) * TDECK_TILE_SIZE;

    uint8_t* raw = (uint8_t*)malloc(out_bytes);
    if (!raw) { free(file); free(idat); return false; }

  #ifdef ESP_PLATFORM
    // Use the ROM miniz/tinfl to inflate the whole zlib stream (header +
    // deflate + adler32). TINFL_FLAG_PARSE_ZLIB_HEADER makes tinfl parse and
    // validate the 2-byte zlib header and the 4-byte adler checksum itself,
    // so we must NOT strip them here - stripping misaligns the deflate
    // bitstream and produces garbage pixels (rainbow static).
    size_t got = tinfl_decompress_mem_to_mem(raw, out_bytes,
                                             inflate_src, inflate_len,
                                             TINFL_FLAG_PARSE_ZLIB_HEADER |
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (got != out_bytes) { free(raw); free(file); free(idat); return false; }
  #else
    // Non-ESP32 fallback: PNG tiles are not supported (BMP was used on
    // platforms without the ROM inflater).
    free(raw); free(file); free(idat); return false;
  #endif

    // ---- Unfilter + convert to RGB565 ----
    uint8_t bpp = channels;  // bytes per pixel for the Sub/Paeth filters
    uint8_t* prev = (uint8_t*)calloc(row_bpp, 1);
    if (!prev) { free(raw); free(file); free(idat); return false; }

    for (uint32_t y = 0; y < TDECK_TILE_SIZE; y++) {
        uint8_t* line = raw + (uint32_t)y * (row_bpp + 1);
        uint8_t filt = line[0];
        uint8_t* cur = line + 1;

        // Apply the PNG scanline filter in place (bpp = bytes per pixel).
        if (filt == 1) {  // Sub
            for (int32_t i = bpp; i < (int32_t)row_bpp; i++)
                cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
        } else if (filt == 2) {  // Up
            for (int32_t i = 0; i < (int32_t)row_bpp; i++)
                cur[i] = (uint8_t)(cur[i] + prev[i]);
        } else if (filt == 3) {  // Average
            for (int32_t i = 0; i < (int32_t)row_bpp; i++) {
                uint8_t left = (i >= bpp) ? cur[i - bpp] : 0;
                cur[i] = (uint8_t)(cur[i] + ((left + prev[i]) >> 1));
            }
        } else if (filt == 4) {  // Paeth
            for (int32_t i = 0; i < (int32_t)row_bpp; i++) {
                uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
                uint8_t b = prev[i];
                uint8_t c = (i >= bpp) ? prev[i - bpp] : 0;
                int32_t p = (int32_t)a + b - c;
                int32_t pa = abs(p - a);
                int32_t pb = abs(p - b);
                int32_t pc = abs(p - c);
                uint8_t pred;
                if (pa <= pb && pa <= pc) pred = a;
                else if (pb <= pc) pred = b;
                else pred = c;
                cur[i] = (uint8_t)(cur[i] + pred);
            }
        }

        // Convert this row's pixels to RGB565.
        uint16_t* dst = out + ((int32_t)y * TDECK_TILE_SIZE);
        for (uint32_t x = 0; x < TDECK_TILE_SIZE; x++) {
            uint8_t r, g, b;
            if (color_type == 0) {         // grayscale
                r = g = b = cur[x];
            } else if (color_type == 2) {  // RGB
                r = cur[x * 3 + 0];
                g = cur[x * 3 + 1];
                b = cur[x * 3 + 2];
            } else if (color_type == 3) {  // palette
                uint8_t idx = cur[x];
                if (has_trans && idx < 256 && palette_alpha[idx] == 0) {
                    r = g = b = 255;       // fully transparent -> white
                } else {
                    uint32_t base = (uint32_t)idx * 3;
                    if (has_plte && base + 2 < 768) {
                        r = palette[base + 0];
                        g = palette[base + 1];
                        b = palette[base + 2];
                    } else {
                        r = g = b = 0;
                    }
                }
            } else if (color_type == 4) {  // grayscale + alpha
                r = g = b = cur[x * 2];
                // ignore alpha (tiles are opaque)
            } else {                       // RGBA (6)
                r = cur[x * 4 + 0];
                g = cur[x * 4 + 1];
                b = cur[x * 4 + 2];
            }
            uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            dst[x] = (rgb565 >> 8) | (rgb565 << 8);   // big-endian for GFX
        }
        memcpy(prev, cur, row_bpp);
    }

    free(prev);
    free(raw);
    free(idat);
    free(file);
    return true;
}

// Render the map viewport onto the main canvas. The viewport shows the
// tiles around the current centre at tdeck_sar_zoom, shifted by the pan
// offset. Positions from the local GPS fix and peer tracks are overlaid.
static void tdeck_ui_draw_sar_map(TDeckCanvas& c) {
    // If we have no centre yet, fall back to the GPS fix if one exists.
    if (!tdeck_sar_has_centre && tdeck_gps_present && tdeck_gps_fix) {
        tdeck_sar_has_centre = true;
        tdeck_sar_center_lat_e7 = (int64_t)(tdeck_gps.latitude() * 10000000.0);
        tdeck_sar_center_lon_e7 = (int64_t)(tdeck_gps.longitude() * 10000000.0);
        tdeck_sar_pan_px_x = 0;
        tdeck_sar_pan_px_y = 0;
    }

    // Map background (dark).
    c.fillRect(TDECK_SAR_MAP_X, TDECK_SAR_MAP_Y, TDECK_SAR_MAP_W, TDECK_SAR_MAP_H, 0x0861);

    if (tdeck_sar_has_centre) {
        // World pixel of the centre at the current zoom.
        double cx_world, cy_world;
        tdeck_sar_latlon_to_world_px(tdeck_sar_center_lat_e7 / 10000000.0,
                                     tdeck_sar_center_lon_e7 / 10000000.0,
                                     tdeck_sar_zoom, cx_world, cy_world);
        // Viewport top-left world pixel = centre - half viewport + pan.
        double vp_x = cx_world - TDECK_SAR_MAP_W / 2.0 + tdeck_sar_pan_px_x;
        double vp_y = cy_world - TDECK_SAR_MAP_H / 2.0 + tdeck_sar_pan_px_y;

        int64_t t0x = (int64_t)floor(vp_x / TDECK_TILE_SIZE);
        int64_t t0y = (int64_t)floor(vp_y / TDECK_TILE_SIZE);
        int64_t t1x = (int64_t)floor((vp_x + TDECK_SAR_MAP_W) / TDECK_TILE_SIZE);
        int64_t t1y = (int64_t)floor((vp_y + TDECK_SAR_MAP_H) / TDECK_TILE_SIZE);

        // Tile cache (PSRAM-backed, reused across frames to avoid SD re-reads).
        // The 128 KB RGB565 tile buffer would otherwise live in internal
        // SRAM and starve the RNS heap (LOW-MEMORY watchdog reset) — a
        // lazy PSRAM allocation keeps it out of the 512 KB internal DRAM.
        static uint16_t* tile_cache = NULL;
        static int64_t   cache_tx = -1, cache_ty = -1;
        static uint8_t   cache_zoom = 0xFF;
        if (!tile_cache) {
          #ifdef ESP_PLATFORM
            tile_cache = (uint16_t*)heap_caps_malloc(
                TDECK_TILE_SIZE * TDECK_TILE_SIZE * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM);
          #endif
            if (!tile_cache) tile_cache = (uint16_t*)malloc(
                TDECK_TILE_SIZE * TDECK_TILE_SIZE * sizeof(uint16_t));
        }

        for (int64_t ty = t0y; ty <= t1y; ty++) {
            for (int64_t tx = t0x; tx <= t1x; tx++) {
                int16_t sx = (int16_t)(tx * TDECK_TILE_SIZE - (int64_t)vp_x);
                int16_t sy = (int16_t)(ty * TDECK_TILE_SIZE - (int64_t)vp_y);
                if (sx + TDECK_TILE_SIZE <= 0 || sx >= TDECK_SAR_MAP_W) continue;
                if (sy + TDECK_TILE_SIZE <= 0 || sy >= TDECK_SAR_MAP_H) continue;

                // Load (cache-miss only).
                if (cache_zoom != tdeck_sar_zoom || cache_tx != tx || cache_ty != ty) {
                    cache_zoom = tdeck_sar_zoom;
                    cache_tx = tx;
                    cache_ty = ty;
                    // Prefer the primary `.png` tile, fall back to the legacy
                    // `.bmp` layout for SD cards provisioned before the PNG
                    // switch (the firmware used to read 24-bit BGR BMPs).
                    char path[64];
                    snprintf(path, sizeof(path), "%s/%u/%lld/%lld%s",
                             TDECK_SAR_TILE_ROOT, (unsigned)tdeck_sar_zoom,
                             (long long)tx, (long long)ty, ".png");
                    bool tile_ok = false;
                    if (SD.exists(path)) {
                        tile_ok = tdeck_sar_decode_png(path, tile_cache);
                    }
                    if (!tile_ok) {
                        snprintf(path, sizeof(path), "%s/%u/%lld/%lld%s",
                                 TDECK_SAR_TILE_ROOT, (unsigned)tdeck_sar_zoom,
                                 (long long)tx, (long long)ty, ".bmp");
                        if (SD.exists(path)) {
                            tile_ok = tdeck_sar_decode_bmp(path, tile_cache);
                        }
                    }
                    // Update the on-screen + serial tile-load debug info.
                    strncpy(tdeck_sar_debug_path, path, sizeof(tdeck_sar_debug_path) - 1);
                    tdeck_sar_debug_path[sizeof(tdeck_sar_debug_path) - 1] = '\0';
                    tdeck_sar_debug_ok = tile_ok;
                    if (!tile_ok) {
                        tdeck_sar_debug_miss++;
                        if (tdeck_sar_debug_miss <= 4) {
                            char dbg[96];
                            snprintf(dbg, sizeof(dbg), "[SAR] tile missing: %s", path);
                            tdeck_ui_debug(dbg);
                        }
                    } else {
                        tdeck_sar_debug_miss = 0;
                    }
                    if (tile_ok) {
                        // draw only the visible portion
                        int16_t clip_x0 = (sx >= 0) ? 0 : -sx;
                        int16_t clip_y0 = (sy >= 0) ? 0 : -sy;
                        int16_t clip_x1 = TDECK_TILE_SIZE;
                        int16_t clip_y1 = TDECK_TILE_SIZE;
                        if (sx + TDECK_TILE_SIZE > TDECK_SAR_MAP_W)
                            clip_x1 = TDECK_SAR_MAP_W - sx;
                        if (sy + TDECK_TILE_SIZE > TDECK_SAR_MAP_H)
                            clip_y1 = TDECK_SAR_MAP_H - sy;
                        if (clip_x1 > clip_x0 && clip_y1 > clip_y0) {
                            for (int16_t yy = clip_y0; yy < clip_y1; yy++) {
                                for (int16_t xx = clip_x0; xx < clip_x1; xx++) {
                                    uint16_t px = tile_cache[(uint16_t)yy * TDECK_TILE_SIZE + (uint16_t)xx];
                                    c.drawPixel(sx + xx, TDECK_SAR_MAP_Y + sy + yy, px);
                                }
                            }
                        }
                    }
                }
            }
        }
    } else {
        // No position yet: show a hint.
        c.setTextSize(1);
        c.setTextColor(TDECK_COL_WARN);
        c.setCursor(TDECK_SAR_MAP_X + 8, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H / 2 - 4);
        c.print("No position yet - GPS or first beacon");
    }

    // Draw the local GPS position (blue crosshair) if we have a fix.
    if (tdeck_gps_present && tdeck_gps_fix && tdeck_sar_has_centre) {
        double wx, wy;
        tdeck_sar_latlon_to_world_px(tdeck_gps.latitude(), tdeck_gps.longitude(),
                                     tdeck_sar_zoom, wx, wy);
        int16_t sx = (int16_t)(wx - (tdeck_sar_center_lon_e7 / 10000000.0 > 0 ? // unused
                            0 : 0));  // placeholder to keep compile sane
        (void)sx;
        // Compute the same way as the viewport origin: centre world px minus
        // vp_x. Simpler: re-derive from the viewport world-px top-left.
        double cx_world, cy_world;
        tdeck_sar_latlon_to_world_px(tdeck_sar_center_lat_e7 / 10000000.0,
                                     tdeck_sar_center_lon_e7 / 10000000.0,
                                     tdeck_sar_zoom, cx_world, cy_world);
        double vp_x = cx_world - TDECK_SAR_MAP_W / 2.0 + tdeck_sar_pan_px_x;
        double vp_y = cy_world - TDECK_SAR_MAP_H / 2.0 + tdeck_sar_pan_px_y;
        int16_t lx = (int16_t)(wx - vp_x);
        int16_t ly = (int16_t)(wy - vp_y);
        if (lx >= 0 && lx < TDECK_SAR_MAP_W && ly >= 0 && ly < TDECK_SAR_MAP_H) {
            c.drawLine(lx - 4, ly, lx + 4, ly, ST77XX_BLUE);
            c.drawLine(lx, ly - 4, lx, ly + 4, ST77XX_BLUE);
            c.fillCircle(lx, ly, 2, ST77XX_WHITE);
        }
    }

    // Draw peer tracks (green markers, red when stale).
    for (uint8_t i = 0; i < tdeck_sar_track_count; i++) {
        tdeck_sar_track_t& tr = tdeck_sar_tracks[i];
        if (!tdeck_sar_has_centre) break;
        double wx, wy;
        tdeck_sar_latlon_to_world_px(tr.lat, tr.lon, tdeck_sar_zoom, wx, wy);
        double cx_world, cy_world;
        tdeck_sar_latlon_to_world_px(tdeck_sar_center_lat_e7 / 10000000.0,
                                     tdeck_sar_center_lon_e7 / 10000000.0,
                                     tdeck_sar_zoom, cx_world, cy_world);
        double vp_x = cx_world - TDECK_SAR_MAP_W / 2.0 + tdeck_sar_pan_px_x;
        double vp_y = cy_world - TDECK_SAR_MAP_H / 2.0 + tdeck_sar_pan_px_y;
        int16_t lx = (int16_t)(wx - vp_x);
        int16_t ly = (int16_t)(wy - vp_y);
        if (lx < -8 || lx >= TDECK_SAR_MAP_W + 8 || ly < -8 || ly >= TDECK_SAR_MAP_H + 8) continue;
        bool fresh = (millis() - tr.last_heard_ms) < 300000UL;  // 5 min
        uint16_t col = fresh ? ST77XX_GREEN : TDECK_COL_ERR;
        c.fillCircle(lx, ly, 4, col);
        c.drawCircle(lx, ly, 4, TDECK_COL_FG);
    }

    // Bottom info strip: zoom + peer count + last tile path debug.
    c.setTextSize(1);
    c.setTextColor(TDECK_COL_DIM);
    c.setCursor(TDECK_SAR_MAP_X, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H + 4);
    c.printf("Z%d  %u peers", (unsigned)tdeck_sar_zoom, (unsigned)tdeck_sar_peer_count);
    c.setCursor(TDECK_SAR_MAP_X + 100, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H + 4);
    if (tdeck_sar_send_enabled) {
        c.printf("beacon %us", (unsigned)tdeck_sar_interval);
    } else {
        c.print("beacon off");
    }

    // Debug line: last tile path the renderer tried to load. Coloured red on
    // a miss, green on a hit, so a broken SD layout is immediately visible.
    if (tdeck_sar_debug_path[0]) {
        c.setTextColor(tdeck_sar_debug_ok ? TDECK_COL_OK : TDECK_COL_ERR);
        c.setCursor(TDECK_SAR_MAP_X, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H + 14);
        c.print(tdeck_sar_debug_ok ? "OK: " : "MS: ");
        c.print(tdeck_sar_debug_path);
    }
}

static void tdeck_ui_draw_sar() {
    TDeckCanvas &c = tdeck_canvas;
    if (tdeck_sub == SAR_MAP) {
        tdeck_ui_draw_sar_map(c);
        tdeck_ui_draw_footer(c, "TB: pan  W/S: zoom  push: menu");
    } else if (tdeck_sub == SAR_PEERS) {
        uint8_t total = tdeck_sar_peer_count + 1;   // "+ Add peer"
        uint8_t rows = tdeck_ui_list_frame(c, "SAR Peers", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[48];
            if (idx == 0) {
                snprintf(line, sizeof(line), "+ Add peer");
            } else {
                tdeck_contact_t* cnt = tdeck_contact_find(tdeck_sar_peers[idx - 1]);
                tdeck_contact_label(cnt, line, sizeof(line));
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: add/remove  L: back");
    } else if (tdeck_sub == SAR_PICK) {
        // Contact picker: choose a contact to add as a beacon peer.
        uint8_t total = tdeck_contact_count;
        uint8_t rows = tdeck_ui_list_frame(c, "Add peer", total,
                                           tdeck_sar_pick_contact_cursor,
                                           &tdeck_sar_pick_contact_scroll);
        for (uint8_t r = 0; r < rows && tdeck_sar_pick_contact_scroll + r < total; r++) {
            uint8_t idx = tdeck_sar_pick_contact_scroll + r;
            char line[48];
            tdeck_contact_label(&tdeck_contacts[idx], line, sizeof(line));
            if (tdeck_sar_peer_index(tdeck_contacts[idx].hash) >= 0) {
                snprintf(line + strlen(line), sizeof(line) - strlen(line), " [added]");
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line,
                              idx == tdeck_sar_pick_contact_cursor);
        }
        if (total == 0) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts - wait for announces");
        }
        tdeck_ui_draw_footer(c, "select: add peer  L: back");
    } else if (tdeck_sub == SAR_CFG) {
        static const char* items[] = { "Beacon interval", "Send beacons",
                                       "Map zoom", "Recentre here" };
        uint8_t n = 4;
        uint8_t rows = tdeck_ui_list_frame(c, "SAR Config", n, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < n; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[56];
            if (idx == 0)      snprintf(line, sizeof(line), "Beacon interval: %us", (unsigned)tdeck_sar_interval);
            else if (idx == 1) snprintf(line, sizeof(line), "Send beacons: %s", tdeck_sar_send_enabled ? "on" : "off");
            else if (idx == 2) snprintf(line, sizeof(line), "Map zoom: %u", (unsigned)tdeck_sar_zoom);
            else               snprintf(line, sizeof(line), "Recentre here");
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: change  L: back");
    } else if (tdeck_sub == SAR_CFG_EDIT) {
        // Value editor for the selected config row (uses the existing
        // SET_EDIT-style layout). Row selectors map to 20/21/22 so the
        // shared tdeck_set_edit_change() SAR cases are reached without
        // colliding with the Settings app row indices 0-11.
        char label[32], value[40];
        bool action = false;
        switch (tdeck_set_edit_idx) {
            case 20: snprintf(label, sizeof(label), "Beacon interval"); snprintf(value, sizeof(value), "%us", (unsigned)tdeck_sar_interval); break;
            case 21: snprintf(label, sizeof(label), "Send beacons"); snprintf(value, sizeof(value), "%s", tdeck_sar_send_enabled ? "on" : "off"); break;
            case 22: snprintf(label, sizeof(label), "Map zoom"); snprintf(value, sizeof(value), "%u", (unsigned)tdeck_sar_zoom); break;
            default: snprintf(label, sizeof(label), ""); value[0] = '\0'; break;
        }
        (void)action;
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
        tdeck_ui_draw_footer(c, "SAR config");
    }
}

// ---- Drawing helpers ---------------------------------------------------------

// Small satellite glyph for the GPS status indicator. The whole icon (antenna
// tip + mast, central body, and two solar panels) is drawn in `color` so GPS
// state is conveyed by colour: green = lock held, yellow = hardware present
// but no lock, red = hardware not responding, dim = disabled.
static void tdeck_draw_gps_icon(TDeckCanvas& c, int16_t x, int16_t y, uint16_t color) {
    // Antenna mast + tip
    c.drawFastVLine(x, y - 3, 2, color);
    c.fillCircle(x, y - 4, 1, color);
    // Solar panels (left + right) joined to the body by a top bar
    c.fillRect(x - 4, y - 1, 3, 2, color);
    c.fillRect(x + 2, y - 1, 3, 2, color);
    c.drawFastHLine(x - 4, y - 1, 9, color);
    // Central body
    c.fillRect(x - 1, y - 1, 2, 3, color);
}

// Small cell-tower glyph for the Reticulum/LoRa mesh status indicator. The
// tower (three antenna stubs, mast, lattice legs and cross-ties) is drawn in
// `color` so mesh state is conveyed by colour: green = any traffic from
// another node heard within the last 5 minutes, yellow = radio online but
// mesh quiet, red = hardware failure, dim = not online yet.
static void tdeck_draw_cell_tower_icon(TDeckCanvas& c, int16_t x, int16_t y, uint16_t color) {
    // Three antenna stubs at the top
    c.drawFastHLine(x - 4, y - 4, 3, color);
    c.drawFastHLine(x - 1, y - 4, 3, color);
    c.drawFastHLine(x + 2, y - 4, 3, color);
    // Mast
    c.drawFastVLine(x, y - 3, 3, color);
    // Lattice legs forming a triangle
    c.drawLine(x - 3, y - 1, x, y + 2, color);
    c.drawLine(x + 3, y - 1, x, y + 2, color);
    // Cross-ties
    c.drawFastHLine(x - 2, y, 5, color);
    c.drawFastHLine(x - 3, y + 2, 7, color);
}

// Top status bar: node name (truncated to 16 chars) on the left; right-justified
// battery percentage, clock, a GPS satellite indicator, a Reticulum cell-tower
// indicator, and the direct (1-hop) connection count. Drawn on every screen.
static void tdeck_ui_draw_status_bar(TDeckCanvas& c) {
    c.fillRect(0, 0, TDECK_SCREEN_W, TDECK_SBAR_H, TDECK_COL_SBAR);
    c.drawFastHLine(0, TDECK_SBAR_H - 1, TDECK_SCREEN_W, TDECK_COL_DIM);
    c.setTextSize(1);

    // Node name (left, truncated to 16 characters)
    c.setTextColor(TDECK_COL_FG);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H / 2 - 4);
    char name[17];
    strncpy(name, tdeck_set_device_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    c.print(name);

    // Right-justified status cluster built right-to-left:
    //   [battery%] [clock] [GPS icon] [cell tower] [direct count]
    int x  = TDECK_SCREEN_W - TDECK_LIST_X;  // right edge
    int cy = TDECK_SBAR_H / 2;

    // Direct connection count -- immediately to the right of the cell tower.
    uint8_t direct = 0;
  #ifdef HAS_RNS
    direct = tdeck_direct_count();
  #endif
    char direct_txt[8];
    snprintf(direct_txt, sizeof(direct_txt), "%u", (unsigned)direct);
    x -= strlen(direct_txt) * 6;
    c.setTextColor(direct > 0 ? TDECK_COL_ACCENT : TDECK_COL_DIM);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(direct_txt);

    // Reticulum/LoRa cell tower: green = any traffic from another node
    // heard within the last 5 minutes, yellow = radio online but mesh quiet,
    // red = hardware failure, dim = radio not online yet.
    x -= 10;  // 9px icon + 1px gap before the count
    bool lora_failed = !modem_installed || (radio_error && !radio_online);
    bool lora_active = false;
  #ifdef HAS_RNS
    // millis()-based so activity works even before the RNS clock is
    // GPS-disciplined (OS::time() returns small uptime values until then).
    if (tdeck_last_rx_ms > 0 && (millis() - tdeck_last_rx_ms) < 300000UL) {
        lora_active = true;
    }
  #endif
    uint16_t lora_col;
    if (lora_failed) {
        lora_col = TDECK_COL_ERR;
    } else if (radio_online) {
        lora_col = lora_active ? TDECK_COL_OK : TDECK_COL_WARN;
    } else {
        lora_col = TDECK_COL_DIM;  // not online yet
    }
    tdeck_draw_cell_tower_icon(c, x, cy, lora_col);

    // GPS status: green = 3D lock, yellow = hardware responding but no lock,
    // red = hardware not responding (init failure), dim = disabled. Drawn as
    // a small satellite icon that takes on the state colour.
    x -= 10;  // 9px icon + 1px so a 3px gap remains before the LoRa dot
    uint16_t gps_col;
    if (!tdeck_set_gps_enabled) {
        gps_col = TDECK_COL_DIM;
    } else if (tdeck_gps_fix) {
        gps_col = TDECK_COL_OK;
    } else if (tdeck_gps_present ||
               (millis() - tdeck_ui_start_ms) < 5000) {
        gps_col = TDECK_COL_WARN;  // present but no lock, or still initialising
    } else {
        gps_col = TDECK_COL_ERR;
    }
    tdeck_draw_gps_icon(c, x, cy, gps_col);

    // Clock
    x -= 6;  // gap before the GPS icon
    char timestr[16];
    tdeck_ui_time_str(timestr, sizeof(timestr));
    x -= strlen(timestr) * 6;
    c.setTextColor(TDECK_COL_FG);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(timestr);

    // Battery percentage (right-justified, left of clock)
    char bat[8];
    if (battery_ready) {
        snprintf(bat, sizeof(bat), "%u%%", (uint8_t)(battery_percent + 0.5f));
    } else {
        snprintf(bat, sizeof(bat), "--%%");
    }
    x -= 6;  // gap before the clock
    x -= strlen(bat) * 6;
    c.setTextColor(TDECK_COL_FG);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(bat);
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
    // Compact 5-app grid: title/rule raised and rows tightened so every app
    // (including the 5th, SAR) fits between the status bar and the footer.
    c.setTextSize(2);
    c.setTextColor(TDECK_COL_ACCENT);
    c.setCursor(10, TDECK_SBAR_H + 6);
    c.print("microReticulum");
    c.drawFastHLine(0, TDECK_SBAR_H + 28, TDECK_SCREEN_W, TDECK_COL_DIM);

    static const char* apps[] = { "Messages", "Contacts", "Reticulum", "Settings", "SAR" };
    uint8_t napps = 5;
    for (uint8_t i = 0; i < napps; i++) {
        uint8_t y = TDECK_SBAR_H + 36 + i * 32;
        bool sel = (i == tdeck_cursor);
        if (sel) c.fillRect(0, y - 1, TDECK_SCREEN_W, 30, TDECK_COL_SEL);
        tdeck_ui_draw_icon(c, 14, y + 5, i, sel ? ST77XX_WHITE : TDECK_COL_ACCENT);
        c.setTextSize(2);
        c.setTextColor(sel ? ST77XX_WHITE : TDECK_COL_FG);
        c.setCursor(40, y + 5);
        c.print(apps[i]);
    }

    tdeck_ui_draw_footer(c, "WASD/TB: nav  Enter: select  Bksp: back");
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
                    if (strcmp(peer, TDECK_BCAST_PEER) == 0) {
                        snprintf(line, sizeof(line), "* Broadcast");
                    } else {
                        tdeck_contact_t* cnt = tdeck_contact_find(peer);
                        char label[40];
                        tdeck_contact_label(cnt, label, sizeof(label));
                        snprintf(line, sizeof(line), "%s%s", (cnt && cnt->favorite) ? "* " : "", label);
                    }
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
        const char* tlabel = (strcmp(tdeck_thread_peer, TDECK_BCAST_PEER) == 0)
                             ? "Broadcast"
                             : [&]() {
                                 tdeck_contact_t* cnt = tdeck_contact_find(tdeck_thread_peer);
                                 static char lbuf[48];
                                 tdeck_contact_label(cnt, lbuf, sizeof(lbuf));
                                 return lbuf;
                             }();
        c.print(tlabel);
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
        uint8_t total = tdeck_contact_count + 1;       // "+ Broadcast" first
        uint8_t rows = tdeck_ui_list_frame(c, "To", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[48];
            if (idx == 0) {
                snprintf(line, sizeof(line), "+ Broadcast");
            } else {
                tdeck_contact_label(&tdeck_contacts[idx - 1], line, sizeof(line));
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        if (total == 1) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts yet - broadcast to all UI nodes");
        }
        tdeck_ui_draw_footer(c, "select contact  L: back");
    } else if (tdeck_sub == MSG_COMPOSE) {
        tdeck_ui_draw_text_input("Message", tdeck_input_buf);
    }
}

// Full-screen text entry used by compose / alias / name editing.
static void tdeck_ui_draw_text_input(const char* title, const char* buf) {
    TDeckCanvas &c = tdeck_canvas;
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
    c.print("Enter: ok   Backspace: delete   L: back");
    tdeck_ui_draw_footer(c, "type on keyboard  L: back");
}

static void tdeck_ui_draw_contacts() {
    TDeckCanvas &c = tdeck_canvas;
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
            if (ct->app_data[0]) {
                char clean[TDECK_APP_DATA_MAX+1];
                tdeck_name_clean(ct->app_data, clean, sizeof(clean));
                c.setCursor(TDECK_LIST_X, y); c.print("Ann: "); c.print(clean); y += 12;
            }
            if (ct->has_key)  { c.setCursor(TDECK_LIST_X, y); c.print("Key: yes"); }
            else              { c.setCursor(TDECK_LIST_X, y); c.print("Key: no announce yet"); }
            y += 12;
            if (ct->lat != 0.0f || ct->lon != 0.0f) {
                c.setTextColor(TDECK_COL_OK);
                c.setCursor(TDECK_LIST_X, y);
                c.printf("Pos: %.4f, %.4f  %uSV", (double)ct->lat, (double)ct->lon, ct->sat);
                y += 12;
                c.setTextColor(TDECK_COL_DIM);
                c.setCursor(TDECK_LIST_X, y);
                c.printf("Alt: %.0f m  Age: %.0f s", (double)ct->alt, (double)ct->age_s);
                y += 12;
            }
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
// Count directly reachable destinations (1 hop): peers whose announce has been
// heard within the last 5 minutes, excluding our own echoed identity. On a
// LoRa broadcast medium anything heard is inherently one hop, so recent
// announces are the correct "direct connection" signal -- the RNS path table
// is not reliably populated by announces alone.
static uint8_t tdeck_direct_count() {
    uint8_t n = 0;
    const RNS::Identity& self = RNS::Transport::identity();
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        // Skip our own identity: our announces echo back and would otherwise
        // inflate the count.
        if (self && strcmp(tdeck_contacts[i].hash, self.hash().toHex().c_str()) == 0) continue;
        // Count nodes whose announce was heard within the last 5 minutes.
        // On a LoRa broadcast medium anything heard is a direct (1-hop)
        // neighbour, so a recent announce is the correct "direct connection"
        // signal -- the RNS path table is not reliably populated by
        // announces alone.
        if (tdeck_contacts[i].last_seen_ms > 0 &&
            (millis() - tdeck_contacts[i].last_seen_ms) < 300000UL) {
            n++;
        }
    }
    return n;
}

static bool tdeck_direct_at(uint8_t idx, char* out, size_t n, uint8_t* hops) {
    uint8_t seen = 0;
    const RNS::Identity& self = RNS::Transport::identity();
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        if (self && strcmp(tdeck_contacts[i].hash, self.hash().toHex().c_str()) == 0) continue;
        if (tdeck_contacts[i].last_seen_ms > 0 &&
            (millis() - tdeck_contacts[i].last_seen_ms) < 300000UL) {
            if (seen == idx) {
                strncpy(out, tdeck_contacts[i].hash, n - 1);
                out[n-1] = '\0';
                if (hops) *hops = 1;  // LoRa: anything heard is direct
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
    if (tdeck_sub == RET_MENU) {
        static const char* items[] = {
            "Ping contact", "Traceroute contact", "Direct contacts",
            "My identity", "Announce now"
        };
        uint8_t n = 5;
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
            c.setTextColor(TDECK_COL_ACCENT);
            c.setCursor(TDECK_LIST_X, y); c.print("LXMF: "); c.print(RNS::Destination::hash(id, "lxmf", "delivery").toHex().c_str()); y += 12;
            // The announced destination hashes are what remote peers (Sideband
            // etc.) actually see for this node. The identity hash on its own
            // cannot be derived back from a destination hash, so show the
            // announced destinations to explain the addresses reported on the
            // remote side of the mesh.
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, y);
            c.print("Announced:");
            y += 12;
            c.setTextColor(TDECK_COL_DIM);
            c.setCursor(TDECK_LIST_X, y);
            if (tdeck_rns_msg_dest) {
                c.print("msgs "); c.print(tdeck_rns_msg_dest.hash().toHex().c_str());
            } else {
                c.print("msgs (pending)");
            }
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            if (tdeck_rns_ping_dest) {
                c.print("ping "); c.print(tdeck_rns_ping_dest.hash().toHex().c_str());
            } else {
                c.print("ping (pending)");
            }
            y += 12;
            c.setTextColor(TDECK_COL_FG);
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
#define TDECK_SET_ROWS 15
static void tdeck_set_row(uint8_t idx, char* label, size_t n, char* value, size_t vn, bool* action) {
    switch (idx) {
        case 0: snprintf(label, n, "Node name");   snprintf(value, vn, "%s", tdeck_set_device_name); *action = true; break;
        case 1: snprintf(label, n, "Brightness");  snprintf(value, vn, "%u", tdeck_set_brightness);   *action = false; break;
        case 2: snprintf(label, n, "Blank timeout"); snprintf(value, vn, "%us", tdeck_set_blank_timeout); *action = false; break;
        case 3: snprintf(label, n, "Location Enabled"); snprintf(value, vn, "%s", tdeck_set_gps_enabled ? "yes" : "no"); *action = false; break;
        case 4: {
            snprintf(label, n, "Timezone");
            int h = tdeck_set_tz_offset_min / 60;
            int m = tdeck_set_tz_offset_min % 60;
            if (m < 0) m = -m;
            if (tdeck_set_tz_offset_min == 0) snprintf(value, vn, "UTC");
            else snprintf(value, vn, "UTC%+d:%02d", h, m);
            *action = false;
            break;
        }
        case 5: snprintf(label, n, "Announce min"); snprintf(value, vn, "%u", tdeck_set_announce_interval); *action = false; break;
        case 6: snprintf(label, n, "Confirm send"); snprintf(value, vn, "%s", tdeck_set_confirm_send ? "yes" : "no"); *action = false; break;
        case 7: {   // Radio frequency (Hz)
            snprintf(label, n, "Radio freq");
            if (lora_freq > 0) snprintf(value, vn, "%u.%03u MHz", lora_freq / 1000000, (lora_freq / 1000) % 1000);
            else snprintf(value, vn, "unset");
            *action = false;
            break;
        }
        case 8: {   // Radio bandwidth (Hz)
            snprintf(label, n, "Radio BW");
            if (lora_bw > 0) {
                if (lora_bw % 1000 == 0) snprintf(value, vn, "%u kHz", lora_bw / 1000);
                else snprintf(value, vn, "%.1f kHz", lora_bw / 1000.0f);
            } else {
                snprintf(value, vn, "unset");
            }
            *action = false;
            break;
        }
        case 9: {   // Spreading factor
            snprintf(label, n, "Radio SF");
            if (lora_sf > 0) snprintf(value, vn, "%d", lora_sf);
            else snprintf(value, vn, "unset");
            *action = false;
            break;
        }
        case 10: {  // Coding rate (LoRa 4/x, denominator x = 5-8)
            snprintf(label, n, "Radio CR");
            snprintf(value, vn, "%d", lora_cr);
            *action = false;
            break;
        }
        case 11: {  // TX power (dBm)
            snprintf(label, n, "Radio TXP");
            if (lora_txp != 0xFF) snprintf(value, vn, "%d dBm", lora_txp);
            else snprintf(value, vn, "unset");
            *action = false;
            break;
        }
        case 12: snprintf(label, n, "Save to SD");  snprintf(value, vn, ""); *action = true; break;
        case 13: snprintf(label, n, "Reload from SD"); snprintf(value, vn, ""); *action = true; break;
        default: snprintf(label, n, "Export contacts"); snprintf(value, vn, ""); *action = true; break;
    }
}

static void tdeck_ui_draw_settings() {
    TDeckCanvas &c = tdeck_canvas;
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

// Master draw: the canvas is cleared and the status bar is drawn exactly
// once here, before the active app paints its content. Centralising the
// frame preamble guarantees every screen (including tertiary sub-screens
// like the Reticulum "Direct contacts" list) starts from a clean frame,
// preventing stale pixels from a previous screen bleeding through.
static void tdeck_ui_draw() {
    if (!tdeck_canvas.valid()) return;
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);
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
        case SCREEN_SAR:       tdeck_ui_draw_sar(); break;
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
        case 4: {   // Timezone offset: +/- 15 min steps, clamped to ±24 h
            int v = (int)tdeck_set_tz_offset_min + dir * 15;
            tdeck_set_tz_offset_min = (int16_t)constrain(v, -1440, 1440);
            tdeck_settings_dirty = true;
            break;
        }
        case 5: {
            int v = (int)tdeck_set_announce_interval + dir * 5;
            tdeck_set_announce_interval = (uint16_t)constrain(v, 0, 240);
            tdeck_settings_dirty = true;
            break;
        }
        case 6: tdeck_set_confirm_send = !tdeck_set_confirm_send; tdeck_settings_dirty = true; break;
        case 7: {   // Radio frequency: +/- 100 kHz
            long v = (lora_freq > 0) ? (long)lora_freq : 868000000L;
            v += (long)dir * 100000L;
            lora_freq = (uint32_t)constrain(v, 100000000L, 1000000000L);
            if (radio_online) setFrequency();
            tdeck_settings_dirty = true;
            break;
        }
        case 8: {   // Radio bandwidth: cycle through valid SX126x options
            uint8_t best = 0;
            if (lora_bw > 0) {
                for (uint8_t i = 0; i < TDECK_BW_OPTIONS; i++) {
                    if (tdeck_bw_options[i] <= lora_bw) best = i;
                }
            }
            if (dir > 0) {
                if (best + 1 < TDECK_BW_OPTIONS) best++;
            } else {
                if (best > 0) best--;
            }
            lora_bw = tdeck_bw_options[best];
            if (radio_online) setBandwidth();
            tdeck_settings_dirty = true;
            break;
        }
        case 9: {   // Spreading factor: 5-12
            int v = (lora_sf > 0) ? lora_sf : 7;
            v += dir;
            lora_sf = constrain(v, 5, 12);
            if (radio_online) setSpreadingFactor();
            tdeck_settings_dirty = true;
            break;
        }
        case 10: {  // Coding rate: 4/5 - 4/8
            int v = (lora_cr > 0) ? lora_cr : 5;
            v += dir;
            lora_cr = constrain(v, 5, 8);
            if (radio_online) setCodingRate();
            tdeck_settings_dirty = true;
            break;
        }
        case 11: {  // TX power: -9 to 22 dBm
            int v = (lora_txp != 0xFF) ? lora_txp : 17;
            v += dir;
            lora_txp = constrain(v, -9, 22);
            if (radio_online) setTXPower();
            tdeck_settings_dirty = true;
            break;
        }
        // SAR config rows (reuse tdeck_set_edit_idx while in the SAR app)
        case 20: {  // SAR beacon interval: +/- 5 s
            int v = (int)tdeck_sar_interval + dir * 5;
            tdeck_sar_interval = (uint16_t)constrain(v, 10, 3600);
            tdeck_settings_dirty = true;
            break;
        }
        case 21: {  // SAR send toggle
            tdeck_sar_send_enabled = !tdeck_sar_send_enabled;
            tdeck_settings_dirty = true;
            break;
        }
        case 22: {  // SAR map zoom
            int v = (int)tdeck_sar_zoom + dir;
            tdeck_sar_zoom = (uint8_t)constrain(v, TDECK_SAR_MIN_ZOOM, TDECK_SAR_MAX_ZOOM);
            break;
        }
        default: break;
    }
    tdeck_dirty = true;
}

static void tdeck_ui_nav_select() {
    switch (tdeck_screen) {
      case SCREEN_HOME:
        if (tdeck_cursor < 5) {
            static const uint8_t targets[5][2] = {
                { SCREEN_MESSAGES, MSG_INBOX },
                { SCREEN_CONTACTS, CT_LIST },
                { SCREEN_RETICULUM, RET_MENU },
                { SCREEN_SETTINGS, SET_LIST },
                { SCREEN_SAR, SAR_MAP },
            };
            tdeck_ui_nav_push(targets[tdeck_cursor][0], targets[tdeck_cursor][1]);
        }
        break;

      case SCREEN_SAR:
        if (tdeck_sub == SAR_MAP) {
            // Enter the SAR menu (peers/config)
            tdeck_ui_nav_push(SCREEN_SAR, SAR_PEERS);
        } else if (tdeck_sub == SAR_PEERS) {
            if (tdeck_cursor == 0) {
                // Open contact picker (curated list of contacts)
                if (tdeck_contact_count == 0) {
                    tdeck_toast_set("No contacts - wait for announces");
                } else {
                    tdeck_sar_pick_contact_cursor = 0;
                    tdeck_sar_pick_contact_scroll = 0;
                    tdeck_ui_nav_push(SCREEN_SAR, SAR_PICK);
                }
            } else if (tdeck_cursor - 1 < tdeck_sar_peer_count) {
                // Remove the selected peer
                uint8_t idx = tdeck_cursor - 1;
                memmove(&tdeck_sar_peers[idx], &tdeck_sar_peers[idx + 1],
                        sizeof(tdeck_sar_peers[0]) * (tdeck_sar_peer_count - idx - 1));
                tdeck_sar_peer_count--;
                tdeck_settings_dirty = true;
                tdeck_toast_set("Peer removed");
            }
        } else if (tdeck_sub == SAR_PICK) {
            if (tdeck_sar_pick_contact_cursor < tdeck_contact_count) {
                tdeck_contact_t* c = &tdeck_contacts[tdeck_sar_pick_contact_cursor];
                if (tdeck_sar_peer_index(c->hash) >= 0) {
                    tdeck_toast_set("Peer already added");
                } else if (tdeck_sar_peer_count >= TDECK_SAR_MAX_PEERS) {
                    tdeck_toast_set("Max %u peers", TDECK_SAR_MAX_PEERS);
                } else {
                    strncpy(tdeck_sar_peers[tdeck_sar_peer_count], c->hash,
                            sizeof(tdeck_sar_peers[0]) - 1);
                    tdeck_sar_peers[tdeck_sar_peer_count][sizeof(tdeck_sar_peers[0]) - 1] = '\0';
                    tdeck_sar_peer_count++;
                    tdeck_settings_dirty = true;
                    tdeck_toast_set("Peer added");
                    tdeck_ui_nav_back();
                }
            }
        } else if (tdeck_sub == SAR_CFG) {
            switch (tdeck_cursor) {
                case 0: // beacon interval
                    tdeck_set_edit_idx = 20;
                    tdeck_ui_nav_push(SCREEN_SAR, SAR_CFG_EDIT);
                    break;
                case 1: // send toggle
                    tdeck_set_edit_idx = 21;
                    tdeck_ui_nav_push(SCREEN_SAR, SAR_CFG_EDIT);
                    break;
                case 2: // zoom
                    tdeck_set_edit_idx = 22;
                    tdeck_ui_nav_push(SCREEN_SAR, SAR_CFG_EDIT);
                    break;
                case 3: // recentre
                    if (tdeck_gps_present && tdeck_gps_fix) {
                        tdeck_sar_center_lat_e7 = (int64_t)(tdeck_gps.latitude() * 10000000.0);
                        tdeck_sar_center_lon_e7 = (int64_t)(tdeck_gps.longitude() * 10000000.0);
                        tdeck_sar_pan_px_x = 0;
                        tdeck_sar_pan_px_y = 0;
                        tdeck_sar_has_centre = true;
                        tdeck_toast_set("Centred");
                    } else {
                        tdeck_toast_set("No GPS fix");
                    }
                    break;
            }
        } else if (tdeck_sub == SAR_CFG_EDIT) {
            tdeck_settings_dirty = true;
            tdeck_ui_nav_back();
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
            if (tdeck_cursor == 0) {
                // Broadcast to all UI nodes via the shared PLAIN destination.
                strncpy(tdeck_input_peer, TDECK_BCAST_PEER, sizeof(tdeck_input_peer)-1);
                tdeck_input_start(0, "");
                tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
            } else if (tdeck_cursor - 1 < tdeck_contact_count) {
                strncpy(tdeck_input_peer, tdeck_contacts[tdeck_cursor - 1].hash, sizeof(tdeck_input_peer)-1);
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
                case 4:
                  #ifdef HAS_RNS
                    if (tdeck_rns_ready) {
                        tdeck_rns_announce_ui();
                        tdeck_toast_set("Announced");
                    } else {
                        tdeck_toast_set("RNS not ready");
                    }
                  #else
                    tdeck_toast_set("RNS unavailable");
                  #endif
                    break;
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
                case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
                    tdeck_set_edit_idx = tdeck_cursor;
                    tdeck_ui_nav_push(SCREEN_SETTINGS, SET_EDIT);
                    break;
                case 12:
                    if (tdeck_settings_save()) {
                        // Radio parameters are also persisted into EEPROM so a
                        // reboot applies them via the normal eeprom_conf_load()
                        // + startRadio() path (mirrors the Provisioning
                        // subsystem's FF_REBOOT_REQUIRED radio fields).
                        if (hw_ready && radio_online) {
                            eeprom_conf_save();
                            tdeck_toast_set("Saved (SD+EEPROM)");
                        } else {
                            tdeck_toast_set("Saved to SD only");
                        }
                    } else {
                        tdeck_toast_set("SD save failed");
                    }
                    break;
                case 13:
                    tdeck_settings_load();
                    tdeck_apply_brightness();
                    // Apply any radio parameters loaded from settings.yaml to
                    // the live radio hardware.
                    if (radio_online) {
                        setBandwidth();
                        setSpreadingFactor();
                        setCodingRate();
                        setTXPower();
                        setFrequency();
                    }
                    tdeck_toast_set("Reloaded");
                    break;
                case 14:
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
        tdeck_cursor = (tdeck_cursor + 4) % 5;      // wrap around 5 apps
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
      case SCREEN_SAR:
        if (tdeck_sub == SAR_MAP) {
            // Trackball up/down = pan the map up/down
            tdeck_sar_pan_px_y -= 24;
            tdeck_dirty = true;
        } else if (tdeck_sub == SAR_PEERS) {
            if (tdeck_cursor > 0) tdeck_cursor--;
        } else if (tdeck_sub == SAR_CFG) {
            if (tdeck_cursor > 0) tdeck_cursor--;
        } else if (tdeck_sub == SAR_CFG_EDIT) {
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
        tdeck_cursor = (tdeck_cursor + 1) % 5;
        break;
      case SCREEN_MESSAGES:
        if (tdeck_sub == MSG_INBOX) {
            uint8_t total = tdeck_inbox_peer_count() + 1;
            if (tdeck_cursor + 1 < total) tdeck_cursor++;
        } else if (tdeck_sub == MSG_THREAD) {
            if (tdeck_scroll > 0) tdeck_scroll--;   // view newer messages
        } else if (tdeck_sub == MSG_PICK) {
            if (tdeck_cursor + 1 < tdeck_contact_count + 1) tdeck_cursor++;
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
            if (tdeck_cursor + 1 < 5) tdeck_cursor++;
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
      case SCREEN_SAR:
        if (tdeck_sub == SAR_MAP) {
            // Trackball down = pan the map down
            tdeck_sar_pan_px_y += 24;
            tdeck_dirty = true;
        } else if (tdeck_sub == SAR_PEERS) {
            uint8_t total = tdeck_sar_peer_count + 1;
            if (tdeck_cursor + 1 < total) tdeck_cursor++;
        } else if (tdeck_sub == SAR_PICK) {
            if (tdeck_sar_pick_contact_cursor + 1 < tdeck_contact_count)
                tdeck_sar_pick_contact_cursor++;
        } else if (tdeck_sub == SAR_CFG) {
            if (tdeck_cursor + 1 < 4) tdeck_cursor++;
        } else if (tdeck_sub == SAR_CFG_EDIT) {
            tdeck_set_edit_change(-1);
        }
        break;
      default: break;
    }
    tdeck_dirty = true;
}

// ---- Input handling ------------------------------------------------------------
//
// Navigation input map:
//   back           = Backspace key, trackball-left
//   enter          = Enter key, trackball push
//   scroll up      = W key, trackball up
//   scroll down    = S key, trackball down
//   scroll left    = A key, trackball left
//   scroll right   = D key, trackball right
//
// Key-based navigation never exits a text field. In a text field the W/A/S/D
// keys type their letters, Backspace deletes, and Enter applies; only the
// trackball-left "back" event leaves text input without applying.

static void tdeck_ui_handle_key(char key, uint8_t state) {
    // Only act on press events; ignore release/auto-repeat noise.
    if (state != KB_KEY_STATE_PRESS && state != KB_KEY_STATE_LONG_PRESS) return;

    // In a text field every printable key (including W/A/S/D) types a
    // character. Backspace deletes without leaving the field; Enter applies.
    // Key-based navigation never exits text input - only the trackball-left
    // event leaves a text field.
    if (tdeck_in_text_input()) {
        switch (key) {
            case 0x0A:   // LF / Enter: apply the text
            case 0x0D:   // CR / Enter: apply the text
                tdeck_input_apply();
                break;
            case 0x08:   // Backspace: delete character (does not exit)
                tdeck_input_del();
                break;
            default:
                if (key >= 0x20 && key <= 0x7E) {
                    tdeck_input_add(key);
                }
                break;
        }
        tdeck_dirty = true;
        return;
    }

    // Navigation mapping (outside text input).
    switch (key) {
        case 0x0A:   // LF / Enter -> enter
        case 0x0D:   // CR / Enter -> enter
            tdeck_ui_nav_select();
            break;
        case 0x08:   // Backspace -> back
            tdeck_ui_nav_back();
            break;
        case 'w': case 'W':   // scroll up (SAR map: zoom in)
            if (tdeck_screen == SCREEN_SAR && tdeck_sub == SAR_MAP) {
                if (tdeck_sar_zoom < TDECK_SAR_MAX_ZOOM) tdeck_sar_zoom++;
                tdeck_dirty = true;
            } else {
                tdeck_ui_nav_up();
            }
            break;
        case 's': case 'S':   // scroll down (SAR map: zoom out)
            if (tdeck_screen == SCREEN_SAR && tdeck_sub == SAR_MAP) {
                if (tdeck_sar_zoom > TDECK_SAR_MIN_ZOOM) tdeck_sar_zoom--;
                tdeck_dirty = true;
            } else {
                tdeck_ui_nav_down();
            }
            break;
        case 'a': case 'A':   // scroll left -> back
            tdeck_ui_nav_back();
            break;
        case 'd': case 'D':   // scroll right -> select
            tdeck_ui_nav_select();
            break;
        default:
            // Home-screen app shortcuts (1-5) remain available.
            if (tdeck_screen == SCREEN_HOME && key >= '1' && key <= '5') {
                tdeck_cursor = (uint8_t)(key - '1');
                tdeck_ui_nav_select();
            }
            break;
    }
    tdeck_dirty = true;
}

static void tdeck_ui_handle_trackball(tb_event_t event) {
    // On the SAR map view the trackball's primary purpose is panning the
    // map: all four directions move the viewport, and press still acts as
    // Enter (opens the SAR menu). Leave the map with the Backspace key.
    if (tdeck_screen == SCREEN_SAR && tdeck_sub == SAR_MAP) {
        switch (event) {
            case TB_EVENT_UP:         tdeck_sar_pan_px_y -= 24; break;
            case TB_EVENT_DOWN:       tdeck_sar_pan_px_y += 24; break;
            case TB_EVENT_LEFT:       tdeck_sar_pan_px_x -= 24; break;
            case TB_EVENT_RIGHT:      tdeck_sar_pan_px_x += 24; break;
            case TB_EVENT_PRESS:      tdeck_ui_nav_select(); break;
            case TB_EVENT_PRESS_LONG: tdeck_ui_go_home(); break;
            default: break;
        }
        tdeck_dirty = true;
        return;
    }

    // In a text field, scroll events (up/down/right) and long-press-home do
    // not navigate; only trackball-left exits the field and trackball-push
    // acts as Enter (applies the text).
    bool in_text = tdeck_in_text_input();
    switch (event) {
        case TB_EVENT_UP:         if (!in_text) tdeck_ui_nav_up(); break;
        case TB_EVENT_DOWN:       if (!in_text) tdeck_ui_nav_down(); break;
        case TB_EVENT_LEFT:       tdeck_ui_nav_back(); break;   // back / exit text field
        case TB_EVENT_RIGHT:      if (!in_text) tdeck_ui_nav_select(); break;
        case TB_EVENT_PRESS:      tdeck_ui_nav_select(); break; // enter / apply text
        case TB_EVENT_PRESS_LONG: if (!in_text) tdeck_ui_go_home(); break;
        default: break;
    }
    tdeck_dirty = true;
}

// ---- EEPROM provisioning ------------------------------------------------------

// A T-Deck provisioned purely through the UI (settings.yaml) never goes
// through rnodeconf, so the EEPROM identity block (product/model/hwrev/serial/
// made + lock + MD5 checksum) is left blank and validate_status() reports
// "Device unprovisioned". Seed an rnodeconf-style identity here so the device
// is provisioned exactly like every other board in the repo. Runs from
// tdeck_ui_init() *before* validate_status() so the same boot already sees a
// valid identity.
static void tdeck_eeprom_provision() {
    if (eeprom_lock_set()) return;                    // already provisioned

    // Product / model / revision for the LilyGO T-Deck (Boards.h). MODEL_D9
    // is the 868 MHz variant; use MODEL_D4 for the 433 MHz variant.
    eeprom_write(ADDR_PRODUCT, PRODUCT_TDECK_V1);
    eeprom_write(ADDR_MODEL,   MODEL_D9);
    eeprom_write(ADDR_HW_REV,  0x01);

    // Serial: first 4 bytes of the hardware UID (unique per device).
    for (uint8_t i = 0; i < 4 && i < DEVICE_UID_LEN; i++) {
        eeprom_write(ADDR_SERIAL + i, device_uid[i]);
    }

    // Manufacturing date: non-zero marker (not a real date).
    uint32_t made = 0x4D525443;                       // "CTMR"
    for (uint8_t i = 0; i < 4; i++) {
        eeprom_write(ADDR_MADE + i, (uint8_t)(made >> (8 * (3 - i))));
    }

    // Lock the identity block last: eeprom_write() refuses further writes
    // once the lock byte is set.
    eeprom_update(eeprom_addr(ADDR_INFO_LOCK), INFO_LOCK_BYTE);

    // Compute + store the MD5 over the 11-byte identity block so
    // eeprom_checksum_valid() passes, exactly like rnodeconf produces.
    uint8_t data[CHECKSUMMED_SIZE];
    for (uint8_t i = 0; i < CHECKSUMMED_SIZE; i++) {
        data[i] = EEPROM.read(eeprom_addr(i));
    }
    unsigned char *hash = MD5::make_hash((char *)data, CHECKSUMMED_SIZE);
    for (uint8_t i = 0; i < 16; i++) {
        eeprom_update(eeprom_addr(ADDR_CHKSUM + i), hash[i]);
    }
    free(hash);

    tdeck_ui_debug("[TDeck] EEPROM provisioned (identity)");
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

// Called by the UI loop (or externally by GPS code) once a fresh GNSS UTC
// epoch is available. Installs the epoch into the Reticulum system clock so
// the status-bar clock, message timestamps and announce timestamps all follow
// GPS-disciplined real-world time. Returns true the first time it applies.
bool tdeck_ui_gps_set_time(uint64_t epoch_s) {
    if (tdeck_gps_time_set) return false;
    // sanity: Oct 2020 floor, then shift the GNSS UTC epoch by the user's
    // configured timezone offset so the status-bar clock displays local time
    // (the UI renders the RNS epoch with gmtime_r, i.e. as UTC).
    if (epoch_s < 1600000000ULL) return false;   // sanity: Oct 2020 floor
    int64_t tz_s = (int64_t)tdeck_set_tz_offset_min * 60;
    if (tz_s > 0) epoch_s += (uint64_t)tz_s;
    else if (tz_s < 0) epoch_s -= (uint64_t)(-tz_s);

  #ifdef HAS_RNS
    // RNS::OS::ltime() (Arduino) = millis() + _time_offset. We want
    // ltime() to equal epoch_s * 1000 at this instant, so the offset =
    // epoch_ms - millis().
    uint64_t epoch_ms = epoch_s * 1000ULL;
    uint64_t now_ms   = (uint64_t)millis();
    uint64_t offset   = (epoch_ms > now_ms) ? (epoch_ms - now_ms) : 0;
    RNS::Utilities::OS::setTimeOffset(offset);

    tdeck_gps_time_set = true;
    tdeck_toast_set("GPS time set");
    tdeck_dirty = true;
    tdeck_ui_debug("[TDeck] GPS time applied to RNS clock");
    return true;
  #else
    (void)epoch_s;
    return false;
  #endif
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
    switch (tdeck_kb.protocol()) {
        case 1: tdeck_ui_debug("[TDeck] keyboard: BBQ10 registers"); break;
        case 2: tdeck_ui_debug("[TDeck] keyboard: ESP32-C3 byte-stream"); break;
        default: tdeck_ui_debug("[TDeck] keyboard: not detected"); break;
    }

    // Start the trackball GPIO interrupts
    tdeck_trackball.begin();

    // Start the GPS/GNSS receiver on UART1 (L76K or compatible NMEA module
    // on the Gover interface; harmless if no module is fitted).
    tdeck_gps.begin();

    // Allocate the off-screen frame buffer (PSRAM-backed)
    tdeck_canvas.allocate();
    if (!tdeck_canvas.valid()) {
        tdeck_ui_debug("[TDeck] warning: could not allocate frame buffer");
    }

    // Mount the microSD card and load /settings.yaml (device settings plus
    // contact aliases/favourites). A missing card simply leaves defaults.
    tdeck_sd_init();
    tdeck_settings_load();
    tdeck_apply_brightness();

    // Location is now final (default or loaded from settings.yaml). When
    // disabled, immediately put the GNSS hardware into standby so it draws
    // no tracking current instead of merely ignoring its output.
    if (!tdeck_set_gps_enabled) {
        tdeck_gps.standby();
    }

    // Seed the rnodeconf-style EEPROM identity so the device is provisioned
    // like every other board in the repo (must run before validate_status()).
    tdeck_eeprom_provision();

    tdeck_ui_ready = true;
    tdeck_dirty = true;
    tdeck_ui_start_ms = millis();
    tdeck_last_tick = millis();
    tdeck_ui_go_home();
}

void tdeck_ui_loop() {
    if (!tdeck_ui_ready) return;

  #ifdef HAS_RNS
    // The boot path only starts the radio when EEPROM holds a valid radio
    // config AND device_init() succeeds. device_init() requires a firmware
    // signature hash that only rnodeconf (with the private signing key) can
    // write (VALIDATE_FIRMWARE), so on a UI-provisioned T-Deck hw_ready stays
    // false even after tdeck_eeprom_provision(). The radio hardware itself is
    // present (modem_installed), so bring it up here now that RNS is up, and
    // re-enable transport since we are acting as a full node.
    if (modem_installed && !radio_online && !console_active) {
        // The standard hw_ready path is unreachable on this device (see
        // above); mark the hardware ready so startRadio() and the EEPROM
        // save path (eeprom_conf_save()) treat the device as provisioned.
        if (!hw_ready) hw_ready = true;
        // Apply sane defaults for any radio parameter that is unset or
        // invalid so the radio can always start out of the box:
        //   867.2 MHz, 125 kHz bandwidth, SF8, CR 4/5, +7 dBm
        if (lora_freq == 0 || lora_freq < 100000000L || lora_freq > 1000000000L) {
            lora_freq = 867200000;
            tdeck_ui_debug("[TDeck] Radio freq default 867.2 MHz");
        }
        if (lora_bw == 0) {
            lora_bw = 125000;
            tdeck_ui_debug("[TDeck] Radio BW default 125 kHz");
        }
        if (lora_sf == 0) {
            lora_sf = 8;
            tdeck_ui_debug("[TDeck] Radio SF default 8");
        }
        if (lora_cr == 0) {
            lora_cr = 5;
            tdeck_ui_debug("[TDeck] Radio CR default 4/5");
        }
        if (lora_txp == 0xFF) {
            // 14 dBm sits at the bottom of the SX1262 high-power PA range
            // (14-22 dBm) so a fresh device radiates a usable signal out of
            // the box while staying near typical 868 MHz duty-cycle limits.
            lora_txp = 14;
            tdeck_ui_debug("[TDeck] Radio TXP default 14 dBm");
        }

        update_radio_lock();
        if (startRadio()) {
            op_mode = MODE_TNC;
            RNS::Reticulum::transport_enabled(true);
            tdeck_ui_debug("[TDeck] Radio started from settings");
            // Diagnostics: log the runtime device model and the effective TX
            // power so a silent PA misconfiguration or a failed EEPROM
            // identity seed (model==0x00) is immediately visible on serial.
            #if defined(LORA_TRANSPORT)
                printf("[radio] model: 0x%02X", (unsigned int)model);
                printf("[radio] TX power: %d dBm", LoRa->getTxPower());
                // Full RF config so the two ends of a link can be compared
                // byte-for-byte (freq/bw/sf/cr must match on both devices).
                printf("[radio] freq: %lu Hz, bw: %lu Hz, sf: %d, cr: 4/%d",
                       (unsigned long)lora_freq, (unsigned long)lora_bw,
                       lora_sf, lora_cr);
            #endif
            // Any announces/broadcasts enqueued before the radio came up
            // (e.g. the NomadNet startup announce) need a CSMA pass to reach
            // the SX1262 - the main loop only calls tx_queue_handler() inside
            // `if (radio_online)`, so flush them right away.
            if (queue_height > 0) tx_queue_handler();
        }
    }

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

    // Track any traffic received from other nodes so the status bar can show
    // mesh activity (green cell tower) within the last 5 minutes. The
    // transport-level RX counter increments for every packet the LoRa
    // interface receives -- announces, proofs, propagation and data alike --
    // which is exactly "any traffic from another node heard". The timestamp
    // is millis()-based (not RNS wall time) so activity is detected even
    // before a GPS-disciplined clock has been applied to RNS.
    if (tdeck_rns_ready) {
        uint32_t rx = RNS::Transport::packets_received();
        if (rx != tdeck_last_rx_count) {
            tdeck_last_rx_count = rx;
            tdeck_last_rx_ms = millis();
        }
    }

    // SAR beacon pacing: send the current fix to all configured peers at the
    // configured interval (millis-based so it works before GPS-disciplined
    // time). Also expire stale peer tracks.
    tdeck_sar_expire_tracks();
    if (tdeck_sar_interval > 0 &&
        (millis() - tdeck_sar_last_beacon_ms >= (uint32_t)tdeck_sar_interval * 1000)) {
        tdeck_rns_sar_beacon();
    }
  #endif

    // Poll the GPS/GNSS UART and keep the status bar + RNS announce in sync.
    if (tdeck_set_gps_enabled) {
        // Wake the receiver once if it was put to standby (boot-disabled, or
        // toggled off then back on in Settings).
        if (tdeck_gps.asleep()) {
            tdeck_gps.wake();
        }
        tdeck_gps.loop();

        // A module has been detected once any NMEA traffic arrives.
        if (tdeck_gps.dataSeen() && !tdeck_gps_present) {
            tdeck_gps_present = true;
            tdeck_ui_debug("[TDeck] GPS module detected");
        }

        // Track fix state transitions for the status bar.
        bool fix = tdeck_gps.hasFix();
        if (fix != tdeck_gps_fix) tdeck_ui_set_gps_fix(fix);

        // Apply the GNSS UTC epoch to the RNS clock (once).
        if (tdeck_gps.epochReady() && !tdeck_gps_time_set) {
            tdeck_ui_gps_set_time(tdeck_gps.gpsEpoch());
        }

        // On first fix, push an immediate announce so peers learn our
        // position without waiting for the periodic re-announce.
      #ifdef HAS_RNS
        if (tdeck_rns_ready && fix && !tdeck_gps_announced) {
            tdeck_gps_announced = true;
            tdeck_rns_announce_ui();
        }
      #endif
    } else {
        // GPS disabled in settings: put the receiver hardware into standby
        // (idempotent) and drop any stale UI indication.
        tdeck_gps.standby();
        if (tdeck_gps_present) { tdeck_gps_present = false; tdeck_gps_fix = false; tdeck_dirty = true; }
    }

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
