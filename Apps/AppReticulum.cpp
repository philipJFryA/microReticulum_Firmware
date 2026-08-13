// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Reticulum application implementation.
//
// Self-contained module. Provides the extern ping/trace helpers declared in
// TDeckApp.h (tdeck_rns_ping_contact / tdeck_rns_ping_poll /
// tdeck_rns_trace_contact), owns the "ping" RNS aspect handler (answers
// !mrping! and collects !mrpong!), and renders the menu / ping-pick /
// ping-run / trace-pick / trace-run / direct / identity / name-edit screens.

#include "AppReticulum.h"

#include <Arduino.h>
#include <string.h>

#include "../TDeckFirmwareExterns.h"

#ifdef HAS_RNS
#include <microReticulum.h>
#endif

// ---- Self-registration -------------------------------------------------------
namespace {
AppReticulum s_app;
TDeckAppRegistrar s_reg(&s_app);
} // namespace

// ============================================================================
// RNS ping aspect (answers !mrping!, collects !mrpong!)
// ============================================================================

#ifdef HAS_RNS

// Packet callback for the local "tdeck_ui.ping" IN destination. Answers ping
// requests and collects pong replies for the in-flight ping.
static bool on_ping(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();

    if (s.rfind(TDECK_FRAME_PING, 0) == 0) {
        char seq[16], sent[24], pub[140], sender_dest[40];
        const char* p = s.c_str() + strlen(TDECK_FRAME_PING);
        p = tdeck_rns_field(p, 0, seq, sizeof(seq));
        if (!p) return true;
        p = tdeck_rns_field(p, 0, sent, sizeof(sent));
        if (!p) return true;
        p = tdeck_rns_field(p, 0, pub, sizeof(pub));
        if (!p || strlen(pub) != 128) return true;
        // sender_dest is the final field: tdeck_rns_field returns NULL for the
        // last field (no trailing '!') but still copies the bytes.
        tdeck_rns_field(p, 0, sender_dest, sizeof(sender_dest));
        if (strlen(sender_dest) != 32) return true;

        uint8_t key[64], dest_raw[16];
        if (!tdeck_hex_decode(pub, key, 64)) return true;
        if (!tdeck_hex_decode(sender_dest, dest_raw, 16)) return true;
        RNS::Identity rid(false);
        rid.load_public_key(RNS::Bytes(key, 64));
        RNS::Destination out(rid, RNS::Type::Destination::OUT,
                             RNS::Type::Destination::SINGLE, RNS::Bytes(dest_raw, 16));
        char pong[64];
        snprintf(pong, sizeof(pong), "%s%s!%s", TDECK_FRAME_PONG, seq, sent);
        RNS::Packet(out, RNS::Bytes(pong)).send();
        return true;
    }

    if (s.rfind(TDECK_FRAME_PONG, 0) == 0) {
        if (!tdeck_ping_inflight) return true;
        char seq[16], sent[24];
        const char* p = s.c_str() + strlen(TDECK_FRAME_PONG);
        p = tdeck_rns_field(p, 0, seq, sizeof(seq));
        if (!p) return true;
        tdeck_rns_field(p, 0, sent, sizeof(sent));
        if ((uint32_t)strtoul(seq, NULL, 16) != tdeck_ping_seq) return true;

        double sent_at = strtod(sent, NULL);
        double rtt = (RNS::Utilities::OS::time() - sent_at) * 1000.0;
        if (rtt < 0) rtt = 0;
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "PONG in %.0f ms", rtt);
        tdeck_ping_inflight = false;
        tdeck_dirty = true;
        return true;
    }
    return true;
}

#endif // HAS_RNS

#ifdef HAS_RNS
void AppReticulum::onRnsSetup() {
    tdeck_rns_register_aspect(TDECK_UI_RNS_ASPECT_PING, on_ping);
}
#endif

// ============================================================================
// Extern ping/trace helpers (implemented here; declared in TDeckApp.h)
// ============================================================================

