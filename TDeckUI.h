// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Meshtastic-inspired user interface for the LilyGO T-Deck.
//
// This file is the UI FRAMEWORK. It owns:
//
//   * the home screen (app launcher) + shared status bar / footer
//   * the shared contact + message stores, device settings and /settings.yaml
//     persistence, and the microSD/EEPROM/brightness plumbing
//   * the shared text-input widget, toast, list and cursor helpers
//   * the RNS destinations (LXMF delivery, ping, broadcast, SAR position),
//     announce handler, and a small aspect -> packet-handler dispatch table
//     that routes incoming payloads to the owning app module
//   * the input loop (keyboard + trackball) and the nav stack
//
// Each APPLICATION (Messages, Contacts, Reticulum, Settings, SAR) is a
// self-contained module in Apps/ — a TDeckApp subclass with its own
// header/cpp pair that self-registers with TDeckAppRegistry. The launcher
// simply iterates the registry; adding a new app never touches this file.
//
// Because Config.h / Utilities.h / Display.h define firmware globals at
// header scope, this framework header must be included from exactly ONE
// translation unit (RNode_Firmware.ino does this), like the original
// monolithic TDeckUI.h. App modules are separate TUs and reach shared state
// through the extern API in TDeckApp.h.

#pragma once

#include <Arduino.h>

#if BOARD_MODEL == BOARD_TDECK

#include <Adafruit_GFX.h>
#include <SD.h>
#ifdef ESP_PLATFORM
  #include <esp_heap_caps.h>
  // ROM miniz/tinfl raw-deflate inflater used by the SAR PNG tile decoder.
  // The ROM symbols are mapped at link time via esp32s3.rom.ld - no external
  // PNG/zlib library is needed. (The SAR app module includes rom/miniz.h
  // itself; this include is kept for historical parity.)
  #include "rom/miniz.h"
#endif

#include "TDeckTypes.h"
// The trackball implementation (global singleton + ISR pointer) and GPS
// driver implementation (global singleton) must be defined BEFORE TDeckApp.h
// is included: TDeckApp.h pulls in both driver headers (which guard their
// singleton definitions behind these macros), and the headers are #pragma-once
// protected, so a later re-include in this TU would silently skip the
// implementation blocks. TDeckUI.h is included from exactly one translation
// unit (RNode_Firmware.ino), so the symbols are defined exactly once.
#define TDECK_TRACKBALL_IMPLEMENTATION
#define TDECK_GPS_IMPLEMENTATION
#include "TDeckApp.h"
#include "TDeckFirmwareExterns.h"
#include "TDeckKeyboard.h"
#include "TDeckTrackball.h"
#include "TDeckGPS.h"
#include "LXMF.h"

// Defined in RNode_Firmware.ino after this header is included; forward
// declared here so tdeck_ui_debug() can consult it.
extern bool kiss_framed_logs;

#ifdef HAS_RNS
extern RNS::Reticulum reticulum;
// Forward declarations for the radio bootstrap in tdeck_ui_loop().
// startRadio(), update_radio_lock() and tx_queue_handler() are defined in
// RNode_Firmware.ino after this header is included, so they must be
// declared here.
bool startRadio();
void update_radio_lock();
void tx_queue_handler();
// packet queue depth (defined in RNode_Firmware.ino after this header).
extern volatile uint8_t queue_height;
#endif

// ============================================================================
// Shared state (definitions matching the externs in TDeckApp.h)
// ============================================================================

TDeckCanvas tdeck_canvas(TDECK_SCREEN_W, TDECK_SCREEN_H);
TDeckKeyboard tdeck_kb;

tdeck_contact_t tdeck_contacts[TDECK_MAX_CONTACTS];
uint8_t tdeck_contact_count = 0;

tdeck_message_t tdeck_messages[TDECK_MAX_MESSAGES];
uint8_t tdeck_msg_count = 0;

// ---- Device settings (persisted to /settings.yaml) ---------------------------
char     tdeck_set_device_name[64]    = "microReticulum Node";
uint8_t  tdeck_set_brightness         = 128;
uint16_t tdeck_set_blank_timeout      = 30;   // seconds; 0 = never
bool     tdeck_set_gps_enabled        = true;
int16_t  tdeck_set_tz_offset_min      = 0;    // minutes from UTC (e.g. -480 EST)
uint16_t tdeck_set_announce_interval  = 0;    // minutes; 0 = off
bool     tdeck_set_confirm_send       = false;
bool     tdeck_settings_dirty         = false;

// Valid SX126x signal bandwidth options (Hz), used by the radio BW editor.
const uint32_t tdeck_bw_options[] = {
    7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000
};

// ---- Runtime state -----------------------------------------------------------
bool     tdeck_ui_ready       = false;
bool     tdeck_dirty          = true;
bool     tdeck_sd_ready       = false;
bool     tdeck_gps_fix        = false;  // updated from tdeck_gps in the loop
bool     tdeck_gps_present    = false;  // set when a GNSS is detected on UART1
bool     tdeck_gps_time_set   = false;  // GNSS UTC epoch applied to RNS clock
uint32_t tdeck_ui_start_ms    = 0;
uint32_t tdeck_last_rx_ms     = 0;

uint8_t tdeck_screen = SCREEN_HOME;
uint8_t tdeck_sub    = 0;
uint8_t tdeck_cursor = 0;
uint8_t tdeck_scroll = 0;

// Loop-periodic bookkeeping (must appear before tdeck_ui_init/loop).
static uint32_t tdeck_last_tick     = 0;
static uint32_t tdeck_tick_ms       = 1000;
static bool     tdeck_gps_announced = false;  // first-fix announce already sent

// Back-stack (small fixed depth; long-press also returns to home)
#define TDECK_NAV_DEPTH 8
static uint8_t tdeck_nav_screen[TDECK_NAV_DEPTH];
static uint8_t tdeck_nav_sub[TDECK_NAV_DEPTH];
static uint8_t tdeck_nav_depth = 0;

// Text input state
char    tdeck_input_buf[TDECK_MSG_MAX_LEN+1];
uint8_t tdeck_input_len  = 0;
uint8_t tdeck_input_ctx  = 0;   // 0 = compose, 1 = alias, 2 = node name
char    tdeck_input_peer[40];   // peer for compose context

// Contact detail / thread / toast scratch
uint8_t tdeck_contact_detail_idx = 0;
char    tdeck_thread_peer[40];
char    tdeck_toast[40];
uint32_t tdeck_toast_until = 0;

// Settings app scratch (SAR config editor reuses rows 20/21/22)
uint8_t tdeck_set_edit_idx = 0;

// ping/trace UI state (declared unconditionally so the draw code still
// renders "RNS unavailable" screens when RNS is compiled out).
bool     tdeck_ping_inflight = false;
uint32_t tdeck_ping_seq      = 0;
double   tdeck_ping_sent_at  = 0.0;
char     tdeck_ping_peer[40];
char     tdeck_ping_status[48];
uint32_t tdeck_ping_deadline = 0;

bool  tdeck_trace_inflight = false;
char  tdeck_trace_peer[40];
char  tdeck_trace_out[400];

// ---- RNS integration state ---------------------------------------------------
#ifdef HAS_RNS
RNS::Destination tdeck_rns_msg_dest({RNS::Type::NONE});
RNS::Destination tdeck_rns_ping_dest({RNS::Type::NONE});
RNS::Destination tdeck_rns_bcast_dest({RNS::Type::NONE});
RNS::Destination tdeck_rns_sar_dest({RNS::Type::NONE});
static RNS::HAnnounceHandler tdeck_rns_ann_handler;
static bool tdeck_rns_announce_registered = false;
bool tdeck_rns_ready = false;
double tdeck_last_announce_at = 0.0;
uint32_t tdeck_last_rx_count = 0;
#endif

// ============================================================================
// Small shared helpers
// ============================================================================

// Route diagnostic text through the same output path as firmware logs so it
// never interleaves as unframed bytes into a KISS-aware host stream when
// kiss_framed_logs is enabled (it defaults to false on embedded builds).
void tdeck_ui_debug(const char* msg) {
  if (kiss_framed_logs) {
    kiss_indicate_log(msg, strlen(msg));
  } else {
    Serial.println(msg);
  }
  Serial.flush();
}

bool tdeck_hex_decode(const char* hex, uint8_t* out, size_t n) {
    if (!hex) return false;
    size_t hlen = strlen(hex);
    if (hlen != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        char hi = hex[i*2], lo = hex[i*2+1];
        uint8_t hv, lv;
        if (hi >= '0' && hi <= '9') hv = hi - '0';
        else if (hi >= 'a' && hi <= 'f') hv = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') hv = hi - 'A' + 10;
        else return false;
        if (lo >= '0' && lo <= '9') lv = lo - '0';
        else if (lo >= 'a' && lo <= 'f') lv = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') lv = lo - 'A' + 10;
        else return false;
        out[i] = (uint8_t)((hv << 4) | lv);
    }
    return true;
}

// Clock for the status bar: real unix time from RNS when available, otherwise
// uptime in the T+hh:mm format.
void tdeck_ui_time_str(char* out, size_t n) {
  #ifdef HAS_RNS
    double now = RNS::Utilities::OS::time();
    if (now > 1600000000.0) {
        time_t t = (time_t)now;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        snprintf(out, n, "%02u:%02u", tmv.tm_hour, tmv.tm_min);
        return;
    }
  #endif
    uint32_t up = millis() / 1000;
    snprintf(out, n, "T+%02u:%02u", (up / 3600) % 100, (up / 60) % 60);
}

