// Copyright (C) 2024, Mark Qvist
// T-Deck LVGL Dark Theme — colours, fonts, and shared style definitions

#pragma once
#include <lvgl.h>

#ifdef BOARD_TDECK

namespace td_theme {

// ── palette ─────────────────────────────────────────────────────────
constexpr lv_color_t BLACK       = LV_COLOR_MAKE(0x00, 0x00, 0x00);
constexpr lv_color_t DARK_BG     = LV_COLOR_MAKE(0x10, 0x10, 0x14);
constexpr lv_color_t DARK_BG2    = LV_COLOR_MAKE(0x1C, 0x1C, 0x22);
constexpr lv_color_t CARD_BG     = LV_COLOR_MAKE(0x22, 0x22, 0x2A);
constexpr lv_color_t BORDER_DIM  = LV_COLOR_MAKE(0x2E, 0x2E, 0x38);
constexpr lv_color_t TEXT_MUTED  = LV_COLOR_MAKE(0x80, 0x80, 0x90);
constexpr lv_color_t TEXT_BODY   = LV_COLOR_MAKE(0xC0, 0xC0, 0xCC);
constexpr lv_color_t TEXT_TITLE  = LV_COLOR_MAKE(0xF0, 0xF0, 0xF6);
constexpr lv_color_t ACCENT      = LV_COLOR_MAKE(0x50, 0xAA, 0xFF);
constexpr lv_color_t ACCENT_DIM  = LV_COLOR_MAKE(0x30, 0x60, 0xA0);
constexpr lv_color_t WARN        = LV_COLOR_MAKE(0xFF, 0xA0, 0x40);
constexpr lv_color_t DANGER      = LV_COLOR_MAKE(0xFF, 0x50, 0x50);
constexpr lv_color_t SUCCESS     = LV_COLOR_MAKE(0x40, 0xC0, 0x70);

// ── layout constants (pixels) ───────────────────────────────────────
namespace Layout {
  // display
  constexpr int SCREEN_W = 320;
  constexpr int SCREEN_H = 240;

  // dashboard dual‑card columns
  constexpr int DASH_COL_W    = 152;
  constexpr int DASH_COL_H    = 140;
  constexpr int DASH_COL_XL   = 4;
  constexpr int DASH_COL_XR   = -4;
  constexpr int DASH_COL_Y    = -12;

  // content area below title
  constexpr int CONTENT_W     = 308;
  constexpr int CONTENT_H     = 200;
  constexpr int CONTENT_BOT   = -4;

  // settings scroll section
  constexpr int SETTINGS_W    = 310;
  constexpr int SETTINGS_H    = 215;
  constexpr int SECTION_W     = 298;
  constexpr int ROW_W         = 280;

  // button sizes
  constexpr int BACK_BTN_W    = 48;
  constexpr int BACK_BTN_H    = 24;
  constexpr int REBOOT_BTN_W  = 120;
  constexpr int REBOOT_BTN_H  = 32;

  // row heights
  constexpr int ROW_H_SM      = 22;   // dashboard row
  constexpr int ROW_H_MD      = 36;   // slider row
  constexpr int ROW_H_LG      = 40;   // list item

  // dropdown widths
  constexpr int DD_W_MD       = 80;
  constexpr int DD_W_LG       = 100;

  // slider width
  constexpr int SLIDER_W      = 120;

  // pads
  constexpr int PAD_TITLE_X   = 8;
  constexpr int PAD_TITLE_Y   = 4;
  constexpr int PAD_CARD      = 8;
  constexpr int PAD_CONTENT   = 6;
  constexpr int PAD_ROW       = 4;
  constexpr int PAD_ROW_SM    = 2;

  // titles / text
  constexpr int FONT_TITLE    = 16;
  constexpr int FONT_BODY     = 14;
  constexpr int FONT_MUTED    = 12;
} // namespace Layout

// ── style handles ───────────────────────────────────────────────────
extern lv_style_t style_screen;
extern lv_style_t style_card;
extern lv_style_t style_title;
extern lv_style_t style_body;
extern lv_style_t style_label_muted;
extern lv_style_t style_btn;
extern lv_style_t style_btn_selected;
extern lv_style_t style_slider;
extern lv_style_t style_switch_bg;
extern lv_style_t style_switch_indicator;
extern lv_style_t style_dropdown;
extern lv_style_t style_dropdown_list;
extern lv_style_t style_list_item;

// ── initialise all styles (idempotent) ──────────────────────────────
void init();
bool isInitialized();

} // namespace td_theme

#endif // BOARD_TDECK