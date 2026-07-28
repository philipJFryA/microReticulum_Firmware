// Shared LVGL widget factories — one source of truth for row/slider/dropdown layout

#pragma once
#ifdef BOARD_TDECK

#include <lvgl.h>
#include "../UITheme.h"

namespace td_ui {
namespace Build {

// ── simple label + value row (used by Dashboard & Messages) ──────────
// `rowW` is the width of the row object; height defaults to Layout::ROW_H_SM.
// Returns the row container so callers can further customize alignment.
inline lv_obj_t* addRow(lv_obj_t *parent, int rowW, const char *label,
                         lv_obj_t *&outValueLabel, int rowH = td_theme::Layout::ROW_H_SM) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, rowW, rowH);
  lv_obj_add_style(row, &td_theme::style_screen, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_border_width(row, 0, 0);

  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, label);
  lv_obj_add_style(lbl, &td_theme::style_label_muted, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

  outValueLabel = lv_label_create(row);
  lv_label_set_text(outValueLabel, "--");
  lv_obj_add_style(outValueLabel, &td_theme::style_body, 0);
  lv_obj_align(outValueLabel, LV_ALIGN_RIGHT_MID, 0, 0);

  return row;
}

// ── slider + value‑label row (used by Settings) ──────────────────────
inline lv_obj_t* addSliderRow(lv_obj_t *parent, int rowW, const char *label,
                               int32_t min, int32_t max,
                               lv_obj_t *&outSlider, lv_obj_t *&outValLabel,
                               int rowH = td_theme::Layout::ROW_H_MD,
                               int sliderW = td_theme::Layout::SLIDER_W) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, rowW, rowH);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_add_style(row, &td_theme::style_screen, 0);

  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, label);
  lv_obj_add_style(lbl, &td_theme::style_label_muted, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

  outSlider = lv_slider_create(row);
  lv_obj_set_size(outSlider, sliderW, 8);
  lv_obj_align(outSlider, LV_ALIGN_RIGHT_MID, -40, 0);
  lv_slider_set_range(outSlider, min, max);
  lv_obj_add_style(outSlider, &td_theme::style_slider, 0);
  lv_obj_add_style(outSlider, &td_theme::style_slider, LV_PART_KNOB);

  outValLabel = lv_label_create(row);
  lv_obj_add_style(outValLabel, &td_theme::style_body, 0);
  lv_obj_align(outValLabel, LV_ALIGN_RIGHT_MID, 0, 0);

  return row;
}

// ── dropdown row (label left, dropdown right) ────────────────────────
inline lv_obj_t* addDropdownRow(lv_obj_t *parent, int rowW, const char *label,
                                 int ddW, lv_obj_t *&outDropdown,
                                 int rowH = 32) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, rowW, rowH);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_add_style(row, &td_theme::style_screen, 0);

  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, label);
  lv_obj_add_style(lbl, &td_theme::style_label_muted, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

  outDropdown = lv_dropdown_create(row);
  lv_obj_set_size(outDropdown, ddW, rowH);
  lv_obj_align(outDropdown, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_style(outDropdown, &td_theme::style_dropdown, 0);
  lv_obj_add_style(outDropdown, &td_theme::style_dropdown_list, LV_PART_ITEMS);

  return row;
}

// ── section builder (card‑themed flex‑column container) ──────────────
inline lv_obj_t* addSection(lv_obj_t *parent, int sectionW, const char *title,
                              int padRow = td_theme::Layout::PAD_ROW,
                              int padAll = td_theme::Layout::PAD_CARD) {
  lv_obj_t *sec = lv_obj_create(parent);
  lv_obj_set_size(sec, sectionW, LV_SIZE_CONTENT);
  lv_obj_add_style(sec, &td_theme::style_card, 0);
  lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sec, padRow, 0);
  lv_obj_set_style_pad_all(sec, padAll, 0);

  lv_obj_t *lbl = lv_label_create(sec);
  lv_label_set_text(lbl, title);
  lv_obj_add_style(lbl, &td_theme::style_title, 0);

  return sec;
}

} // namespace Build
} // namespace td_ui

#endif // BOARD_TDECK