tdeck_contact_t* tdeck_contact_find(const char* hash_hex) {
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        if (strcmp(tdeck_contacts[i].hash, hash_hex) == 0) return &tdeck_contacts[i];
    }
    return NULL;
}

// Announce app_data from Reticulum peers is not always a plain display name:
// Sideband-style announces can carry binary metadata/control bytes, a newline,
// and then the actual human-readable name. Extract a clean printable name:
// the content of the last line (after any embedded newlines), with control
// characters stripped and surrounding whitespace trimmed.
void tdeck_name_clean(const char* raw, char* out, size_t n) {
    if (!raw) { if (n > 0) out[0] = '\0'; return; }
    // The human-readable name follows the final newline; everything before it
    // is metadata/garbage (matches "garbage on first line, name on second").
    const char* last = raw;
    for (const char* p = raw; *p; p++) {
        if (*p == '\n') last = p + 1;
    }
    size_t o = 0;
    for (const char* p = last; *p && o < n - 1; p++) {
        char ch = *p;
        if (ch == '\r' || ch == '\n') continue;
        if (ch >= 0x20 && ch <= 0x7E) out[o++] = ch;  // printable ASCII only
        // control / high bytes are dropped
    }
    while (o > 0 && out[o-1] == ' ') o--;
    out[o] = '\0';
}

// Insert or refresh a contact. Missing contacts are appended up to the store
// limit; existing entries keep their user alias/favourite state.
void tdeck_contact_upsert(const char* hash_hex, const char* dest_hex,
                          const char* pubkey_hex, const char* app_data,
                          double last_seen) {
    if (!hash_hex || !hash_hex[0]) return;
    tdeck_contact_t* c = tdeck_contact_find(hash_hex);
    if (!c) {
        if (tdeck_contact_count >= TDECK_MAX_CONTACTS) return;
        c = &tdeck_contacts[tdeck_contact_count++];
        memset(c, 0, sizeof(*c));
        strncpy(c->hash, hash_hex, sizeof(c->hash)-1);
    }
    if (dest_hex && dest_hex[0]) strncpy(c->dest_hash, dest_hex, sizeof(c->dest_hash)-1);
    if (pubkey_hex && pubkey_hex[0]) {
        strncpy(c->pubkey, pubkey_hex, sizeof(c->pubkey)-1);
        c->has_key = true;
    }
    if (app_data && app_data[0]) {
        char clean[TDECK_APP_DATA_MAX+1];
        tdeck_name_clean(app_data, clean, sizeof(clean));
        if (clean[0]) strncpy(c->app_data, clean, sizeof(c->app_data)-1);
    }
    if (last_seen > c->last_seen) c->last_seen = last_seen;
}

// Best display name for a contact: alias, else announced name, else hash.
// The announced name is cleaned at display time so pre-existing persisted
// names (saved before the ingest sanitizer existed) still render correctly.
const char* tdeck_contact_label(const tdeck_contact_t* c, char* buf, size_t n) {
    if (!c) { snprintf(buf, n, "(none)"); return buf; }
    if (c->alias[0]) { snprintf(buf, n, "%s", c->alias); return buf; }
    if (c->app_data[0]) {
        char clean[TDECK_APP_DATA_MAX+1];
        tdeck_name_clean(c->app_data, clean, sizeof(clean));
        if (clean[0]) { snprintf(buf, n, "%s", clean); return buf; }
    }
    snprintf(buf, n, "%s", c->hash);
    return buf;
}

// Number of distinct peers that have messages (conversations).
uint8_t tdeck_inbox_peer_count() {
    uint8_t count = 0;
    for (int i = tdeck_msg_count - 1; i >= 0; i--) {
        bool dup = false;
        for (int j = i + 1; j < (int)tdeck_msg_count; j++) {
            if (strcmp(tdeck_messages[i].peer, tdeck_messages[j].peer) == 0) { dup = true; break; }
        }
        if (!dup) count++;
    }
    return count;
}

// Fill `out` with the peer hash of the idx-th conversation (newest first).
bool tdeck_inbox_peer_at(uint8_t idx, char* out, size_t n) {
    char seen[TDECK_MAX_MESSAGES][40];
    uint8_t scount = 0;
    for (int i = tdeck_msg_count - 1; i >= 0; i--) {
        const char* p = tdeck_messages[i].peer;
        bool dup = false;
        for (uint8_t j = 0; j < scount; j++) {
            if (strcmp(seen[j], p) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strncpy(seen[scount], p, 40);
        seen[scount][39] = '\0';
        if (scount == idx) {
            strncpy(out, p, n);
            out[n-1] = '\0';
            return true;
        }
        scount++;
    }
    return false;
}

void tdeck_msg_add(const char* peer, const char* text, bool inbound) {
    if (!peer || !text) return;
    if (tdeck_msg_count >= TDECK_MAX_MESSAGES) {
        // Drop the oldest message to make room.
        memmove(&tdeck_messages[0], &tdeck_messages[1],
                sizeof(tdeck_message_t) * (TDECK_MAX_MESSAGES - 1));
        tdeck_msg_count--;
    }
    tdeck_message_t* m = &tdeck_messages[tdeck_msg_count++];
    memset(m, 0, sizeof(*m));
    strncpy(m->peer, peer, sizeof(m->peer)-1);
    strncpy(m->text, text, sizeof(m->text)-1);
    m->inbound = inbound;
  #ifdef HAS_RNS
    m->time = RNS::Utilities::OS::time();
  #else
    m->time = 0;
  #endif
}

void tdeck_toast_set(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tdeck_toast, sizeof(tdeck_toast), fmt, ap);
    va_end(ap);
    tdeck_toast_until = millis() + 2200;
}

// ============================================================================
// Text input
// ============================================================================

// True while the ACTIVE app is showing a full-screen text field. The
// framework routes printable keys to the shared buffer and Enter to
// tdeck_input_apply() while this is true.
bool tdeck_in_text_input() {
    TDeckApp* a = TDeckAppRegistry::active();
    return a && a->inTextInput();
}

void tdeck_input_start(uint8_t ctx, const char* initial) {
    tdeck_input_ctx = ctx;
    tdeck_input_len = 0;
    tdeck_input_buf[0] = '\0';
    if (initial) {
        strncpy(tdeck_input_buf, initial, sizeof(tdeck_input_buf)-1);
        tdeck_input_buf[sizeof(tdeck_input_buf)-1] = '\0';
        tdeck_input_len = strlen(tdeck_input_buf);
    }
}

void tdeck_input_add(char c) {
    if (c < 0x20 || c > 0x7E) return;
    if (tdeck_input_len >= TDECK_MSG_MAX_LEN) return;
    tdeck_input_buf[tdeck_input_len++] = c;
    tdeck_input_buf[tdeck_input_len] = '\0';
    tdeck_dirty = true;
}

void tdeck_input_del() {
    if (tdeck_input_len == 0) return;
    tdeck_input_buf[--tdeck_input_len] = '\0';
    tdeck_dirty = true;
}

// Commit the text entry buffer. The context (compose / alias / node name) is
// dispatched to the ACTIVE app's onTextApply(); the app performs the send /
// save and pops its own navigation.
void tdeck_input_apply() {
    TDeckApp* a = TDeckAppRegistry::active();
    if (a) a->onTextApply(tdeck_input_ctx);
}

// ============================================================================
// SD card + settings.yaml persistence
// ============================================================================

// Mount the T-Deck's microSD card. The SD slot shares the display/LoRa SPI bus
// (SCLK 40, MISO 38, MOSI 41) with its own chip-select on GPIO 39; the shared
// bus is already running at this point (radio + display init precede the UI).
static bool tdeck_sd_init() {
  #ifdef ESP_PLATFORM
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    if (SD.begin(SD_CS, SPI, 4000000)) {
        tdeck_sd_ready = true;
        tdeck_ui_debug("[TDeck] SD card ready");
        return true;
    }
    tdeck_ui_debug("[TDeck] SD card not found (settings will not persist)");
  #endif
    return false;
}

void tdeck_settings_apply_contact(const char* hash, const char* alias,
                                  bool fav, const char* pubkey,
                                  const char* app_data) {
    tdeck_contact_t* c = tdeck_contact_find(hash);
    if (!c) {
        tdeck_contact_upsert(hash, NULL, pubkey, app_data, 0);
        c = tdeck_contact_find(hash);
    }
    if (!c) return;
    if (alias && alias[0]) {
        strncpy(c->alias, alias, TDECK_ALIAS_MAX);
        c->alias[TDECK_ALIAS_MAX] = '\0';
    }
    c->favorite = fav;
    c->persisted = true;
    if (pubkey && pubkey[0]) {
        strncpy(c->pubkey, pubkey, sizeof(c->pubkey)-1);
        c->has_key = true;
    }
    if (app_data && app_data[0]) {
        char clean[TDECK_APP_DATA_MAX+1];
        tdeck_name_clean(app_data, clean, sizeof(clean));
        if (clean[0]) strncpy(c->app_data, clean, sizeof(c->app_data)-1);
    }
}

// Strip surrounding quotes from a YAML scalar value (in place at *start).
// Loops so already-corrupted values with multiple accumulated quote pairs
// (""""MyNode"""") are fully unwrapped rather than one pair per load.
static void tdeck_yaml_strip_quotes(const char** start) {
    const char* s = *start;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len >= 2 && ((s[0] == '"' && s[len-1] == '"') || (s[0] == '\'' && s[len-1] == '\''))) {
        char* tmp = (char*)s;
        tmp[len-1] = '\0';
        s++;
        len -= 2;
    }
    *start = s;
}

