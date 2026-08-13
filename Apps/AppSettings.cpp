// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Settings application implementation.
//
// Self-contained module. Renders the Settings list / value editor and
// implements row actions (node name edit, brightness, blank timeout, GPS
// toggle, timezone, announce interval, radio parameters, SD save/reload,
// contact export). Radio parameter changes call the firmware's live radio
// setters via TDeckFirmwareExterns.h.

#include "AppSettings.h"

#include <Arduino.h>
#include <string.h>

#include "../TDeckFirmwareExterns.h"

// ---- Self-registration -------------------------------------------------------
namespace {
AppSettings s_app;
TDeckAppRegistrar s_reg(&s_app);
} // namespace

// ============================================================================
// Row model shared by the list and the value editor
// ============================================================================

// Fills `label`/`value` for a given row; `action` marks rows that execute
// immediately instead of being edited numerically.
static void set_row(uint8_t idx, char* label, size_t n, char* value, size_t vn, bool* action) {
    switch (idx) {
        case 0: snprintf(label, n, "Node name");   snprintf(value, vn, "%s", tdeck_set_device_name); *action = true; break;
        case 1: snprintf(label, n, "Brightness");  snprintf(value, vn, "%u", tdeck_set_brightness);   *action = false; break;
        case 2: snprintf(label, n, "Blank timeout"); snprintf(value, vn, "%us", tdeck_set_blank_timeout); *action = false; break;
        case 3: snprintf(label, n, "Location Enabled"); snprintf(value, vn, "%s", tdeck_set_gps_enabled ? "yes" : "no"); *action = false; break;
        case 4: {
            snprintf(label, n, "Timezone");
            int h = tdeck_set_tz_offset_min / 60;
            int m = tdeck_set_tz_offset_min % 60;
            if (m < 0) m = -m;
            if (tdeck_set_tz_offset_min == 0) snprintf(value, vn, "UTC");
            else snprintf(value, vn, "UTC%+d:%02d", h, m);
            *action = false;
            break;
        }
        case 5: snprintf(label, n, "Announce min"); snprintf(value, vn, "%u", tdeck_set_announce_interval); *action = false; break;
        case 6: snprintf(label, n, "Confirm send"); snprintf(value, vn, "%s", tdeck_set_confirm_send ? "yes" : "no"); *action = false; break;
        case 7: {   // Radio frequency (Hz)
            snprintf(label, n, "Radio freq");
            if (lora_freq > 0) snprintf(value, vn, "%u.%03u MHz", lora_freq / 1000000, (lora_freq / 1000) % 1000);
            else snprintf(value, vn, "unset");
            *action = false;
            break;
        }
        case 8: {   // Radio bandwidth (Hz)
            snprintf(label, n, "Radio BW");
            if (lora_bw > 0) {
                if (lora_bw % 1000 == 0) snprintf(value, vn, "%u kHz", lora_bw / 1000);
                else snprintf(value, vn, "%.1f kHz", lora_bw / 1000.0f);
            } else {
                snprintf(value, vn, "unset");
            }
            *action = false;
            break;
        }
        case 9: {   // Spreading factor
            snprintf(label, n, "Radio SF");
            if (lora_sf > 0) snprintf(value, vn, "%d", lora_sf);
            else snprintf(value, vn, "unset");
            *action = false;
            break;
        }
        case 10: {  // Coding rate (LoRa 4/x)
            snprintf(label, n, "Radio CR");
            snprintf(value, vn, "%d", lora_cr);
            *action = false;
            break;
        }
        case 11: {  // TX power (dBm)
            snprintf(label, n, "Radio TXP");
            if (lora_txp != 0xFF) snprintf(value, vn, "%d dBm", lora_txp);
            else snprintf(value, vn, "unset");
            *action = false;
            break;
        }
        case 12: snprintf(label, n, "Save to SD");  snprintf(value, vn, ""); *action = true; break;
        case 13: snprintf(label, n, "Reload from SD"); snprintf(value, vn, ""); *action = true; break;
        default: snprintf(label, n, "Export contacts"); snprintf(value, vn, ""); *action = true; break;
    }
}