void tdeck_rns_ping_contact(uint8_t idx) {
#ifndef HAS_RNS
    (void)idx;
    snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "RNS unavailable");
#else
    if (!tdeck_rns_ready) return;
    if (idx >= tdeck_contact_count) return;
    tdeck_contact_t* c = &tdeck_contacts[idx];
    if (!c->has_key) {
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "No key for contact");
        return;
    }
    strncpy(tdeck_ping_peer, c->hash, sizeof(tdeck_ping_peer)-1);
    tdeck_ping_seq++;

    const RNS::Identity& self = RNS::Transport::identity();
    const RNS::Destination& local_dest = tdeck_rns_ping_dest;
    if (!self || !local_dest) {
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "No identity");
        return;
    }
    char frame[256 + 40];
    snprintf(frame, sizeof(frame), "%s%08x!%.3f!%s!%s", TDECK_FRAME_PING, tdeck_ping_seq,
             RNS::Utilities::OS::time(), self.get_public_key().toHex().c_str(),
             local_dest.hash().toHex().c_str());
    if (!tdeck_rns_send_payload(c, TDECK_UI_RNS_ASPECT_PING, RNS::Bytes(frame))) {
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "Send failed");
        return;
    }
    tdeck_ping_inflight  = true;
    tdeck_ping_sent_at   = RNS::Utilities::OS::time();
    tdeck_ping_deadline  = millis() + TDECK_PING_TIMEOUT_MS;
    snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "Pinging...");
    tdeck_dirty = true;
#endif
}

void tdeck_rns_ping_poll() {
    if (!tdeck_ping_inflight) return;
    if (millis() >= tdeck_ping_deadline) {
        tdeck_ping_inflight = false;
        snprintf(tdeck_ping_status, sizeof(tdeck_ping_status), "Timeout");
        tdeck_dirty = true;
    }
}

void tdeck_rns_trace_contact(uint8_t idx) {
#ifndef HAS_RNS
    (void)idx;
    snprintf(tdeck_trace_out, sizeof(tdeck_trace_out), "RNS unavailable");
#else
    if (!tdeck_rns_ready) return;
    if (idx >= tdeck_contact_count) return;
    tdeck_contact_t* c = &tdeck_contacts[idx];
    strncpy(tdeck_trace_peer, c->hash, sizeof(tdeck_trace_peer)-1);

    char label[48];
    tdeck_contact_label(c, label, sizeof(label));

    RNS::Bytes probe = {RNS::Type::NONE};
    uint8_t raw[16];
    if (c->dest_hash[0] && tdeck_hex_decode(c->dest_hash, raw, 16)) {
        probe = RNS::Bytes(raw, 16);
    } else if (tdeck_hex_decode(c->hash, raw, 16)) {
        probe = RNS::Bytes(raw, 16);
    }

    char out[400];
    int off = snprintf(out, sizeof(out), "Trace to %s\n", label);
    if (probe && RNS::Transport::has_path(probe)) {
        off += snprintf(out + off, sizeof(out) - off, "Hops: %u\n", RNS::Transport::hops_to(probe));
        RNS::Bytes nh = RNS::Transport::next_hop(probe);
        if (nh) off += snprintf(out + off, sizeof(out) - off, "Next hop: %s\n", nh.toHex().c_str());
        const auto& states = RNS::Transport::path_states();
        auto sit = states.find(probe);
        if (sit != states.end()) {
            const char* st = (sit->second == RNS::Type::Transport::STATE_RESPONSIVE)   ? "responsive" :
                             (sit->second == RNS::Type::Transport::STATE_UNRESPONSIVE) ? "unresponsive" :
                                                                                         "unknown";
            off += snprintf(out + off, sizeof(out) - off, "Path state: %s\n", st);
        }
        RNS::Interface inf = RNS::Transport::next_hop_interface(probe);
        if (inf) off += snprintf(out + off, sizeof(out) - off, "Interface: %s\n", inf.name().c_str());
        off += snprintf(out + off, sizeof(out) - off, "Route known; pinging...");
    } else {
        off += snprintf(out + off, sizeof(out) - off, "No route yet; pinging to establish one...");
    }
    strncpy(tdeck_trace_out, out, sizeof(tdeck_trace_out)-1);
    tdeck_trace_out[sizeof(tdeck_trace_out)-1] = '\0';
    tdeck_trace_inflight = true;
    tdeck_rns_ping_contact(idx);
    tdeck_dirty = true;
#endif
}

