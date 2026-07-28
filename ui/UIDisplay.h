// Copyright (C) 2024, Mark Qvist
// T-Deck ST7789 display driver via Arduino_GFX with LVGL flush callback

#pragma once

#ifdef BOARD_TDECK

#include <lvgl.h>
#include <Arduino_GFX_Library.h>

namespace td_display {

// ── constants ────────────────────────────────────────────────────────
extern const int WIDTH;
extern const int HEIGHT;

// ── initialise display hardware and register LVGL display driver ─────
// Returns true on success.
bool init();

// ── set backlight level 0-15 (0 = off) ───────────────────────────────
void setBacklight(uint8_t level);

// ── LVGL flush callback — copies the dirty area to the TFT ───────────
void flushCb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);

// ── raw buffer for debug / fallback ──────────────────────────────────
extern lv_color_t *drawBuf1;

} // namespace td_display

#endif // BOARD_TDECK