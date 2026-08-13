// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Settings application: device settings persisted to /settings.yaml on the
// T-Deck's microSD card (node name, brightness, blank timeout, GPS, timezone,
// announce interval, radio parameters, save/reload/export). Self-contained
// module (header + AppSettings.cpp) that registers with TDeckAppRegistry.

#pragma once

#include "TDeckApp.h"

class AppSettings : public TDeckApp {
  public:
    AppSettings() : TDeckApp("Settings", 3, SCREEN_SETTINGS, SET_LIST) {}

    void draw() override;
    void onSelect() override;
    void onUp() override;
    void onDown() override;
    void onBack() override;
    void onKey(char key, uint8_t state) override;
    void onTrackball(tb_event_t event) override;
    bool inTextInput() override { return false; }   // name edits via Reticulum app
    void onValueEdit(int dir) override;
};