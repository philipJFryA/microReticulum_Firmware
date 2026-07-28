// Copyright (C) 2024, Mark Qvist
// T-Deck unified input driver: trackball + keyboard → LVGL indevs

#pragma once

#ifdef BOARD_TDECK

#include <lvgl.h>

namespace td_input {

// ── initialise both input devices and register them with LVGL ────────
void init();

// ── poll for new input events (call from LVGL tick timer) ────────────
void poll();

// ── get last key code for app-level shortcuts ────────────────────────
uint8_t getLastKey();

// ── keyboard key codes (single‑byte I²C reports) ─────────────────────
// These match the LilyGO Keyboard_T_Deck_Master example
enum KeyCode : uint8_t {
  KEY_NONE   = 0x00,
  KEY_ESC    = 0x1B,
  KEY_ENTER  = 0x0D,
  KEY_BACK   = 0x08,
  KEY_TAB    = 0x09,
  KEY_SPACE  = 0x20,
  KEY_UP     = 0xB5,   // arrow cluster
  KEY_DOWN   = 0xB6,
  KEY_LEFT   = 0xB4,
  KEY_RIGHT  = 0xB7,
  KEY_F1     = 0xC1,
  KEY_F2     = 0xC2,
  KEY_F3     = 0xC3,
  KEY_F4     = 0xC4,
  KEY_F5     = 0xC5,
  KEY_SHIFT  = 0xD1,
  KEY_CTRL   = 0xD2,
  KEY_ALT    = 0xD3,
};

// ── trackball direction flags (used internally) ──────────────────────
enum TB : uint8_t {
  TB_UP    = 0x01,
  TB_DOWN  = 0x02,
  TB_LEFT  = 0x04,
  TB_RIGHT = 0x08,
  TB_PRESS = 0x10,
};

} // namespace td_input

#endif // BOARD_TDECK