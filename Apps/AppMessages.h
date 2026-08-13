// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Messages application: send/receive point-to-point text messages to other
// nodes running this UI over the standard "lxmf.delivery" aspect, plus
// unencrypted broadcasts over "broadcast". Self-contained module (header +
// AppMessages.cpp) that registers with TDeckAppRegistry.

#pragma once

#include "TDeckApp.h"

class AppMessages : public TDeckApp {
  public:
    AppMessages() : TDeckApp("Messages", 0, SCREEN_MESSAGES, MSG_INBOX) {}

    void draw() override;
    void onSelect() override;
    void onUp() override;
    void onDown() override;
    void onBack() override;
    void onKey(char key, uint8_t state) override;
    void onTrackball(tb_event_t event) override;
    bool inTextInput() override { return tdeck_sub == MSG_COMPOSE; }
    void onTextApply(uint8_t ctx) override;
#ifdef HAS_RNS
    void onRnsSetup() override;
#endif
};