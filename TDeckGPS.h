// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// NMEA-0183 GPS/GNSS driver for the LilyGO T-Deck.
//
// The T-Deck's GPS shield (or an equivalent external GNSS module wired to
// the Gover interface) connects to the ESP32-S3's UART1 on GPIO 43 (TX,
// module RX) and GPIO 44 (RX, module TX) at 9600 baud — see LilyGO's
// official GPSShield example:
//   https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/examples/GPSShield
//
// The driver is deliberately self-contained (no TinyGPS++ dependency) and
// parses just the two NMEA sentence types it needs, matching the location
// and timing precision of the rest of this repository:
//
//   * $GPGGA / $GNGGA — fix quality, position, altitude, satellite count
//   * $GPRMC / $GNRMC — UTC date/time + validity, ground speed, course
//
// When a valid fix with a plausible UTC date/time is observed, the driver
// exposes the GNSS epoch via epochReady()/gpsEpoch(); the UI layer (which
// owns the RNS instance) applies it to the Reticulum clock exactly once.
//
// The singleton instance is defined exactly once by defining
// TDECK_GPS_IMPLEMENTATION in a single translation unit (TDeckUI.h does
// this), mirroring TDeckTrackball.h.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef PIN_GPS_TX
  #define PIN_GPS_TX 43
#endif
#ifndef PIN_GPS_RX
  #define PIN_GPS_RX 44
#endif
#ifndef GPS_BAUD_RATE
  #define GPS_BAUD_RATE 9600
#endif

// Seconds a fix must remain up-to-date before the UI shows "GPS --" again.
#ifndef TDECK_GPS_FIX_TIMEOUT_S
  #define TDECK_GPS_FIX_TIMEOUT_S 30
#endif

// When this header is included with TDECK_GPS_IMPLEMENTATION (TDeckUI.h
// does this), driver diagnostics route through the TU-local debug router so
// GPS logs honour KISS framing exactly like the rest of the UI logs.
#ifdef TDECK_GPS_IMPLEMENTATION
  static void tdeck_ui_debug(const char* msg);
#endif

class TDeckGPS {
  public:
    TDeckGPS() : _has_fix(false), _line_len(0), _satellites(0),
                 _last_fix_ms(0), _lat(0.0f), _lon(0.0f), _alt(0.0f),
                 _speed_kmh(0.0f), _course(0.0f),
                 _epoch_valid(false), _epoch_s(0), _epoch_fresh_ms(0),
                 _nmea_seen(false), _debug_last_fix(false), _baud(9600) {}

    void begin() {
      // UART1 (RX from GPS on GPIO 44, TX to GPS on GPIO 43).
      // SERIAL_8N1 matches the L76K / u-blox default NMEA output.
      _open(GPS_BAUD_RATE);
      _line_len = 0;
      char dbg[64];
      snprintf(dbg, sizeof(dbg), "[GPS] UART1 opened (TX=%d RX=%d baud=%d 8N1)",
               (int)PIN_GPS_TX, (int)PIN_GPS_RX, (int)_baud);
      _debug(dbg);

      if (_init_l76k()) {
        _debug("[GPS] L76K detected and configured (GPS+GLONASS, NMEA out)");
        return;
      }

      // The module did not answer $PCAS at 9600 baud. The typical cause is an
      // L76K whose configuration was previously persisted at 38400 baud (a
      // common leftover from LilyGO's own recovery sketches): at that baud
      // the receiver answers UBX binary, not NMEA prose. Run the exact UBX
      // CFG-CLR recovery from GPSShield.ino's GPS_Recovery() at 38400 to
      // wipe flash+BBR+startup config back to factory defaults.
      _debug("[GPS] L76K probe failed at 9600, trying UBX recovery @ 38400 baud");
      _open(38400);
      if (_ubx_recover()) {
        // Mirrors the official GPSShield.ino: after a successful UBX
        // recovery the receiver's effective operating baud is 38400 and the
        // host UART stays at 38400 — the sketch only drops to 9600 as a
        // retry when the 38400 recovery FAILS (and re-applies 38400 on
        // success there too). The module now emits standard GGA/RMC NMEA on
        // this same UART, so the driver's polling loop picks it straight up.
        _debug("[GPS] UBX recovery succeeded, staying at 38400 baud (NMEA out)");
        return;
      }

      // Final fallback: back to the default baud with no configuration —
      // some receivers already emit GGA/RMC at 9600 on power-up.
      _debug("[GPS] module type not detected, returning to 9600 baud default");
      _open(GPS_BAUD_RATE);
    }

