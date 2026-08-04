// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Meshtastic-inspired user interface for the LilyGO T-Deck. Provides:
//
//   * A full-screen RGB test sequence (guarded by TDECKUITEST)
//   * An input test screen showing keyboard + trackball events
//   * A compact status UI with radio state and battery
//
// Frames are composed in an off-screen RGB565 buffer in PSRAM and pushed to
// the ST7789 in one full-frame SPI burst, eliminating the flicker caused by
// incremental clears and redraws. The panel only repaints when an input
// event arrives or the periodic status refresh fires.
//
// All hardware initialisation (keyboard, trackball, backlight, I2C) is
// performed by tdeck_ui_init() and is NOT guarded - only the test
// sequence itself is behind #ifdef TDECKUITEST.

#pragma once

#include <Arduino.h>

#if BOARD_MODEL == BOARD_TDECK

#include <Adafruit_GFX.h>
#ifdef ESP_PLATFORM
  #include <esp_heap_caps.h>
#endif

#include "TDeckKeyboard.h"
// Define the trackball implementation only here - TDeckUI.h is included from
// exactly one translation unit (RNode_Firmware.ino), so the global singleton
// and ISR pointer are defined exactly once in the firmware image.
#define TDECK_TRACKBALL_IMPLEMENTATION
#include "TDeckTrackball.h"
// NOTE: Display.h is intentionally NOT included here. It has no include
// guards and is already pulled in by Utilities.h (via Config.h/Boards.h)
// before this header is included from RNode_Firmware.ino. All symbols used
// below (display, SSD1306_BLACK/WHITE, ST77XX_*, radio_online, battery_*)
// are therefore already defined in this translation unit.

// The T-Deck's ST7789 panel is mounted in landscape; the drawing surface is
// 320x240 (matching display.setRotation(3) configured in Display.h).
#define TDECK_SCREEN_W 320
#define TDECK_SCREEN_H 240

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

#define TDECK_UI_VERSION "T-Deck UI v1.1"

// UI states
#define TDECK_UI_STATE_STATUS  0
#define TDECK_UI_STATE_INPUT   1
#define TDECK_UI_STATE_TEST    2

static uint8_t  tdeck_ui_state      = TDECK_UI_STATE_STATUS;
static bool     tdeck_ui_ready      = false;
static bool     tdeck_ui_dirty      = true;
static uint32_t tdeck_ui_last_tick  = 0;
static uint32_t tdeck_ui_tick_ms    = 1000;
static char     tdeck_ui_status[64] = "Booting...";

// Test sequence state
static uint8_t  tdeck_test_colours   = 0;
static uint32_t tdeck_test_last_tick = 0;
static uint8_t  tdeck_test_step      = 0;
static bool     tdeck_test_done      = false;

#ifdef TDECKUITEST
  #define TDECK_TEST_RGB_STEPS 3
  #define TDECK_TEST_RGB_MS    1000
#endif

// Events from the input test
typedef struct {
    char     last_key;
    uint8_t  last_key_state;
    uint32_t key_count;
    uint8_t  last_tb;
    uint32_t tb_count;
} tdeck_input_state_t;

static tdeck_input_state_t tdeck_input_state;

// Keyboard instance (defined once per TU inclusion; firmwares only include
// this from the single RNode_Firmware.ino translation unit).
static TDeckKeyboard tdeck_kb;

// Off-screen frame buffer; the pixel memory is allocated in tdeck_ui_init().
static TDeckCanvas tdeck_canvas(TDECK_SCREEN_W, TDECK_SCREEN_H);

// ---- Internal helpers -----------------------------------------------------

static void tdeck_ui_draw_status() {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(SSD1306_BLACK);

    // Header bar
    c.fillRect(0, 0, TDECK_SCREEN_W, 28, ST77XX_BLUE);
    c.setTextColor(ST77XX_WHITE);
    c.setTextSize(2);
    c.setCursor(8, 6);
    c.print("RDECK");

    // Status line
    c.setTextColor(ST77XX_WHITE);
    c.setTextSize(1);
    c.setCursor(4, 38);
    c.print("State: ");
    if (radio_online) {
        c.print("LoRa ONLINE");
    } else {
        c.print("LoRa standby");
    }

    c.setCursor(4, 52);
    c.print("Batt:  ");
    if (battery_ready) {
        c.printf("%.0f%%", battery_percent);
    } else {
        c.print("n/a");
    }

    c.setCursor(4, 66);
    c.print("Keys:  ");
    c.print(tdeck_input_state.key_count);
    c.setCursor(4, 80);
    c.print("Track: ");
    c.print(tdeck_input_state.tb_count);

    // Footer hint
    c.drawFastHLine(0, 96, TDECK_SCREEN_W, ST77XX_WHITE);
    c.setTextSize(1);
    c.setCursor(4, 102);
    c.print("Trackball: scroll/press   Keys: type");

    // Status line
    c.setTextSize(2);
    c.setCursor(4, 120);
    if (tdeck_ui_status[0] != '\0') {
        c.print(tdeck_ui_status);
    }

    #ifdef TDECKUITEST
    c.setTextSize(1);
    c.setCursor(4, 150);
    c.print("[Press key to run input test]");
    #endif
}

