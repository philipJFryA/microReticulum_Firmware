// Copyright (C) 2024, Mark Qvist
// T-Deck LVGL UI system orchestrator — initialisation and main loop

#pragma once

#ifdef BOARD_TDECK

namespace td_ui {

// ── initialise display, theme, input, and build all pages ────────────
void begin();

// ── tick (call from loop() as often as possible) ─────────────────────
void update();

// ── switch to a named page (by index from enum) ──────────────────────
void showPage(int pageIndex);

// ── page indices (match tabview order) ───────────────────────────────
enum Page : uint8_t {
  PAGE_DASHBOARD = 0,
  PAGE_MESSAGES  = 1,
  PAGE_SETTINGS  = 2,
  PAGE_LOG       = 3,
  PAGE_COUNT
};

} // namespace td_ui

#endif // BOARD_TDECK