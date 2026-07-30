// Copyright (C) 2024, Mark Qvist
// T-Deck settings screen implementation

#include <Arduino.h>
#include "SettingsScreen.h"
#include "../UITheme.h"
#include "ScreenBuilder.h"

#ifdef BOARD_TDECK

using namespace td_theme::Layout;

// ── extern firmware globals ────────────────────────────────────────────
extern float   lora_freq;
extern float   lora_bw;
extern uint8_t lora_sf;
extern uint8_t lora_cr;
extern int8_t  lora_txp;
extern uint8_t display_intensity;

// ── firmware setter functions ─────────────────────────────────────────
extern void set_lora_frequency(float freq);
extern void set_lora_bandwidth(float bw);
extern void set_lora_spreading_factor(uint8_t sf);
extern void set_lora_coding_rate(uint8_t cr);
extern void set_lora_tx_power(int8_t dbm);
extern void set_display_intensity(uint8_t level);
extern void request_restart();

// ── bandwidth options (kHz) ───────────────────────────────────────────
static constexpr float bw_values[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f };
static constexpr uint8_t bw_count = sizeof(bw_values) / sizeof(bw_values[0]);

// ── frequency bounds (MHz x 100 for slider integer range) ────────────
static constexpr int32_t FREQ_MIN = 41000;   // 410.00 MHz
static constexpr int32_t FREQ_MAX = 96000;   // 960.00 MHz
static constexpr int32_t TXP_MIN  = -20;
static constexpr int32_t TXP_MAX  = 22;
static constexpr int32_t BL_MIN   = 0;
static constexpr int32_t BL_MAX   = 15;

// ── helpers ───────────────────────────────────────────────────────────
static int findBwIndex(float bw) {
  for (uint8_t i = 0; i < bw_count; i++) {
    if (labs(bw - bw_values[i]) < 0.6f) return i;
  }
  return 7; // default 125 kHz
}

// Clamp a dropdown index to valid range
static uint8_t clampBwIdx(uint8_t idx) { return (idx < bw_count) ? idx : 7; }
static uint8_t clampSfIdx(uint8_t idx) { return (idx < 6) ? idx : 0; }    // 0-5 -> SF7-SF12
static uint8_t clampCrIdx(uint8_t idx) { return (idx < 4) ? idx : 0; }    // 0-3 -> 4/5-4/8

// ── static event handlers ────────────────────────────────────────────
static void onFreqSliderChanged(lv_event_t *e) {
  (void)e;
}

void onTxpSliderChanged(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t*)lv_event_get_target(e);
  if (!slider) return;
  int32_t v = lv_slider_get_value(slider);
  if (v < TXP_MIN) v = TXP_MIN;
  if (v > TXP_MAX) v = TXP_MAX;
  SettingsScreen *self = (SettingsScreen*)lv_event_get_user_data(e);
  if (!self || !self->txpLabel || !lv_obj_is_valid(self->txpLabel)) return;
  char b[32];
  snprintf(b, sizeof(b), "%d dBm", (int)v);
  lv_label_set_text(self->txpLabel, b);
}

void onBlSliderChanged(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t*)lv_event_get_target(e);
  if (!slider) return;
  int32_t v = lv_slider_get_value(slider);
  if (v < BL_MIN) v = BL_MIN;
  if (v > BL_MAX) v = BL_MAX;
  SettingsScreen *self = (SettingsScreen*)lv_event_get_user_data(e);
  if (!self || !self->blLabel || !lv_obj_is_valid(self->blLabel)) return;
  char b[32];
  snprintf(b, sizeof(b), "%d/15", (int)v);
  lv_label_set_text(self->blLabel, b);
}

void onRebootClicked(lv_event_t *e) {
  (void)e;
  request_restart();
}