bool tdeck_settings_load() {
    if (!tdeck_sd_ready) return false;
    if (!SD.exists(TDECK_SETTINGS_PATH)) return false;
    File f = SD.open(TDECK_SETTINGS_PATH, FILE_READ);
    if (!f) return false;

    bool in_contacts = false;
    char hash[40] = {0}, alias[TDECK_ALIAS_MAX+1] = {0}, pub[140] = {0};
    char appd[TDECK_APP_DATA_MAX+1] = {0};
    bool fav = false;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();   // strips \r\n and surrounding whitespace
        if (line.length() == 0 || line.startsWith("#")) continue;

        if (line == "contacts:") { in_contacts = true; continue; }

        if (!in_contacts) {
            int ci = line.indexOf(':');
            if (ci <= 0) continue;
            String k = line.substring(0, ci);
            k.trim();
            String v = line.substring(ci + 1);
            v.trim();
            const char* vs = v.c_str();
            if (k == "device_name") {
                tdeck_yaml_strip_quotes(&vs);
                strncpy(tdeck_set_device_name, vs, sizeof(tdeck_set_device_name)-1);
                tdeck_set_device_name[sizeof(tdeck_set_device_name)-1] = '\0';
            } else if (k == "brightness") {
                tdeck_set_brightness = (uint8_t)constrain(atoi(vs), 0, 255);
            } else if (k == "blank_timeout") {
                tdeck_set_blank_timeout = (uint16_t)constrain(atoi(vs), 0, 3600);
            } else if (k == "gps_enabled") {
                tdeck_set_gps_enabled = (v == "true");
            } else if (k == "timezone_offset") {
                tdeck_set_tz_offset_min = (int16_t)constrain(atoi(vs), -1440, 1440);
            } else if (k == "announce_interval") {
                tdeck_set_announce_interval = (uint16_t)constrain(atoi(vs), 0, 1440);
            } else if (k == "confirm_send") {
                tdeck_set_confirm_send = (v == "true");
            } else if (k == "radio_freq") {
                lora_freq = (uint32_t)constrain(atol(vs), 100000000L, 1000000000L);
            } else if (k == "radio_bw") {
                lora_bw = (uint32_t)constrain(atol(vs), 7800L, 500000L);
            } else if (k == "radio_sf") {
                lora_sf = constrain(atoi(vs), 5, 12);
            } else if (k == "radio_cr") {
                lora_cr = constrain(atoi(vs), 5, 8);
            } else if (k == "radio_txp") {
                lora_txp = constrain(atoi(vs), -9, 22);
            }
        } else {
            // Contacts section: "- hash:" starts an entry, indented keys follow.
            if (line.startsWith("- hash:")) {
                if (hash[0]) tdeck_settings_apply_contact(hash, alias, fav, pub, appd);
                memset(hash, 0, sizeof(hash));
                memset(alias, 0, sizeof(alias));
                memset(pub, 0, sizeof(pub));
                memset(appd, 0, sizeof(appd));
                fav = false;
                const char* p = line.c_str() + 7;
                tdeck_yaml_strip_quotes(&p);
                strncpy(hash, p, sizeof(hash)-1);
            } else {
                int ci = line.indexOf(':');
                if (ci <= 0) continue;
                String k = line.substring(0, ci);
                k.trim();
                String v = line.substring(ci + 1);
                v.trim();
                const char* vs = v.c_str();
                tdeck_yaml_strip_quotes(&vs);
                if (k == "alias") {
                    strncpy(alias, vs, sizeof(alias)-1);
                } else if (k == "favorite") {
                    fav = (v == "true");
                } else if (k == "public_key") {
                    strncpy(pub, vs, sizeof(pub)-1);
                } else if (k == "app_data") {
                    strncpy(appd, vs, sizeof(appd)-1);
                }
            }
        }
    }
    if (hash[0]) tdeck_settings_apply_contact(hash, alias, fav, pub, appd);
    f.close();
    tdeck_settings_dirty = false;
    return true;
}

bool tdeck_settings_save() {
    if (!tdeck_sd_ready) return false;
    File f = SD.open(TDECK_SETTINGS_PATH, FILE_WRITE);
    if (!f) return false;

    f.println("# microReticulum T-Deck settings");
    f.printf("device_name: \"%s\"\n", tdeck_set_device_name);
    f.printf("brightness: %u\n", tdeck_set_brightness);
    f.printf("blank_timeout: %u\n", tdeck_set_blank_timeout);
    f.printf("gps_enabled: %s\n", tdeck_set_gps_enabled ? "true" : "false");
    f.printf("timezone_offset: %d\n", tdeck_set_tz_offset_min);
    f.printf("announce_interval: %u\n", tdeck_set_announce_interval);
    f.printf("confirm_send: %s\n", tdeck_set_confirm_send ? "true" : "false");
    f.printf("radio_freq: %u\n", lora_freq);
    f.printf("radio_bw: %u\n", lora_bw);
    f.printf("radio_sf: %d\n", lora_sf);
    f.printf("radio_cr: %d\n", lora_cr);
    f.printf("radio_txp: %d\n", lora_txp);
    f.println("contacts:");
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        tdeck_contact_t& c = tdeck_contacts[i];
        if (!c.persisted && !c.favorite && !c.alias[0]) continue;
        f.printf("  - hash: \"%s\"\n", c.hash);
        if (c.alias[0]) f.printf("    alias: \"%s\"\n", c.alias);
        if (c.favorite) f.printf("    favorite: true\n");
        if (c.has_key && c.pubkey[0]) f.printf("    public_key: \"%s\"\n", c.pubkey);
        if (c.app_data[0]) f.printf("    app_data: \"%s\"\n", c.app_data);
        if (c.last_seen > 0) f.printf("    last_seen: %.0f\n", c.last_seen);
    }
    f.close();
    tdeck_settings_dirty = false;
    return true;
}

// Write a human-readable YAML export of all known contacts to the SD card.
bool tdeck_contacts_export() {
    if (!tdeck_sd_ready) return false;
    File f = SD.open(TDECK_EXPORT_PATH, FILE_WRITE);
    if (!f) return false;

    f.println("# microReticulum T-Deck contact export");
    f.printf("exported: %.0f\n", (double)(millis() / 1000));
    f.printf("count: %u\n", tdeck_contact_count);
    f.println("contacts:");
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        tdeck_contact_t& c = tdeck_contacts[i];
        f.printf("  - hash: \"%s\"\n", c.hash);
        if (c.alias[0]) f.printf("    alias: \"%s\"\n", c.alias);
        if (c.app_data[0]) f.printf("    name: \"%s\"\n", c.app_data);
        if (c.favorite) f.printf("    favorite: true\n");
        if (c.has_key && c.pubkey[0]) f.printf("    public_key: \"%s\"\n", c.pubkey);
        if (c.dest_hash[0]) f.printf("    destination: \"%s\"\n", c.dest_hash);
        if (c.last_seen > 0) f.printf("    last_seen: %.0f\n", c.last_seen);
    }
    f.close();
    return true;
}

// Apply the configured brightness: display backlight on/off plus keyboard
// backlight level, kept in sync with Display.h's contrast globals so the two
// never fight over the panel.
void tdeck_apply_brightness() {
    uint8_t v = tdeck_set_brightness;
    display_intensity = v;
    display_contrast  = v;
    set_contrast(&display, v);
    tdeck_kb.setBacklight(v);
}

// ============================================================================
// Drawing helpers
// ============================================================================

// Small satellite glyph for the GPS status indicator. The whole icon (antenna
// tip + mast, central body, and two solar panels) is drawn in `color` so GPS
// state is conveyed by colour.
static void tdeck_draw_gps_icon(TDeckCanvas& c, int16_t x, int16_t y, uint16_t color) {
    c.drawFastVLine(x, y - 3, 2, color);
    c.fillCircle(x, y - 4, 1, color);
    c.fillRect(x - 4, y - 1, 3, 2, color);
    c.fillRect(x + 2, y - 1, 3, 2, color);
    c.drawFastHLine(x - 4, y - 1, 9, color);
    c.fillRect(x - 1, y - 1, 2, 3, color);
}

// Small cell-tower glyph for the Reticulum/LoRa mesh status indicator.
static void tdeck_draw_cell_tower_icon(TDeckCanvas& c, int16_t x, int16_t y, uint16_t color) {
    c.drawFastHLine(x - 4, y - 4, 3, color);
    c.drawFastHLine(x - 1, y - 4, 3, color);
    c.drawFastHLine(x + 2, y - 4, 3, color);
    c.drawFastVLine(x, y - 3, 3, color);
    c.drawLine(x - 3, y - 1, x, y + 2, color);
    c.drawLine(x + 3, y - 1, x, y + 2, color);
    c.drawFastHLine(x - 2, y, 5, color);
    c.drawFastHLine(x - 3, y + 2, 7, color);
}

