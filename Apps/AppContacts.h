// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Contacts application: browse Reticulum contacts (from announces and the
// persisted known-destinations store), favourite them, give them a readable
// alias, save and export them. Self-contained module (header + AppContacts.cpp)
// that registers with TDeckAppRegistry.

#pragma once

#include "TDeckApp.h"

class AppContacts : public TDeckApp {
  public:
    AppContacts() : TDeckApp("Contacts", 1, SCREEN_CONTACTS, CT_LIST) {}

    void draw() override;
    void onSelect() override;
    void onUp() override;
    void onDown() override;
    void onBack() override;
    void onKey(char key, uint8_t state) override;
    void onTrackball(tb_event_t event) override;
    bool inTextInput() override { return tdeck_sub == CT_ALIAS; }
    void onTextApply(uint8_t ctx) override;
};