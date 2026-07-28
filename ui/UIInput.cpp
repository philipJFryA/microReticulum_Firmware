// Copyright (C) 2024, Mark Qvist
// T-Deck unified input driver: trackball + keyboard → LVGL indevs

#include "UIInput.h"

#ifdef BOARD_TDECK

#include <Wire.h>
#include <driver/gpio.h>

namespace td_input {

// ── pin constants ─────────────────────────────────────────────────────
constexpr gpio_num_t PIN_TB_UP    = GPIO_NUM_3;
constexpr gpio_num_t PIN_TB_DOWN  = GPIO_NUM_15;
constexpr gpio_num_t PIN_TB_LEFT  = GPIO_NUM_1;
constexpr gpio_num_t PIN_TB_RIGHT = GPIO_NUM_2;
constexpr gpio_num_t PIN_TB_PRESS = GPIO_NUM_0;   // shared with BOOT
constexpr gpio_num_t PIN_KB_INT   = GPIO_NUM_46;  // keyboard interrupt

constexpr uint8_t KB_I2C_ADDR     = 0x55;
constexpr uint8_t TB_DEBOUNCE_MS  = 20;
constexpr uint8_t TB_REPEAT_MS    = 350;
constexpr uint8_t TB_REPEAT_INIT  = 500;  // initial repeat delay

// ── volatile state (ISR‑safe) ─────────────────────────────────────────
static volatile uint8_t  tb_flags    = 0;
static volatile bool     tb_pressed  = false;
static volatile uint32_t tb_last_ms  = 0;
static volatile uint8_t  kb_last_key = 0;
static volatile bool     kb_pending  = false;

// ── LVGL indev handles ────────────────────────────────────────────────
static lv_indev_t *indev_keypad = nullptr;   // trackball
static lv_indev_t *indev_kb     = nullptr;   // keyboard

// ── ISRs ──────────────────────────────────────────────────────────────
static void IRAM_ATTR isr_tb_up()    { tb_flags |= TB_UP;    tb_last_ms = millis(); }
static void IRAM_ATTR isr_tb_down()  { tb_flags |= TB_DOWN;  tb_last_ms = millis(); }
static void IRAM_ATTR isr_tb_left()  { tb_flags |= TB_LEFT;  tb_last_ms = millis(); }
static void IRAM_ATTR isr_tb_right() { tb_flags |= TB_RIGHT; tb_last_ms = millis(); }
static void IRAM_ATTR isr_tb_press() { tb_flags |= TB_PRESS; tb_last_ms = millis(); }
static void IRAM_ATTR isr_kb_int()   { /* just wake poll */ }

// ── trackball LVGL read callback ──────────────────────────────────────
static void tb_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  // debounce
  if (millis() - tb_last_ms < TB_DEBOUNCE_MS) {
    data->key   = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  uint8_t flags = tb_flags;
  tb_flags = 0;

  // long-press repeat (press-and-hold on centre button)
  static bool   center_held  = false;
  static uint32_t center_down = 0;
  static uint32_t last_repeat = 0;

  if (flags & TB_PRESS) {
    if (!center_held) {
      center_down = millis();
      center_held = true;
    }
    data->key   = LV_KEY_ENTER;
    data->state = LV_INDEV_STATE_PRESSED;
    last_repeat = millis();
    return;
  }

  if (center_held && !digitalRead(PIN_TB_PRESS)) {
    // still held — send repeat or ESC on long press
    uint32_t hold = millis() - center_down;
    if (hold > 2000) {
      // Long press → ESC (back / app switcher)
      data->key   = LV_KEY_ESC;
      data->state = LV_INDEV_STATE_PRESSED;
      center_held = false;
      return;
    }
    if (hold > TB_REPEAT_INIT && millis() - last_repeat >= TB_REPEAT_MS) {
      data->key   = LV_KEY_ENTER;
      data->state = LV_INDEV_STATE_PRESSED;
      last_repeat = millis();
      return;
    }
    data->key   = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  if (center_held && digitalRead(PIN_TB_PRESS)) {
    // released
    if (millis() - center_down < 2000) {
      data->key   = LV_KEY_ENTER;
      data->state = LV_INDEV_STATE_PRESSED;
    }
    center_held = false;
    return;
  }

  // directional inputs
  if (flags & TB_UP)    { data->key = LV_KEY_UP;    data->state = LV_INDEV_STATE_PRESSED; return; }
  if (flags & TB_DOWN)  { data->key = LV_KEY_DOWN;  data->state = LV_INDEV_STATE_PRESSED; return; }
  if (flags & TB_LEFT)  { data->key = LV_KEY_LEFT;  data->state = LV_INDEV_STATE_PRESSED; return; }
  if (flags & TB_RIGHT) { data->key = LV_KEY_RIGHT; data->state = LV_INDEV_STATE_PRESSED; return; }

  data->key   = 0;
  data->state = LV_INDEV_STATE_RELEASED;
}

// ── keyboard LVGL read callback ───────────────────────────────────────
static void kb_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (!kb_pending) {
    data->key   = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  uint8_t code = kb_last_key;
  kb_pending = false;

  // Map keyboard key codes to LVGL keys
  switch (code) {
    case KEY_UP:    data->key = LV_KEY_UP;    break;
    case KEY_DOWN:  data->key = LV_KEY_DOWN;  break;
    case KEY_LEFT:  data->key = LV_KEY_LEFT;  break;
    case KEY_RIGHT: data->key = LV_KEY_RIGHT; break;
    case KEY_ENTER: data->key = LV_KEY_ENTER; break;
    case KEY_ESC:   data->key = LV_KEY_ESC;   break;
    case KEY_BACK:  data->key = LV_KEY_BACKSPACE; break;
    case KEY_TAB:   data->key = LV_KEY_NEXT;  break;
    default:
      // Pass ASCII characters through
      if (code >= 0x20 && code <= 0x7E) {
        data->key = code;
      } else {
        data->key = 0; // unknown key
        data->state = LV_INDEV_STATE_RELEASED;
        return;
      }
  }
  data->state = LV_INDEV_STATE_PRESSED;
}

// ── public getLastKey ─────────────────────────────────────────────────
uint8_t getLastKey() {
  return kb_last_key;
}

// ── poll (called from tick) ───────────────────────────────────────────
void poll() {
  // Keyboard I²C read (triggered by INT pin LOW)
  if (digitalRead(PIN_KB_INT) == LOW) {
    Wire.requestFrom(KB_I2C_ADDR, 1);
    if (Wire.available()) {
      uint8_t c = Wire.read();
      kb_last_key = c;
      kb_pending  = true;
    }
    // Drain any extra bytes
    while (Wire.available()) Wire.read();
  }

  // Trackball is fully ISR‑driven — just kick the indev read
  // (LVGL calls tb_read on its own timer)
}

// ── initialisation ────────────────────────────────────────────────────
void init() {
  // ── trackball GPIOs ──────────────────────────────────────────────
  pinMode(PIN_TB_UP,    INPUT_PULLUP);
  pinMode(PIN_TB_DOWN,  INPUT_PULLUP);
  pinMode(PIN_TB_LEFT,  INPUT_PULLUP);
  pinMode(PIN_TB_RIGHT, INPUT_PULLUP);
  // PIN_TB_PRESS (GPIO 0) already pulled up by BOOT button circuit

  attachInterrupt(digitalPinToInterrupt(PIN_TB_UP),    isr_tb_up,    FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_TB_DOWN),  isr_tb_down,  FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_TB_LEFT),  isr_tb_left,  FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_TB_RIGHT), isr_tb_right, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_TB_PRESS), isr_tb_press, FALLING);

  // ── keyboard I²C ─────────────────────────────────────────────────
  Wire.begin(18, 8);  // SDA=18, SCL=8
  pinMode(PIN_KB_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_KB_INT), isr_kb_int, FALLING);

  // ── register LVGL indevs ─────────────────────────────────────────
  static lv_indev_drv_t drv_keypad;
  lv_indev_drv_init(&drv_keypad);
  drv_keypad.type    = LV_INDEV_TYPE_KEYPAD;
  drv_keypad.read_cb = tb_read;
  indev_keypad = lv_indev_drv_register(&drv_keypad);

  static lv_indev_drv_t drv_kb;
  lv_indev_drv_init(&drv_kb);
  drv_kb.type    = LV_INDEV_TYPE_KEYPAD;
  drv_kb.read_cb = kb_read;
  indev_kb = lv_indev_drv_register(&drv_kb);
}

} // namespace td_input

#endif // BOARD_TDECK