// ============================================================================
// Drawing
// ============================================================================

void AppSettings::draw() {
    TDeckCanvas& c = tdeck_canvas;
    if (tdeck_sub == SET_LIST) {
        uint8_t rows = tdeck_ui_list_frame(c, "Settings", TDECK_SET_ROWS, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < TDECK_SET_ROWS; r++) {
            uint8_t idx = tdeck_scroll + r;
            char label[32], value[40];
            bool action = false;
            set_row(idx, label, sizeof(label), value, sizeof(value), &action);
            char line[72];
            if (action && !value[0]) snprintf(line, sizeof(line), "%s", label);
            else snprintf(line, sizeof(line), "%s: %s", label, value);
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: edit  L: back");
    } else if (tdeck_sub == SET_EDIT) {
        char label[32], value[40];
        bool action = false;
        set_row(tdeck_set_edit_idx, label, sizeof(label), value, sizeof(value), &action);
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 8);
        c.print(label);
        c.drawFastHLine(0, TDECK_SBAR_H + 32, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(2);
        c.setTextColor(TDECK_COL_FG);
        c.setCursor(20, TDECK_SBAR_H + 60);
        c.print(value);
        c.setTextSize(1);
        c.setTextColor(TDECK_COL_DIM);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 100);
        c.print("UP/DOWN: change   Enter: save   L: back");
        tdeck_ui_draw_footer(c, "changes are saved on 'Save to SD'");
    }
}

// ============================================================================
// Value editor (rows 0-14)
// ============================================================================

void AppSettings::onValueEdit(int dir) {
    switch (tdeck_set_edit_idx) {
        case 1: {
            int v = (int)tdeck_set_brightness + dir * 16;
            tdeck_set_brightness = (uint8_t)constrain(v, 0, 255);
            tdeck_apply_brightness();
            tdeck_settings_dirty = true;
            break;
        }
        case 2: {
            int v = (int)tdeck_set_blank_timeout + dir * 15;
            tdeck_set_blank_timeout = (uint16_t)constrain(v, 0, 600);
            tdeck_settings_dirty = true;
            break;
        }
        case 3: tdeck_set_gps_enabled = !tdeck_set_gps_enabled; tdeck_settings_dirty = true; break;
        case 4: {   // Timezone offset: +/- 15 min steps, clamped to ±24 h
            int v = (int)tdeck_set_tz_offset_min + dir * 15;
            tdeck_set_tz_offset_min = (int16_t)constrain(v, -1440, 1440);
            tdeck_settings_dirty = true;
            break;
        }
        case 5: {
            int v = (int)tdeck_set_announce_interval + dir * 5;
            tdeck_set_announce_interval = (uint16_t)constrain(v, 0, 240);
            tdeck_settings_dirty = true;
            break;
        }
        case 6: tdeck_set_confirm_send = !tdeck_set_confirm_send; tdeck_settings_dirty = true; break;
        case 7: {   // Radio frequency: +/- 100 kHz
            long v = (lora_freq > 0) ? (long)lora_freq : 868000000L;
            v += (long)dir * 100000L;
            lora_freq = (uint32_t)constrain(v, 100000000L, 1000000000L);
            if (radio_online) setFrequency();
            tdeck_settings_dirty = true;
            break;
        }
        case 8: {   // Radio bandwidth: cycle through valid SX126x options
            uint8_t best = 0;
            if (lora_bw > 0) {
                for (uint8_t i = 0; i < TDECK_BW_OPTIONS; i++) {
                    if (tdeck_bw_options[i] <= lora_bw) best = i;
                }
            }
            if (dir > 0) {
                if (best + 1 < TDECK_BW_OPTIONS) best++;
            } else {
                if (best > 0) best--;
            }
            lora_bw = tdeck_bw_options[best];
            if (radio_online) setBandwidth();
            tdeck_settings_dirty = true;
            break;
        }
        case 9: {   // Spreading factor: 5-12
            int v = (lora_sf > 0) ? lora_sf : 7;
            v += dir;
            lora_sf = constrain(v, 5, 12);
            if (radio_online) setSpreadingFactor();
            tdeck_settings_dirty = true;
            break;
        }
        case 10: {  // Coding rate: 4/5 - 4/8
            int v = (lora_cr > 0) ? lora_cr : 5;
            v += dir;
            lora_cr = constrain(v, 5, 8);
            if (radio_online) setCodingRate();
            tdeck_settings_dirty = true;
            break;
        }
        case 11: {  // TX power: -9 to 22 dBm
            int v = (lora_txp != 0xFF) ? lora_txp : 17;
            v += dir;
            lora_txp = constrain(v, -9, 22);
            if (radio_online) setTXPower();
            tdeck_settings_dirty = true;
            break;
        }
        default: break;
    }
    tdeck_dirty = true;
}