#ifdef HAS_RNS
// Count directly reachable destinations (1 hop): peers whose announce has been
// heard within the last 5 minutes, excluding our own echoed identity.
uint8_t tdeck_direct_count() {
    uint8_t n = 0;
    const RNS::Identity& self = RNS::Transport::identity();
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        if (self && strcmp(tdeck_contacts[i].hash, self.hash().toHex().c_str()) == 0) continue;
        if (tdeck_contacts[i].last_seen_ms > 0 &&
            (millis() - tdeck_contacts[i].last_seen_ms) < 300000UL) {
            n++;
        }
    }
    return n;
}

bool tdeck_direct_at(uint8_t idx, char* out, size_t n, uint8_t* hops) {
    uint8_t seen = 0;
    const RNS::Identity& self = RNS::Transport::identity();
    for (uint8_t i = 0; i < tdeck_contact_count; i++) {
        if (self && strcmp(tdeck_contacts[i].hash, self.hash().toHex().c_str()) == 0) continue;
        if (tdeck_contacts[i].last_seen_ms > 0 &&
            (millis() - tdeck_contacts[i].last_seen_ms) < 300000UL) {
            if (seen == idx) {
                strncpy(out, tdeck_contacts[i].hash, n - 1);
                out[n-1] = '\0';
                if (hops) *hops = 1;  // LoRa: anything heard is direct
                return true;
            }
            seen++;
        }
    }
    return false;
}
#endif

// Top status bar: node name on the left; right-justified cluster of battery
// percentage, clock, GPS satellite, cell tower, and direct connection count.
static void tdeck_ui_draw_status_bar(TDeckCanvas& c) {
    c.fillRect(0, 0, TDECK_SCREEN_W, TDECK_SBAR_H, TDECK_COL_SBAR);
    c.drawFastHLine(0, TDECK_SBAR_H - 1, TDECK_SCREEN_W, TDECK_COL_DIM);
    c.setTextSize(1);

    c.setTextColor(TDECK_COL_FG);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H / 2 - 4);
    char name[17];
    strncpy(name, tdeck_set_device_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    c.print(name);

    int x  = TDECK_SCREEN_W - TDECK_LIST_X;
    int cy = TDECK_SBAR_H / 2;

    uint8_t direct = 0;
  #ifdef HAS_RNS
    direct = tdeck_direct_count();
  #endif
    char direct_txt[8];
    snprintf(direct_txt, sizeof(direct_txt), "%u", (unsigned)direct);
    x -= strlen(direct_txt) * 6;
    c.setTextColor(direct > 0 ? TDECK_COL_ACCENT : TDECK_COL_DIM);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(direct_txt);

    x -= 10;
    bool lora_failed = !modem_installed || (radio_error && !radio_online);
    bool lora_active = false;
  #ifdef HAS_RNS
    if (tdeck_last_rx_ms > 0 && (millis() - tdeck_last_rx_ms) < 300000UL) {
        lora_active = true;
    }
  #endif
    uint16_t lora_col;
    if (lora_failed) {
        lora_col = TDECK_COL_ERR;
    } else if (radio_online) {
        lora_col = lora_active ? TDECK_COL_OK : TDECK_COL_WARN;
    } else {
        lora_col = TDECK_COL_DIM;
    }
    tdeck_draw_cell_tower_icon(c, x, cy, lora_col);

    x -= 10;
    uint16_t gps_col;
    if (!tdeck_set_gps_enabled) {
        gps_col = TDECK_COL_DIM;
    } else if (tdeck_gps_fix) {
        gps_col = TDECK_COL_OK;
    } else if (tdeck_gps_present ||
               (millis() - tdeck_ui_start_ms) < 5000) {
        gps_col = TDECK_COL_WARN;
    } else {
        gps_col = TDECK_COL_ERR;
    }
    tdeck_draw_gps_icon(c, x, cy, gps_col);

    x -= 6;
    char timestr[16];
    tdeck_ui_time_str(timestr, sizeof(timestr));
    x -= strlen(timestr) * 6;
    c.setTextColor(TDECK_COL_FG);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(timestr);

    char bat[8];
    if (battery_ready) {
        snprintf(bat, sizeof(bat), "%u%%", (uint8_t)(battery_percent + 0.5f));
    } else {
        snprintf(bat, sizeof(bat), "--%%");
    }
    x -= 6;
    x -= strlen(bat) * 6;
    c.setTextColor(TDECK_COL_FG);
    c.setCursor(x, TDECK_SBAR_H / 2 - 4);
    c.print(bat);
}

// Bottom hint bar with an optional transient toast message.
void tdeck_ui_draw_footer(TDeckCanvas& c, const char* hint) {
    c.fillRect(0, TDECK_SCREEN_H - TDECK_FOOT_H, TDECK_SCREEN_W, TDECK_FOOT_H, TDECK_COL_SBAR);
    c.drawFastHLine(0, TDECK_SCREEN_H - TDECK_FOOT_H, TDECK_SCREEN_W, TDECK_COL_DIM);
    c.setTextSize(1);
    c.setTextColor(TDECK_COL_DIM);
    c.setCursor(TDECK_LIST_X, TDECK_SCREEN_H - TDECK_FOOT_H + 5);
    if (tdeck_toast[0] && millis() < tdeck_toast_until) {
        c.print(tdeck_toast);
    } else if (hint) {
        c.print(hint);
    }
}

// Highlighted list row with a selection bar.
void tdeck_ui_draw_row(TDeckCanvas& c, uint8_t y, const char* text, bool selected) {
    if (selected) {
        c.fillRect(0, y, TDECK_SCREEN_W, 14, TDECK_COL_SEL);
    }
    c.setTextSize(1);
    c.setTextColor(selected ? ST77XX_WHITE : TDECK_COL_FG);
    c.setCursor(TDECK_LIST_X, y + 3);
    c.print(text);
}

// Draws a scrollable list of up to `n` items with a title header. `cursor` is
// the selected index, `scroll` the first visible index. Returns the number of
// visible rows.
uint8_t tdeck_ui_list_frame(TDeckCanvas& c, const char* title, uint8_t n,
                            uint8_t cursor, uint8_t* scroll) {
    c.setTextSize(2);
    c.setTextColor(TDECK_COL_ACCENT);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 4);
    c.print(title);
    c.drawFastHLine(0, TDECK_SBAR_H + 24, TDECK_SCREEN_W, TDECK_COL_DIM);

    uint8_t rows = (TDECK_SCREEN_H - TDECK_SBAR_H - 24 - TDECK_FOOT_H) / 16;
    if (rows < 1) rows = 1;
    if (*scroll > cursor) *scroll = cursor;
    if (cursor >= *scroll + rows) *scroll = cursor - rows + 1;
    return rows;
}

// Full-screen text entry used by compose / alias / name editing.
void tdeck_ui_draw_text_input(const char* title, const char* buf) {
    TDeckCanvas &c = tdeck_canvas;
    c.setTextSize(2);
    c.setTextColor(TDECK_COL_ACCENT);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 8);
    c.print(title);
    c.drawFastHLine(0, TDECK_SBAR_H + 32, TDECK_SCREEN_W, TDECK_COL_DIM);

    c.fillRect(6, TDECK_SBAR_H + 42, TDECK_SCREEN_W - 12, 42, TDECK_COL_SBAR);
    c.drawRect(6, TDECK_SBAR_H + 42, TDECK_SCREEN_W - 12, 42, TDECK_COL_DIM);
    c.setTextSize(2);
    c.setTextColor(TDECK_COL_FG);
    uint8_t start_line = strlen(buf) > 50 ? strlen(buf) - 50 : 0;
    const char* p = buf + start_line;
    c.setCursor(12, TDECK_SBAR_H + 48);
    uint8_t col = 0;
    for (; *p; p++) {
        c.print(*p);
        if (++col >= 25) {
            col = 0;
            c.setCursor(12, TDECK_SBAR_H + 66);
        }
    }
    c.drawFastHLine(0, TDECK_SBAR_H + 96, TDECK_SCREEN_W, TDECK_COL_DIM);

    c.setTextSize(1);
    c.setTextColor(TDECK_COL_DIM);
    c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 104);
    c.print("Enter: ok   Backspace: delete   L: back");
    tdeck_ui_draw_footer(c, "type on keyboard  L: back");
}

// Simple 12x12 app glyphs for the home screen menu.
static void tdeck_ui_draw_icon(TDeckCanvas& c, uint8_t x, uint8_t y, uint8_t icon, uint16_t color) {
    c.fillRect(x, y, 12, 12, color);
    switch (icon) {
        case 0: // envelope
            c.fillRect(x, y + 3, 12, 6, TDECK_COL_SEL);
            c.drawLine(x, y + 3, x + 6, y + 9, TDECK_COL_SEL);
            c.drawLine(x + 12, y + 3, x + 6, y + 9, TDECK_COL_SEL);
            break;
        case 1: // contacts (two heads)
            c.fillCircle(x + 4, y + 4, 3, TDECK_COL_SEL);
            c.fillCircle(x + 9, y + 4, 3, TDECK_COL_SEL);
            c.fillRect(x + 1, y + 8, 6, 4, TDECK_COL_SEL);
            c.fillRect(x + 7, y + 8, 6, 4, TDECK_COL_SEL);
            break;
        case 2: // network (circle + dot)
            c.drawCircle(x + 6, y + 6, 5, TDECK_COL_SEL);
            c.fillCircle(x + 6, y + 6, 2, TDECK_COL_SEL);
            break;
        case 3: // settings (gear = circle + spokes)
            c.drawCircle(x + 6, y + 6, 4, TDECK_COL_SEL);
            c.fillCircle(x + 6, y + 6, 2, TDECK_COL_SEL);
            c.drawFastHLine(x + 2, y + 6, 8, TDECK_COL_SEL);
            c.drawFastVLine(x + 6, y + 2, 8, TDECK_COL_SEL);
            break;
        case 4: // SAR (crosshair map pin)
            c.drawCircle(x + 6, y + 6, 5, TDECK_COL_SEL);
            c.drawLine(x + 6, y + 2, x + 6, y + 10, TDECK_COL_SEL);
            c.drawLine(x + 2, y + 6, x + 10, y + 6, TDECK_COL_SEL);
            break;
    }
}

