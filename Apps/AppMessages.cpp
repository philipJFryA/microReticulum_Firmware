// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Messages application implementation.
//
// Self-contained module: depends only on the framework contract in
// TDeckApp.h plus the LXMF protocol helpers in LXMF.h. Registers its two
// RNS aspect packet handlers ("lxmf.delivery" and "broadcast") with the
// framework's dispatch table during onRnsSetup(), and implements the
// inbox / thread / pick / compose sub-screens.

#include "AppMessages.h"

#include <Arduino.h>
#include <string.h>

#include "../LXMF.h"

#ifdef HAS_RNS
#include <microReticulum.h>
#endif

// ---- Self-registration -------------------------------------------------------
// Static init happens before setup(), so the launcher always sees this app.
namespace {
AppMessages s_app;
TDeckAppRegistrar s_reg(&s_app);
} // namespace

// ============================================================================
// RNS packet callbacks (registered with the framework dispatch table)
// ============================================================================

#ifdef HAS_RNS

// LXMF delivery: reconstruct the full wire format and surface the message.
static bool on_lxmf(const RNS::Bytes& data, const RNS::Packet& packet) {
    // Acknowledge the packet to the sender (mirrors LXMRouter.delivery_packet).
    const RNS::Identity& local_id = RNS::Transport::identity();
    if (local_id) local_id.prove(packet);

    RNS::Bytes lxmf_data;
    const size_t dest_len = RNS::Type::Reticulum::DESTINATION_LENGTH;
    if (data.size() >= 2*dest_len + LXMF::SIGNATURE_LENGTH + 2 &&
        data.left(dest_len) == tdeck_rns_msg_dest.hash()) {
        lxmf_data.append(data);   // link-delivered: already has destination
    } else {
        lxmf_data.append(tdeck_rns_msg_dest.hash());   // opportunistic
        lxmf_data.append(data);
    }

    LXMF::LXMessage msg;
    if (!LXMF::LXMessage::unpack(msg, lxmf_data)) return true;

    std::string source_hex = msg.source_hash.toHex();
    char sender[40] = {0};
    strncpy(sender, source_hex.c_str(), sizeof(sender)-1);

    RNS::Identity source_identity = {RNS::Type::NONE};
    bool signature_valid = false;
    if (msg.source_hash) {
        source_identity = RNS::Identity::recall(msg.source_hash);
        if (source_identity) {
            signature_valid = LXMF::LXMessage::verify(msg, source_identity);
        }
    }

    char contact_hash[40] = {0};
    if (signature_valid && source_identity) {
        std::string ih = source_identity.hash().toHex();
        strncpy(contact_hash, ih.c_str(), sizeof(contact_hash)-1);
    } else {
        strncpy(contact_hash, sender, sizeof(contact_hash)-1);
    }

    tdeck_contact_upsert(contact_hash, sender, NULL, NULL, 0);
    if (source_identity) {
        std::string pk = source_identity.get_public_key().toHex();
        tdeck_contact_t* cnt = tdeck_contact_find(contact_hash);
        if (cnt) {
            strncpy(cnt->pubkey, pk.c_str(), sizeof(cnt->pubkey)-1);
            cnt->has_key = true;
        }
    }

    char text[TDECK_MSG_MAX_LEN+1];
    size_t content_len = msg.content.size();
    if (content_len > TDECK_MSG_MAX_LEN) content_len = TDECK_MSG_MAX_LEN;
    if (content_len > 0) {
        memcpy(text, msg.content.data(), content_len);
    }
    text[content_len] = '\0';
    tdeck_msg_add(contact_hash, text, true);

    tdeck_contact_t* cnt = tdeck_contact_find(contact_hash);
    if (cnt) {
        char label[48];
        tdeck_contact_label(cnt, label, sizeof(label));
        tdeck_toast_set("Msg from %s", label);
    } else {
        tdeck_toast_set("Message received");
    }
    tdeck_dirty = true;
    return true;
}

// Broadcast: unencrypted line frames "!mrbcast!<hash>!<text>". The sender's
// echoed own-frame is dropped (matching the original monolithic behaviour).
static bool on_broadcast(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();
    if (s.rfind(TDECK_FRAME_BCAST, 0) != 0) return true;
    const char* p = s.c_str() + strlen(TDECK_FRAME_BCAST);

    char sender[40];
    p = tdeck_rns_field(p, 0, sender, sizeof(sender));
    if (!p || !sender[0]) return true;

    const RNS::Identity& id = RNS::Transport::identity();
    if (id && strcmp(sender, id.hash().toHex().c_str()) == 0) return true;

    char text[TDECK_MSG_MAX_LEN+1];
    strncpy(text, p, sizeof(text)-1);
    text[sizeof(text)-1] = '\0';

    tdeck_contact_upsert(sender, NULL, NULL, NULL, 0);
    tdeck_msg_add(TDECK_BCAST_PEER, text, true);
    tdeck_dirty = true;
    return true;
}

// The "ping" aspect is owned by the Reticulum app (which answers !mrping!
// frames and collects !mrpong! replies for its ping UI).

#endif // HAS_RNS