    // Current UART baud used by the active GPS configuration.
    uint32_t baud() const { return _baud; }
    // -------- bootstrap helpers --------

    // (Re)open the GPS UART at the given baud.
    void _open(uint32_t baud) {
      Serial1.end();
      delay(20);
      Serial1.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
      _baud = baud;
      // Drain stale bytes from the previous configuration.
      uint32_t drain_at = millis() + 50;
      while ((int32_t)(millis() - drain_at) < 0) {
        if (Serial1.available()) Serial1.read();
      }
      _line_len = 0;
    }

    // Read up to `timeout_ms` for the subscription prefix, returning true
    // if `needle` is seen (used to detect the L76K's $GPTXT version reply).
    bool _await(const char* needle, uint32_t timeout_ms) {
      char acc[64];
      uint8_t n = 0;
      size_t need_len = strlen(needle);
      uint32_t until = millis() + timeout_ms;
      while ((int32_t)(millis() - until) < 0) {
        while (Serial1.available() > 0) {
          char c = (char)Serial1.read();
          _nmea_seen = true;
          if (c == '\n') {
            acc[n] = '\0';
            // Compare the accumulated line with the needle.
            if (n >= need_len && strncmp(acc, needle, need_len) == 0) return true;
            n = 0;
          } else if (c == '\r') {
            // drop
          } else if (n < sizeof(acc) - 1) {
            acc[n++] = c;
          } else {
            n = 0;
          }
        }
      }
      return false;
    }

    // Probe + configure an L76K (or other $PCAS-capable receiver).
    // Returns true if the L76K version string was observed.
    bool _init_l76k() {
      // 1. Stop NMEA output so the configuration commands answer cleanly.
      Serial1.print("$PCAS03,0,0,0,0,0,0,0,0,0,0,,,0,0*02\r\n");
      delay(200);
      // 2. Ask for the version string; the L76K answers "$GPTXT,01,01,xx".
      //    match any revision byte (the official example only accepts
      //    "...02", but other firmware revisions reply 03/04/...).
      Serial1.print("$PCAS06,0*1B\r\n");
      if (!_await("$GPTXT,01,01,", 2000)) return false;
      delay(200);

      // 3. GPS + GLONASS constellation.
      Serial1.print("$PCAS04,5*1C\r\n");
      delay(250);
      // 4. Re-enable the standard NMEA sentence set (GGA, GLL, GSA, GSV,
      //    RMC, VTG, ZDA) so the driver's GGA/RMC parser sees data.
      Serial1.print("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*02\r\n");
      delay(250);
      // 5. Vehicle mode (matches SoftRF's published L76K configuration).
      Serial1.print("$PCAS11,3*1E\r\n");
      delay(100);
      return true;
    }

    // Wait for a UBX ACK-ACK frame (class 0x05, id 0x01). Matches the
    // official getAck(): the pair "header + class 0x05 + id 0x01 + 2-byte
    // length" is sufficient — ACK-ACK always carries a 2-byte payload (the
    // acknowledged message's class/id) so we accept once those arrive.
    bool _await_ubx_ack(uint32_t timeout_ms) {
      uint8_t st = 0;
      uint8_t len_lo = 0, len_hi = 0;
      uint32_t until = millis() + timeout_ms;
      while ((int32_t)(millis() - until) < 0) {
        while (Serial1.available() > 0) {
          uint8_t c = (uint8_t)Serial1.read();
          _nmea_seen = true;
          switch (st) {
            case 0: if (c == 0xB5) st = 1; break;
            case 1: if (c == 0x62) st = 2; else st = (c == 0xB5) ? 1 : 0; break;
            case 2: if (c == 0x05) st = 3; else st = (c == 0xB5) ? 1 : 0; break;
            case 3: if (c == 0x01) st = 4; else st = (c == 0xB5) ? 1 : 0; break;
            case 4: len_lo = c; st = 5; break;
            case 5:
              len_hi = c;
              // Consume the payload bytes (the length recorded in the frame)
              // and optionally the two CK bytes, then accept.
              {
                uint16_t plen = (uint16_t)len_lo | ((uint16_t)len_hi << 8);
                uint32_t done_at = millis() + 50;
                while (plen > 0 && (int32_t)(millis() - done_at) < 0) {
                  if (Serial1.available()) {
                    Serial1.read();
                    plen--;
                  }
                }
              }
              return true;
            default: st = 0; break;
          }
        }
      }
      return false;
    }

