// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Contacts application implementation.
//
// Self-contained module: depends only on the framework contract in
// TDeckApp.h. Implements the contact list / detail / alias-editing
// sub-screens and manages contact favourites, aliases, export and deletion.

#include "AppContacts.h"

#include <Arduino.h>
#include <string.h>

// ---- Self-registration -------------------------------------------------------
// Static init happens before setup(), so the launcher always sees this app.
namespace {
AppContacts s_app;
TDeckAppRegistrar s_reg(&s_app);
} // namespace

// ============================================================================
// Drawing
// ============================================================================

void AppContacts::draw() {
    TDeckCanvas& c = tdeck_canvas;
    if (tdeck_sub == CT_LIST) {
        uint8_t total = tdeck_contact_count;
        uint8_t rows = tdeck_ui_list_frame(c, "Contacts", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[80];
            tdeck_contact_t& ct = tdeck_contacts[idx];
            char label[48];
            tdeck_contact_label(&ct, label, sizeof(label));
            snprintf(line, sizeof(line), "%s%s", ct.favorite ? "* " : "", label);
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        if (total == 0) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts yet - wait for announces");
        }
        tdeck_ui_draw_footer(c, "select: detail  L: back");
    } else if (tdeck_sub == CT_DETAIL) {
        tdeck_contact_t* ct = (tdeck_contact_detail_idx < tdeck_contact_count)
                              ? &tdeck_contacts[tdeck_contact_detail_idx] : NULL;
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
        c.print("Contact");
        c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(1);
        int y = TDECK_SBAR_H + 34;
        if (ct) {
            char label[48];
            tdeck_contact_label(ct, label, sizeof(label));
            c.setTextColor(TDECK_COL_FG);
            c.setCursor(TDECK_LIST_X, y); c.print("Name: "); c.print(label); y += 12;
            c.setTextColor(TDECK_COL_DIM);
            c.setCursor(TDECK_LIST_X, y); c.print("Hash: "); c.print(ct->hash); y += 12;
            if (ct->app_data[0]) {
                char clean[TDECK_APP_DATA_MAX+1];
                tdeck_name_clean(ct->app_data, clean, sizeof(clean));
                c.setCursor(TDECK_LIST_X, y); c.print("Ann: "); c.print(clean); y += 12;
            }
            if (ct->has_key)  { c.setCursor(TDECK_LIST_X, y); c.print("Key: yes"); }
            else              { c.setCursor(TDECK_LIST_X, y); c.print("Key: no announce yet"); }
            y += 12;
            if (ct->lat != 0.0f || ct->lon != 0.0f) {
                c.setTextColor(TDECK_COL_OK);
                c.setCursor(TDECK_LIST_X, y);
                c.printf("Pos: %.4f, %.4f  %uSV", (double)ct->lat, (double)ct->lon, ct->sat);
                y += 12;
                c.setTextColor(TDECK_COL_DIM);
                c.setCursor(TDECK_LIST_X, y);
                c.printf("Alt: %.0f m  Age: %.0f s", (double)ct->alt, (double)ct->age_s);
                y += 12;
            }
        } else {
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, y); c.print("Contact lost"); y += 12;
        }
        c.drawFastHLine(0, y + 4, TDECK_SCREEN_W, TDECK_COL_DIM);

        // Actions (drawn below the info block)
        static const char* actions[] = {
            "Toggle favorite", "Edit alias", "Send message",
            "Export all", "Delete contact", "Back"
        };
        uint8_t ay = y + 12;
        if (ay < TDECK_SBAR_H + 30) ay = TDECK_SBAR_H + 30;
        uint8_t n = 6;
        uint8_t rows = (TDECK_SCREEN_H - TDECK_FOOT_H - ay) / 16;
        if (rows > n) rows = n;
        if (tdeck_cursor >= n) tdeck_cursor = n - 1;
        for (uint8_t r = 0; r < rows; r++) {
            uint8_t idx = r;
            char line[48];
            if (idx == 0 && ct) snprintf(line, sizeof(line), "%s", ct->favorite ? "Unfavorite" : "Favorite");
            else if (idx < n) snprintf(line, sizeof(line), "%s", actions[idx]);
            else snprintf(line, sizeof(line), "");
            tdeck_ui_draw_row(c, ay + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: run  L: back");
    } else if (tdeck_sub == CT_ALIAS) {
        tdeck_ui_draw_text_input("Alias", tdeck_input_buf);
    }
}

// ============================================================================
// Navigation
// ============================================================================

void AppContacts::onSelect() {
    if (tdeck_sub == CT_LIST) {
        if (tdeck_cursor < tdeck_contact_count) {
            tdeck_contact_detail_idx = tdeck_cursor;
            tdeck_ui_nav_push(SCREEN_CONTACTS, CT_DETAIL);
        }
    } else if (tdeck_sub == CT_DETAIL) {
        switch (tdeck_cursor) {
            case 0: { // toggle favorite
                if (tdeck_contact_detail_idx < tdeck_contact_count) {
                    tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
                    c->favorite = !c->favorite;
                    c->persisted = true;
                    tdeck_settings_dirty = true;
                    tdeck_toast_set(c->favorite ? "Favorited" : "Unfavorited");
                }
                break;
            }
            case 1: { // edit alias
                if (tdeck_contact_detail_idx < tdeck_contact_count) {
                    tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
                    tdeck_input_start(1, c->alias);
                    tdeck_ui_nav_push(SCREEN_CONTACTS, CT_ALIAS);
                }
                break;
            }
            case 2: { // send message -> Messages compose
                if (tdeck_contact_detail_idx < tdeck_contact_count) {
                    tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
                    strncpy(tdeck_input_peer, c->hash, sizeof(tdeck_input_peer)-1);
                    tdeck_input_start(0, "");
                    tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
                }
                break;
            }
            case 3: // export all
                if (tdeck_contacts_export()) tdeck_toast_set("Exported");
                else tdeck_toast_set("Export failed");
                break;
            case 4: { // delete contact
                if (tdeck_contact_detail_idx < tdeck_contact_count) {
                    memmove(&tdeck_contacts[tdeck_contact_detail_idx],
                            &tdeck_contacts[tdeck_contact_detail_idx + 1],
                            sizeof(tdeck_contact_t) *
                                (tdeck_contact_count - tdeck_contact_detail_idx - 1));
                    tdeck_contact_count--;
                    tdeck_settings_dirty = true;
                }
                tdeck_ui_nav_back();
                break;
            }
            default:
                tdeck_ui_nav_back();
                break;
        }
    } else if (tdeck_sub == CT_ALIAS) {
        tdeck_input_apply();
    }
    tdeck_dirty = true;
}

void AppContacts::onUp() {
    if (tdeck_sub == CT_LIST || tdeck_sub == CT_DETAIL) {
        if (tdeck_cursor > 0) tdeck_cursor--;
    }
    tdeck_dirty = true;
}

void AppContacts::onDown() {
    if (tdeck_sub == CT_LIST) {
        if (tdeck_cursor + 1 < tdeck_contact_count) tdeck_cursor++;
    } else if (tdeck_sub == CT_DETAIL) {
        if (tdeck_cursor + 1 < 6) tdeck_cursor++;
    }
    tdeck_dirty = true;
}

void AppContacts::onBack() {
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}

void AppContacts::onKey(char key, uint8_t state) {
    // Text field keys are handled by the framework (inTextInput()).
    (void)key;
    (void)state;
}

void AppContacts::onTrackball(tb_event_t event) {
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

// ============================================================================
// Text input commit: contact alias (ctx 1)
// ============================================================================

void AppContacts::onTextApply(uint8_t ctx) {
    if (ctx != 1) return;   // ctx 0 (compose) and ctx 2 (node name) elsewhere

    tdeck_contact_t* c = &tdeck_contacts[tdeck_contact_detail_idx];
    if (c && c->hash[0]) {
        if (tdeck_input_len == 0) {
            c->alias[0] = '\0';
        } else {
            strncpy(c->alias, tdeck_input_buf, TDECK_ALIAS_MAX);
            c->alias[TDECK_ALIAS_MAX] = '\0';
        }
        c->persisted = true;
        tdeck_settings_dirty = true;
        tdeck_toast_set("Alias saved");
    }
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}