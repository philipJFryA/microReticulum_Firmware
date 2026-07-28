// Copyright (C) 2024, Mark Qvist
// T-Deck messages screen implementation

#include "MessagesScreen.h"
#include "../UITheme.h"

#ifdef BOARD_TDECK

using namespace td_theme::Layout;

// ── extern ────────────────────────────────────────────────────────────
extern uint32_t lora_packet_rx;

// ── static storage ────────────────────────────────────────────────────
char    MessagesScreen::msgs[10][128] = {};
uint8_t MessagesScreen::msgHead  = 0;
uint8_t MessagesScreen::msgCount = 0;
uint32_t MessagesScreen::lastPacketsRx = 0;

// ── public helper ─────────────────────────────────────────────────────
void MessagesScreen::pushMessage(const char *text) {
  if (!text) return;
  snprintf(msgs[msgHead], sizeof(msgs[0]), "%s", text);
  msgHead = (msgHead + 1) % 10;
  if (msgCount < 10) msgCount++;
}

// ── rebuild list from static msgs ─────────────────────────────────────
void MessagesScreen::rebuildList() {
  if (!list) return;
  if (!lv_obj_is_valid(list)) return;

  lv_obj_clean(list);
  emptyLabel = nullptr;  // invalidated by lv_obj_clean

  if (msgCount == 0) {
    emptyLabel = lv_label_create(list);
    if (emptyLabel) {
      lv_label_set_text(emptyLabel, "No messages yet");
      lv_obj_add_style(emptyLabel, &td_theme::style_label_muted, 0);
      lv_obj_center(emptyLabel);
    }
    return;
  }

  // iterate from oldest to newest
  uint8_t oldest = msgCount < 10 ? 0 : msgHead;
  for (uint8_t i = 0; i < msgCount; i++) {
    uint8_t idx = (oldest + i) % 10;
    lv_obj_t *item = lv_obj_create(list);
    if (!item) continue;
    lv_obj_set_size(item, CONTENT_W - 8, ROW_H_LG);
    lv_obj_add_style(item, &td_theme::style_list_item, 0);

    lv_obj_t *lbl = lv_label_create(item);
    if (lbl) {
      lv_label_set_text(lbl, msgs[idx]);
      lv_obj_add_style(lbl, &td_theme::style_body, 0);
      lv_obj_center(lbl);
    }
  }
}

// ── create ────────────────────────────────────────────────────────────
void MessagesScreen::create(lv_obj_t *parent) {
  container = lv_obj_create(parent);
  lv_obj_add_style(container, &td_theme::style_screen, 0);
  lv_obj_set_size(container, SCREEN_W, SCREEN_H);
  lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

  titleLabel = lv_label_create(container);
  lv_label_set_text(titleLabel, "Messages");
  lv_obj_add_style(titleLabel, &td_theme::style_title, 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, PAD_TITLE_X, PAD_TITLE_Y);

  backBtn = lv_btn_create(container);
  lv_obj_set_size(backBtn, BACK_BTN_W, BACK_BTN_H);
  lv_obj_add_style(backBtn, &td_theme::style_btn, 0);
  lv_obj_add_style(backBtn, &td_theme::style_btn_selected, LV_STATE_FOCUSED);
  lv_obj_align(backBtn, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_obj_t *backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
  lv_obj_center(backLbl);

  list = lv_obj_create(container);
  lv_obj_set_size(list, CONTENT_W, CONTENT_H);
  lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, CONTENT_BOT);
  lv_obj_add_style(list, &td_theme::style_card, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, PAD_ROW, 0);
  lv_obj_set_style_pad_all(list, PAD_ROW, 0);

  emptyLabel = lv_label_create(list);
  lv_label_set_text(emptyLabel, "No messages yet");
  lv_obj_add_style(emptyLabel, &td_theme::style_label_muted, 0);
  lv_obj_center(emptyLabel);
}

void MessagesScreen::update() {
  // Detect new incoming announces by monitoring packet counter
  if (lora_packet_rx != lastPacketsRx) {
    // Simulated placeholder: real announce text would come from RNS
    // pushMessage is the intended public API when integrated with RNS
    lastPacketsRx = lora_packet_rx;
  }
}

void MessagesScreen::enter() {
  rebuildList();
}

#endif // BOARD_TDECK