// Copyright (C) 2024, Mark Qvist
// T-Deck LVGL Theme implementation

#include "UITheme.h"

#ifdef BOARD_TDECK

namespace td_theme {

static bool initialized = false;
bool isInitialized() { return initialized; }

lv_style_t style_screen;
lv_style_t style_card;
lv_style_t style_title;
lv_style_t style_body;
lv_style_t style_label_muted;
lv_style_t style_btn;
lv_style_t style_btn_selected;
lv_style_t style_slider;
lv_style_t style_switch_bg;
lv_style_t style_switch_indicator;
lv_style_t style_dropdown;
lv_style_t style_dropdown_list;
lv_style_t style_list_item;

void init() {
  // ── screen ─────────────────────────────────────────────────────
  lv_style_init(&style_screen);
  lv_style_set_bg_color(&style_screen, DARK_BG);
  lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
  lv_style_set_pad_all(&style_screen, 0);

  // ── card ───────────────────────────────────────────────────────
  lv_style_init(&style_card);
  lv_style_set_bg_color(&style_card, CARD_BG);
  lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
  lv_style_set_border_color(&style_card, BORDER_DIM);
  lv_style_set_border_width(&style_card, 1);
  lv_style_set_border_opa(&style_card, LV_OPA_40);
  lv_style_set_radius(&style_card, 8);
  lv_style_set_pad_all(&style_card, 10);

  // ── title ──────────────────────────────────────────────────────
  lv_style_init(&style_title);
  lv_style_set_text_color(&style_title, TEXT_TITLE);
  lv_style_set_text_font(&style_title, &lv_font_montserrat_16);

  // ── body ───────────────────────────────────────────────────────
  lv_style_init(&style_body);
  lv_style_set_text_color(&style_body, TEXT_BODY);
  lv_style_set_text_font(&style_body, &lv_font_montserrat_14);

  // ── muted label ────────────────────────────────────────────────
  lv_style_init(&style_label_muted);
  lv_style_set_text_color(&style_label_muted, TEXT_MUTED);
  lv_style_set_text_font(&style_label_muted, &lv_font_montserrat_12);

  // ── button ─────────────────────────────────────────────────────
  lv_style_init(&style_btn);
  lv_style_set_bg_color(&style_btn, CARD_BG);
  lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
  lv_style_set_border_color(&style_btn, BORDER_DIM);
  lv_style_set_border_width(&style_btn, 1);
  lv_style_set_border_opa(&style_btn, LV_OPA_50);
  lv_style_set_radius(&style_btn, 6);
  lv_style_set_pad_all(&style_btn, 8);
  lv_style_set_text_color(&style_btn, TEXT_BODY);
  lv_style_set_text_font(&style_btn, &lv_font_montserrat_14);

  // ── selected button / focus ────────────────────────────────────
  lv_style_init(&style_btn_selected);
  lv_style_set_bg_color(&style_btn_selected, ACCENT_DIM);
  lv_style_set_bg_opa(&style_btn_selected, LV_OPA_COVER);
  lv_style_set_border_color(&style_btn_selected, ACCENT);
  lv_style_set_border_width(&style_btn_selected, 2);
  lv_style_set_radius(&style_btn_selected, 6);
  lv_style_set_pad_all(&style_btn_selected, 8);
  lv_style_set_text_color(&style_btn_selected, LV_COLOR_MAKE(0xFF, 0xFF, 0xFF));
  lv_style_set_text_font(&style_btn_selected, &lv_font_montserrat_14);

  // ── slider ─────────────────────────────────────────────────────
  lv_style_init(&style_slider);
  lv_style_set_bg_color(&style_slider, CARD_BG);
  lv_style_set_bg_opa(&style_slider, LV_OPA_COVER);
  lv_style_set_border_color(&style_slider, BORDER_DIM);
  lv_style_set_border_width(&style_slider, 1);
  lv_style_set_radius(&style_slider, 4);
  lv_style_set_outline_color(&style_slider, ACCENT);
  lv_style_set_outline_width(&style_slider, 2);

  // ── switch background ──────────────────────────────────────────
  lv_style_init(&style_switch_bg);
  lv_style_set_bg_color(&style_switch_bg, LV_COLOR_MAKE(0x40, 0x40, 0x48));
  lv_style_set_radius(&style_switch_bg, LV_RADIUS_CIRCLE);

  // ── switch indicator ───────────────────────────────────────────
  lv_style_init(&style_switch_indicator);
  lv_style_set_bg_color(&style_switch_indicator, ACCENT);
  lv_style_set_radius(&style_switch_indicator, LV_RADIUS_CIRCLE);

  // ── dropdown ───────────────────────────────────────────────────
  lv_style_init(&style_dropdown);
  lv_style_set_bg_color(&style_dropdown, CARD_BG);
  lv_style_set_border_color(&style_dropdown, BORDER_DIM);
  lv_style_set_border_width(&style_dropdown, 1);
  lv_style_set_radius(&style_dropdown, 6);
  lv_style_set_pad_all(&style_dropdown, 8);
  lv_style_set_text_color(&style_dropdown, TEXT_BODY);
  lv_style_set_text_font(&style_dropdown, &lv_font_montserrat_14);

  // ── dropdown list ──────────────────────────────────────────────
  lv_style_init(&style_dropdown_list);
  lv_style_set_bg_color(&style_dropdown_list, DARK_BG2);
  lv_style_set_border_color(&style_dropdown_list, BORDER_DIM);
  lv_style_set_border_width(&style_dropdown_list, 1);
  lv_style_set_radius(&style_dropdown_list, 6);
  lv_style_set_pad_all(&style_dropdown_list, 4);
  lv_style_set_text_color(&style_dropdown_list, TEXT_BODY);
  lv_style_set_text_font(&style_dropdown_list, &lv_font_montserrat_14);

  // ── list item ──────────────────────────────────────────────────
  lv_style_init(&style_list_item);
  lv_style_set_bg_color(&style_list_item, CARD_BG);
  lv_style_set_bg_opa(&style_list_item, LV_OPA_COVER);
  lv_style_set_border_color(&style_list_item, BORDER_DIM);
  lv_style_set_border_width(&style_list_item, 1);
  lv_style_set_radius(&style_list_item, 6);
  lv_style_set_pad_left(&style_list_item, 12);
  lv_style_set_pad_right(&style_list_item, 12);
  lv_style_set_pad_top(&style_list_item, 10);
  lv_style_set_pad_bottom(&style_list_item, 10);

  initialized = true;
}

} // namespace td_theme

#endif // BOARD_TDECK