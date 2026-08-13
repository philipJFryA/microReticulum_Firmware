// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// T-Deck application framework.
//
// Every application on the T-Deck launcher is a self-contained module that
// derives from TDeckApp and registers itself with TDeckAppRegistry. Each app
// lives in its own header/cpp pair under Apps/ (e.g. Apps/AppMessages.h +
// Apps/AppMessages.cpp) and provides:
//
//   * a display name and 12x12 launcher-glyph id
//   * an entry sub-screen id (what tdeck_screen/tdeck_sub show when launched)
//   * draw(), onSelect()/onUp()/onDown()/onBack(), onKey(), onTrackball(),
//     onLoop(), onBackground() and lifecycle hooks
//   * optional RNS hooks (onRnsSetup for destinations, aspect handlers)
//
// Registration is automatic: each app module defines a static registrar
// instance whose constructor calls TDeckAppRegistry::registerApp(). New apps
// are added by dropping an App*.cpp in Apps/ — no launcher edits needed.
//
// The framework implementation lives in TDeckUI.h, which is included from
// exactly one translation unit (RNode_Firmware.ino). This header is the
// contract that both the framework and the app modules compile against.

#pragma once

#include <Arduino.h>

#include "TDeckTypes.h"
#include "TDeckTrackball.h"
#include "TDeckGPS.h"
#include "TDeckKeyboard.h"

// ---- Screen ids ---------------------------------------------------------------
// The framework owns the home/launcher screen (SCREEN_HOME). Apps use the
// fixed ids below (preserving the original enum values from the monolithic
// UI) so the launcher, navigation stack and input dispatch stay fully data
// driven. Apps that do not want a hardcoded id can request one at
// registration time from SCREEN_APP_BASE via the registry.
#define SCREEN_HOME       0
#define SCREEN_MESSAGES   1
#define SCREEN_CONTACTS   2
#define SCREEN_RETICULUM  3
#define SCREEN_SETTINGS   4
#define SCREEN_SAR        5

// ---- Shared canvas + hardware singletons (defined in TDeckUI.h's TU) ----------
// The trackball singleton is defined by TDeckTrackball.h's implementation
// block (TDECK_TRACKBALL_IMPLEMENTATION), which the framework TU enables
// before including this header.
extern TDeckCanvas    tdeck_canvas;
extern TDeckGPS       tdeck_gps;
extern TDeckKeyboard  tdeck_kb;
extern TDeckTrackball tdeck_trackball;

// ---- Navigation state (owned by the framework) -------------------------------
extern uint8_t tdeck_screen;
extern uint8_t tdeck_sub;
extern uint8_t tdeck_cursor;
extern uint8_t tdeck_scroll;
extern bool    tdeck_dirty;

// ---- Contact + message stores -------------------------------------------------
extern tdeck_contact_t tdeck_contacts[TDECK_MAX_CONTACTS];
extern uint8_t tdeck_contact_count;
extern tdeck_message_t tdeck_messages[TDECK_MAX_MESSAGES];
extern uint8_t tdeck_msg_count;

// ---- Device settings (persisted to /settings.yaml) ----------------------------
extern char     tdeck_set_device_name[64];
extern uint8_t  tdeck_set_brightness;
extern uint16_t tdeck_set_blank_timeout;
extern bool     tdeck_set_gps_enabled;
extern int16_t  tdeck_set_tz_offset_min;
extern uint16_t tdeck_set_announce_interval;
extern bool     tdeck_set_confirm_send;
extern bool     tdeck_settings_dirty;

// ---- Runtime state -------------------------------------------------------------
extern bool     tdeck_ui_ready;
extern bool     tdeck_sd_ready;
extern bool     tdeck_gps_fix;       // updated from tdeck_gps in the loop
extern bool     tdeck_gps_present;   // set when a GNSS is detected on UART1
extern bool     tdeck_gps_time_set;  // GNSS UTC epoch applied to RNS clock
extern uint32_t tdeck_ui_start_ms;
extern uint32_t tdeck_last_rx_ms;    // millis() of last packet from another node

// ---- Text input state ----------------------------------------------------------
extern char    tdeck_input_buf[TDECK_MSG_MAX_LEN+1];
extern uint8_t tdeck_input_len;
extern uint8_t tdeck_input_ctx;   // 0 = compose, 1 = alias, 2 = node name
extern char    tdeck_input_peer[40];