// ============================================================================
// Drawing
// ============================================================================

void AppReticulum::draw() {
    TDeckCanvas& c = tdeck_canvas;
    if (tdeck_sub == RET_MENU) {
        static const char* items[] = {
            "Ping contact", "Traceroute contact", "Direct contacts",
            "My identity", "Announce now"
        };
        uint8_t n = 5;
        uint8_t rows = tdeck_ui_list_frame(c, "Reticulum", n, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < n; r++) {
            uint8_t idx = tdeck_scroll + r;
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, items[idx], idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: run  L: back");
    } else if (tdeck_sub == RET_PING_PICK || tdeck_sub == RET_TRACE_PICK) {
        uint8_t total = tdeck_contact_count;
        uint8_t rows = tdeck_ui_list_frame(c, "Pick contact", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char label[48];
            tdeck_contact_label(&tdeck_contacts[idx], label, sizeof(label));
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, label, idx == tdeck_cursor);
        }
        if (total == 0) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts yet");
        }
        tdeck_ui_draw_footer(c, "select contact  L: back");
    } else if (tdeck_sub == RET_PING_RUN || tdeck_sub == RET_TRACE_RUN) {
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 6);
        c.print(tdeck_sub == RET_PING_RUN ? "Ping" : "Traceroute");
        c.drawFastHLine(0, TDECK_SBAR_H + 26, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(1);
        int y = TDECK_SBAR_H + 36;
        c.setTextColor(TDECK_COL_FG);
        c.setCursor(TDECK_LIST_X, y);
        c.print("Peer: ");
        c.print(tdeck_sub == RET_PING_RUN ? tdeck_ping_peer : tdeck_trace_peer);
        y += 12;
        c.setTextColor(TDECK_COL_WARN);
        c.setCursor(TDECK_LIST_X, y);
        c.print(tdeck_ping_status);
        y += 14;

        if (tdeck_sub == RET_TRACE_RUN && tdeck_trace_out[0]) {
            c.drawFastHLine(0, y, TDECK_SCREEN_W, TDECK_COL_DIM);
            y += 6;
            const char* p = tdeck_trace_out;
            while (*p && y < TDECK_SCREEN_H - TDECK_FOOT_H - 6) {
                char line[60];
                uint8_t li = 0;
                while (*p && *p != '\n' && li < sizeof(line)-1) line[li++] = *p++;
                line[li] = '\0';
                c.setTextColor(TDECK_COL_FG);
                c.setCursor(TDECK_LIST_X, y);
                c.print(line);
                y += 12;
                if (*p == '\n') p++;
            }
        }
        tdeck_ui_draw_footer(c, "press: retry  L: back");
    } else if (tdeck_sub == RET_DIRECT) {
        uint8_t total = tdeck_direct_count();
        uint8_t rows = tdeck_ui_list_frame(c, "Direct contacts", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[80];
            uint8_t hops = 0;
            char h[40];
            if (tdeck_direct_at(idx, h, sizeof(h), &hops)) {
                snprintf(line, sizeof(line), "%s  (%u hop)", h, hops);
            } else {
                snprintf(line, sizeof(line), "?");
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "L: back");
    } else if (tdeck_sub == RET_IDENTITY) {
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
        c.print("My identity");
        c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

        c.setTextSize(1);
        int y = TDECK_SBAR_H + 34;
      #ifdef HAS_RNS
        if (tdeck_rns_ready) {
            const RNS::Identity& id = RNS::Transport::identity();
            c.setTextColor(TDECK_COL_FG);
            c.setCursor(TDECK_LIST_X, y); c.print("Hash: "); c.print(id.hash().toHex().c_str()); y += 12;
            c.setTextColor(TDECK_COL_ACCENT);
            c.setCursor(TDECK_LIST_X, y); c.print("LXMF: "); c.print(RNS::Destination::hash(id, "lxmf", "delivery").toHex().c_str()); y += 12;
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, y); c.print("Announced:"); y += 12;
            c.setTextColor(TDECK_COL_DIM);
            c.setCursor(TDECK_LIST_X, y);
            if (tdeck_rns_msg_dest) c.print("msgs "); else c.print("msgs (pending)");
            if (tdeck_rns_msg_dest) c.print(tdeck_rns_msg_dest.hash().toHex().c_str());
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            if (tdeck_rns_ping_dest) c.print("ping "); else c.print("ping (pending)");
            if (tdeck_rns_ping_dest) c.print(tdeck_rns_ping_dest.hash().toHex().c_str());
            y += 12;
            c.setTextColor(TDECK_COL_FG);
            c.setCursor(TDECK_LIST_X, y); c.print("Name: "); c.print(tdeck_set_device_name); y += 12;
            c.setTextColor(TDECK_COL_DIM);
            c.setCursor(TDECK_LIST_X, y);
            c.print("Transport: ");
            c.print(RNS::Reticulum::transport_enabled() ? "enabled" : "disabled");
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            c.printf("Paths: %u  Ann: %u", (unsigned)RNS::Transport::path_table().size(),
                     (unsigned)RNS::Transport::announces_received());
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            c.printf("TX: %u  RX: %u", (unsigned)RNS::Transport::packets_sent(),
                     (unsigned)RNS::Transport::packets_received());
            y += 12;
            c.setCursor(TDECK_LIST_X, y);
            c.printf("Interfaces: %u", (unsigned)RNS::Transport::get_interfaces().size());
            y += 12;
        } else {
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, y); c.print("RNS not ready yet"); y += 12;
        }
      #else
        c.setTextColor(TDECK_COL_WARN);
        c.setCursor(TDECK_LIST_X, y); c.print("RNS not compiled in"); y += 12;
      #endif
        c.drawFastHLine(0, y + 4, TDECK_SCREEN_W, TDECK_COL_DIM);

        uint8_t ay = y + 12;
        if (ay < TDECK_SBAR_H + 30) ay = TDECK_SBAR_H + 30;
        static const char* acts[] = { "Edit name", "Back" };
        for (uint8_t r = 0; r < 2; r++) {
            tdeck_ui_draw_row(c, ay + r * 16, acts[r], r == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: run  L: back");
    } else if (tdeck_sub == RET_NAME_EDIT) {
        tdeck_ui_draw_text_input("Node name", tdeck_input_buf);
    }
}

