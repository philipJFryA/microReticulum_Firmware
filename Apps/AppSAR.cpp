// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// SAR (Situational Awareness) application implementation.
//
// Self-contained module that owns every piece of the SAR feature: the
// Web Mercator / XYZ slippy-map renderer, the SD-card BMP tile decoder,
// the periodic encrypted position-beacon aspect handler, the beacon peer
// list and the four sub-screens. All SAR state is file-local.

#include "AppSAR.h"

#include <Arduino.h>
#include <SD.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
  #include <esp_heap_caps.h>
#endif

#ifdef HAS_RNS
#include <microReticulum.h>
#endif

// ---- SAR constants -----------------------------------------------------------
#define TDECK_SAR_MAX_PEERS     8
#define TDECK_SAR_MAX_TRACKS    24
#define TDECK_SAR_TRACK_TIMEOUT (60*30)     // seconds; drop tracks not updated
#define TDECK_SAR_DEFAULT_INTERVAL 60       // seconds between beacons
#define TDECK_SAR_TILE_ROOT     "/tiles"
#define TDECK_TILE_SIZE         256
#define TDECK_SAR_MIN_ZOOM      3
#define TDECK_SAR_MAX_ZOOM      18
#define TDECK_SAR_MAP_X         2
#define TDECK_SAR_MAP_Y         (TDECK_SBAR_H + 2)
#define TDECK_SAR_MAP_W         (TDECK_SCREEN_W - 2*TDECK_SAR_MAP_X)
#define TDECK_SAR_MAP_H         (TDECK_SCREEN_H - TDECK_SAR_MAP_Y - TDECK_FOOT_H - 26 - 2)

#define TDECK_SAR_FRAME_POS     "!mrsar!"   // !mrsar!<sender_hash>!<lat>!<lon>!<alt>!<sat>!<age>
#define TDECK_SAR_FRAME_SOS     "!mrsos!"   // !mrsos!<sender_hash>[!<lat>!<lon>]
#define TDECK_SAR_SOS_WINDOW    (60*1000)   // ms; highlight peer markers for 1 minute

// ---- Self-registration -------------------------------------------------------
namespace {
AppSAR s_app;
TDeckAppRegistrar s_reg(&s_app);
} // namespace

// ---- SAR state ---------------------------------------------------------------
static char     s_peers[TDECK_SAR_MAX_PEERS][40];
static uint8_t  s_peer_count   = 0;
static uint16_t s_interval     = TDECK_SAR_DEFAULT_INTERVAL;
static uint8_t  s_send_enabled = 1;

// Map view state (Web Mercator / XYZ).
static int64_t  s_center_lat_e7 = 0;
static int64_t  s_center_lon_e7 = 0;
static uint8_t  s_zoom          = 14;
static int32_t  s_pan_px_x      = 0;
static int32_t  s_pan_px_y      = 0;
static bool     s_has_centre    = false;

// Position tracks (per beacon peer, most recent position).
typedef struct {
  char     hash[40];
  float    lat;
  float    lon;
  double   last_heard;       // RNS unix time
  uint32_t last_heard_ms;    // millis() of last beacon
  uint32_t seq;
} sar_track_t;
static sar_track_t s_tracks[TDECK_SAR_MAX_PEERS];
static uint8_t s_track_count = 0;

// Beacon pacing (millis-based so it works before GPS-disciplined clock).
static uint32_t s_last_beacon_ms = 0;

// SOS state: peers that have sent an SOS recently (millis timestamp so the
// highlight auto-expires  60 seconds after the last SOS frame).
typedef struct {
  char     hash[40];
  uint32_t at_ms;
} sar_sos_t;
static sar_sos_t s_sos[TDECK_SAR_MAX_TRACKS];
static uint8_t  s_sos_count = 0;
static uint32_t s_own_sos_ms = 0;   // local P-press timestamp (own-marker feedback)

// Tile-load debug (shown on the map view bottom strip).
static char    s_debug_path[80] = "";
static bool    s_debug_ok       = false;
static uint8_t s_debug_miss     = 0;

// SAR contact picker scratch.
static uint8_t s_pick_contact_cursor = 0;
static uint8_t s_pick_contact_scroll = 0;

// ============================================================================
// Peer list persistence (small side file kept app-agnostic from settings.yaml)
// ============================================================================

static bool sar_peers_load() {
    if (!tdeck_sd_ready) return false;
    if (!SD.exists("/sar_peers.txt")) return false;
    File f = SD.open("/sar_peers.txt", FILE_READ);
    if (!f) return false;
    s_peer_count = 0;
    char buf[64];
    while (f.available() && s_peer_count < TDECK_SAR_MAX_PEERS) {
        int n = f.readBytesUntil('\n', buf, sizeof(buf)-1);
        if (n <= 0) continue;
        buf[n] = '\0';
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n' || buf[len-1] == ' ')) buf[--len] = '\0';
        if (len == 32) {
            strncpy(s_peers[s_peer_count], buf, sizeof(s_peers[0])-1);
            s_peers[s_peer_count][sizeof(s_peers[0])-1] = '\0';
            s_peer_count++;
        }
    }
    f.close();
    return true;
}

static bool sar_peers_save() {
    if (!tdeck_sd_ready) return false;
    File f = SD.open("/sar_peers.txt", FILE_WRITE);
    if (!f) return false;
    for (uint8_t i = 0; i < s_peer_count; i++) {
        f.printf("%s\n", s_peers[i]);
    }
    f.close();
    return true;
}