// Home screen / app launcher: iterates the app registry. New apps added to
// the registry automatically appear here — no code changes needed.
static void tdeck_ui_draw_home() {
    TDeckCanvas &c = tdeck_canvas;
    c.setTextSize(2);
    c.setTextColor(TDECK_COL_ACCENT);
    c.setCursor(10, TDECK_SBAR_H + 6);
    c.print("microReticulum");
    c.drawFastHLine(0, TDECK_SBAR_H + 28, TDECK_SCREEN_W, TDECK_COL_DIM);

    uint8_t napps = TDeckAppRegistry::count();
    if (napps == 0) {
        c.setTextSize(1);
        c.setTextColor(TDECK_COL_WARN);
        c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 48);
        c.print("No applications registered");
        tdeck_ui_draw_footer(c, "build with Apps/*.cpp to enable");
        return;
    }
    if (tdeck_cursor >= napps) tdeck_cursor = napps - 1;

    for (uint8_t i = 0; i < napps; i++) {
        TDeckApp* app = TDeckAppRegistry::app(i);
        if (!app) continue;
        uint8_t y = TDECK_SBAR_H + 36 + i * 32;
        bool sel = (i == tdeck_cursor);
        if (sel) c.fillRect(0, y - 1, TDECK_SCREEN_W, 30, TDECK_COL_SEL);
        tdeck_ui_draw_icon(c, 14, y + 5, app->icon(), sel ? ST77XX_WHITE : TDECK_COL_ACCENT);
        c.setTextSize(2);
        c.setTextColor(sel ? ST77XX_WHITE : TDECK_COL_FG);
        c.setCursor(40, y + 5);
        c.print(app->name());
    }

    tdeck_ui_draw_footer(c, "WASD/TB: nav  Enter: select  Bksp: back");
}

// Master draw: the canvas is cleared and the status bar is drawn exactly
// once here, before the active app paints its content. Home draws the
// launcher; any other screen delegates to the owning app's draw().
static void tdeck_ui_draw() {
    if (!tdeck_canvas.valid()) return;
    TDeckCanvas &c = tdeck_canvas;
    c.fillScreen(TDECK_COL_BG);
    tdeck_ui_draw_status_bar(c);
    if (tdeck_screen == SCREEN_HOME) {
        tdeck_ui_draw_home();
    } else {
        TDeckApp* app = TDeckAppRegistry::byScreen(tdeck_screen);
        if (app) {
            app->draw();
        } else {
            tdeck_ui_draw_home();  // unknown screen: fall back to launcher
        }
    }
    display.drawRGBBitmap(0, 0, tdeck_canvas.buf(), TDECK_SCREEN_W, TDECK_SCREEN_H);
}

// ============================================================================
// Navigation
// ============================================================================

void tdeck_ui_go_home() {
    TDeckApp* leaving = TDeckAppRegistry::byScreen(tdeck_screen);
    if (leaving) leaving->onDeactivate();
    tdeck_nav_depth = 0;
    tdeck_screen = SCREEN_HOME;
    tdeck_sub = 0;
    tdeck_cursor = 0;
    tdeck_scroll = 0;
    tdeck_dirty = true;
}

void tdeck_ui_nav_push(uint8_t screen, uint8_t sub) {
    if (tdeck_nav_depth < TDECK_NAV_DEPTH) {
        tdeck_nav_screen[tdeck_nav_depth] = tdeck_screen;
        tdeck_nav_sub[tdeck_nav_depth]    = tdeck_sub;
        tdeck_nav_depth++;
    }
    TDeckApp* leaving = TDeckAppRegistry::byScreen(tdeck_screen);
    if (leaving) leaving->onDeactivate();
    tdeck_screen = screen;
    tdeck_sub = sub;
    tdeck_cursor = 0;
    tdeck_scroll = 0;
    TDeckApp* entering = TDeckAppRegistry::byScreen(screen);
    if (entering) entering->onActivate();
    tdeck_dirty = true;
}

void tdeck_ui_nav_back() {
    TDeckApp* leaving = TDeckAppRegistry::byScreen(tdeck_screen);
    if (leaving) leaving->onDeactivate();
    if (tdeck_nav_depth > 0) {
        tdeck_nav_depth--;
        tdeck_screen = tdeck_nav_screen[tdeck_nav_depth];
        tdeck_sub    = tdeck_nav_sub[tdeck_nav_depth];
    } else {
        tdeck_screen = SCREEN_HOME;
        tdeck_sub = 0;
    }
    tdeck_cursor = 0;
    tdeck_scroll = 0;
    TDeckApp* entering = TDeckAppRegistry::byScreen(tdeck_screen);
    if (entering) entering->onActivate();
    tdeck_dirty = true;
}

// Launch the app at the current launcher cursor.
static void tdeck_ui_launch(uint8_t idx) {
    TDeckApp* app = TDeckAppRegistry::app(idx);
    if (!app) return;
    tdeck_ui_nav_push(app->screen_id(), app->entry_sub());
}

// ============================================================================
// Input handling
// ============================================================================
//
// Navigation input map:
//   back           = Backspace key, A key, trackball-left
//   enter          = Enter key, D key, trackball right, trackball push
//   scroll up      = W key, trackball up
//   scroll down    = S key, trackball down
//   home           = trackball long press
//
// In a text field every printable key (including W/A/S/D) types a character.
// Backspace deletes without leaving the field; Enter applies. Key-based
// navigation never exits text input - only the trackball-left event (or an
// app's onBack, which apps route to tdeck_ui_nav_back()) leaves a text field.

static void tdeck_ui_handle_key(char key, uint8_t state) {
    // Only act on press events; ignore release/auto-repeat noise.
    if (state != KB_KEY_STATE_PRESS && state != KB_KEY_STATE_LONG_PRESS) return;

    // In a text field every printable key types a character. Backspace
    // deletes without leaving the field; Enter applies.
    if (tdeck_in_text_input()) {
        switch (key) {
            case 0x0A:   // LF / Enter: apply the text
            case 0x0D:   // CR / Enter: apply the text
                tdeck_input_apply();
                break;
            case 0x08:   // Backspace: delete character (does not exit)
                tdeck_input_del();
                break;
            default:
                if (key >= 0x20 && key <= 0x7E) {
                    tdeck_input_add(key);
                }
                break;
        }
        tdeck_dirty = true;
        return;
    }

    TDeckApp* app = TDeckAppRegistry::active();

    // Apps may claim the WASD navigation keys before the framework's default
    // mapping. The SAR map, for example, redefines W/S as zoom (rather than
    // onUp()/onDown() pan) while keeping A/D as back/forward. Returning true
    // suppresses the default translation below.
    if (app &&
        (key == 'w' || key == 'W' || key == 'a' || key == 'A' ||
         key == 's' || key == 'S' || key == 'd' || key == 'D')) {
        if (app->onNavKey(key, state)) {
            tdeck_dirty = true;
            return;
        }
    }

    if (!app) {
        // ---- Home / launcher ----
        switch (key) {
            case 0x0A:
            case 0x0D:   // enter -> launch
            case 'd': case 'D':
                tdeck_ui_launch(tdeck_cursor);
                break;
            case 'w': case 'W':
                if (TDeckAppRegistry::count() > 0)
                    tdeck_cursor = (tdeck_cursor + TDeckAppRegistry::count() - 1) % TDeckAppRegistry::count();
                break;
            case 's': case 'S':
                if (TDeckAppRegistry::count() > 0)
                    tdeck_cursor = (tdeck_cursor + 1) % TDeckAppRegistry::count();
                break;
            default:
                // Home-screen app shortcuts (1-N) remain available.
                if (key >= '1' && key <= '9') {
                    uint8_t idx = (uint8_t)(key - '1');
                    if (idx < TDeckAppRegistry::count()) tdeck_ui_launch(idx);
                }
                break;
        }
        tdeck_dirty = true;
        return;
    }

    // ---- App screen ----
    switch (key) {
        case 0x0A:   // LF / Enter -> select
        case 0x0D:   // CR / Enter -> select
        case 'd': case 'D':
            app->onSelect();
            break;
        case 0x08:   // Backspace -> back
        case 'a': case 'A':
            app->onBack();
            break;
        case 'w': case 'W':
            app->onUp();
            break;
        case 's': case 'S':
            app->onDown();
            break;
        default:
            // Apps with additional key handling (e.g. the SAR map zoom keys)
            // receive the raw printable key here.
            app->onKey(key, state);
            break;
    }
    tdeck_dirty = true;
}

