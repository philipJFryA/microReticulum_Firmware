// Copyright (C) 2024, Mark Qvist
// T-Deck ST7789 display driver via Arduino_GFX

#include "UIDisplay.h"

#ifdef BOARD_TDECK

#include <Arduino.h>
#include "Boards.h"
#include <Arduino_GFX_Library.h>

namespace td_display {

const int WIDTH  = 320;
const int HEIGHT = 240;

// global handles
static Arduino_DataBus *bus  = nullptr;
static Arduino_GFX     *gfx  = nullptr;

lv_color_t *drawBuf1 = nullptr;
lv_color_t *drawBuf2 = nullptr;
static lv_disp_draw_buf_t draw_buf;

// backlight PWM - matches set_contrast in Display.h
void setBacklight(uint8_t level) {
  static uint8_t cur = 0;
  constexpr uint8_t steps = 16;

  if (level > 15) level = 15;
  if (level == 0) {
    digitalWrite(DISPLAY_BL_PIN, 0);
    delay(3);
    cur = 0;
    return;
  }
  if (cur == 0) {
    digitalWrite(DISPLAY_BL_PIN, 1);
    cur = steps;
    delayMicroseconds(30);
  }
  int from  = steps - cur;
  int to    = steps - level;
  int num   = (steps + to - from) % steps;
  for (int i = 0; i < num; i++) {
    digitalWrite(DISPLAY_BL_PIN, 0);
    digitalWrite(DISPLAY_BL_PIN, 1);
  }
  cur = level;
}

// LVGL flush callback
void flushCb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  if (!gfx) {
    lv_disp_flush_ready(disp);
    return;
  }
  int w = lv_area_get_width(area);
  int h = lv_area_get_height(area);

  gfx->draw16bitBeRGBBitmap(area->x1, area->y1,
                            reinterpret_cast<uint16_t *>(color_p),
                            w, h);
  lv_disp_flush_ready(disp);
}

// initialisation
bool init() {
  // T-Deck display shares SPI pins (CLK=40, MOSI=41, MISO=38) with the
  // LoRa radio on different CS.  The original firmware (Display.h) uses
  // software SPI via Adafruit_ST7789(CS, DC, RST) to avoid driver-level
  // conflicts on the shared bus.  Arduino_GFX's Arduino_SoftwareSPI bus
  // achieves the same thing.

  pinMode(DISPLAY_DC, OUTPUT);
  pinMode(DISPLAY_CS, OUTPUT);
  pinMode(DISPLAY_BL_PIN, OUTPUT);
  digitalWrite(DISPLAY_CS, HIGH);
  digitalWrite(DISPLAY_BL_PIN, HIGH);   // backlight ON before init
  delay(10);

  // Use hardware SPI on the default controller (FSPI on ESP32-S3) with
  // is_shared_interface=true so it doesn't disrupt the LoRa radio on the
  // same physical bus (shared CLK=40, MOSI=41, MISO=38, different CS).
  bus = new Arduino_ESP32SPI(
      DISPLAY_DC, DISPLAY_CS,
      DISPLAY_CLK, DISPLAY_MOSI, DISPLAY_MISO,
      HSPI,       // SPI3 bus - free for user peripherals on ESP32-S3
      true);       // is_shared_interface

  if (!bus->begin()) return false;

  gfx = new Arduino_ST7789(bus, -1 /* RST */, 1 /* rotation */,
                           true /* IPS */, 240, 320);
  gfx->begin();
  gfx->fillScreen(BLACK);

  // Restore original backlight behaviour: bit-banged PWM started at
  // full brightness (15/15).  The user can adjust it from the settings
  // screen later.
  // After init, backlight is ON, we switch to PWM mode at full brightness
  setBacklight(15);

  // LVGL display driver
  lv_init();

  drawBuf1 = (lv_color_t *)ps_malloc(WIDTH * HEIGHT * sizeof(lv_color_t));
  drawBuf2 = (lv_color_t *)ps_malloc(WIDTH * HEIGHT * sizeof(lv_color_t));
  if (!drawBuf1 || !drawBuf2) {
    if (drawBuf1) { free(drawBuf1); drawBuf1 = nullptr; }
    if (drawBuf2) { free(drawBuf2); drawBuf2 = nullptr; }

    drawBuf1 = (lv_color_t *)ps_malloc(WIDTH * 40 * sizeof(lv_color_t));
    lv_disp_draw_buf_init(&draw_buf, drawBuf1, nullptr, WIDTH * 40);
  } else {
    lv_disp_draw_buf_init(&draw_buf, drawBuf1, drawBuf2, WIDTH * HEIGHT);
  }

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = WIDTH;
  disp_drv.ver_res  = HEIGHT;
  disp_drv.flush_cb = flushCb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  return true;
}

} // namespace td_display

#endif // BOARD_TDECK