// ============================================================================
// SAR helpers
// ============================================================================

static bool sar_active() {
    return s_send_enabled && s_peer_count > 0;
}

static int8_t sar_peer_index(const char* hash) {
    if (!hash) return -1;
    for (uint8_t i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i], hash) == 0) return (int8_t)i;
    }
    return -1;
}

// Record an SOS from the given peer (or refresh an existing one). Expired
// entries are dropped so the 1-minute highlight window is always respected.
static void sar_sos_add(const char* hash) {
    if (!hash || !hash[0]) return;
    uint32_t now = millis();
    for (uint8_t i = 0; i < s_sos_count; i++) {
        if (strcmp(s_sos[i].hash, hash) == 0) {
            s_sos[i].at_ms = now;
            return;
        }
    }
    if (s_sos_count >= TDECK_SAR_MAX_TRACKS) {
        // Replace the oldest entry.
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < s_sos_count; i++) {
            if (s_sos[i].at_ms < s_sos[oldest].at_ms) oldest = i;
        }
        strncpy(s_sos[oldest].hash, hash, sizeof(s_sos[oldest].hash) - 1);
        s_sos[oldest].hash[sizeof(s_sos[oldest].hash) - 1] = '\0';
        s_sos[oldest].at_ms = now;
        return;
    }
    strncpy(s_sos[s_sos_count].hash, hash, sizeof(s_sos[0].hash) - 1);
    s_sos[s_sos_count].hash[sizeof(s_sos[0].hash) - 1] = '\0';
    s_sos[s_sos_count].at_ms = now;
    s_sos_count++;
}

static bool sar_sos_active(const char* hash) {
    if (!hash) return false;
    uint32_t now = millis();
    for (uint8_t i = 0; i < s_sos_count; i++) {
        if (strcmp(s_sos[i].hash, hash) == 0) {
            return (now - s_sos[i].at_ms) < TDECK_SAR_SOS_WINDOW;
        }
    }
    return false;
}

static void sar_sos_expire() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < s_sos_count; ) {
        if ((now - s_sos[i].at_ms) >= TDECK_SAR_SOS_WINDOW) {
            memmove(&s_sos[i], &s_sos[i+1],
                    sizeof(sar_sos_t) * (s_sos_count - i - 1));
            s_sos_count--;
        } else {
            i++;
        }
    }
    if (s_own_sos_ms && (now - s_own_sos_ms) >= TDECK_SAR_SOS_WINDOW) {
        s_own_sos_ms = 0;
    }
}

#ifdef HAS_RNS
static void sar_send_sos();   // defined below with the beacon senders
#endif

// Web Mercator (EPSG:3857) projection helpers for XYZ/slippy map tiles.
static void sar_latlon_to_world_px(double lat, double lon, uint8_t zoom,
                                   double& px, double& py) {
    double n = pow(2.0, (double)zoom);
    double tx = (lon + 180.0) / 360.0 * n;
    double lat_rad = lat * (PI / 180.0);
    double ty = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) / 2.0 * n;
    px = tx * TDECK_TILE_SIZE;
    py = ty * TDECK_TILE_SIZE;
}

// ============================================================================
// SD tile decode helpers (declared here; defined below the map renderer)
// ============================================================================

static bool sar_decode_bmp(const char* path, uint16_t* out);

// ============================================================================
// Drawing (map viewport render + tile decoders + sub-screens)
// ============================================================================

// Decode a 24-bit RGB BMP tile from the SD card into an RGB565 buffer.
// Rows are bottom-up and padded to 4 bytes; each pixel is 3 bytes BGR.
// MapTiler/OSM XYZ tiles converted to BMP by fetch_sar_tiles.py are 256x256.
static bool sar_decode_bmp(const char* path, uint16_t* out) {
    if (!tdeck_sd_ready) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    uint8_t hdr[18];
    if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { f.close(); return false; }
    uint32_t data_offset = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
                           ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    if (data_offset < sizeof(hdr)) data_offset = sizeof(hdr);
    f.seek(data_offset);

    const uint16_t pad = (4 - (TDECK_TILE_SIZE * 3) % 4) % 4;
    uint8_t* row = (uint8_t*)malloc(TDECK_TILE_SIZE * 3 + 3);
    if (!row) { f.close(); return false; }
    for (int32_t yy = TDECK_TILE_SIZE - 1; yy >= 0; yy--) {
        size_t got = f.read(row, TDECK_TILE_SIZE * 3);
        if (got != TDECK_TILE_SIZE * 3) { free(row); f.close(); return false; }
        if (pad) f.read(row + TDECK_TILE_SIZE * 3, pad);
        uint16_t* dst = out + ((int32_t)yy * TDECK_TILE_SIZE);
        for (uint16_t xx = 0; xx < TDECK_TILE_SIZE; xx++) {
            uint8_t b = row[xx * 3 + 0];
            uint8_t g = row[xx * 3 + 1];
            uint8_t r = row[xx * 3 + 2];
            uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            dst[xx] = rgb565;
        }
    }
    free(row);
    f.close();
    return true;
}

