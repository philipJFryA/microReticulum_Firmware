// Copyright (C) 2024, Mark Qvist
// T-Deck settings screen — all device configuration

#pragma once
#ifdef BOARD_TDECK

#include <lvgl.h>
#include "BaseScreen.h"

class SettingsScreen : public BaseScreen {
public:
  void create(lv_obj_t *parent) override;
  void update() override;
  void enter() override;
  void leave() override;

private:
  lv_obj_t *titleLabel;

  // frequency
  lv_obj_t *freqSlider;
  lv_obj_t *freqLabel;

  // bandwidth dropdown
  lv_obj_t *bwDropdown;

  // spreading factor
  lv_obj_t *sfDropdown;

  // coding rate
  lv_obj_t *crDropdown;

  // TX power
  lv_obj_t *txpSlider;
  lv_obj_t *txpLabel;

  // backlight
  lv_obj_t *blSlider;
  lv_obj_t *blLabel;

  // reset button
  lv_obj_t *rebootBtn;

  // helpers
  void applyChanges();
};

#endif // BOARD_TDECK