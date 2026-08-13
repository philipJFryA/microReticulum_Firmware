// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Extern declarations for firmware globals that T-Deck app modules need.
//
// Config.h, Utilities.h and Display.h define firmware globals at header scope
// (lora_freq, radio_online, display, …), so those headers are only safe to
// include from ONE translation unit (RNode_Firmware.ino, which also includes
// TDeckUI.h). App modules in Apps/ are separate TUs and reach this state
// through the extern declarations below.
//
// The actual definitions live in Config.h / Utilities.h / Display.h (compiled
// exactly once inside the firmware's main TU).

#pragma once

#include <Arduino.h>

// ---- Config.h globals used by the Settings app -------------------------------
extern int      lora_sf;
extern int      lora_cr;
extern int      lora_txp;
extern uint32_t lora_bw;
extern uint32_t lora_freq;
extern bool     radio_online;
extern bool     hw_ready;
extern uint8_t  display_intensity;
extern uint8_t  display_contrast;

// ---- Utilities.h radio setters (Settings app) --------------------------------
extern void setFrequency();
extern void setBandwidth();
extern void setSpreadingFactor();
extern void setCodingRate();
extern void setTXPower();

// ---- Utilities.h EEPROM config persistence (Settings app) ---------------------
extern void eeprom_conf_save();

// ---- Utilities.h logging / KISS (used by framework + apps) --------------------
extern bool kiss_framed_logs;