// ============================================================================
// Navigation
// ============================================================================

void AppReticulum::onSelect() {
    if (tdeck_sub == RET_MENU) {
        switch (tdeck_cursor) {
            case 0: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_PING_PICK); break;
            case 1: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_TRACE_PICK); break;
            case 2: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_DIRECT); break;
            case 3: tdeck_ui_nav_push(SCREEN_RETICULUM, RET_IDENTITY); break;
            case 4:
              #ifdef HAS_RNS
                if (tdeck_rns_ready) {
                    tdeck_rns_announce_ui();
                    tdeck_toast_set("Announced");
                } else {
                    tdeck_toast_set("RNS not ready");
                }
              #else
                tdeck_toast_set("RNS unavailable");
              #endif
                break;
            default: break;
        }
    } else if (tdeck_sub == RET_PING_PICK) {
        if (tdeck_cursor < tdeck_contact_count) {
            tdeck_rns_ping_contact(tdeck_cursor);
            tdeck_ui_nav_push(SCREEN_RETICULUM, RET_PING_RUN);
        }
    } else if (tdeck_sub == RET_TRACE_PICK) {
        if (tdeck_cursor < tdeck_contact_count) {
            tdeck_rns_trace_contact(tdeck_cursor);
            tdeck_ui_nav_push(SCREEN_RETICULUM, RET_TRACE_RUN);
        }
    } else if (tdeck_sub == RET_PING_RUN) {
        // press = retry the ping against the stored peer
      #ifdef HAS_RNS
        tdeck_contact_t* c = tdeck_contact_find(tdeck_ping_peer);
        if (c) {
            for (uint8_t i = 0; i < tdeck_contact_count; i++) {
                if (&tdeck_contacts[i] == c) { tdeck_rns_ping_contact(i); break; }
            }
        }
      #endif
    } else if (tdeck_sub == RET_TRACE_RUN) {
      #ifdef HAS_RNS
        tdeck_contact_t* c = tdeck_contact_find(tdeck_trace_peer);
        if (c) {
            for (uint8_t i = 0; i < tdeck_contact_count; i++) {
                if (&tdeck_contacts[i] == c) { tdeck_rns_trace_contact(i); break; }
            }
        }
      #endif
    } else if (tdeck_sub == RET_IDENTITY) {
        if (tdeck_cursor == 0) {
            tdeck_input_start(2, tdeck_set_device_name);
            tdeck_ui_nav_push(SCREEN_RETICULUM, RET_NAME_EDIT);
        } else {
            tdeck_ui_nav_back();
        }
    } else if (tdeck_sub == RET_NAME_EDIT) {
        tdeck_input_apply();
    }
    tdeck_dirty = true;
}

