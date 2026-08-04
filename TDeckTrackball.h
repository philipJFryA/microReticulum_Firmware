// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Trackball driver for the LilyGO T-Deck. The trackball reports directional
// movement through four GPIO lines (UP/DOWN/LEFT/RIGHT) and a centre press.
// Each axis line produces a falling-edge pulse when the ball is rolled, and
// the centre button is an active-low switch. The centre button is edge-tracked
// on CHANGE so the release edge can latch the press event (a FALLING-only
// interrupt would never complete a tap). This driver implements the same
// GPIO-interrupt polling pattern used by Meshtastic's TrackballInterruptBase.
//
// The singleton instance and the static ISR pointer are defined exactly once
// by defining TDECK_TRACKBALL_IMPLEMENTATION in a single translation unit
// (TDeckUI.h does this). The static pointer used by the ISR trampolines is
// rebound by begin() whenever the instance is initialised.

#pragma once

#include <Arduino.h>

#ifndef TB_UP
  #define TB_UP 3
#endif
#ifndef TB_DOWN
  #define TB_DOWN 15
#endif
#ifndef TB_LEFT
  #define TB_LEFT 1
#endif
#ifndef TB_RIGHT
  #define TB_RIGHT 2
#endif
#ifndef TB_PRESS
  #define TB_PRESS 0
#endif
#ifndef TB_DIRECTION
  #define TB_DIRECTION FALLING
#endif
#ifndef TB_THRESHOLD
  #define TB_THRESHOLD 3
#endif

typedef enum {
    TB_EVENT_NONE      = 0x00,
    TB_EVENT_UP        = 0x01,
    TB_EVENT_DOWN      = 0x02,
    TB_EVENT_LEFT      = 0x04,
    TB_EVENT_RIGHT     = 0x08,
    TB_EVENT_PRESS     = 0x10,
    TB_EVENT_PRESS_LONG = 0x20
} tb_event_t;

class TDeckTrackball {
  public:
    TDeckTrackball() : _latched(TB_EVENT_NONE), _last_clear(0), _press_started(0),
                       _press_level(true), _last_press_edge(0) {}

    void begin() {
        // Rebind the ISR trampolines to this instance
        s_instance = this;

        pinMode(TB_UP, INPUT_PULLUP);
        pinMode(TB_DOWN, INPUT_PULLUP);
        pinMode(TB_LEFT, INPUT_PULLUP);
        pinMode(TB_RIGHT, INPUT_PULLUP);
        pinMode(TB_PRESS, INPUT_PULLUP);

        attachInterrupt(digitalPinToInterrupt(TB_UP), s_isr_up, TB_DIRECTION);
        attachInterrupt(digitalPinToInterrupt(TB_DOWN), s_isr_down, TB_DIRECTION);
        attachInterrupt(digitalPinToInterrupt(TB_LEFT), s_isr_left, TB_DIRECTION);
        attachInterrupt(digitalPinToInterrupt(TB_RIGHT), s_isr_right, TB_DIRECTION);
        // The centre button is an active-low switch whose press event is
        // latched on the RELEASE (rising) edge, so it must be edge-tracked
        // on CHANGE. A FALLING-only interrupt would record the press start
        // but never complete a tap. The direction lines stay on FALLING.
        attachInterrupt(digitalPinToInterrupt(TB_PRESS), s_isr_press, CHANGE);

        _last_clear = millis();
    }

    // Reads and clears the pending trackball event bitmap.
    tb_event_t readEvents() {
        tb_event_t events = TB_EVENT_NONE;

        // Resolve a long press if the button has been held beyond the threshold
        if (_press_started != 0 && (millis() - _press_started) >= 1000) {
            events = (tb_event_t)(events | TB_EVENT_PRESS_LONG);
            _press_started = 0;
        }

        // Merge interrupt-latched flags (cleared by the polling side)
        events = (tb_event_t)(events | _latched);

        // Debounce: ignore extremely rapid re-triggers from contact bounce
        if (events != TB_EVENT_NONE && _latched != TB_EVENT_NONE && (millis() - _last_clear) < 50) {
            events = TB_EVENT_NONE;
        }

        if (events != TB_EVENT_NONE) {
            _last_clear = millis();
            _latched = TB_EVENT_NONE;
        }
        return events;
    }

    // ISR-safe latch API
    void latch(tb_event_t event) {
        _latched = (tb_event_t)(_latched | event);
    }

    void latchPressStart() {
        _press_started = millis();
    }

    void latchPressEnd() {
        _press_started = 0;
    }

    // Debounced centre-button handler, invoked on both press and release
    // edges. Contact bounce on either edge is filtered by tracking the last
    // accepted level and requiring a short quiet window between edges.
    void handlePressISR() {
        bool down = (digitalRead(TB_PRESS) == LOW);
        if (down == _press_level) return;          // bounce of the same level
        uint32_t now = millis();
        if ((now - _last_press_edge) < 25) return; // contact-bounce filter
        _press_level = down;
        _last_press_edge = now;
        if (down) {
            latchPressStart();
        } else {
            latchPressEnd();
            latch(TB_EVENT_PRESS);
        }
    }

  private:
    volatile tb_event_t _latched;
    volatile uint32_t _last_clear;
    volatile uint32_t _press_started;
    volatile bool     _press_level;
    volatile uint32_t _last_press_edge;

    static TDeckTrackball *s_instance;

    // ISR trampolines — deliberately NOT IRAM_ATTR. The trackball's edge
    // pulses are slow (human-scale) and loss-tolerant, unlike e.g. LoRa
    // DIO, so the minor flash-cache stall on a flash erase is acceptable.
    // Keeping the handlers out of IRAM also avoids the Xtensa "literal
    // placed after use" linker failure on ESP32 for static-member ISRs
    // that must address a DRAM pointer through a literal pool.
    static void s_isr_up()    { if (s_instance) s_instance->latch(TB_EVENT_UP); }
    static void s_isr_down()  { if (s_instance) s_instance->latch(TB_EVENT_DOWN); }
    static void s_isr_left()  { if (s_instance) s_instance->latch(TB_EVENT_LEFT); }
    static void s_isr_right() { if (s_instance) s_instance->latch(TB_EVENT_RIGHT); }
    static void s_isr_press() {
        if (!s_instance) return;
        s_instance->handlePressISR();
    }
};

#ifdef TDECK_TRACKBALL_IMPLEMENTATION
  // Single definition of the global trackball instance and the static ISR
  // pointer. The firmware defines TDECK_TRACKBALL_IMPLEMENTATION in
  // TDeckUI.h (the only TU that includes this for the T-Deck build), so
  // both symbols appear exactly once in the firmware image.
  TDeckTrackball *TDeckTrackball::s_instance = nullptr;
  TDeckTrackball tdeck_trackball;
#endif