#ifdef HAS_RNS
void AppMessages::onRnsSetup() {
    tdeck_rns_register_aspect(TDECK_UI_RNS_ASPECT_MSGS, on_lxmf);
    tdeck_rns_register_aspect(TDECK_UI_RNS_ASPECT_BCAST, on_broadcast);
}
#endif

// ============================================================================
// Drawing
// ============================================================================

void AppMessages::draw() {
    TDeckCanvas& c = tdeck_canvas;
    if (tdeck_sub == MSG_INBOX) {
        uint8_t nconv = tdeck_inbox_peer_count();
        uint8_t total = nconv + 1;                       // "+ New message"
        uint8_t rows = tdeck_ui_list_frame(c, "Messages", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[80];
            if (idx == 0) {
                snprintf(line, sizeof(line), "+ New message");
            } else {
                char peer[40];
                if (tdeck_inbox_peer_at(idx - 1, peer, sizeof(peer))) {
                    if (strcmp(peer, TDECK_BCAST_PEER) == 0) {
                        snprintf(line, sizeof(line), "* Broadcast");
                    } else {
                        tdeck_contact_t* cnt = tdeck_contact_find(peer);
                        char label[40];
                        tdeck_contact_label(cnt, label, sizeof(label));
                        snprintf(line, sizeof(line), "%s%s", (cnt && cnt->favorite) ? "* " : "", label);
                    }
                } else {
                    snprintf(line, sizeof(line), "?");
                }
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "TB: scroll/select  L: back");
    } else if (tdeck_sub == MSG_THREAD) {
        c.setTextSize(2);
        c.setTextColor(TDECK_COL_ACCENT);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
        const char* tlabel = (strcmp(tdeck_thread_peer, TDECK_BCAST_PEER) == 0)
                             ? "Broadcast"
                             : [&]() {
                                 tdeck_contact_t* cnt = tdeck_contact_find(tdeck_thread_peer);
                                 static char lbuf[48];
                                 tdeck_contact_label(cnt, lbuf, sizeof(lbuf));
                                 return lbuf;
                             }();
        c.print(tlabel);
        c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

        uint8_t idxs[TDECK_MAX_MESSAGES];
        uint8_t n = 0;
        for (uint8_t i = 0; i < tdeck_msg_count; i++) {
            if (strcmp(tdeck_messages[i].peer, tdeck_thread_peer) == 0) {
                idxs[n++] = i;
            }
        }
        uint8_t rows = (TDECK_SCREEN_H - TDECK_SBAR_H - 24 - TDECK_FOOT_H) / 12;
        if (rows < 1) rows = 1;
        if (tdeck_scroll > n) tdeck_scroll = n;
        uint8_t end = n - tdeck_scroll;
        uint8_t start = end > rows ? end - rows : 0;
        for (uint8_t r = 0; r < rows && start + r < n; r++) {
            uint8_t mi = idxs[start + r];
            bool inbound = tdeck_messages[mi].inbound;
            uint8_t y = TDECK_SBAR_H + 28 + r * 12;
            if (inbound) {
                c.fillRect(0, y, TDECK_SCREEN_W - 80, 11, TDECK_COL_SEL);
                c.setTextColor(ST77XX_WHITE);
            } else {
                c.fillRect(TDECK_SCREEN_W - 80, y, 80, 11, TDECK_COL_SBAR);
                c.setTextColor(TDECK_COL_DIM);
            }
            c.setTextSize(1);
            c.setCursor(inbound ? TDECK_LIST_X : TDECK_SCREEN_W - 78, y + 2);
            c.print(tdeck_messages[mi].text);
        }
        tdeck_ui_draw_footer(c, "press: reply  L: back");
    } else if (tdeck_sub == MSG_PICK) {
        uint8_t total = tdeck_contact_count + 1;       // "+ Broadcast" first
        uint8_t rows = tdeck_ui_list_frame(c, "To", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[48];
            if (idx == 0) {
                snprintf(line, sizeof(line), "+ Broadcast");
            } else {
                tdeck_contact_label(&tdeck_contacts[idx - 1], line, sizeof(line));
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        if (total == 1) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts yet - broadcast to all UI nodes");
        }
        tdeck_ui_draw_footer(c, "select contact  L: back");
    } else if (tdeck_sub == MSG_COMPOSE) {
        tdeck_ui_draw_text_input("Message", tdeck_input_buf);
    }
}

// ============================================================================
// Navigation
// ============================================================================

void AppMessages::onSelect() {
    if (tdeck_sub == MSG_INBOX) {
        uint8_t nconv = tdeck_inbox_peer_count();
        if (tdeck_cursor == 0) {
            tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_PICK);
        } else if (tdeck_cursor <= nconv) {
            char peer[40];
            if (tdeck_inbox_peer_at(tdeck_cursor - 1, peer, sizeof(peer))) {
                strncpy(tdeck_thread_peer, peer, sizeof(tdeck_thread_peer)-1);
                tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_THREAD);
            }
        }
    } else if (tdeck_sub == MSG_THREAD) {
        // press = quick reply to this contact
        strncpy(tdeck_input_peer, tdeck_thread_peer, sizeof(tdeck_input_peer)-1);
        tdeck_input_start(0, "");
        tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
    } else if (tdeck_sub == MSG_PICK) {
        if (tdeck_cursor == 0) {
            strncpy(tdeck_input_peer, TDECK_BCAST_PEER, sizeof(tdeck_input_peer)-1);
            tdeck_input_start(0, "");
            tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
        } else if (tdeck_cursor - 1 < tdeck_contact_count) {
            strncpy(tdeck_input_peer, tdeck_contacts[tdeck_cursor - 1].hash, sizeof(tdeck_input_peer)-1);
            tdeck_input_start(0, "");
            tdeck_ui_nav_push(SCREEN_MESSAGES, MSG_COMPOSE);
        }
    } else if (tdeck_sub == MSG_COMPOSE) {
        tdeck_input_apply();
    }
}

