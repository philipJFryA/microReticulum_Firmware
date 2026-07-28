// Copyright (C) 2024, Mark Qvist
// T-Deck dashboard — status overview with key metrics

#pragma once
#ifdef BOARD_TDECK

#include "BaseScreen.h"

class DashboardScreen : public BaseScreen {
public:
  void create(lv_obj_t *parent) override;
  void update() override;
  void enter() override;

private:
  lv_obj_t *titleLabel;
  lv_obj_t *boardLabel;
  lv_obj_t *frequencyLabel;
  lv_obj_t *bandwidthLabel;
  lv_obj_t *spreadingFactorLabel;
  lv_obj_t *codingRateLabel;
  lv_obj_t *txPowerLabel;
  lv_obj_t *stateLabel;
  lv_obj_t *uptimeLabel;
  lv_obj_t *airUtilLabel;
  lv_obj_t *packetsRxLabel;
  lv_obj_t *packetsTxLabel;
  lv_obj_t *batteryLabel;
  lv_obj_t *currentLabel;
  lv_obj_t *temperatureLabel;
};

#endif // BOARD_TDECK