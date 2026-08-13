// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// SAR (Situational Awareness) application: shares periodic encrypted position
// beacons with a configured set of peer nodes over "tdeck_ui.sar.position",
// and renders a Web Mercator / XYZ slippy-map on the T-Deck screen.
//
// Self-contained module (header + AppSAR.cpp) that registers with
// TDeckAppRegistry. Owns all SAR state (beacon peers, map view, position
// tracks, tile-load debug) as file-local statics in the implementation.

#pragma once

#include "TDeckApp.h"

class AppSAR : public TDeckApp {
  public:
    AppSAR() : TDeckApp("SAR", 4, SCREEN_SAR, SAR_MAP) {}

  void draw() override;
  void onActivate() override;
  void onSelect() override;
    void onUp() override;
    void onDown() override;
    void onBack() override;
    void onKey(char key, uint8_t state) override;
    bool onNavKey(char key, uint8_t state) override;
    void onTrackball(tb_event_t event) override;
    void onValueEdit(int dir) override;
    bool onLoop() override;
#ifdef HAS_RNS
    void onRnsSetup() override;
    bool onRnsLoop() override;
#endif
};