// ── create ────────────────────────────────────────────────────────────
void SettingsScreen::create(lv_obj_t *parent) {
  using namespace td_ui::Build;

  container = lv_obj_create(parent);
  lv_obj_add_style(container, &td_theme::style_screen, 0);
  lv_obj_set_size(container, SCREEN_W, SCREEN_H);
  lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

  // title
  titleLabel = lv_label_create(container);
  lv_label_set_text(titleLabel, "Settings");
  lv_obj_add_style(titleLabel, &td_theme::style_title, 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, PAD_TITLE_Y);

  // ── scrollable content ───────────────────────────────────────────
  lv_obj_t *cont = lv_obj_create(container);
  lv_obj_set_size(cont, SETTINGS_W, SETTINGS_H);
  lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_style(cont, &td_theme::style_screen, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cont, PAD_CONTENT, 0);
  lv_obj_set_style_pad_all(cont, PAD_CONTENT, 0);

  // ── radio section ────────────────────────────────────────────────
  lv_obj_t *sec1 = addSection(cont, SECTION_W, "Radio");

  // frequency slider
  addSliderRow(sec1, ROW_W, "Frequency", FREQ_MIN, FREQ_MAX, freqSlider, freqLabel);
  lv_slider_set_value(freqSlider, (int32_t)(lora_freq * 100), LV_ANIM_OFF);

  lv_obj_set_style_text_color(freqLabel, td_theme::ACCENT, 0);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.2f MHz", lora_freq);
  lv_label_set_text(freqLabel, buf);

  lv_obj_add_event_cb(freqSlider, onFreqSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  // bandwidth dropdown (factory sets row, label, dd styles)
  addDropdownRow(sec1, ROW_W, "Bandwidth", DD_W_LG, bwDropdown);
  lv_dropdown_clear_options(bwDropdown);
  for (uint8_t i = 0; i < bw_count; i++) {
    snprintf(buf, sizeof(buf), "%.1f kHz", bw_values[i]);
    lv_dropdown_add_option(bwDropdown, buf, LV_DROPDOWN_POS_LAST);
  }
  lv_dropdown_set_selected(bwDropdown, findBwIndex(lora_bw));

  // spreading factor dropdown
  addDropdownRow(sec1, ROW_W, "Spread Factor", DD_W_MD, sfDropdown);
  lv_dropdown_clear_options(sfDropdown);
  for (uint8_t s = 7; s <= 12; s++) {
    snprintf(buf, sizeof(buf), "SF%u", s);
    lv_dropdown_add_option(sfDropdown, buf, LV_DROPDOWN_POS_LAST);
  }
  lv_dropdown_set_selected(sfDropdown, (uint16_t)(lora_sf - 7));

  // coding rate dropdown
  addDropdownRow(sec1, ROW_W, "Coding Rate", DD_W_MD, crDropdown);
  lv_dropdown_clear_options(crDropdown);
  for (uint8_t r = 5; r <= 8; r++) {
    snprintf(buf, sizeof(buf), "4/%u", r);
    lv_dropdown_add_option(crDropdown, buf, LV_DROPDOWN_POS_LAST);
  }
  lv_dropdown_set_selected(crDropdown, (uint16_t)(lora_cr - 5));

  // TX power slider
  addSliderRow(sec1, ROW_W, "TX Power", TXP_MIN, TXP_MAX, txpSlider, txpLabel);
  lv_slider_set_value(txpSlider, lora_txp, LV_ANIM_OFF);
  snprintf(buf, sizeof(buf), "%d dBm", lora_txp);
  lv_label_set_text(txpLabel, buf);

  lv_obj_add_event_cb(txpSlider, onTxpSliderChanged, LV_EVENT_VALUE_CHANGED, this);

  // ── display section ───────────────────────────────────────────────
  lv_obj_t *sec2 = addSection(cont, SECTION_W, "Display");
  addSliderRow(sec2, ROW_W, "Backlight", BL_MIN, BL_MAX, blSlider, blLabel);
  lv_slider_set_value(blSlider, display_intensity, LV_ANIM_OFF);
  snprintf(buf, sizeof(buf), "%u/15", display_intensity);
  lv_label_set_text(blLabel, buf);
  lv_obj_add_event_cb(blSlider, onBlSliderChanged, LV_EVENT_VALUE_CHANGED, this);

  // ── system section ────────────────────────────────────────────────
  lv_obj_t *sec3 = addSection(cont, SECTION_W, "System");

  rebootBtn = lv_btn_create(sec3);
  lv_obj_set_size(rebootBtn, REBOOT_BTN_W, REBOOT_BTN_H);
  lv_obj_add_style(rebootBtn, &td_theme::style_btn, 0);
  lv_obj_add_style(rebootBtn, &td_theme::style_btn_selected, LV_STATE_FOCUSED);
  lv_obj_t *rebootLbl = lv_label_create(rebootBtn);
  lv_label_set_text(rebootLbl, "Reboot");
  lv_obj_center(rebootLbl);

  lv_obj_add_event_cb(rebootBtn, onRebootClicked, LV_EVENT_CLICKED, nullptr);
}

// ── apply changed values ──────────────────────────────────────────────
void SettingsScreen::applyChanges() {
  if (!freqSlider || !lv_obj_is_valid(freqSlider)) return;

  // Frequency
  int32_t freqVal = lv_slider_get_value(freqSlider);
  if (freqVal < FREQ_MIN) freqVal = FREQ_MIN;
  if (freqVal > FREQ_MAX) freqVal = FREQ_MAX;
  set_lora_frequency(freqVal / 100.0f);

  // Bandwidth
  if (bwDropdown && lv_obj_is_valid(bwDropdown)) {
    uint8_t bwIdx = clampBwIdx((uint8_t)lv_dropdown_get_selected(bwDropdown));
    set_lora_bandwidth(bw_values[bwIdx]);
  }

  // Spreading factor
  if (sfDropdown && lv_obj_is_valid(sfDropdown)) {
    uint8_t sfIdx = clampSfIdx((uint8_t)lv_dropdown_get_selected(sfDropdown));
    set_lora_spreading_factor((uint8_t)(7 + sfIdx));
  }

  // Coding rate
  if (crDropdown && lv_obj_is_valid(crDropdown)) {
    uint8_t crIdx = clampCrIdx((uint8_t)lv_dropdown_get_selected(crDropdown));
    set_lora_coding_rate((uint8_t)(5 + crIdx));
  }

  // TX power
  if (txpSlider && lv_obj_is_valid(txpSlider)) {
    int32_t txpVal = lv_slider_get_value(txpSlider);
    if (txpVal < TXP_MIN) txpVal = TXP_MIN;
    if (txpVal > TXP_MAX) txpVal = TXP_MAX;
    set_lora_tx_power((int8_t)txpVal);
  }

  // Backlight
  if (blSlider && lv_obj_is_valid(blSlider)) {
    int32_t blVal = lv_slider_get_value(blSlider);
    if (blVal < BL_MIN) blVal = BL_MIN;
    if (blVal > BL_MAX) blVal = BL_MAX;
    set_display_intensity((uint8_t)blVal);
  }
}

void SettingsScreen::update() {
  // read-only refresh is lightweight; no auto-update needed during editing
}

void SettingsScreen::enter() {
  // sync sliders with current firmware state
  if (freqSlider && lv_obj_is_valid(freqSlider))
    lv_slider_set_value(freqSlider, (int32_t)(lora_freq * 100), LV_ANIM_OFF);
  if (bwDropdown && lv_obj_is_valid(bwDropdown))
    lv_dropdown_set_selected(bwDropdown, findBwIndex(lora_bw));
  if (sfDropdown && lv_obj_is_valid(sfDropdown))
    lv_dropdown_set_selected(sfDropdown, (uint16_t)(lora_sf - 7));
  if (crDropdown && lv_obj_is_valid(crDropdown))
    lv_dropdown_set_selected(crDropdown, (uint16_t)(lora_cr - 5));
  if (txpSlider && lv_obj_is_valid(txpSlider))
    lv_slider_set_value(txpSlider, lora_txp, LV_ANIM_OFF);
  if (blSlider && lv_obj_is_valid(blSlider))
    lv_slider_set_value(blSlider, display_intensity, LV_ANIM_OFF);
}

void SettingsScreen::leave() {
  // Apply any pending changes when navigating away
  applyChanges();
}

#endif // BOARD_TDECK