static void tdeck_ui_handle_trackball(tb_event_t event) {
    // Long press always returns to the home screen (unless a text field is
    // active - key-based navigation never exits a text field).
    if (event == TB_EVENT_PRESS_LONG) {
        if (!tdeck_in_text_input()) tdeck_ui_go_home();
        tdeck_dirty = true;
        return;
    }

    TDeckApp* app = TDeckAppRegistry::active();
    if (!app) {
        // ---- Home: scroll + launch ----
        switch (event) {
            case TB_EVENT_UP:
                if (TDeckAppRegistry::count() > 0)
                    tdeck_cursor = (tdeck_cursor + TDeckAppRegistry::count() - 1) % TDeckAppRegistry::count();
                break;
            case TB_EVENT_DOWN:
                if (TDeckAppRegistry::count() > 0)
                    tdeck_cursor = (tdeck_cursor + 1) % TDeckAppRegistry::count();
                break;
            case TB_EVENT_PRESS:
            case TB_EVENT_RIGHT:
                tdeck_ui_launch(tdeck_cursor);
                break;
            default: break;
        }
        tdeck_dirty = true;
        return;
    }

    // ---- App: all trackball events go to the active app. The default
    //      onTrackball() in TDeckApp maps up/down/left/right/press to
    //      onUp/onDown/onBack/onSelect/onSelect; apps like SAR override it
    //      for pan/zoom behaviour. ----
    if (tdeck_in_text_input()) {
        // In a text field only the left (back) event leaves the field.
        if (event == TB_EVENT_LEFT) {
            app->onBack();
            tdeck_dirty = true;
        }
        return;
    }
    app->onTrackball(event);
    tdeck_dirty = true;
}

// ============================================================================
// RNS integration
// ============================================================================

#ifdef HAS_RNS

// ---- Aspect dispatch table --------------------------------------------------
// Apps register a packet handler for an aspect via tdeck_rns_register_aspect()
// during onRnsSetup(). The framework's destination packet callbacks route
// incoming payloads to the registered handler (or drop them).

#define TDECK_MAX_ASPECT_HANDLERS 8
static struct {
    const char* aspect;
    tdeck_aspect_handler_t handler;
} s_aspect_handlers[TDECK_MAX_ASPECT_HANDLERS];
static uint8_t s_aspect_count = 0;

void tdeck_rns_register_aspect(const char* aspect, tdeck_aspect_handler_t handler) {
    if (!aspect || !handler) return;
    for (uint8_t i = 0; i < s_aspect_count; i++) {
        if (strcmp(s_aspect_handlers[i].aspect, aspect) == 0) {
            s_aspect_handlers[i].handler = handler;
            return;
        }
    }
    if (s_aspect_count < TDECK_MAX_ASPECT_HANDLERS) {
        s_aspect_handlers[s_aspect_count].aspect = aspect;
        s_aspect_handlers[s_aspect_count].handler = handler;
        s_aspect_count++;
    }
}

tdeck_aspect_handler_t tdeck_rns_get_handler(const char* aspect) {
    if (!aspect) return NULL;
    for (uint8_t i = 0; i < s_aspect_count; i++) {
        if (strcmp(s_aspect_handlers[i].aspect, aspect) == 0) {
            return s_aspect_handlers[i].handler;
        }
    }
    return NULL;
}

// Fetches the (idx)th '!'-separated field after `start`. Returns a pointer to
// the first byte following the field (or NULL when the field was the last one),
// and copies the field into `out`.
const char* tdeck_rns_field(const char* start, uint8_t idx, char* out, size_t n) {
    const char* p = start;
    for (uint8_t i = 0; i < idx; i++) {
        p = strchr(p, '!');
        if (!p) return NULL;
        p++;
    }
    const char* end = strchr(p, '!');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return end ? end + 1 : NULL;
}

// Decode the optional location element appended by other T-Deck UIs to
// their LXMF announce app_data:
//   [display_name, stamp_cost, [supported_features], [lat, lon, alt_m, sat, age_s]]
static bool tdeck_announce_location(const uint8_t* data, size_t size,
                                    float& lat, float& lon, float& alt,
                                    uint8_t& sat, float& age_s) {
    if (!data || size < 3) return false;
    LXMF::MsgPackReader reader(data, size);

    size_t count = 0;
    if (!reader.read_array_header(count) || count < 5) return false;

    std::vector<uint8_t> name;
    if (!reader.read_bin(name)) return false;

    if (!reader.skip_object()) return false;
    if (!reader.skip_object()) return false;

    size_t loc_count = 0;
    if (!reader.read_array_header(loc_count) || loc_count < 5) return false;

    double lat_d = 0, lon_d = 0, alt_d = 0, age_d = 0;
    if (!reader.read_double(lat_d) || !reader.read_double(lon_d) ||
        !reader.read_double(alt_d)) return false;

    int64_t sat_i = 0;
    if (!reader.read_int(sat_i)) return false;
    if (sat_i < 0 || sat_i > 255) return false;

    if (!reader.read_double(age_d)) return false;

    lat = (float)lat_d;
    lon = (float)lon_d;
    alt = (float)alt_d;
    sat = (uint8_t)sat_i;
    age_s = (float)age_d;
    return true;
}

// Fired by the Transport for every announce heard on the mesh. Populates the
// shared contact store (framework-owned).
static void tdeck_rns_on_announce(const RNS::Bytes& dst_hash, const RNS::Identity& id,
                                  const RNS::Bytes& app_data) {
    if (dst_hash.size() != RNS::Type::Reticulum::DESTINATION_LENGTH) return;
    char hash[40] = {0}, dest[40] = {0}, pub[140] = {0}, appd[TDECK_APP_DATA_MAX+1] = {0};

    std::string dh = dst_hash.toHex();
    strncpy(dest, dh.c_str(), sizeof(dest)-1);

    if (id) {
        std::string h = id.hash().toHex();
        strncpy(hash, h.c_str(), sizeof(hash)-1);
        std::string pk = id.get_public_key().toHex();
        strncpy(pub, pk.c_str(), sizeof(pub)-1);
    } else {
        strncpy(hash, dest, sizeof(hash)-1);
    }

    float peer_lat = 0.0f, peer_lon = 0.0f, peer_alt = 0.0f, peer_age = 0.0f;
    uint8_t peer_sat = 0;
    bool has_loc = false;

    if (app_data.size() > 0) {
        std::vector<uint8_t> lxmf_name;
        bool lxmf_decoded = LXMF::announce_display_name(app_data.data(),
                                                        app_data.size(),
                                                        lxmf_name);
        if (lxmf_decoded) {
            size_t n = lxmf_name.size();
            if (n > TDECK_APP_DATA_MAX) n = TDECK_APP_DATA_MAX;
            if (n > 0) {
                memcpy(appd, lxmf_name.data(), n);
                appd[n] = '\0';
            }
        } else {
            std::string ad = app_data.toString();
            strncpy(appd, ad.c_str(), sizeof(appd)-1);
        }

        has_loc = tdeck_announce_location(app_data.data(), app_data.size(),
                                          peer_lat, peer_lon, peer_alt,
                                          peer_sat, peer_age);
    }
    tdeck_contact_upsert(hash, dest, pub[0] ? pub : NULL, appd, RNS::Utilities::OS::time());
    tdeck_contact_t* up = tdeck_contact_find(hash);
    if (up) up->last_seen_ms = millis();

    if (has_loc) {
        tdeck_contact_t* c = tdeck_contact_find(hash);
        if (c) {
            c->lat  = peer_lat;
            c->lon  = peer_lon;
            c->alt  = peer_alt;
            c->sat  = peer_sat;
            c->age_s = peer_age;
        }
    }
    tdeck_dirty = true;
}

class TDeckAnnounceHandler : public RNS::AnnounceHandler {
  public:
    TDeckAnnounceHandler() : RNS::AnnounceHandler() {}
    virtual void received_announce(const RNS::Bytes& destination_hash,
                                   const RNS::Identity& announced_identity,
                                   const RNS::Bytes& app_data) override {
        tdeck_rns_on_announce(destination_hash, announced_identity, app_data);
    }
};

// Unified packet callbacks: each framework destination dispatches to the
// aspect handler registered by the owning app (if any).
static void tdeck_rns_on_message(const RNS::Bytes& data, const RNS::Packet& packet) {
    tdeck_aspect_handler_t h = tdeck_rns_get_handler(TDECK_UI_RNS_ASPECT_MSGS);
    if (h) h(data, packet);
}

static void tdeck_rns_on_broadcast(const RNS::Bytes& data, const RNS::Packet& packet) {
    tdeck_aspect_handler_t h = tdeck_rns_get_handler(TDECK_UI_RNS_ASPECT_BCAST);
    if (h) h(data, packet);
}

static void tdeck_rns_on_ping(const RNS::Bytes& data, const RNS::Packet& packet) {
    tdeck_aspect_handler_t h = tdeck_rns_get_handler(TDECK_UI_RNS_ASPECT_PING);
    if (h) h(data, packet);
}

static void tdeck_rns_on_sar_position(const RNS::Bytes& data, const RNS::Packet& packet) {
    tdeck_aspect_handler_t h = tdeck_rns_get_handler(TDECK_SAR_RNS_ASPECT);
    if (h) h(data, packet);
}

static void tdeck_rns_on_link_established(RNS::Link& link) {
    link.set_packet_callback(tdeck_rns_on_message);
}