void AppReticulum::onUp() {
    if (tdeck_sub == RET_MENU || tdeck_sub == RET_PING_PICK ||
        tdeck_sub == RET_TRACE_PICK || tdeck_sub == RET_DIRECT ||
        tdeck_sub == RET_IDENTITY) {
        if (tdeck_cursor > 0) tdeck_cursor--;
    }
    tdeck_dirty = true;
}

void AppReticulum::onDown() {
    if (tdeck_sub == RET_MENU) {
        if (tdeck_cursor + 1 < 5) tdeck_cursor++;
    } else if (tdeck_sub == RET_PING_PICK || tdeck_sub == RET_TRACE_PICK) {
        if (tdeck_cursor + 1 < tdeck_contact_count) tdeck_cursor++;
    } else if (tdeck_sub == RET_DIRECT) {
        uint8_t total = tdeck_direct_count();
        if (tdeck_cursor + 1 < total) tdeck_cursor++;
    } else if (tdeck_sub == RET_IDENTITY) {
        if (tdeck_cursor + 1 < 2) tdeck_cursor++;
    }
    tdeck_dirty = true;
}

void AppReticulum::onBack() {
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}

void AppReticulum::onKey(char key, uint8_t state) {
    // Text field keys are handled by the framework (inTextInput()).
    (void)key;
    (void)state;
}

void AppReticulum::onTrackball(tb_event_t event) {
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
// Loop / background
// ============================================================================

bool AppReticulum::onLoop() {
    tdeck_rns_ping_poll();
    return false;   // repaint is requested via tdeck_dirty by the poll helper
}

#ifdef HAS_RNS
bool AppReticulum::onRnsLoop() {
    return false;
}
#endif

// ============================================================================
// Text input commit: node name (ctx 2)
// ============================================================================

void AppReticulum::onTextApply(uint8_t ctx) {
    if (ctx != 2) return;   // ctx 0 (compose) and ctx 1 (alias) elsewhere

    if (tdeck_input_len > 0) {
        strncpy(tdeck_set_device_name, tdeck_input_buf, sizeof(tdeck_set_device_name)-1);
        tdeck_settings_dirty = true;
      #ifdef HAS_RNS
        if (tdeck_rns_ready) tdeck_rns_announce_ui();
      #endif
        tdeck_toast_set("Name updated");
    }
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}