    // Recover a receiver whose persisted configuration (stored in flash /
    // battery-backed RAM) was left at a non-default baud or NMEA mode. This
    // is the exact UBX payload sequence from LilyGO's GPSShield.ino
    // GPS_Recovery(): three CFG-CLR frames clearing flash, battery-backed and
    // start-up configurations, then a 1 Hz CFG-RATE. Returns true when the
    // receiver acknowledges with an UBX ACK-ACK.
    bool _ubx_recover() {
      // UBX-CFG-CLR: clear flash config (layer 0x02).
      static const uint8_t cfg_clear1[] = {
        0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x1C, 0xA2
      };
      // UBX-CFG-CLR: clear battery-backed config (layer 0x01).
      static const uint8_t cfg_clear2[] = {
        0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x1B, 0xA1
      };
      // UBX-CFG-CLR: clear start-up config (layer 0x03 with all masks).
      static const uint8_t cfg_clear3[] = {
        0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
        0x03, 0x1D, 0xB3
      };

      Serial1.write(cfg_clear1, sizeof(cfg_clear1));
      _await_ubx_ack(800);
      Serial1.write(cfg_clear2, sizeof(cfg_clear2));
      _await_ubx_ack(800);
      Serial1.write(cfg_clear3, sizeof(cfg_clear3));
      _await_ubx_ack(800);

      // UBX-CFG-RATE (length 0): 1 Hz navigation/measurement rate.
      static const uint8_t cfg_rate[] = { 0xB5, 0x62, 0x06, 0x08, 0x00, 0x00, 0x0E, 0x30 };
      Serial1.write(cfg_rate, sizeof(cfg_rate));
      return _await_ubx_ack(800);
    }

    // Call from the UI loop (or any polling loop) to consume and parse
    // whatever NMEA data has arrived since the last poll.
    void loop() {
      while (Serial1.available() > 0) {
        _nmea_seen = true;
        char c = (char)Serial1.read();
        if (c == '\n') {
          _line[_line_len] = '\0';
          _parse_line(_line, _line_len);
          _line_len = 0;
        } else if (c == '\r') {
          // NMEA lines end with CRLF; the LF above closes the line, so the
          // CR carries no data and is dropped.
        } else if (_line_len < (uint8_t)(sizeof(_line) - 1)) {
          _line[_line_len++] = c;
        } else {
          // Overrun: drop the line and resync on the next '\n'.
          _line_len = 0;
        }
      }

      // A GPS that was previously fixed but has gone stale (or the module
      // was unplugged) reverts to no-fix after TDECK_GPS_FIX_TIMEOUT_S.
      if (_has_fix && (millis() - _last_fix_ms) > (uint32_t)TDECK_GPS_FIX_TIMEOUT_S * 1000) {
        _has_fix = false;
      }
      // Likewise, a GPS time/date observed too long ago is no longer fresh
      // enough for the UI to apply to the RNS clock.
      if (_epoch_valid && (millis() - _epoch_fresh_ms) > (uint32_t)TDECK_GPS_FIX_TIMEOUT_S * 1000) {
        _epoch_valid = false;
      }

      // Debug logging on fix state transitions (avoid 1 Hz sentence spam).
      bool fix_now = hasFix();
      if (fix_now != _debug_last_fix) {
        _debug_last_fix = fix_now;
        if (fix_now) {
          char dbg[96];
          snprintf(dbg, sizeof(dbg),
                   "[GPS] fix acquired (lat=%.5f lon=%.5f alt=%.0f m, sats=%u)",
                   (double)_lat, (double)_lon, (double)_alt, _satellites);
          _debug(dbg);
        } else {
          _debug("[GPS] fix lost");
        }
      }
    }

