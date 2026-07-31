// Copyright (C) 2026, microReticulum contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// BBQ10 keyboard driver for the LilyGO T-Deck. The T-Deck's keyboard is a
// BBQ10-compatible I2C keypad (device address 0x55) controlled through a
// small register bank. This driver vendors the same protocol used by the
// arturo182 arduino_bbq10kbd library and Meshtastic firmware:
//
//   Registers:
//     0x01 VER   - firmware version
//     0x02 CFG   - configuration (key interrupt, reporting mods)
//     0x03 INT   - interrupt status
//     0x04 KEY   - key status / count
//     0x05 BKL   - backlight (0-255)
//     0x06 DEB   - debounce
//     0x07 FRQ   - scan frequency
//     0x08 RST   - reset
//     0x09 FIF   - key FIFO (16-bit entries: char << 8 | key state)
//
// Writes use the MSB set on the register address (register | 0x80).

#pragma once

#include <Arduino.h>
#include <Wire.h>

#define TDECK_KB_ADDR     0x55
#define TDECK_KB_INT_PIN  16

#define KB_KEY_STATE_IDLE       0
#define KB_KEY_STATE_PRESS      1
#define KB_KEY_STATE_LONG_PRESS 2
#define KB_KEY_STATE_RELEASE    3

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

    TDeckKeyboard() : _addr(TDECK_KB_ADDR), _wire(nullptr) {}

    void begin(TwoWire *wire = &Wire) {
        _wire = wire;
        reset();
    }

    void reset() {
        if (_wire) {
            _wire->beginTransmission(_addr);
            _wire->write(0x08); // RST register
            _wire->endTransmission();
        }
        delay(100);
        // Enable key-change interrupt reporting and modifier reporting
        writeRegister(0x02, readRegister8(0x02) | 0x50); // CFG_KEY_INT | CFG_REPORT_MODS
        delay(100);
    }

    void attachInterruptPin(uint8_t pin, void (*func)(void)) const {
        pinMode(pin, INPUT_PULLUP);
        ::attachInterrupt(digitalPinToInterrupt(pin), func, RISING);
    }

    void clearInterruptStatus() {
        writeRegister(0x03, 0x00); // INT register
    }

    uint8_t status() const {
        return readRegister8(0x04); // KEY status
    }

    uint8_t keyCount() const {
        return status() & 0x1F;
    }

    KeyEvent keyEvent() const {
        KeyEvent event = {.key = '\0', .state = KB_KEY_STATE_IDLE};
        if (keyCount() == 0) return event;

        uint8_t data[2] = {0, 0};
        if (_wire) {
            _wire->beginTransmission(_addr);
            _wire->write(0x09); // FIF register
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

    void setBacklight(uint8_t value) {
        writeRegister(0x05, value); // BKL register
    }

    void setDebounce(uint8_t value) {
        writeRegister(0x06, value); // DEB register
    }

  private:
    uint8_t _addr;
    TwoWire *_wire;

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
        _wire->write(reg | 0x80); // write mask
        _wire->write(value);
        _wire->endTransmission();
    }
};