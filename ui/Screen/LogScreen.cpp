// Copyright (C) 2024, Mark Qvist
// T-Deck log screen implementation

#include <Arduino.h>
#include "LogScreen.h"
#include "../UITheme.h"

#ifdef BOARD_TDECK

using namespace td_theme::Layout;

// ── static ring buffer ────────────────────────────────────────────────
char     LogScreen::logBuf[MAX_LINES][128] = {};
uint16_t LogScreen::logHead  = 0;
uint16_t LogScreen::logCount = 0;

void LogScreen::pushLine(const char *line) {
  if (!line) return;
  // Truncate to buffer size, guarantee null termination
  snprintf(logBuf[logHead], sizeof(logBuf[0]), "%s", line);
  logHead = (logHead + 1) % MAX_LINES;
  if (logCount < MAX_LINES) logCount++;
}

// ── build a single large string from ring buffer ─────────────────────
// Uses a thread‑local static buffer (BSS, zero‑cost) instead of a
// 12.8 KB stack frame. Sized for MAX_LINES × 128 chars.
void LogScreen::rebuildTextarea() {
  if (!textarea) return;
  if (!lv_obj_is_valid(textarea)) return;

  // Thread‑local so multiple LogScreen instances (should never happen,
  // but just in case) don't clobber each other.
  static char full[MAX_LINES * 128];
  uint16_t pos = 0;
  uint16_t oldest = logCount < MAX_LINES ? 0 : logHead;

  for (uint16_t i = 0; i < logCount && pos < sizeof(full) - 2; i++) {
    uint16_t idx = (oldest + i) % MAX_LINES;
    // Manual copy to avoid strncat quadratic behavior
    const char *src = logBuf[idx];
    while (*src && pos < sizeof(full) - 2) { full[pos++] = *src++; }
    if (pos < sizeof(full) - 2) { full[pos++] = '\n'; }
  }
  full[pos] = '\0';

  lv_textarea_set_text(textarea, full);

  // Scroll to bottom
  lv_obj_t *ta = lv_textarea_get_label(textarea);
  if (ta) {
    lv_obj_scroll_to_y(ta, LV_COORD_MAX, LV_ANIM_OFF);
  }
}

void LogScreen::create(lv_obj_t *parent) {
  container = lv_obj_create(parent);
  lv_obj_add_style(container, &td_theme::style_screen, 0);
  lv_obj_set_size(container, SCREEN_W, SCREEN_H);
  lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

  titleLabel = lv_label_create(container);
  lv_label_set_text(titleLabel, "System Log");
  lv_obj_add_style(titleLabel, &td_theme::style_title, 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, PAD_TITLE_X, PAD_TITLE_Y);

  backBtn = lv_btn_create(container);
  lv_obj_set_size(backBtn, BACK_BTN_W, BACK_BTN_H);
  lv_obj_add_style(backBtn, &td_theme::style_btn, 0);
  lv_obj_add_style(backBtn, &td_theme::style_btn_selected, LV_STATE_FOCUSED);
  lv_obj_align(backBtn, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_obj_t *backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
  lv_obj_center(backLbl);

  textarea = lv_textarea_create(container);
  lv_obj_set_size(textarea, CONTENT_W, CONTENT_H);
  lv_obj_align(textarea, LV_ALIGN_BOTTOM_MID, 0, CONTENT_BOT);
  lv_obj_add_style(textarea, &td_theme::style_card, 0);
  lv_textarea_set_cursor_click_pos(textarea, false);
  lv_obj_set_style_text_color(textarea, td_theme::TEXT_BODY, 0);
  lv_obj_set_style_text_font(textarea, &lv_font_montserrat_12, 0);
  lv_obj_set_style_pad_all(textarea, PAD_CONTENT, 0);

  // Seed with initial message
  pushLine("T-Deck UI started");
  pushLine("Firmware v1.0");
  rebuildTextarea();
}

void LogScreen::update() {
  // Log updates are push-driven; nothing to poll
}

void LogScreen::enter() {
  rebuildTextarea();
}

#endif // BOARD_TDECK