    bool hasFix() const {
      return _has_fix && (millis() - _last_fix_ms) <= (uint32_t)TDECK_GPS_FIX_TIMEOUT_S * 1000;
    }
    uint8_t satellites() const { return _satellites; }
    float latitude() const  { return _lat; }
    float longitude() const { return _lon; }
    float altitude() const  { return _alt; }
    float speedKmh() const  { return _speed_kmh; }
    float course() const    { return _course; }

    // True once any NMEA traffic has been received since boot — used by the
    // UI to switch the status indicator from "n/a" to "--"/"3D" when an
    // actual GPS shield is attached to the Gover interface.
    bool dataSeen() const { return _nmea_seen; }

    // A valid, recent UTC epoch is available (from a $GxRMC sentence).
    bool epochReady() const {
      return _epoch_valid && (millis() - _epoch_fresh_ms) <= (uint32_t)TDECK_GPS_FIX_TIMEOUT_S * 1000;
    }

    // Unix epoch (seconds) of the current (or last observed) GPS time.
    uint64_t gpsEpoch() const { return _epoch_s; }

    // Millisecond timestamp of when the current fix was last updated
    // (used by the announce builder to compute fix age).
    uint32_t lastFixMs() const { return _last_fix_ms; }

  private:
    char     _line[96];
    uint8_t  _line_len;
    bool     _has_fix;
    uint8_t  _satellites;
    uint32_t _last_fix_ms;
    float    _lat;
    float    _lon;
    float    _alt;
    float    _speed_kmh;
    float    _course;
    bool     _epoch_valid;
    uint64_t _epoch_s;
    uint32_t _epoch_fresh_ms;
    bool     _nmea_seen;
    bool     _debug_last_fix;
    uint32_t _baud;

    // -------- tiny helpers (no String heap inside the fast path) --------

    // Route a driver diagnostic through the UI debug router when compiled
    // into the T-Deck firmware (KISS-framing aware); otherwise print raw.
    static void _debug(const char* msg) {
#ifdef TDECK_GPS_IMPLEMENTATION
      tdeck_ui_debug(msg);
#else
      Serial.println(msg);
#endif
    }

    static bool _is_valid_checksum(const char* line, uint8_t len) {
      // NMEA checksum: XOR of all bytes between '$' and '*'.
      uint8_t sum = 0;
      uint8_t i = 1; // skip '$'
      for (; i < len && line[i] != '*'; i++) {
        sum ^= (uint8_t)line[i];
      }
      if (i >= len || line[i] != '*') return false;
      if (i + 2 >= len) return false;
      char hi = line[i+1], lo = line[i+2];
      uint8_t hv, lv;
      if (hi >= '0' && hi <= '9') hv = hi - '0';
      else if (hi >= 'A' && hi <= 'F') hv = hi - 'A' + 10;
      else return false;
      if (lo >= '0' && lo <= '9') lv = lo - '0';
      else if (lo >= 'A' && lo <= 'F') lv = lo - 'A' + 10;
      else return false;
      return (uint8_t)((hv << 4) | lv) == sum;
    }

    // Returns pointer to the idx-th comma-separated field of the sentence
    // (field 0 is the talker/type token after '$'), or NULL if absent.
    static const char* _field(const char* p, uint8_t idx, uint8_t* len_out) {
      uint8_t f = 0;
      while (true) {
        const char* next = strchr(p, ',');
        if (f == idx) {
          *len_out = next ? (uint8_t)(next - p) : (uint8_t)strlen(p);
          return p;
        }
        if (!next) return NULL;
        p = next + 1;
        f++;
      }
    }