static void tdeck_ui_draw_input() {
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(SSD1306_BLACK);

    c.fillRect(0, 0, TDECK_SCREEN_W, 28, ST77XX_GREEN);
    c.setTextColor(ST77XX_BLACK);
    c.setTextSize(2);
    c.setCursor(8, 6);
    c.print("INPUT TEST");

    // Keyboard area
    c.setTextColor(ST77XX_WHITE);
    c.setTextSize(2);
    c.setCursor(4, 40);
    c.print("KB: ");
    if (tdeck_input_state.last_key >= 0x20 && tdeck_input_state.last_key <= 0x7E) {
        c.print((char)tdeck_input_state.last_key);
    } else if (tdeck_input_state.last_key != '\0') {
        c.print("(mod)");
    } else {
        c.print("-");
    }

    char state_name[16];
    switch (tdeck_input_state.last_key_state) {
        case KB_KEY_STATE_PRESS:       snprintf(state_name, sizeof(state_name), "PRESS"); break;
        case KB_KEY_STATE_LONG_PRESS:  snprintf(state_name, sizeof(state_name), "LONG"); break;
        case KB_KEY_STATE_RELEASE:     snprintf(state_name, sizeof(state_name), "RELEASE"); break;
        default:                       snprintf(state_name, sizeof(state_name), "idle"); break;
    }
    c.setCursor(4, 60);
    c.print("St:  ");
    c.print(state_name);

    // Trackball area
    c.setCursor(4, 82);
    c.print("TB:  ");
    const char *tbn = "--";
    switch (tdeck_input_state.last_tb) {
        case TB_EVENT_UP:         tbn = "UP"; break;
        case TB_EVENT_DOWN:       tbn = "DN"; break;
        case TB_EVENT_LEFT:       tbn = "LT"; break;
        case TB_EVENT_RIGHT:      tbn = "RT"; break;
        case TB_EVENT_PRESS:      tbn = "PRESS"; break;
        case TB_EVENT_PRESS_LONG: tbn = "LONG"; break;
        default: break;
    }
    c.print(tbn);

    // Counters
    c.setTextSize(1);
    c.setCursor(4, 106);
    c.print("Key count: ");
    c.print(tdeck_input_state.key_count);
    c.setCursor(4, 118);
    c.print("Trackball count: ");
    c.print(tdeck_input_state.tb_count);

    // Back hint
    c.drawFastHLine(0, 136, TDECK_SCREEN_W, ST77XX_WHITE);
    c.setCursor(4, 142);
    c.print("Trackball PRESS = back");
}

#ifdef TDECKUITEST
static void tdeck_test_next_step() {
    tdeck_test_step++;
    if (tdeck_test_step >= TDECK_TEST_RGB_STEPS) {
        // Move to the input test screen
        tdeck_ui_state = TDECK_UI_STATE_INPUT;
        tdeck_test_done = true;
        tdeck_ui_dirty = true;
        return;
    }
    tdeck_test_colours = (tdeck_test_colours + 1) % TDECK_TEST_RGB_STEPS;
}

static void tdeck_test_run_rgb() {
    uint32_t now = millis();
    if (now - tdeck_test_last_tick >= TDECK_TEST_RGB_MS) {
        tdeck_test_last_tick = now;
        switch (tdeck_test_colours) {
            case 0:
                display.fillScreen(ST77XX_RED);
                break;
            case 1:
                display.fillScreen(ST77XX_GREEN);
                break;
            case 2:
                display.fillScreen(ST77XX_BLUE);
                break;
        }
        tdeck_test_next_step();
    }
}
#endif

static void tdeck_ui_run_test() {
  #ifdef TDECKUITEST
    if (!tdeck_test_done) {
        tdeck_test_run_rgb();
    }
  #endif
}

static void tdeck_ui_draw() {
    if (!tdeck_canvas.valid()) return;
    switch (tdeck_ui_state) {
        case TDECK_UI_STATE_STATUS:
            tdeck_ui_draw_status();
            break;
        case TDECK_UI_STATE_INPUT:
            tdeck_ui_draw_input();
            break;
        default:
            tdeck_ui_draw_status();
            break;
    }
    // Push the composed frame in one burst - no per-primitive SPI traffic,
    // so the panel never shows a half-painted frame.
    display.drawRGBBitmap(0, 0, tdeck_canvas.buf(), TDECK_SCREEN_W, TDECK_SCREEN_H);
}