// One-time setup once RNS has started (deferred from tdeck_ui_init because the
// Reticulum instance comes up later in setup()).
void tdeck_rns_setup() {
    if (tdeck_rns_ready) return;
    const RNS::Identity& id = RNS::Transport::identity();
    if (!id) return;

    tdeck_rns_ready = true;

    tdeck_rns_msg_dest = RNS::Destination(id, RNS::Type::Destination::IN,
                                          RNS::Type::Destination::SINGLE,
                                          TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_MSGS);
    tdeck_rns_msg_dest.set_packet_callback(tdeck_rns_on_message);
    tdeck_rns_msg_dest.set_link_established_callback(tdeck_rns_on_link_established);
    tdeck_rns_msg_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    tdeck_rns_ping_dest = RNS::Destination(id, RNS::Type::Destination::IN,
                                           RNS::Type::Destination::SINGLE,
                                           TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_PING);
    tdeck_rns_ping_dest.set_packet_callback(tdeck_rns_on_ping);
    tdeck_rns_ping_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    // Shared PLAIN broadcast destination: any node running this UI can
    // transmit unencrypted broadcasts without the recipient's public key.
    tdeck_rns_bcast_dest = RNS::Destination(RNS::Identity({RNS::Type::NONE}),
                                            RNS::Type::Destination::IN,
                                            RNS::Type::Destination::PLAIN,
                                            TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_BCAST);
    tdeck_rns_bcast_dest.set_packet_callback(tdeck_rns_on_broadcast);
    tdeck_rns_bcast_dest.set_proof_strategy(RNS::Type::Destination::PROVE_NONE);

    // Encrypted SAR position-beacon destination (per-node identity).
    tdeck_rns_sar_dest = RNS::Destination(id, RNS::Type::Destination::IN,
                                          RNS::Type::Destination::SINGLE,
                                          TDECK_UI_RNS_APP, TDECK_SAR_RNS_ASPECT);
    tdeck_rns_sar_dest.set_packet_callback(tdeck_rns_on_sar_position);
    tdeck_rns_sar_dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    tdeck_rns_ann_handler = RNS::HAnnounceHandler(new TDeckAnnounceHandler());
    if (!tdeck_rns_announce_registered) {
        RNS::Transport::register_announce_handler(tdeck_rns_ann_handler);
        tdeck_rns_announce_registered = true;
    }

    // Let each app register its aspect packet handlers / additional state.
    for (uint8_t i = 0; i < TDeckAppRegistry::count(); i++) {
        TDeckApp* app = TDeckAppRegistry::app(i);
        if (app) app->onRnsSetup();
    }

    tdeck_rns_announce_ui();
    tdeck_rns_sync_contacts();
    tdeck_toast_set("RNS ready");
    tdeck_dirty = true;
}

// Send an unencrypted broadcast to the shared "tdeck_ui.broadcast" PLAIN
// destination.
bool tdeck_rns_send_broadcast(const char* text) {
    if (!tdeck_rns_ready || !tdeck_rns_bcast_dest) return false;
    const RNS::Identity& id = RNS::Transport::identity();
    if (!id) return false;

    char frame[TDECK_MSG_MAX_LEN + 64];
    snprintf(frame, sizeof(frame), "%s%s!%s", TDECK_FRAME_BCAST,
             id.hash().toHex().c_str(), text);
    RNS::Destination out({RNS::Type::NONE}, RNS::Type::Destination::OUT,
                         RNS::Type::Destination::PLAIN,
                         TDECK_UI_RNS_APP, TDECK_UI_RNS_ASPECT_BCAST);
    RNS::Packet(out, RNS::Bytes(frame)).send();
    return true;
}

// Re-announce the UI destinations (used at startup and when the name changes).
// The LXMF announce app_data is msgpack-encoded as:
//   [display_name, stamp_cost, [supported_functionality], location]
void tdeck_rns_announce_ui() {
    if (!tdeck_rns_ready) return;

    arduino::msgpack::Packer packer;
    packer.reserve_buffer(128);
    std::vector<uint8_t> name(tdeck_set_device_name,
                              tdeck_set_device_name + strlen(tdeck_set_device_name));
    arduino::msgpack::object::nil_t stamp_cost;
    std::vector<uint8_t> sf_list = {LXMF::SF_COMPRESSION};
    packer.to_array(name, stamp_cost, sf_list);
    RNS::Bytes app_data(packer.data(), packer.size());

    // Append a location element when a fresh fix is held: the final announce
    // array becomes [name, stamp, sf_list, loc].
    if (tdeck_gps_present && tdeck_gps_fix) {
        float lat = tdeck_gps.latitude();
        float lon = tdeck_gps.longitude();
        float alt = tdeck_gps.altitude();
        uint8_t sat = tdeck_gps.satellites();
        if (lat != 0.0f || lon != 0.0f) {
            uint32_t age = (uint32_t)((millis() - tdeck_gps.lastFixMs()) / 1000);

            arduino::msgpack::Packer loc;
            loc.reserve_buffer(32);
            loc.to_array((double)lat, (double)lon, (double)alt,
                         sat, (double)age);

            std::vector<uint8_t> out;
            out.reserve(app_data.size() + loc.size() + 1);
            out.push_back(0x94);                                    // fixarray4
            if (app_data.size() > 0) {
                out.insert(out.end(), app_data.data() + 1, app_data.data() + app_data.size());
            }
            out.insert(out.end(), loc.data(), loc.data() + loc.size());

            app_data = RNS::Bytes(out.data(), out.size());
        }
    }

    if (tdeck_rns_msg_dest)  tdeck_rns_msg_dest.announce(app_data);
    if (tdeck_rns_ping_dest) tdeck_rns_ping_dest.announce(app_data);
    tdeck_last_announce_at = RNS::Utilities::OS::time();
}

// Import identities RNS has persisted into the contact store.
void tdeck_rns_sync_contacts() {
    auto& known = const_cast<RNS::Persistence::KnownDestinations&>(RNS::Identity::known_destinations());
    for (auto it = known.begin(); it != known.end(); ++it) {
        const auto& entry = *it;
        std::string h = entry.key.toHex();
        const RNS::Persistence::IdentityEntry& ie = entry.value;
        char hash[40] = {0}, pub[140] = {0}, appd[TDECK_APP_DATA_MAX+1] = {0};
        strncpy(hash, h.c_str(), sizeof(hash)-1);
        if (ie._public_key.size() > 0) {
            std::string pk = ie._public_key.toHex();
            strncpy(pub, pk.c_str(), sizeof(pub)-1);
        }
        if (ie._app_data.size() > 0) {
            std::string ad = ie._app_data.toString();
            strncpy(appd, ad.c_str(), sizeof(appd)-1);
        }
        tdeck_contact_upsert(hash, NULL, pub[0] ? pub : NULL, appd, ie._timestamp);
    }
    tdeck_dirty = true;
}

// Send an arbitrary payload to a contact's destination for `aspect`.
// The destination is the deterministic per-identity hash of `<app>.<aspect>`.
bool tdeck_rns_send_payload(const tdeck_contact_t* c, const char* aspect,
                            const RNS::Bytes& payload) {
    if (!tdeck_rns_ready) return false;
    if (!c || !c->has_key || !c->pubkey[0]) return false;
    uint8_t key[64];
    if (!tdeck_hex_decode(c->pubkey, key, 64)) return false;

    RNS::Identity rid(false);
    rid.load_public_key(RNS::Bytes(key, 64));
    RNS::Bytes aspect_hash = RNS::Destination::hash(rid, TDECK_UI_RNS_APP, aspect);
    if (aspect_hash.size() != RNS::Type::Reticulum::DESTINATION_LENGTH) return false;

    RNS::Destination out(rid, RNS::Type::Destination::OUT,
                         RNS::Type::Destination::SINGLE, aspect_hash);
    RNS::Packet(out, payload).send();
    return true;
}

#endif // HAS_RNS

// ============================================================================
// EEPROM provisioning
// ============================================================================

// A T-Deck provisioned purely through the UI (settings.yaml) never goes
// through rnodeconf, so the EEPROM identity block is left blank. Seed an
// rnodeconf-style identity here so the device is provisioned exactly like
// every other board in the repo. Runs from tdeck_ui_init() *before*
// validate_status() so the same boot already sees a valid identity.
static void tdeck_eeprom_provision() {
    if (eeprom_lock_set()) return;                    // already provisioned

    eeprom_write(ADDR_PRODUCT, PRODUCT_TDECK_V1);
    eeprom_write(ADDR_MODEL,   MODEL_D9);
    eeprom_write(ADDR_HW_REV,  0x01);

    for (uint8_t i = 0; i < 4 && i < DEVICE_UID_LEN; i++) {
        eeprom_write(ADDR_SERIAL + i, device_uid[i]);
    }

    uint32_t made = 0x4D525443;                       // "CTMR"
    for (uint8_t i = 0; i < 4; i++) {
        eeprom_write(ADDR_MADE + i, (uint8_t)(made >> (8 * (3 - i))));
    }

    eeprom_update(eeprom_addr(ADDR_INFO_LOCK), INFO_LOCK_BYTE);

    uint8_t data[CHECKSUMMED_SIZE];
    for (uint8_t i = 0; i < CHECKSUMMED_SIZE; i++) {
        data[i] = EEPROM.read(eeprom_addr(i));
    }
    unsigned char *hash = MD5::make_hash((char *)data, CHECKSUMMED_SIZE);
    for (uint8_t i = 0; i < 16; i++) {
        eeprom_update(eeprom_addr(ADDR_CHKSUM + i), hash[i]);
    }
    free(hash);

    tdeck_ui_debug("[TDeck] EEPROM provisioned (identity)");
}

// ============================================================================
// Public API
// ============================================================================