    // Parse a NMEA coordinate "ddmm.mmmm" or "dddmm.mmmm" into decimal
    // degrees (hemisphere applied by the caller).
    static float _parse_coord(const char* s, uint8_t len) {
      if (!s || len < 4) return 0.0f;
      const char* dot = NULL;
      for (uint8_t i = 0; i < len; i++) {
        if (s[i] == '.') { dot = s + i; break; }
      }
      if (!dot) return 0.0f;
      uint8_t int_len = (uint8_t)(dot - s);
      if (int_len < 2) return 0.0f;

      // NMEA integer part is dddmm (longitude) or ddmm (latitude); the last
      // two integer digits are minutes, everything before them is degrees.
      uint8_t min_start = (int_len > 2) ? (uint8_t)(int_len - 2) : int_len;

      char deg_buf[8];
      uint8_t di = 0;
      for (uint8_t i = 0; i < min_start && i < 3; i++) deg_buf[di++] = s[i];
      deg_buf[di] = '\0';

      char min_buf[16];
      uint8_t mi = 0;
      for (uint8_t i = min_start; i < int_len; i++) min_buf[mi++] = s[i];
      min_buf[mi++] = '.';
      const char* frac = dot + 1;
      while (*frac && *frac != ',' && *frac != '*' && mi < sizeof(min_buf) - 1) {
        min_buf[mi++] = *frac++;
      }
      min_buf[mi] = '\0';

      return (float)atoi(deg_buf) + (float)atof(min_buf) / 60.0f;
    }

    // -------- sentence dispatcher --------

    void _parse_line(const char* line, uint8_t len) {
      if (len < 7 || line[0] != '$') return;
      if (!_is_valid_checksum(line, len)) return;

      // Sentence type is the 5 characters at offset 1 ("GPGGA", "GNRMC").
      // Accept any talker id (GP, GN, GL, GA, BD, QZ, …).
      if (line[3] == 'G' && line[4] == 'G' && line[5] == 'A') {
        _handle_gga(line, len);
      } else if (line[3] == 'R' && line[4] == 'M' && line[5] == 'C') {
        _handle_rmc(line, len);
      }
    }

    // -------- sentence handlers --------

    // $GxGGA,hhmmss.ss,lat,N,lon,E,quality,numSV,hdop,alt,M, ...
    void _handle_gga(const char* line, uint8_t len) {
      (void)len; // checksum already validated in _parse_line
      uint8_t f_len;
      const char* f;

      // Field 6 (0-indexed): fix quality. 0 = no fix.
      f = _field(line, 6, &f_len);
      if (!f || f_len == 0) return;
      if (atoi(f) == 0) { _has_fix = false; return; }

      // Field 2 + 3: latitude + hemisphere.
      f = _field(line, 2, &f_len);
      if (!f || f_len == 0) return;
      float lat = _parse_coord(f, f_len);
      const char* ns = _field(line, 3, &f_len);
      if (!ns || f_len == 0) return;
      if (ns[0] == 'S') lat = -lat;

      // Field 4 + 5: longitude + hemisphere.
      f = _field(line, 4, &f_len);
      if (!f || f_len == 0) return;
      float lon = _parse_coord(f, f_len);
      const char* ew = _field(line, 5, &f_len);
      if (!ew || f_len == 0) return;
      if (ew[0] == 'W') lon = -lon;

      // Field 7: satellites in use.
      f = _field(line, 7, &f_len);
      if (f && f_len > 0 && f_len <= 3) _satellites = (uint8_t)atoi(f);

      // Field 9: altitude (meters) — may be empty.
      f = _field(line, 9, &f_len);
      if (f && f_len > 0) _alt = (float)atof(f);

      _lat = lat;
      _lon = lon;
      _has_fix = true;
      _last_fix_ms = millis();
    }