// ---- Contact detail / thread / toast scratch -----------------------------------
extern uint8_t tdeck_contact_detail_idx;
extern char    tdeck_thread_peer[40];
extern char    tdeck_toast[40];
extern uint32_t tdeck_toast_until;

// ---- ping/trace UI state --------------------------------------------------------
extern bool     tdeck_ping_inflight;
extern uint32_t tdeck_ping_seq;
extern double   tdeck_ping_sent_at;
extern char     tdeck_ping_peer[40];
extern char     tdeck_ping_status[48];
extern uint32_t tdeck_ping_deadline;
extern bool     tdeck_trace_inflight;
extern char     tdeck_trace_peer[40];
extern char     tdeck_trace_out[400];

// ---- RNS integration state -------------------------------------------------------
#ifdef HAS_RNS
namespace RNS {
  // NOTE: HAnnounceHandler is intentionally NOT forward-declared here — it is
  // a type alias (`using HAnnounceHandler = std::shared_ptr<AnnounceHandler>`)
  // in microReticulum's Transport.h, and forward-declaring it as a class
  // conflicts with the alias in translation units that include both this
  // header and microReticulum.h.
  class Destination;
  class Identity;
  class Bytes;
  class Packet;
  class Link;
}
extern RNS::Destination tdeck_rns_msg_dest;
extern RNS::Destination tdeck_rns_ping_dest;
extern RNS::Destination tdeck_rns_bcast_dest;
extern RNS::Destination tdeck_rns_sar_dest;
extern bool     tdeck_rns_ready;
extern double   tdeck_last_announce_at;
extern uint32_t tdeck_last_rx_count;
#endif

// ---- Per-app sub-screens ------------------------------------------------------
// Shared across app modules so apps can navigate into each other's sub-screens
// (e.g. Contacts "Send message" pushes {SCREEN_MESSAGES, MSG_COMPOSE}).
enum { MSG_INBOX = 0, MSG_THREAD, MSG_COMPOSE, MSG_PICK };
enum { CT_LIST = 0, CT_DETAIL, CT_ALIAS };
enum { RET_MENU = 0, RET_PING_PICK, RET_PING_RUN, RET_TRACE_PICK,
       RET_TRACE_RUN, RET_DIRECT, RET_IDENTITY, RET_NAME_EDIT };
enum { SET_LIST = 0, SET_EDIT };
// SAR: map view, peer list, contact picker, SAR config list, value editor
enum { SAR_MAP = 0, SAR_PEERS, SAR_PICK, SAR_CFG, SAR_CFG_EDIT };

// ---- Settings app shared data ---------------------------------------------------
// Row count for the Settings list (apps that reuse the Settings-style value
// editor also reuse tdeck_set_edit_idx; SAR config rows use 20/21/22 to avoid
// colliding with Settings rows 0-14).
#define TDECK_SET_ROWS 15
extern uint8_t tdeck_set_edit_idx;

// Valid SX126x signal bandwidth options (Hz), used by the radio BW editor in
// the Settings app. The array is defined in TDeckUI.h's TU (exactly ten
// entries — keep in sync); the count is a fixed constant because the array is
// an incomplete type (extern) from the app modules' perspective, so sizeof()
// cannot be applied to it.
extern const uint32_t tdeck_bw_options[];
#define TDECK_BW_OPTIONS 10

// ---- Framework API (implemented in TDeckUI.h's TU) -----------------------------

// Debug/log router (KISS-framing aware).
extern void tdeck_ui_debug(const char* msg);

// Drawing helpers ---------------------------------------------------------------
extern void     tdeck_ui_draw_footer(TDeckCanvas& c, const char* hint);
extern void     tdeck_ui_draw_row(TDeckCanvas& c, uint8_t y, const char* text, bool selected);
extern uint8_t  tdeck_ui_list_frame(TDeckCanvas& c, const char* title, uint8_t n,
                                    uint8_t cursor, uint8_t* scroll);
extern void     tdeck_ui_draw_text_input(const char* title, const char* buf);

// Small helpers ------------------------------------------------------------------
extern bool     tdeck_hex_decode(const char* hex, uint8_t* out, size_t n);
extern void     tdeck_name_clean(const char* raw, char* out, size_t n);
extern void     tdeck_ui_time_str(char* out, size_t n);