// ============================================================================
// Navigation
// ============================================================================

void AppSettings::onSelect() {
    if (tdeck_sub == SET_LIST) {
        switch (tdeck_cursor) {
            case 0: // node name -> text input (Reticulum app owns ctx 2 commit)
                tdeck_input_start(2, tdeck_set_device_name);
                tdeck_ui_nav_push(SCREEN_RETICULUM, RET_NAME_EDIT);
                break;
            case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
                tdeck_set_edit_idx = tdeck_cursor;
                tdeck_ui_nav_push(SCREEN_SETTINGS, SET_EDIT);
                break;
            case 12:
                if (tdeck_settings_save()) {
                    // Radio parameters are also persisted into EEPROM so a
                    // reboot applies them via the normal eeprom_conf_load()
                    // + startRadio() path.
                    if (hw_ready && radio_online) {
                        eeprom_conf_save();
                        tdeck_toast_set("Saved (SD+EEPROM)");
                    } else {
                        tdeck_toast_set("Saved to SD only");
                    }
                } else {
                    tdeck_toast_set("SD save failed");
                }
                break;
            case 13:
                tdeck_settings_load();
                tdeck_apply_brightness();
                if (radio_online) {
                    setBandwidth();
                    setSpreadingFactor();
                    setCodingRate();
                    setTXPower();
                    setFrequency();
                }
                tdeck_toast_set("Reloaded");
                break;
            case 14:
                if (tdeck_contacts_export()) tdeck_toast_set("Exported");
                else tdeck_toast_set("Export failed");
                break;
            default: break;
        }
    } else if (tdeck_sub == SET_EDIT) {
        tdeck_settings_dirty = true;
        tdeck_ui_nav_back();
    }
    tdeck_dirty = true;
}

void AppSettings::onUp() {
    if (tdeck_sub == SET_LIST) {
        if (tdeck_cursor > 0) tdeck_cursor--;
    } else if (tdeck_sub == SET_EDIT) {
        onValueEdit(+1);
    }
    tdeck_dirty = true;
}

void AppSettings::onDown() {
    if (tdeck_sub == SET_LIST) {
        if (tdeck_cursor + 1 < TDECK_SET_ROWS) tdeck_cursor++;
    } else if (tdeck_sub == SET_EDIT) {
        onValueEdit(-1);
    }
    tdeck_dirty = true;
}

void AppSettings::onBack() {
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}

void AppSettings::onKey(char key, uint8_t state) {
    (void)key;
    (void)state;
}

void AppSettings::onTrackball(tb_event_t event) {
    switch (event) {
        case TB_EVENT_UP:   onUp(); break;
        case TB_EVENT_DOWN: onDown(); break;
        case TB_EVENT_LEFT: onBack(); break;
        case TB_EVENT_RIGHT:
        case TB_EVENT_PRESS: onSelect(); break;
        default: break;
    }
    tdeck_dirty = true;
}