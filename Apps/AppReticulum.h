// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Reticulum application: ping and traceroute contacts, list direct contacts,
// view/edit the local identity. Self-contained module (header +
// AppReticulum.cpp) that registers with TDeckAppRegistry.

#pragma once

#include "TDeckApp.h"

class AppReticulum : public TDeckApp {
  public:
    AppReticulum() : TDeckApp("Reticulum", 2, SCREEN_RETICULUM, RET_MENU) {}

    void draw() override;
    void onSelect() override;
    void onUp() override;
    void onDown() override;
    void onBack() override;
    void onKey(char key, uint8_t state) override;
    void onTrackball(tb_event_t event) override;
    bool inTextInput() override { return tdeck_sub == RET_NAME_EDIT; }
    void onTextApply(uint8_t ctx) override;
    bool onLoop() override;
#ifdef HAS_RNS
    void onRnsSetup() override;
    bool onRnsLoop() override;
#endif
};