// Contact + message helpers --------------------------------------------------------
extern tdeck_contact_t* tdeck_contact_find(const char* hash_hex);
extern void     tdeck_contact_upsert(const char* hash_hex, const char* dest_hex,
                                     const char* pubkey_hex, const char* app_data,
                                     double last_seen);
extern const char* tdeck_contact_label(const tdeck_contact_t* c, char* buf, size_t n);
extern void     tdeck_msg_add(const char* peer, const char* text, bool inbound);
extern uint8_t  tdeck_inbox_peer_count();
extern bool     tdeck_inbox_peer_at(uint8_t idx, char* out, size_t n);

// Settings persistence ------------------------------------------------------------
extern bool tdeck_settings_load();
extern bool tdeck_settings_save();
extern bool tdeck_contacts_export();
extern void tdeck_settings_apply_contact(const char* hash, const char* alias,
                                         bool fav, const char* pubkey,
                                         const char* app_data);
extern void tdeck_apply_brightness();

// Text input ------------------------------------------------------------------------
extern void tdeck_input_start(uint8_t ctx, const char* initial);
extern void tdeck_input_add(char c);
extern void tdeck_input_del();
extern void tdeck_input_apply();
extern bool tdeck_in_text_input();

// Toast -----------------------------------------------------------------------------
extern void tdeck_toast_set(const char* fmt, ...);

// Navigation ------------------------------------------------------------------------
extern void tdeck_ui_go_home();
extern void tdeck_ui_nav_push(uint8_t screen, uint8_t sub);
extern void tdeck_ui_nav_back();

// RNS integration (framework owns core destinations + announce) ----------------------
#ifdef HAS_RNS
extern void tdeck_rns_setup();
extern void tdeck_rns_announce_ui();
extern void tdeck_rns_sync_contacts();
extern bool tdeck_rns_send_payload(const tdeck_contact_t* c, const char* aspect,
                                   const RNS::Bytes& payload);
extern bool tdeck_rns_send_broadcast(const char* text);
extern void tdeck_rns_ping_contact(uint8_t idx);
extern void tdeck_rns_ping_poll();
extern void tdeck_rns_trace_contact(uint8_t idx);

// Direct (1-hop) contact helpers (implemented in the framework; used by the
// Reticulum app and the status bar).
extern uint8_t tdeck_direct_count();
extern bool    tdeck_direct_at(uint8_t idx, char* out, size_t n, uint8_t* hops);

// RNS packet-callback dispatch: apps register an aspect handler and the
// framework's destination packet callbacks route incoming payloads to it.
typedef bool (*tdeck_aspect_handler_t)(const RNS::Bytes& data, const RNS::Packet& packet);
extern void tdeck_rns_register_aspect(const char* aspect, tdeck_aspect_handler_t handler);
extern tdeck_aspect_handler_t tdeck_rns_get_handler(const char* aspect);

// Line-based frame field helper (used by the SAR app's beacon parser).
extern const char* tdeck_rns_field(const char* start, uint8_t idx, char* out, size_t n);
#endif

// ---- Application base class ---------------------------------------------------------

// An application on the T-Deck launcher. Every virtual has a default no-op
// except draw(), which the framework calls whenever the app's screen is
// active and the canvas needs repainting.
class TDeckApp {
  public:
    TDeckApp(const char* name, uint8_t icon, uint8_t screen_id, uint8_t entry_sub)
      : _name(name), _icon(icon), _screen_id(screen_id), _entry_sub(entry_sub) {}
    virtual ~TDeckApp() {}

    const char* name() const { return _name; }
    uint8_t icon() const { return _icon; }
    uint8_t screen_id() const { return _screen_id; }
    uint8_t entry_sub() const { return _entry_sub; }

    // ---- Lifecycle ------------------------------------------------------------
    // Called when the app's screen becomes active (launch or return from a
    // framework-level screen like the home launcher). The framework has
    // already reset cursor/scroll.
    virtual void onActivate() {}

    // Called when the app's screen is left for another screen.
    virtual void onDeactivate() {}

    // Paint the current app sub-screen. tdeck_screen == _screen_id.
    virtual void draw() = 0;

    // Called once per framework loop pass while the app is active AND on the
    // home screen (for background services such as SAR beacons / ping
    // polling). Return true to request a repaint.
    virtual bool onLoop() { return false; }

