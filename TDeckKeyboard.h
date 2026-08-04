// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Keyboard driver for the LilyGO T-Deck.
//
// The T-Deck's keyboard hardware comes in two variants, both on the I2C bus
// at address 0x55:
//
//   1. ESP32-C3 "T-Keyboard" coprocessor (most units). The C3 scans the key
//      matrix itself and exposes a simple byte-stream protocol:
//
//        Read:   a single byte, the last pressed key character (0x00 = none)
//        Write:  [0x01][duty] set backlight brightness (0-255)
//                [0x02][duty] set default backlight brightness
//                [0x03]       switch to raw key-matrix mode
//                [0x04]       switch to key-character mode
//
//   2. BBQ10 keyboard module wired directly to the bus. The BBQ10 exposes a
//      register bank (VER 0x01, CFG 0x02, INT 0x03, KEY 0x04, BKL 0x05,
//      DEB 0x06, FRQ 0x07, RST 0x08, FIF 0x09) where writes set the MSB of
//      the register address (reg | 0x80).
//
// This driver probes for the BBQ10 register protocol at init (write a
// distinctive CFG value and read it back) and falls back to the ESP32-C3
// byte-stream protocol, so both hardware variants work. Key events are
// polled over I2C; no interrupt pin is used.

#pragma once

#include <Arduino.h>
#include <Wire.h>

#define TDECK_KB_ADDR     0x55
// NOTE: GPIO 16 on the T-Deck is the AXP2101 PMU interrupt, not a keyboard
// interrupt. The keyboard is polled over I2C, so no interrupt pin is needed.

#define KB_KEY_STATE_IDLE       0
#define KB_KEY_STATE_PRESS      1
#define KB_KEY_STATE_LONG_PRESS 2
#define KB_KEY_STATE_RELEASE    3

// BBQ10 register map
#define BBQ10_REG_VER  0x01
#define BBQ10_REG_CFG  0x02
#define BBQ10_REG_INT  0x03
#define BBQ10_REG_KEY  0x04
#define BBQ10_REG_BKL  0x05
#define BBQ10_REG_DEB  0x06
#define BBQ10_REG_FRQ  0x07
#define BBQ10_REG_RST  0x08
#define BBQ10_REG_FIF  0x09

// ESP32-C3 keyboard slave commands
#define TDECK_KB_CMD_BRIGHTNESS  0x01
#define TDECK_KB_CMD_DEFAULT_BL  0x02
#define TDECK_KB_CMD_RAW_MODE    0x03
#define TDECK_KB_CMD_KEY_MODE    0x04

// Legacy modifier scan codes (key mode only reports printable characters;
// kept for compatibility with code that maps modifier keys).
#define KB_MOD_ALT 0x1A
#define KB_MOD_SHL 0x1B
#define KB_MOD_SHR 0x1C
#define KB_MOD_SYM 0x1D

class TDeckKeyboard {
  public:
    typedef struct {
        char key;
        uint8_t state;
    } KeyEvent;

    TDeckKeyboard() : _addr(TDECK_KB_ADDR), _wire(nullptr), _proto(PROTO_NONE) {}

    void begin(TwoWire *wire = &Wire) {
        _wire = wire;
        _proto = detectProtocol();
        if (_proto == PROTO_BBQ10) {
            reset();
            // Enable key-change interrupt reporting and modifier reporting
            writeRegister(BBQ10_REG_CFG, readRegister8(BBQ10_REG_CFG) | 0x50);
            delay(100);
        } else if (_proto == PROTO_C3) {
            // Ask the C3 for key-character mode (raw mode reports the matrix)
            writeC3Command(TDECK_KB_CMD_KEY_MODE);
        }
    }

    void reset() {
        if (_proto != PROTO_BBQ10 || !_wire) return;
        _wire->beginTransmission(_addr);
        _wire->write(BBQ10_REG_RST); // RST register
        _wire->endTransmission();
        delay(100);
    }

    // The keyboard is polled over I2C; no interrupt pin is used.
    void attachInterruptPin(uint8_t pin, void (*func)(void)) const {
        (void)pin;
        (void)func;
    }

    void clearInterruptStatus() {
        if (_proto == PROTO_BBQ10) {
            writeRegister(BBQ10_REG_INT, 0x00); // INT register
        }
    }

