// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Shared types and constants for the T-Deck user interface.
//
// This header is firmware-free on purpose: it only depends on Arduino and
// library headers (Adafruit_GFX, ESP heap), never on the project's own
// Config.h / Utilities.h / Display.h which define firmware globals at
// header scope and are therefore only safe to include from a single
// translation unit (RNode_Firmware.ino).
//
// Both the framework (TDeckUI.h) and every app module (Apps/App*.cpp)
// include this header.

#pragma once

#include <Arduino.h>
#include <string.h>

#include <Adafruit_GFX.h>
#ifdef ESP_PLATFORM
  #include <esp_heap_caps.h>
#endif

// ST77xx panel colour constants (RGB565). The real values are defined by the
// Adafruit_ST7789 driver header included in the framework's translation unit
// (TDeckUI.h, via Display.h). App modules are separate TUs that only include
// Adafruit_GFX, so provide fallback definitions of the same values; the
// #ifndef guards let the authoritative driver header win when it is present.
#ifndef ST77XX_BLACK
  #define ST77XX_BLACK   0x0000
#endif
#ifndef ST77XX_WHITE
  #define ST77XX_WHITE   0xFFFF
#endif
#ifndef ST77XX_RED
  #define ST77XX_RED     0xF800
#endif
#ifndef ST77XX_GREEN
  #define ST77XX_GREEN   0x07E0
#endif
#ifndef ST77XX_BLUE
  #define ST77XX_BLUE    0x001F
#endif
#ifndef ST77XX_CYAN
  #define ST77XX_CYAN    0x07FF
#endif
#ifndef ST77XX_YELLOW
  #define ST77XX_YELLOW  0xFFE0
#endif

// The T-Deck's ST7789 panel is mounted in landscape; the drawing surface is
// 320x240 (matching display.setRotation(3) configured in Display.h).
#define TDECK_SCREEN_W 320
#define TDECK_SCREEN_H 240

// ---- Layout -----------------------------------------------------------------
#define TDECK_SBAR_H   24   // top status bar, always visible
#define TDECK_FOOT_H   18   // bottom hint bar
#define TDECK_LIST_X    6   // left padding for list content
#define TDECK_LIST_W   (TDECK_SCREEN_W - 2*TDECK_LIST_X)

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

// ---- RNS app aspects used by the UI -----------------------------------------
// The messages app registers the standard LXMF delivery destination so it is
// interoperable with Sideband, NomadNet, MeshChat and all other LXMF clients.
// See https://github.com/markqvist/lxmf
#define TDECK_UI_RNS_APP          "lxmf"
#define TDECK_UI_RNS_ASPECT_MSGS  "delivery"
#define TDECK_UI_RNS_ASPECT_PING "ping"
#define TDECK_UI_RNS_ASPECT_BCAST "broadcast"

// Simple line-based payload framing for the ping/trace helpers (not part of
// the LXMF protocol - these are UI-internal diagnostics).
#define TDECK_FRAME_PING "!mrping!"   // !mrping!<seq>!<sent_at>!<sender_pubkey>!<sender_dest_hash>
#define TDECK_FRAME_PONG "!mrpong!"   // !mrpong!<seq>!<sent_at>
#define TDECK_FRAME_BCAST "!mrbcast!" // !mrbcast!<sender_hash>!<text>

// Broadcast conversation sentinel peer (not a contact hash: "bcast" is not
// a 32-char hex string, so it can never collide with a real identity hash).
#define TDECK_BCAST_PEER "bcast"

// ---- Storage / data limits (shared by multiple modules) ----------------------
#define TDECK_SETTINGS_PATH "/settings.yaml"
#define TDECK_EXPORT_PATH   "/contacts.yaml"
#define TDECK_MAX_CONTACTS  32
#define TDECK_ALIAS_MAX     32
#define TDECK_APP_DATA_MAX  40
#define TDECK_MAX_MESSAGES  24
#define TDECK_MSG_MAX_LEN   220
#define TDECK_PING_TIMEOUT_MS 8000

// ---- SAR (Situational Awareness) shared RNS aspect ----------------------------
// The SAR app shares periodic encrypted position beacons with a configured set
// of peer nodes over this aspect (all other SAR constants live in the SAR app
// module itself).
#define TDECK_SAR_RNS_ASPECT "sar.position"

// ---- Message store ------------------------------------------------------------
typedef struct {
  char  peer[40];              // peer identity hash (32 hex chars)
  char  text[TDECK_MSG_MAX_LEN+1];
  bool  inbound;
  double time;
} tdeck_message_t;

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