void AppMessages::onUp() {
    if (tdeck_sub == MSG_INBOX || tdeck_sub == MSG_PICK) {
        if (tdeck_cursor > 0) tdeck_cursor--;
    } else if (tdeck_sub == MSG_THREAD) {
        tdeck_scroll++;                          // view older messages
    }
    tdeck_dirty = true;
}

void AppMessages::onDown() {
    if (tdeck_sub == MSG_INBOX) {
        uint8_t total = tdeck_inbox_peer_count() + 1;
        if (tdeck_cursor + 1 < total) tdeck_cursor++;
    } else if (tdeck_sub == MSG_PICK) {
        if (tdeck_cursor + 1 < tdeck_contact_count + 1) tdeck_cursor++;
    } else if (tdeck_sub == MSG_THREAD) {
        if (tdeck_scroll > 0) tdeck_scroll--;   // view newer messages
    }
    tdeck_dirty = true;
}

void AppMessages::onBack() {
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}

void AppMessages::onKey(char key, uint8_t state) {
    // Text field keys are handled by the framework (inTextInput()).
    (void)key;
    (void)state;
}

void AppMessages::onTrackball(tb_event_t event) {
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
// Text input commit: compose message (ctx 0)
// ============================================================================

void AppMessages::onTextApply(uint8_t ctx) {
    if (ctx != 0) return;   // ctx 1 (alias) and ctx 2 (node name) belong elsewhere

    if (tdeck_input_len == 0) {
        tdeck_ui_nav_back();
        return;
    }
    bool is_bcast = (strcmp(tdeck_input_peer, TDECK_BCAST_PEER) == 0);
    tdeck_contact_t* c = is_bcast ? NULL : tdeck_contact_find(tdeck_input_peer);

#ifdef HAS_RNS
    if (is_bcast) {
        if (tdeck_rns_send_broadcast(tdeck_input_buf)) {
            tdeck_msg_add(TDECK_BCAST_PEER, tdeck_input_buf, false);
            tdeck_toast_set("Broadcast sent");
        } else {
            tdeck_toast_set("Send failed");
            return;
        }
    } else if (!c || !c->has_key) {
        tdeck_toast_set("No key for contact");
        return;
    } else {
        const RNS::Identity& sender = RNS::Transport::identity();
        const RNS::Destination& msg_local_dest = tdeck_rns_msg_dest;
        if (!sender || !msg_local_dest) {
            tdeck_toast_set("No identity");
            return;
        }
        RNS::Bytes dest_raw;
        uint8_t raw_dest[16];
        if (c->dest_hash[0] && tdeck_hex_decode(c->dest_hash, raw_dest, 16)) {
            dest_raw = RNS::Bytes(raw_dest, 16);
        } else {
            uint8_t key[64];
            if (!tdeck_hex_decode(c->pubkey, key, 64)) {
                tdeck_toast_set("No key for contact");
                return;
            }
            RNS::Identity rid(false);
            rid.load_public_key(RNS::Bytes(key, 64));
            dest_raw = RNS::Destination::hash(rid, LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);
        }

        RNS::Bytes source_raw = msg_local_dest.hash();

        RNS::Bytes lxmf_packed;
        if (!LXMF::LXMessage::pack(lxmf_packed, dest_raw, source_raw,
                                    sender, tdeck_input_buf)) {
            tdeck_toast_set("Pack failed");
            return;
        }
        const size_t lxmf_dest_len = RNS::Type::Reticulum::DESTINATION_LENGTH;
        RNS::Bytes lxmf_opportunistic;
        if (lxmf_packed.size() >= lxmf_dest_len) {
            lxmf_opportunistic = lxmf_packed.mid(lxmf_dest_len);
        } else {
            lxmf_opportunistic = lxmf_packed;
        }
        if (tdeck_rns_send_payload(c, TDECK_UI_RNS_ASPECT_MSGS,
                                   lxmf_opportunistic)) {
            tdeck_msg_add(c->hash, tdeck_input_buf, false);
            tdeck_toast_set("Message sent");
        } else {
            tdeck_toast_set("Send failed");
            return;
        }
    }
#else
    (void)c;
    (void)is_bcast;
    tdeck_toast_set("RNS unavailable");
    return;
#endif
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}