static void tdeck_ui_handle_key(char key, uint8_t state) {
    tdeck_input_state.last_key = key;
    tdeck_input_state.last_key_state = state;
    if (state == KB_KEY_STATE_PRESS || state == KB_KEY_STATE_LONG_PRESS) {
        tdeck_input_state.key_count++;
        snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "Key: %c", key >= 0x20 && key <= 0x7E ? key : '?');
    }

    // During the input test, any printable key keeps us on the input screen.
    // From the status screen, an unmodified printable key enters the input test.
    #ifdef TDECKUITEST
    if (tdeck_ui_state == TDECK_UI_STATE_STATUS && key >= 'a' && key <= 'z' && state == KB_KEY_STATE_PRESS) {
        tdeck_ui_state = TDECK_UI_STATE_INPUT;
        tdeck_ui_dirty = true;
    }
    #endif
}

static void tdeck_ui_handle_trackball(tb_event_t event) {
    tdeck_input_state.last_tb = event;
    if (event != TB_EVENT_NONE) {
        tdeck_input_state.tb_count++;
        switch (event) {
            case TB_EVENT_UP:         snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "TB: UP"); break;
            case TB_EVENT_DOWN:       snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "TB: DOWN"); break;
            case TB_EVENT_LEFT:       snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "TB: LEFT"); break;
            case TB_EVENT_RIGHT:      snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "TB: RIGHT"); break;
            case TB_EVENT_PRESS:      snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "TB: PRESS"); break;
            case TB_EVENT_PRESS_LONG: snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "TB: LONG PRESS"); break;
            default: break;
        }
    }

    // Return to the status screen from input test on a short centre press
    if (tdeck_ui_state == TDECK_UI_STATE_INPUT && event == TB_EVENT_PRESS) {
        tdeck_ui_state = TDECK_UI_STATE_STATUS;
        tdeck_ui_dirty = true;
    }
}

// ---- Public API -----------------------------------------------------------

// Called by Display.h's update_display() to determine whether the T-Deck UI
// owns the screen. When true, the legacy 64x64 canvas renderer yields.
bool tdeck_ui_active() {
    return tdeck_ui_ready;
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

    // Set a sensible keyboard backlight
    tdeck_kb.setBacklight(64);

    // Allocate the off-screen frame buffer (PSRAM-backed)
    tdeck_canvas.allocate();
    if (!tdeck_canvas.valid()) {
        Serial.println("[TDeck] warning: could not allocate frame buffer");
    }

    // Select the initial UI state: the status screen by default, or the
    // test sequence when built with TDECKUITEST.
    #ifdef TDECKUITEST
    tdeck_ui_state = TDECK_UI_STATE_TEST;
    tdeck_test_step = 0;
    tdeck_test_colours = 0;
    tdeck_test_done = false;
    tdeck_test_last_tick = millis();
    #else
    tdeck_ui_state = TDECK_UI_STATE_STATUS;
    #endif

    tdeck_ui_ready = true;
    tdeck_ui_dirty = true;
    tdeck_ui_last_tick = millis();
    snprintf(tdeck_ui_status, sizeof(tdeck_ui_status), "Ready");
}

void tdeck_ui_loop() {
    if (!tdeck_ui_ready) return;

    // Poll keyboard (the BBQ10/C3 latch key events until read)
    TDeckKeyboard::KeyEvent key_event = tdeck_kb.keyEvent();
    if (key_event.key != '\0' || key_event.state != KB_KEY_STATE_IDLE) {
        tdeck_ui_handle_key(key_event.key, key_event.state);
        tdeck_kb.clearInterruptStatus();
        tdeck_ui_dirty = true;
    }

    // Poll trackball
    tb_event_t tb_events = tdeck_trackball.readEvents();
    if (tb_events != TB_EVENT_NONE) {
        tdeck_ui_handle_trackball(tb_events);
        tdeck_ui_dirty = true;
    }

    // The boot-time RGB sequence drives the panel directly on its own timer.
    #ifdef TDECKUITEST
    if (tdeck_ui_state == TDECK_UI_STATE_TEST) {
        tdeck_ui_run_test();
        return;
    }
    #endif

    // Repaint on input events, or periodically on the status screen so the
    // radio/battery state stays fresh. Full-frame pushes mean no flicker.
    bool periodic = (millis() - tdeck_ui_last_tick >= tdeck_ui_tick_ms);
    if (periodic) {
        tdeck_ui_last_tick = millis();
    }
    if (tdeck_ui_dirty || (periodic && tdeck_ui_state == TDECK_UI_STATE_STATUS)) {
        tdeck_ui_dirty = false;
        tdeck_ui_draw();
    }
}

#endif // BOARD_MODEL == BOARD_TDECK