// Called by Display.h's update_display() to determine whether the T-Deck UI
// owns the screen. When true, the legacy 64x64 canvas renderer yields.
bool tdeck_ui_active() {
    return tdeck_ui_ready;
}

// External hook for GPS code: sets whether a 3D fix is currently held.
void tdeck_ui_set_gps_fix(bool fix) {
    if (tdeck_gps_fix != fix) {
        tdeck_gps_fix = fix;
        tdeck_gps_present = true;
        tdeck_dirty = true;
    }
}

// Called by the UI loop (or externally by GPS code) once a fresh GNSS UTC
// epoch is available. Installs the epoch into the Reticulum system clock so
// the status-bar clock, message timestamps and announce timestamps all follow
// GPS-disciplined real-world time. Returns true the first time it applies.
bool tdeck_ui_gps_set_time(uint64_t epoch_s) {
    if (tdeck_gps_time_set) return false;
    if (epoch_s < 1600000000ULL) return false;   // sanity: Oct 2020 floor
    int64_t tz_s = (int64_t)tdeck_set_tz_offset_min * 60;
    if (tz_s > 0) epoch_s += (uint64_t)tz_s;
    else if (tz_s < 0) epoch_s -= (uint64_t)(-tz_s);

  #ifdef HAS_RNS
    uint64_t epoch_ms = epoch_s * 1000ULL;
    uint64_t now_ms   = (uint64_t)millis();
    uint64_t offset   = (epoch_ms > now_ms) ? (epoch_ms - now_ms) : 0;
    RNS::Utilities::OS::setTimeOffset(offset);

    tdeck_gps_time_set = true;
    tdeck_toast_set("GPS time set");
    tdeck_dirty = true;
    tdeck_ui_debug("[TDeck] GPS time applied to RNS clock");
    return true;
  #else
    (void)epoch_s;
    return false;
  #endif
}

void tdeck_ui_init() {
    // Power on the keyboard rail
    pinMode(KB_POWERON, OUTPUT);
    digitalWrite(KB_POWERON, HIGH);
    delay(50);

    // The T-Deck's keyboard and PMU share the I2C bus
    Wire.begin(I2C_SDA, I2C_SCL);

    // Start the keyboard. TDeckKeyboard auto-detects whether the unit has
    // the ESP32-C3 "T-Keyboard" byte-stream controller or a direct BBQ10.
    tdeck_kb.begin(&Wire);
    switch (tdeck_kb.protocol()) {
        case 1: tdeck_ui_debug("[TDeck] keyboard: BBQ10 registers"); break;
        case 2: tdeck_ui_debug("[TDeck] keyboard: ESP32-C3 byte-stream"); break;
        default: tdeck_ui_debug("[TDeck] keyboard: not detected"); break;
    }

    // Start the trackball GPIO interrupts
    tdeck_trackball.begin();

    // Start the GPS/GNSS receiver on UART1 (L76K or compatible NMEA module
    // on the Gover interface; harmless if no module is fitted).
    tdeck_gps.begin();

    // Allocate the off-screen frame buffer (PSRAM-backed)
    tdeck_canvas.allocate();
    if (!tdeck_canvas.valid()) {
        tdeck_ui_debug("[TDeck] warning: could not allocate frame buffer");
    }

    // Mount the microSD card and load /settings.yaml.
    tdeck_sd_init();
    tdeck_settings_load();
    tdeck_apply_brightness();

    if (!tdeck_set_gps_enabled) {
        tdeck_gps.standby();
    }

    // Seed the rnodeconf-style EEPROM identity (must run before
    // validate_status()).
    tdeck_eeprom_provision();

    tdeck_ui_ready = true;
    tdeck_dirty = true;
    tdeck_ui_start_ms = millis();
    tdeck_last_tick = millis();
    tdeck_ui_go_home();

    char dbg[48];
    snprintf(dbg, sizeof(dbg), "[TDeck] UI ready (%u apps registered)",
             (unsigned)TDeckAppRegistry::count());
    tdeck_ui_debug(dbg);
}

void tdeck_ui_loop() {
    if (!tdeck_ui_ready) return;

  #ifdef HAS_RNS
    // The boot path only starts the radio when EEPROM holds a valid radio
    // config AND device_init() succeeds. On a UI-provisioned T-Deck hw_ready
    // stays false, so bring the radio up here now that RNS is up.
    if (modem_installed && !radio_online && !console_active) {
        if (!hw_ready) hw_ready = true;
        if (lora_freq == 0 || lora_freq < 100000000L || lora_freq > 1000000000L) {
            lora_freq = 867200000;
            tdeck_ui_debug("[TDeck] Radio freq default 867.2 MHz");
        }
        if (lora_bw == 0) {
            lora_bw = 125000;
            tdeck_ui_debug("[TDeck] Radio BW default 125 kHz");
        }
        if (lora_sf == 0) {
            lora_sf = 8;
            tdeck_ui_debug("[TDeck] Radio SF default 8");
        }
        if (lora_cr == 0) {
            lora_cr = 5;
            tdeck_ui_debug("[TDeck] Radio CR default 4/5");
        }
        if (lora_txp == 0xFF) {
            lora_txp = 14;
            tdeck_ui_debug("[TDeck] Radio TXP default 14 dBm");
        }

        update_radio_lock();
        if (startRadio()) {
            op_mode = MODE_TNC;
            RNS::Reticulum::transport_enabled(true);
            tdeck_ui_debug("[TDeck] Radio started from settings");
            #if defined(LORA_TRANSPORT)
                printf("[radio] model: 0x%02X", (unsigned int)model);
                printf("[radio] TX power: %d dBm", LoRa->getTxPower());
                printf("[radio] freq: %lu Hz, bw: %lu Hz, sf: %d, cr: 4/%d",
                       (unsigned long)lora_freq, (unsigned long)lora_bw,
                       lora_sf, lora_cr);
            #endif
            if (queue_height > 0) tx_queue_handler();
        }
    }

    // Deferred RNS setup: register the framework destinations + announce
    // handler once Reticulum is up.
    if (!tdeck_rns_ready && reticulum && RNS::Transport::identity()) {
        tdeck_rns_setup();
    }
    if (tdeck_rns_ready && tdeck_set_announce_interval > 0) {
        double now = RNS::Utilities::OS::time();
        if ((now - tdeck_last_announce_at) >= tdeck_set_announce_interval * 60.0) {
            tdeck_rns_announce_ui();
        }
    }

    // Track any traffic received from other nodes so the status bar can show
    // mesh activity (green cell tower) within the last 5 minutes.
    if (tdeck_rns_ready) {
        uint32_t rx = RNS::Transport::packets_received();
        if (rx != tdeck_last_rx_count) {
            tdeck_last_rx_count = rx;
            tdeck_last_rx_ms = millis();
        }
    }

    // Background service for every registered app (SAR beacon pacing, ping
    // polling, stale-track expiry, etc.).
    for (uint8_t i = 0; i < TDeckAppRegistry::count(); i++) {
        TDeckApp* app = TDeckAppRegistry::app(i);
        if (!app) continue;
        if (app->onLoop()) tdeck_dirty = true;
        if (tdeck_rns_ready && app->onRnsLoop()) tdeck_dirty = true;
    }
  #endif

    // Poll the GPS/GNSS UART and keep the status bar + RNS announce in sync.
    if (tdeck_set_gps_enabled) {
        if (tdeck_gps.asleep()) {
            tdeck_gps.wake();
        }
        tdeck_gps.loop();

        if (tdeck_gps.dataSeen() && !tdeck_gps_present) {
            tdeck_gps_present = true;
            tdeck_ui_debug("[TDeck] GPS module detected");
        }

        bool fix = tdeck_gps.hasFix();
        if (fix != tdeck_gps_fix) tdeck_ui_set_gps_fix(fix);

        if (tdeck_gps.epochReady() && !tdeck_gps_time_set) {
            tdeck_ui_gps_set_time(tdeck_gps.gpsEpoch());
        }

      #ifdef HAS_RNS
        if (tdeck_rns_ready && fix && !tdeck_gps_announced) {
            tdeck_gps_announced = true;
            tdeck_rns_announce_ui();
        }
      #endif
    } else {
        tdeck_gps.standby();
        if (tdeck_gps_present) { tdeck_gps_present = false; tdeck_gps_fix = false; tdeck_dirty = true; }
    }

    // Poll keyboard (the BBQ10/C3 latch key events until read)
    TDeckKeyboard::KeyEvent key_event = tdeck_kb.keyEvent();
    if (key_event.key != '\0' || key_event.state != KB_KEY_STATE_IDLE) {
        tdeck_ui_handle_key(key_event.key, key_event.state);
        tdeck_kb.clearInterruptStatus();
        tdeck_dirty = true;
    }

    // Poll trackball
    tb_event_t tb_events = tdeck_trackball.readEvents();
    if (tb_events != TB_EVENT_NONE) {
        tdeck_ui_handle_trackball(tb_events);
        tdeck_dirty = true;
    }

    // Repaint on input events, or periodically so the status bar clock and
    // live screens (ping, battery) stay fresh.
    bool periodic = (millis() - tdeck_last_tick >= tdeck_tick_ms);
    if (periodic) {
        tdeck_last_tick = millis();
    }
    if (tdeck_dirty || periodic) {
        tdeck_dirty = false;
        tdeck_ui_draw();
    }
}

#endif // BOARD_MODEL == BOARD_TDECK
