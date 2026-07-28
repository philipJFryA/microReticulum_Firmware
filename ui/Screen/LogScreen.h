// Copyright (C) 2024, Mark Qvist
// T-Deck log screen — scolling system log

#pragma once
#ifdef BOARD_TDECK

#include "BaseScreen.h"
#include <lvgl.h>

class LogScreen : public BaseScreen {
public:
  void create(lv_obj_t *parent) override;
  void update() override;
  void enter() override;

  static void pushLine(const char *line);

private:
  static constexpr uint8_t MAX_LINES = 100;
  static char     logBuf[MAX_LINES][128];
  static uint16_t logHead;
  static uint16_t logCount;
  
  lv_obj_t *titleLabel;
  lv_obj_t *textarea;
  lv_obj_t *backBtn;

  void rebuildTextarea();
};

#endif // BOARD_TDECK