// Best 4-character node tag for a peer track: the contact's display name
// (alias, announced name, or a hash fallback) truncated to 4 characters.
static void sar_peer_tag(const char* hash, char* out, size_t n) {
    tdeck_contact_t* cnt = tdeck_contact_find(hash);
    char label[48];
    if (cnt) {
        tdeck_contact_label(cnt, label, sizeof(label));
    } else {
        snprintf(label, sizeof(label), "%s", hash ? hash : "");
    }
    // Trim trailing whitespace, then truncate.
    size_t len = strlen(label);
    while (len > 0 && (label[len-1] == ' ' || label[len-1] == '\t')) label[--len] = '\0';
    size_t max = (n > 0) ? n - 1 : 0;
    if (len > max) len = max;
    memcpy(out, label, len);
    out[len] = '\0';
}

// Render the map viewport onto the main canvas. The viewport shows the tiles
// around the current centre at s_zoom, shifted by the pan offset. Positions
// from the local GPS fix and peer tracks are overlaid.
static void draw_sar_map(TDeckCanvas& c) {
    // If we have no centre yet, fall back to the GPS fix if one exists.
    if (!s_has_centre && tdeck_gps_present && tdeck_gps_fix) {
        s_has_centre = true;
        s_center_lat_e7 = (int64_t)(tdeck_gps.latitude() * 10000000.0);
        s_center_lon_e7 = (int64_t)(tdeck_gps.longitude() * 10000000.0);
        s_pan_px_x = 0;
        s_pan_px_y = 0;
    }

    c.fillRect(TDECK_SAR_MAP_X, TDECK_SAR_MAP_Y, TDECK_SAR_MAP_W, TDECK_SAR_MAP_H, 0x0861);

    if (s_has_centre) {
        double cx_world, cy_world;
        sar_latlon_to_world_px(s_center_lat_e7 / 10000000.0,
                               s_center_lon_e7 / 10000000.0,
                               s_zoom, cx_world, cy_world);
        double vp_x = cx_world - TDECK_SAR_MAP_W / 2.0 + s_pan_px_x;
        double vp_y = cy_world - TDECK_SAR_MAP_H / 2.0 + s_pan_px_y;

        int64_t t0x = (int64_t)floor(vp_x / TDECK_TILE_SIZE);
        int64_t t0y = (int64_t)floor(vp_y / TDECK_TILE_SIZE);
        int64_t t1x = (int64_t)floor((vp_x + TDECK_SAR_MAP_W) / TDECK_TILE_SIZE);
        int64_t t1y = (int64_t)floor((vp_y + TDECK_SAR_MAP_H) / TDECK_TILE_SIZE);

        // Tile cache (PSRAM-backed, reused across frames to avoid SD re-reads).
        static uint16_t* tile_cache = NULL;
        static int64_t   cache_tx = -1, cache_ty = -1;
        static uint8_t   cache_zoom = 0xFF;
        if (!tile_cache) {
          #ifdef ESP_PLATFORM
            tile_cache = (uint16_t*)heap_caps_malloc(
                TDECK_TILE_SIZE * TDECK_TILE_SIZE * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM);
          #endif
            if (!tile_cache) tile_cache = (uint16_t*)malloc(
                TDECK_TILE_SIZE * TDECK_TILE_SIZE * sizeof(uint16_t));
        }

        for (int64_t ty = t0y; ty <= t1y; ty++) {
            for (int64_t tx = t0x; tx <= t1x; tx++) {
                int16_t sx = (int16_t)(tx * TDECK_TILE_SIZE - (int64_t)vp_x);
                int16_t sy = (int16_t)(ty * TDECK_TILE_SIZE - (int64_t)vp_y);
                if (sx + TDECK_TILE_SIZE <= 0 || sx >= TDECK_SAR_MAP_W) continue;
                if (sy + TDECK_TILE_SIZE <= 0 || sy >= TDECK_SAR_MAP_H) continue;

                if (cache_zoom != s_zoom || cache_tx != tx || cache_ty != ty) {
                    cache_zoom = s_zoom;
                    cache_tx = tx;
                    cache_ty = ty;

                    // Tiles are 24-bit BMPs written by fetch_sar_tiles.py.
                    // The debug line always shows the exact file tried.
                    char path[96];
                    bool tile_ok = false;
                    snprintf(path, sizeof(path), "%s/%u/%lld/%lld%s",
                             TDECK_SAR_TILE_ROOT, (unsigned)s_zoom,
                             (long long)tx, (long long)ty, ".bmp");
                    if (SD.exists(path)) {
                        tile_ok = sar_decode_bmp(path, tile_cache);
                    } else {
                        snprintf(path + strlen(path),
                                 sizeof(path) - strlen(path),
                                 "  [tile missing]");
                    }
                    strncpy(s_debug_path, path, sizeof(s_debug_path) - 1);
                    s_debug_path[sizeof(s_debug_path) - 1] = '\0';
                    s_debug_ok = tile_ok;
                    if (!tile_ok) {
                        s_debug_miss++;
                        if (s_debug_miss <= 4) {
                            char dbg[96];
                            snprintf(dbg, sizeof(dbg), "[SAR] tile missing: %s", path);
                            tdeck_ui_debug(dbg);
                        }
                    } else {
                        s_debug_miss = 0;
                    }
                    if (tile_ok) {
                        int16_t clip_x0 = (sx >= 0) ? 0 : -sx;
                        int16_t clip_y0 = (sy >= 0) ? 0 : -sy;
                        int16_t clip_x1 = TDECK_TILE_SIZE;
                        int16_t clip_y1 = TDECK_TILE_SIZE;
                        if (sx + TDECK_TILE_SIZE > TDECK_SAR_MAP_W)
                            clip_x1 = TDECK_SAR_MAP_W - sx;
                        if (sy + TDECK_TILE_SIZE > TDECK_SAR_MAP_H)
                            clip_y1 = TDECK_SAR_MAP_H - sy;
                        if (clip_x1 > clip_x0 && clip_y1 > clip_y0) {
                            for (int16_t yy = clip_y0; yy < clip_y1; yy++) {
                                for (int16_t xx = clip_x0; xx < clip_x1; xx++) {
                                    uint16_t px = tile_cache[(uint16_t)yy * TDECK_TILE_SIZE + (uint16_t)xx];
                                    c.drawPixel(sx + xx, TDECK_SAR_MAP_Y + sy + yy, px);
                                }
                            }
                        }
                    }
                }
            }
        }
    } else {
        c.setTextSize(1);
        c.setTextColor(TDECK_COL_WARN);
        c.setCursor(TDECK_SAR_MAP_X + 8, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H / 2 - 4);
        c.print("No position yet - GPS or first beacon");
    }

    // Draw the local GPS position (blue crosshair + device-name tag).
    if (tdeck_gps_present && tdeck_gps_fix && s_has_centre) {
        double wx, wy;
        sar_latlon_to_world_px(tdeck_gps.latitude(), tdeck_gps.longitude(),
                               s_zoom, wx, wy);
        double cx_world, cy_world;
        sar_latlon_to_world_px(s_center_lat_e7 / 10000000.0,
                               s_center_lon_e7 / 10000000.0,
                               s_zoom, cx_world, cy_world);
        double vp_x = cx_world - TDECK_SAR_MAP_W / 2.0 + s_pan_px_x;
        double vp_y = cy_world - TDECK_SAR_MAP_H / 2.0 + s_pan_px_y;
        int16_t lx = (int16_t)(wx - vp_x);
        int16_t ly = (int16_t)(wy - vp_y);
        if (lx >= 0 && lx < TDECK_SAR_MAP_W && ly >= 0 && ly < TDECK_SAR_MAP_H) {
            c.drawLine(lx - 4, ly, lx + 4, ly, ST77XX_BLUE);
            c.drawLine(lx, ly - 4, lx, ly + 4, ST77XX_BLUE);
            c.fillCircle(lx, ly, 2, ST77XX_WHITE);

            // Red SOS ring around our own position while an SOS is active.
            if (s_own_sos_ms && (millis() - s_own_sos_ms) < TDECK_SAR_SOS_WINDOW) {
                c.drawCircle(lx, ly, 7, TDECK_COL_ERR);
            }

            char tag[5];
            size_t dn_len = strlen(tdeck_set_device_name);
            if (dn_len > 4) dn_len = 4;
            memcpy(tag, tdeck_set_device_name, dn_len);
            tag[dn_len] = '\0';
            c.setTextSize(1);
            c.setTextColor(ST77XX_BLACK);   // dark text contrasts with light map tiles
            c.setCursor(lx + 6, ly - 4);
            c.print(tag);
        }
    }

    // Draw peer tracks (green markers + 4-char name tags, red when stale or SOS).
    for (uint8_t i = 0; i < s_track_count; i++) {
        sar_track_t& tr = s_tracks[i];
        if (!s_has_centre) break;
        double wx, wy;
        sar_latlon_to_world_px(tr.lat, tr.lon, s_zoom, wx, wy);
        double cx_world, cy_world;
        sar_latlon_to_world_px(s_center_lat_e7 / 10000000.0,
                               s_center_lon_e7 / 10000000.0,
                               s_zoom, cx_world, cy_world);
        double vp_x = cx_world - TDECK_SAR_MAP_W / 2.0 + s_pan_px_x;
        double vp_y = cy_world - TDECK_SAR_MAP_H / 2.0 + s_pan_px_y;
        int16_t lx = (int16_t)(wx - vp_x);
        int16_t ly = (int16_t)(wy - vp_y);
        if (lx < -8 || lx >= TDECK_SAR_MAP_W + 8 || ly < -8 || ly >= TDECK_SAR_MAP_H + 8) continue;
        bool sos = sar_sos_active(tr.hash);
        bool fresh = (millis() - tr.last_heard_ms) < 300000UL;  // 5 min
        uint16_t col;
        if (sos) {
            col = TDECK_COL_ERR;                 // SOS overrides everything
        } else {
            col = fresh ? ST77XX_GREEN : TDECK_COL_ERR;
        }
        c.fillCircle(lx, ly, 4, col);
        c.drawCircle(lx, ly, 4, TDECK_COL_FG);
        if (sos) {
            c.drawCircle(lx, ly, 7, TDECK_COL_ERR);   // outer SOS ring
        }

        char tag[5];
        sar_peer_tag(tr.hash, tag, sizeof(tag));
        c.setTextSize(1);
        c.setTextColor(ST77XX_BLACK);   // dark text contrasts with light map tiles
        c.setCursor(lx + 6, ly - 4);
        c.print(tag);
    }

    // Bottom info strip: zoom + peer count + beacon interval.
    c.setTextSize(1);
    c.setTextColor(TDECK_COL_DIM);
    c.setCursor(TDECK_SAR_MAP_X, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H + 4);
    c.printf("Z%d  %u peers", (unsigned)s_zoom, (unsigned)s_peer_count);
    c.setCursor(TDECK_SAR_MAP_X + 100, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H + 4);
    if (s_send_enabled) {
        c.printf("beacon %us", (unsigned)s_interval);
    } else {
        c.print("beacon off");
    }

    // Debug line: last tile path the renderer tried to load.
    if (s_debug_path[0]) {
        c.setTextColor(s_debug_ok ? TDECK_COL_OK : TDECK_COL_ERR);
        c.setCursor(TDECK_SAR_MAP_X, TDECK_SAR_MAP_Y + TDECK_SAR_MAP_H + 14);
        c.print(s_debug_ok ? "OK: " : "MS: ");
        c.print(s_debug_path);
    }
}

