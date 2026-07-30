// Copyright (C) 2024, Mark Qvist
// T-Deck LVGL UI system orchestrator - initialisation and main loop
#pragma once
#include <stdint.h>
#ifdef BOARD_TDECK
namespace td_ui {
void begin();
void update();
void showPage(int pageIndex);
enum Page : uint8_t {
  PAGE_DASHBOARD = 0,
  PAGE_MESSAGES  = 1,
  PAGE_SETTINGS  = 2,
  PAGE_LOG       = 3,
  PAGE_COUNT
};
} // namespace td_ui
#endif // BOARD_TDECK
