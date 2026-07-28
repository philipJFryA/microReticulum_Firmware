// Copyright (C) 2024, Mark Qvist
// T-Deck messages screen — recent Reticulum announces

#pragma once
#ifdef BOARD_TDECK

#include "BaseScreen.h"
#include <lvgl.h>

class MessagesScreen : public BaseScreen {
public:
  void create(lv_obj_t *parent) override;
  void update() override;
  void enter() override;

private:
  lv_obj_t *titleLabel;
  lv_obj_t *list;
  lv_obj_t *backBtn;
  lv_obj_t *emptyLabel;

  static char msgs[10][128];
  static uint8_t msgHead;
  static uint8_t msgCount;
  static uint32_t lastPacketsRx;

  void rebuildList();

public:
  // Called from UI tick loop when new announce/message arrives
  static void pushMessage(const char *text);
};

#endif // BOARD_TDECK