void AppSAR::draw() {
    TDeckCanvas& c = tdeck_canvas;
    if (tdeck_sub == SAR_MAP) {
        draw_sar_map(c);
        tdeck_ui_draw_footer(c, "IJKL: pan  W/S: zoom  A/D: back/fwd  P: SOS");
    } else if (tdeck_sub == SAR_PEERS) {
        uint8_t total = s_peer_count + 1;   // "+ Add peer"
        uint8_t rows = tdeck_ui_list_frame(c, "SAR Peers", total, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < total; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[48];
            if (idx == 0) {
                snprintf(line, sizeof(line), "+ Add peer");
            } else {
                tdeck_contact_t* cnt = tdeck_contact_find(s_peers[idx - 1]);
                tdeck_contact_label(cnt, line, sizeof(line));
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: add/remove  L: back");
    } else if (tdeck_sub == SAR_PICK) {
        uint8_t total = tdeck_contact_count;
        uint8_t rows = tdeck_ui_list_frame(c, "Add peer", total,
                                           s_pick_contact_cursor,
                                           &s_pick_contact_scroll);
        for (uint8_t r = 0; r < rows && s_pick_contact_scroll + r < total; r++) {
            uint8_t idx = s_pick_contact_scroll + r;
            char line[48];
            tdeck_contact_label(&tdeck_contacts[idx], line, sizeof(line));
            if (sar_peer_index(tdeck_contacts[idx].hash) >= 0) {
                snprintf(line + strlen(line), sizeof(line) - strlen(line), " [added]");
            }
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line,
                              idx == s_pick_contact_cursor);
        }
        if (total == 0) {
            c.setTextSize(1);
            c.setTextColor(TDECK_COL_WARN);
            c.setCursor(TDECK_LIST_X, TDECK_SBAR_H + 60);
            c.print("No contacts - wait for announces");
        }
        tdeck_ui_draw_footer(c, "select: add peer  L: back");
    } else if (tdeck_sub == SAR_CFG) {
        static const char* items[] = { "Beacon interval", "Send beacons",
                                       "Map zoom", "Recentre here" };
        uint8_t n = 4;
        uint8_t rows = tdeck_ui_list_frame(c, "SAR Config", n, tdeck_cursor, &tdeck_scroll);
        for (uint8_t r = 0; r < rows && tdeck_scroll + r < n; r++) {
            uint8_t idx = tdeck_scroll + r;
            char line[56];
            if (idx == 0)      snprintf(line, sizeof(line), "Beacon interval: %us", (unsigned)s_interval);
            else if (idx == 1) snprintf(line, sizeof(line), "Send beacons: %s", s_send_enabled ? "on" : "off");
            else if (idx == 2) snprintf(line, sizeof(line), "Map zoom: %u", (unsigned)s_zoom);
            else               snprintf(line, sizeof(line), "Recentre here");
            tdeck_ui_draw_row(c, TDECK_SBAR_H + 30 + r * 16, line, idx == tdeck_cursor);
        }
        tdeck_ui_draw_footer(c, "select: change  L: back");
    } else if (tdeck_sub == SAR_CFG_EDIT) {
        // Value editor for the selected config row (rows 20/21/22 so the
        // shared value-editor layout is reused without colliding with the
        // Settings app rows 0-14).
        char label[32], value[40];
        switch (tdeck_set_edit_idx) {
            case 20: snprintf(label, sizeof(label), "Beacon interval"); snprintf(value, sizeof(value), "%us", (unsigned)s_interval); break;
            case 21: snprintf(label, sizeof(label), "Send beacons"); snprintf(value, sizeof(value), "%s", s_send_enabled ? "on" : "off"); break;
            case 22: snprintf(label, sizeof(label), "Map zoom"); snprintf(value, sizeof(value), "%u", (unsigned)s_zoom); break;
            default: snprintf(label, sizeof(label), ""); value[0] = '\0'; break;
        }
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
        tdeck_ui_draw_footer(c, "SAR config");
    }
}

// ============================================================================
// Navigation / input
// ============================================================================

void AppSAR::onActivate() {
    // Restore the persisted beacon-peer list and interval on each entry so
    // runtime changes survive cycling through the app / reboots.
    sar_peers_load();
    tdeck_dirty = true;
}

void AppSAR::onSelect() {
    if (tdeck_sub == SAR_MAP) {
        tdeck_ui_nav_push(SCREEN_SAR, SAR_PEERS);
    } else if (tdeck_sub == SAR_PEERS) {
        if (tdeck_cursor == 0) {
            if (tdeck_contact_count == 0) {
                tdeck_toast_set("No contacts - wait for announces");
            } else {
                s_pick_contact_cursor = 0;
                s_pick_contact_scroll = 0;
                tdeck_ui_nav_push(SCREEN_SAR, SAR_PICK);
            }
        } else if (tdeck_cursor - 1 < s_peer_count) {
            uint8_t idx = tdeck_cursor - 1;
            memmove(&s_peers[idx], &s_peers[idx + 1],
                    sizeof(s_peers[0]) * (s_peer_count - idx - 1));
            s_peer_count--;
            tdeck_settings_dirty = true;
            sar_peers_save();
            tdeck_toast_set("Peer removed");
        }
    } else if (tdeck_sub == SAR_PICK) {
        if (s_pick_contact_cursor < tdeck_contact_count) {
            tdeck_contact_t* c = &tdeck_contacts[s_pick_contact_cursor];
            if (sar_peer_index(c->hash) >= 0) {
                tdeck_toast_set("Peer already added");
            } else if (s_peer_count >= TDECK_SAR_MAX_PEERS) {
                tdeck_toast_set("Max %u peers", TDECK_SAR_MAX_PEERS);
            } else {
                strncpy(s_peers[s_peer_count], c->hash, sizeof(s_peers[0]) - 1);
                s_peers[s_peer_count][sizeof(s_peers[0]) - 1] = '\0';
                s_peer_count++;
                tdeck_settings_dirty = true;
                sar_peers_save();
                tdeck_toast_set("Peer added");
                tdeck_ui_nav_back();
            }
        }
    } else if (tdeck_sub == SAR_CFG) {
        switch (tdeck_cursor) {
            case 0: tdeck_set_edit_idx = 20;
                    tdeck_ui_nav_push(SCREEN_SAR, SAR_CFG_EDIT); break;
            case 1: tdeck_set_edit_idx = 21;
                    tdeck_ui_nav_push(SCREEN_SAR, SAR_CFG_EDIT); break;
            case 2: tdeck_set_edit_idx = 22;
                    tdeck_ui_nav_push(SCREEN_SAR, SAR_CFG_EDIT); break;
            case 3:
                if (tdeck_gps_present && tdeck_gps_fix) {
                    s_center_lat_e7 = (int64_t)(tdeck_gps.latitude() * 10000000.0);
                    s_center_lon_e7 = (int64_t)(tdeck_gps.longitude() * 10000000.0);
                    s_pan_px_x = 0;
                    s_pan_px_y = 0;
                    s_has_centre = true;
                    tdeck_toast_set("Centred");
                } else {
                    tdeck_toast_set("No GPS fix");
                }
                break;
        }
    } else if (tdeck_sub == SAR_CFG_EDIT) {
        tdeck_settings_dirty = true;
        tdeck_ui_nav_back();
    }
    tdeck_dirty = true;
}

void AppSAR::onValueEdit(int dir) {
    switch (tdeck_set_edit_idx) {
        case 20: {
            int v = (int)s_interval + dir * 5;
            s_interval = (uint16_t)constrain(v, 10, 3600);
            tdeck_settings_dirty = true;
            break;
        }
        case 21:
            s_send_enabled = !s_send_enabled;
            tdeck_settings_dirty = true;
            break;
        case 22: {
            int v = (int)s_zoom + dir;
            s_zoom = (uint8_t)constrain(v, TDECK_SAR_MIN_ZOOM, TDECK_SAR_MAX_ZOOM);
            break;
        }
        default: break;
    }
    tdeck_dirty = true;
}

void AppSAR::onUp() {
    switch (tdeck_sub) {
        case SAR_MAP:   s_pan_px_y -= 24; break;
        case SAR_PEERS: if (tdeck_cursor > 0) tdeck_cursor--; break;
        case SAR_CFG:   if (tdeck_cursor > 0) tdeck_cursor--; break;
        case SAR_CFG_EDIT: onValueEdit(+1); break;
        default: break;
    }
    tdeck_dirty = true;
}

void AppSAR::onDown() {
    switch (tdeck_sub) {
        case SAR_MAP:   s_pan_px_y += 24; break;
        case SAR_PEERS: if (tdeck_cursor + 1 < s_peer_count + 1) tdeck_cursor++; break;
        case SAR_PICK:
            s_pick_contact_cursor++;
            if (s_pick_contact_cursor >= tdeck_contact_count) s_pick_contact_cursor = tdeck_contact_count - 1;
            break;
        case SAR_CFG:   if (tdeck_cursor + 1 < 4) tdeck_cursor++; break;
        case SAR_CFG_EDIT: onValueEdit(-1); break;
        default: break;
    }
    tdeck_dirty = true;
}

void AppSAR::onBack() {
    tdeck_ui_nav_back();
    tdeck_dirty = true;
}

void AppSAR::onKey(char key, uint8_t state) {
    // The WASD navigation keys (W/S zoom, A/D back/forward) are claimed by
    // onNavKey() on the map view; the raw keys arriving here are the IJKL
    // viewport pan keys (and any other printable keys on non-map screens).
    if (tdeck_sub == SAR_MAP) {
        switch (key) {
            case 'i': case 'I':   // pan up
                s_pan_px_y -= 24;
                tdeck_dirty = true;
                break;
            case 'j': case 'J':   // pan left
                s_pan_px_x -= 24;
                tdeck_dirty = true;
                break;
            case 'k': case 'K':   // pan down
                s_pan_px_y += 24;
                tdeck_dirty = true;
                break;
            case 'l': case 'L':   // pan right
                s_pan_px_x += 24;
                tdeck_dirty = true;
                break;
            case 'p': case 'P':   // send SOS
              #ifdef HAS_RNS
                sar_send_sos();
              #else
                tdeck_toast_set("RNS not available");
              #endif
                tdeck_dirty = true;
                break;
            default:
                (void)state;
                break;
        }
        return;
    }
    (void)state;
}

bool AppSAR::onNavKey(char key, uint8_t state) {
    // On the map view W/S become zoom-in/out instead of the framework's
    // onUp()/onDown() pan. A/D are left to the framework (back/forward).
    if (tdeck_sub == SAR_MAP) {
        switch (key) {
            case 'w': case 'W':
                if (s_zoom < TDECK_SAR_MAX_ZOOM) s_zoom++;
                tdeck_dirty = true;
                return true;
            case 's': case 'S':
                if (s_zoom > TDECK_SAR_MIN_ZOOM) s_zoom--;
                tdeck_dirty = true;
                return true;
            default:
                break;
        }
    }
    (void)state;
    return false;
}

void AppSAR::onTrackball(tb_event_t event) {
    switch (event) {
        case TB_EVENT_UP:   if (tdeck_sub == SAR_MAP) { s_pan_px_y -= 24; } else { onUp(); } break;
        case TB_EVENT_DOWN: if (tdeck_sub == SAR_MAP) { s_pan_px_y += 24; } else { onDown(); } break;
        case TB_EVENT_LEFT: if (tdeck_sub == SAR_MAP) { s_pan_px_x -= 24; } else { onBack(); } break;
        case TB_EVENT_RIGHT:if (tdeck_sub == SAR_MAP) { s_pan_px_x += 24; } else { onSelect(); } break;
        case TB_EVENT_PRESS: onSelect(); break;
        default: break;
    }
    tdeck_dirty = true;
}

// ============================================================================
// Background service: beacon pacing + stale-track expiry
// ============================================================================

static void sar_expire_tracks() {
    for (uint8_t i = 0; i < s_track_count; ) {
        if (millis() - s_tracks[i].last_heard_ms > (uint32_t)TDECK_SAR_TRACK_TIMEOUT * 1000) {
            memmove(&s_tracks[i], &s_tracks[i+1],
                    sizeof(sar_track_t) * (s_track_count - i - 1));
            s_track_count--;
        } else {
            i++;
        }
    }
}

#ifdef HAS_RNS
static void sar_send_beacon() {
    if (!tdeck_rns_ready) return;
    if (!sar_active()) return;
    if (!tdeck_gps_present || !tdeck_gps_fix) return;
    float lat = tdeck_gps.latitude();
    float lon = tdeck_gps.longitude();
    if (lat == 0.0f && lon == 0.0f) return;

    const RNS::Identity& id = RNS::Transport::identity();
    if (!id) return;

    char frame[160];
    uint32_t age = (millis() - tdeck_gps.lastFixMs()) / 1000;
    snprintf(frame, sizeof(frame), "%s%s!%.5f!%.5f!%.0f!%u!%lu",
             TDECK_SAR_FRAME_POS, id.hash().toHex().c_str(),
             (double)lat, (double)lon, (double)tdeck_gps.altitude(),
             (unsigned)tdeck_gps.satellites(), (unsigned long)age);

    RNS::Bytes payload(frame);
    uint8_t sent = 0;
    for (uint8_t i = 0; i < s_peer_count; i++) {
        tdeck_contact_t* c = tdeck_contact_find(s_peers[i]);
        if (c && tdeck_rns_send_payload(c, TDECK_SAR_RNS_ASPECT, payload)) sent++;
    }
    if (sent > 0) s_last_beacon_ms = millis();
}

// Send an SOS to every configured SAR peer. The frame carries the sender's
// identity hash and, when a GPS fix is available, the current position.
static void sar_send_sos() {
    if (!tdeck_rns_ready) {
        tdeck_toast_set("RNS not ready");
        return;
    }
    if (s_peer_count == 0) {
        tdeck_toast_set("No SAR peers");
        return;
    }

    const RNS::Identity& id = RNS::Transport::identity();
    if (!id) return;

    char frame[120];
    if (tdeck_gps_present && tdeck_gps_fix) {
        snprintf(frame, sizeof(frame), "%s%s!%.5f!%.5f",
                 TDECK_SAR_FRAME_SOS, id.hash().toHex().c_str(),
                 (double)tdeck_gps.latitude(), (double)tdeck_gps.longitude());
    } else {
        snprintf(frame, sizeof(frame), "%s%s",
                 TDECK_SAR_FRAME_SOS, id.hash().toHex().c_str());
    }

    RNS::Bytes payload(frame);
    uint8_t sent = 0;
    for (uint8_t i = 0; i < s_peer_count; i++) {
        tdeck_contact_t* c = tdeck_contact_find(s_peers[i]);
        if (c && tdeck_rns_send_payload(c, TDECK_SAR_RNS_ASPECT, payload)) sent++;
    }
    if (sent > 0) {
        s_own_sos_ms = millis();
        tdeck_toast_set("SOS sent");
    } else {
        tdeck_toast_set("SOS send failed");
    }
}

static bool on_sar_position(const RNS::Bytes& data, const RNS::Packet& packet) {
    (void)packet;
    std::string s = data.toString();

    // SOS frame: !mrsos!<sender_hash>[!<lat>!<lon>]
    if (s.rfind(TDECK_SAR_FRAME_SOS, 0) == 0) {
        const char* p = s.c_str() + strlen(TDECK_SAR_FRAME_SOS);
        char sender[40];
        p = tdeck_rns_field(p, 0, sender, sizeof(sender));
        if (!p || strlen(sender) != 32) return true;

        // Record the SOS for the 1-minute highlight.
        sar_sos_add(sender);

        // Attach a position if one was included.
        char lat_s[24], lon_s[24];
        const char* latp = tdeck_rns_field(p, 0, lat_s, sizeof(lat_s));
        const char* lonp = latp ? tdeck_rns_field(latp, 0, lon_s, sizeof(lon_s)) : NULL;
        if (latp && lonp) {
            float lat = strtof(lat_s, NULL);
            float lon = strtof(lon_s, NULL);
            if (lat != 0.0f || lon != 0.0f) {
                tdeck_contact_upsert(sender, NULL, NULL, NULL, 0);

                sar_track_t* tr = NULL;
                for (uint8_t i = 0; i < s_track_count; i++) {
                    if (strcmp(s_tracks[i].hash, sender) == 0) { tr = &s_tracks[i]; break; }
                }
                if (!tr) {
                    if (s_track_count < TDECK_SAR_MAX_PEERS) {
                        tr = &s_tracks[s_track_count++];
                        memset(tr, 0, sizeof(*tr));
                        strncpy(tr->hash, sender, sizeof(tr->hash)-1);
                    }
                }
                if (tr) {
                    tr->lat = lat;
                    tr->lon = lon;
                    tr->last_heard = RNS::Utilities::OS::time();
                    tr->last_heard_ms = millis();
                }

                if (!s_has_centre) {
                    s_has_centre = true;
                    s_center_lat_e7 = (int64_t)(lat * 10000000.0);
                    s_center_lon_e7 = (int64_t)(lon * 10000000.0);
                    s_pan_px_x = 0;
                    s_pan_px_y = 0;
                }
            }
        }
        tdeck_dirty = true;
        return true;
    }

    // Position beacon frame.
    if (s.rfind(TDECK_SAR_FRAME_POS, 0) != 0) return true;
    const char* p = s.c_str() + strlen(TDECK_SAR_FRAME_POS);

    char sender[40];
    p = tdeck_rns_field(p, 0, sender, sizeof(sender));
    if (!p || strlen(sender) != 32) return true;

    char lat_s[24], lon_s[24], alt_s[24], sat_s[16], age_s[24];
    p = tdeck_rns_field(p, 0, lat_s, sizeof(lat_s));
    if (!p) return true;
    p = tdeck_rns_field(p, 0, lon_s, sizeof(lon_s));
    if (!p) return true;
    p = tdeck_rns_field(p, 0, alt_s, sizeof(alt_s));
    if (!p) return true;
    p = tdeck_rns_field(p, 0, sat_s, sizeof(sat_s));
    if (!p) return true;
    tdeck_rns_field(p, 0, age_s, sizeof(age_s));

    float lat = strtof(lat_s, NULL);
    float lon = strtof(lon_s, NULL);
    if (lat == 0.0f && lon == 0.0f) return true;

    tdeck_contact_upsert(sender, NULL, NULL, NULL, 0);

    sar_track_t* tr = NULL;
    for (uint8_t i = 0; i < s_track_count; i++) {
        if (strcmp(s_tracks[i].hash, sender) == 0) { tr = &s_tracks[i]; break; }
    }
    if (!tr) {
        if (s_track_count >= TDECK_SAR_MAX_PEERS) return true;
        tr = &s_tracks[s_track_count++];
        memset(tr, 0, sizeof(*tr));
        strncpy(tr->hash, sender, sizeof(tr->hash)-1);
    }
    tr->lat = lat;
    tr->lon = lon;
    tr->last_heard = RNS::Utilities::OS::time();
    tr->last_heard_ms = millis();

    if (!s_has_centre) {
        s_has_centre = true;
        s_center_lat_e7 = (int64_t)(lat * 10000000.0);
        s_center_lon_e7 = (int64_t)(lon * 10000000.0);
        s_pan_px_x = 0;
        s_pan_px_y = 0;
    }
    tdeck_dirty = true;
    return true;
}
#endif // HAS_RNS

#ifdef HAS_RNS
void AppSAR::onRnsSetup() {
    tdeck_rns_register_aspect(TDECK_SAR_RNS_ASPECT, on_sar_position);
}
#endif

bool AppSAR::onLoop() {
    sar_expire_tracks();
    sar_sos_expire();
    return false;
}

#ifdef HAS_RNS
bool AppSAR::onRnsLoop() {
    if (s_interval > 0 &&
        (millis() - s_last_beacon_ms >= (uint32_t)s_interval * 1000)) {
        sar_send_beacon();
    }
    return false;
}
#endif