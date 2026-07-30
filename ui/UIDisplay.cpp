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

// ── global handles ────────────────────────────────────────────────────
static Arduino_DataBus *bus  = nullptr;
static Arduino_GFX     *gfx  = nullptr;

lv_color_t *drawBuf1 = nullptr;
lv_color_t *drawBuf2 = nullptr;
static lv_disp_draw_buf_t draw_buf;

// ── backlight PWM ─────────────────────────────────────────────────────
void setBacklight(uint8_t level) {
  // Uses the same bit‑banged approach as Display.h’s set_contrast
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

// ── LVGL flush callback ───────────────────────────────────────────────
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

// ── initialisation ────────────────────────────────────────────────────
bool init() {
  // SPI bus already configured by the main firmware (same MOSI/CLK as LoRa)

  pinMode(DISPLAY_DC, OUTPUT);
  pinMode(DISPLAY_CS, OUTPUT);
  pinMode(DISPLAY_BL_PIN, OUTPUT);
  digitalWrite(DISPLAY_CS, HIGH);
  digitalWrite(DISPLAY_BL_PIN, LOW);

  bus = new Arduino_ESP32SPI(
      DISPLAY_DC, DISPLAY_CS,
      DISPLAY_CLK, DISPLAY_MOSI, DISPLAY_MISO,
      HSPI,        // Use SPI2 so it won't conflict with LoRa on SPI (FSPI)
      true);       // auto-flush DMA

  if (!bus->begin()) return false;

  gfx = new Arduino_ST7789(bus, -1 /* RST */, 1 /* rotation */,
                           true /* IPS */, 240, 320);
  gfx->begin();
  gfx->fillScreen(BLACK);

  // ── LVGL display driver ──────────────────────────────────────────
  lv_init();

  drawBuf1 = (lv_color_t *)ps_malloc(WIDTH * HEIGHT * sizeof(lv_color_t));
  drawBuf2 = (lv_color_t *)ps_malloc(WIDTH * HEIGHT * sizeof(lv_color_t));
  if (!drawBuf1 || !drawBuf2) {
    // Fall back to single buffer (half size)
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