    // $GxRMC,hhmmss.ss,A,lat,N,lon,E,speed,course,date, ...
    void _handle_rmc(const char* line, uint8_t len) {
      (void)len; // checksum already validated in _parse_line
      uint8_t f_len;
      const char* f;

      // Field 2: validity 'A' = active.
      f = _field(line, 2, &f_len);
      if (!f || f_len != 1 || f[0] != 'A') return;

      // Fields 3-6: latitude / longitude (same layout as GGA, no altitude).
      f = _field(line, 3, &f_len);
      if (!f || f_len == 0) return;
      float lat = _parse_coord(f, f_len);
      const char* ns = _field(line, 4, &f_len);
      if (!ns || f_len == 0) return;
      if (ns[0] == 'S') lat = -lat;

      f = _field(line, 5, &f_len);
      if (!f || f_len == 0) return;
      float lon = _parse_coord(f, f_len);
      const char* ew = _field(line, 6, &f_len);
      if (!ew || f_len == 0) return;
      if (ew[0] == 'W') lon = -lon;

      // Field 7: speed in knots → km/h.
      f = _field(line, 7, &f_len);
      if (f && f_len > 0) _speed_kmh = (float)(atof(f) * 1.852);

      // Field 8: course over ground (degrees).
      f = _field(line, 8, &f_len);
      if (f && f_len > 0) _course = (float)atof(f);

      _lat = lat;
      _lon = lon;
      _has_fix = true;
      _last_fix_ms = millis();

      // ---- UTC time/date (fields 1 and 9) ----
      // Field 1: hhmmss.ss — split into 2-digit segments (a bare atoi()
      // on the whole field would read all six digits as one number).
      f = _field(line, 1, &f_len);
      if (!f || f_len < 6) return;
      char hh_buf[3] = { f[0], f[1], 0 };
      char mm_buf[3] = { f[2], f[3], 0 };
      char ss_buf[3] = { f[4], f[5], 0 };
      uint32_t hh = (uint32_t)atoi(hh_buf) % 24;
      uint32_t mm = (uint32_t)atoi(mm_buf) % 60;
      uint32_t ss = (uint32_t)atoi(ss_buf) % 60;

      // Field 9: ddmmyy — same 2-digit segmentation.
      f = _field(line, 9, &f_len);
      if (!f || f_len < 6) return;
      char dd_buf[3] = { f[0], f[1], 0 };
      char mo_buf[3] = { f[2], f[3], 0 };
      char yy_buf[3] = { f[4], f[5], 0 };
      uint32_t dd = (uint32_t)atoi(dd_buf);
      uint32_t mo = (uint32_t)atoi(mo_buf);
      uint32_t yy = (uint32_t)atoi(yy_buf);
      if (dd < 1 || dd > 31 || mo < 1 || mo > 12) return;
      uint32_t year = 2000 + yy; // NMEA 2-digit year → 2000-2099

      // Days since Unix epoch for the date (Howard Hinnant's civil
      // algorithm — exact for the 2000-2099 range). The era is the
      // 400-year Gregorian cycle (`y/400`), not a century — dividing
      // by 100 here inflates `days` by ~4.6× and produces an epoch
      // in the year 2,212,131 instead of 2026 (which then corrupts
      // the RNS clock and hangs the UI loop).
      int32_t y = (int32_t)year;
      int32_t m = (int32_t)mo;
      if (m <= 2) { y--; m += 12; }
      int32_t era = y / 400;
      uint32_t yoe = (uint32_t)(y - era * 400);
      uint32_t doy = (153 * (m - 3) + 2) / 5 + dd - 1;
      uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      uint32_t days = (uint32_t)era * 146097 + doe - 719468;

      uint64_t epoch_s = (uint64_t)days * 86400ULL +
                         (uint64_t)hh * 3600ULL +
                         (uint64_t)mm * 60ULL + ss;

      // Only accept a plausible recent epoch (protects against a module
      // still emitting a stale almanac / factory default time before lock).
      if (epoch_s > 1700000000ULL) { // ~Nov 2023 conservative floor
        bool was_valid = _epoch_valid;
        _epoch_valid = true;
        _epoch_s = epoch_s;
        _epoch_fresh_ms = millis();
        if (!was_valid) {
          char dbg[80];
          snprintf(dbg, sizeof(dbg), "[GPS] UTC time available (epoch %llu)",
                   (unsigned long long)epoch_s);
          _debug(dbg);
        }
      }
    }
};

#ifdef TDECK_GPS_IMPLEMENTATION
  TDeckGPS tdeck_gps;
#endif