    // ---- Navigation ------------------------------------------------------------
    // Generic navigation events. The framework routes these to whichever app
    // owns tdeck_screen; each app implements the sub-screen-specific logic
    // (moving tdeck_cursor, switching tdeck_sub, pushing/popping nav, etc.).
    virtual void onSelect() {}
    virtual void onUp() {}
    virtual void onDown() {}
    virtual void onBack() {}

    // ---- Input ----------------------------------------------------------------
    // Raw keyboard events routed to the active app. `key` is the printable
    // char or control code (0x0A/0x0D = Enter, 0x08 = Backspace); `state`
    // mirrors the keyboard driver's KB_KEY_STATE_* values. Apps that report
    // inTextInput() handle Enter/Backspace themselves; otherwise the framework
    // translates Enter/Backspace into onSelect()/onBack() before this hook.
    virtual void onKey(char key, uint8_t state) {}

    // Navigation keys (W/A/S/D) BEFORE the framework's default WASD mapping.
    // Return true to claim the key and suppress the default onUp()/onDown()/
    // onSelect()/onBack() translation. Used by apps whose screens redefine
    // the WASD scheme (e.g. the SAR map: IJKL pan, W/S zoom, A/D back/forward).
    // Not called while a text field is active.
    virtual bool onNavKey(char key, uint8_t state) { (void)key; (void)state; return false; }

    // Raw trackball events routed to the active app. The framework translates
    // TB_EVENT_PRESS into onSelect() and TB_EVENT_LEFT into onBack(); apps can
    // additionally interpret TB_EVENT_UP/DOWN/RIGHT for pan/scroll, and always
    // receive the press/left events here too after the framework dispatch.
    virtual void onTrackball(tb_event_t event) {}

    // True while the current app sub-screen is a full-screen text field.
    // The framework routes printable keys to the shared input buffer and
    // Enter to tdeck_input_apply() when any app reports true.
    virtual bool inTextInput() { return false; }

    // Called when text input commits (Enter). `ctx` is the input context id
    // that was passed to tdeck_input_start(). The framework handles ctx 2
    // (node name) itself; ctx 0 (compose) and ctx 1 (contact alias) are
    // dispatched to the active app via this hook.
    virtual void onTextApply(uint8_t ctx) { (void)ctx; }

    // Called when a Settings-style value editor row changes (+1 / -1).
    // Apps that reuse the shared value-editor layout implement this.
    virtual void onValueEdit(int dir) { (void)dir; }

    // ---- RNS ----------------------------------------------------------------
    // Called once after the framework registers its core RNS destinations.
    // Apps override to register additional destinations / aspect handlers.
    virtual void onRnsSetup() {}

    // Called from the framework loop while RNS is ready (active or not).
    // Return true to request a repaint.
    virtual bool onRnsLoop() { return false; }

  protected:
    const char* _name;
    uint8_t     _icon;
    uint8_t     _screen_id;
    uint8_t     _entry_sub;
};

// ---- App registry ---------------------------------------------------------------

// Central registry of all registered apps. The launcher iterates this to
// draw the home screen and dispatch input. Apps self-register via a static
// registrar constructed in their .cpp (see TDeckAppRegistrar). C++17 inline
// static members give us a single cross-TU definition without an out-of-line
// initializer.
#define TDECK_MAX_APPS 8

class TDeckAppRegistry {
  public:
    static void registerApp(TDeckApp* app) {
        if (_count < TDECK_MAX_APPS) {
            _apps[_count++] = app;
        }
    }
    static uint8_t count() { return _count; }
    static TDeckApp* app(uint8_t idx) {
        return (idx < _count) ? _apps[idx] : NULL;
    }
    // Find the app owning the given screen id.
    static TDeckApp* byScreen(uint8_t screen) {
        for (uint8_t i = 0; i < _count; i++) {
            if (_apps[i]->screen_id() == screen) return _apps[i];
        }
        return NULL;
    }
    // The app currently owning tdeck_screen, or NULL on the home screen.
    static TDeckApp* active() {
        return byScreen(tdeck_screen);
    }

  private:
    inline static TDeckApp* _apps[TDECK_MAX_APPS] = {};
    inline static uint8_t _count = 0;
};

// Self-registering helper: `static TDeckAppRegistrar _reg(&myApp);` at file
// scope in an app's .cpp registers the app at static-init time (before
// setup() runs, so tdeck_ui_init() always sees a fully populated registry).
class TDeckAppRegistrar {
  public:
    TDeckAppRegistrar(TDeckApp* app) { TDeckAppRegistry::registerApp(app); }
};