    uint8_t status() const {
        if (_proto == PROTO_BBQ10) return readRegister8(BBQ10_REG_KEY);
        return 0;
    }

    uint8_t keyCount() const {
        return status() & 0x1F;
    }

    KeyEvent keyEvent() const {
        if (_proto == PROTO_BBQ10) return keyEventBBQ10();
        if (_proto == PROTO_C3)    return keyEventC3();
        KeyEvent event = {.key = '\0', .state = KB_KEY_STATE_IDLE};
        return event;
    }

    void setBacklight(uint8_t value) {
        if (_proto == PROTO_BBQ10) {
            writeRegister(BBQ10_REG_BKL, value); // BKL register
        } else if (_proto == PROTO_C3) {
            writeC3Brightness(value);
        }
    }

    void setDebounce(uint8_t value) {
        if (_proto == PROTO_BBQ10) {
            writeRegister(BBQ10_REG_DEB, value); // DEB register
        }
    }

    bool present() const { return _proto != PROTO_NONE; }
    uint8_t protocol() const { return _proto; }

  private:
    enum { PROTO_NONE = 0, PROTO_BBQ10 = 1, PROTO_C3 = 2 };

    uint8_t _addr;
    TwoWire *_wire;
    uint8_t _proto;

    // ---- ESP32-C3 byte-stream protocol ------------------------------------

    KeyEvent keyEventC3() const {
        KeyEvent event = {.key = '\0', .state = KB_KEY_STATE_IDLE};
        if (!_wire) return event;
        _wire->requestFrom((uint8_t)_addr, (uint8_t)1);
        if (_wire->available() >= 1) {
            char c = (char)_wire->read();
            if (c != 0x00) {
                event.key = c;
                event.state = KB_KEY_STATE_PRESS;
            }
        }
        return event;
    }

    void writeC3Command(uint8_t cmd) const {
        if (!_wire) return;
        _wire->beginTransmission(_addr);
        _wire->write(cmd);
        _wire->endTransmission();
    }

    void writeC3Brightness(uint8_t value) const {
        if (!_wire) return;
        _wire->beginTransmission(_addr);
        _wire->write(TDECK_KB_CMD_BRIGHTNESS);
        _wire->write(value);
        _wire->endTransmission();
    }

    // ---- BBQ10 register protocol -------------------------------------------

    KeyEvent keyEventBBQ10() const {
        KeyEvent event = {.key = '\0', .state = KB_KEY_STATE_IDLE};
        if (keyCount() == 0) return event;

        uint8_t data[2] = {0, 0};
        if (_wire) {
            _wire->beginTransmission(_addr);
            _wire->write(BBQ10_REG_FIF); // FIF register
            _wire->endTransmission();
            _wire->requestFrom((uint8_t)_addr, (uint8_t)2);
            if (_wire->available() >= 2) {
                data[0] = _wire->read();
                data[1] = _wire->read();
            }
        }
        event.key = (char)data[1];
        event.state = data[0] & 0x03;
        return event;
    }

    // ---- shared low-level helpers -----------------------------------------

    uint8_t readRegister8(uint8_t reg) const {
        if (!_wire) return 0;
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission();
        _wire->requestFrom((uint8_t)_addr, (uint8_t)1);
        if (_wire->available() < 1) return 0;
        return _wire->read();
    }

    void writeRegister(uint8_t reg, uint8_t value) {
        if (!_wire) return;
        _wire->beginTransmission(_addr);
        _wire->write(reg | 0x80); // write mask (MSB set)
        _wire->write(value);
        _wire->endTransmission();
    }

    // Detect which protocol the device at 0x55 speaks. A real BBQ10 stores a
    // written CFG value and returns it on the next register read; the C3
    // ignores unknown writes and answers reads with the last pressed key
    // (0x00 when idle), so the read-back will not match.
    uint8_t detectProtocol() {
        if (!_wire) return PROTO_NONE;

        // Discard any key the C3 might have buffered since power-on.
        _wire->requestFrom((uint8_t)_addr, (uint8_t)1);
        while (_wire->available()) {
            _wire->read();
        }

        writeRegister(BBQ10_REG_CFG, 0x50); // CFG_KEY_INT | CFG_REPORT_MODS
        if (readRegister8(BBQ10_REG_CFG) == 0x50) {
            return PROTO_BBQ10;
        }
        return PROTO_C3;
    }
};
