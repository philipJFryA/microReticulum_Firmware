// Copyright (C) 2024, Mark Qvist
// T-Deck dashboard implementation

#include <Arduino.h>
#include "DashboardScreen.h"
#include "../UITheme.h"
#include "ScreenBuilder.h"

#ifdef BOARD_TDECK

using namespace td_theme::Layout;

#include "../../firmware_globals.h"

// ── dirty‑tracking state ──────────────────────────────────────────────
static uint32_t lastUpdateMs      = 0;
static uint32_t startMs           = 0;
static float    lastFreq           = 0;
static float    lastBw             = 0;
static uint8_t  lastSf             = 0;
static uint8_t  lastCr             = 0;
static int8_t   lastTxp            = 0;
static bool     lastRadioOnline    = false;
static uint32_t lastPacketsRx      = 0;
static uint32_t lastPacketsTx      = 0;

static constexpr uint32_t UPDATE_MS_FAST   = 250;   // uptime
static constexpr uint32_t UPDATE_MS_SLOW   = 5000;  // battery, temp, packets

void DashboardScreen::create(lv_obj_t *parent) {
  container = lv_obj_create(parent);
  lv_obj_add_style(container, &td_theme::style_screen, 0);
  lv_obj_set_size(container, SCREEN_W, SCREEN_H);
  lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

  // ── title row ────────────────────────────────────────────────────
  titleLabel = lv_label_create(container);
  lv_label_set_text(titleLabel, "RNode T-Deck");
  lv_obj_add_style(titleLabel, &td_theme::style_title, 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, PAD_TITLE_X, PAD_TITLE_Y);

  boardLabel = lv_label_create(container);
  lv_label_set_text(boardLabel, "");
  lv_obj_add_style(boardLabel, &td_theme::style_label_muted, 0);
  lv_obj_align_to(boardLabel, titleLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

  // ── radio parameters box (left column) ───────────────────────────
  lv_obj_t *box1 = lv_obj_create(container);
  lv_obj_add_style(box1, &td_theme::style_card, 0);
  lv_obj_set_size(box1, DASH_COL_W, DASH_COL_H);
  lv_obj_align(box1, LV_ALIGN_LEFT_MID, DASH_COL_XL, DASH_COL_Y);
  lv_obj_set_flex_flow(box1, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box1, PAD_ROW_SM, 0);

  td_ui::Build::addRow(box1, DASH_COL_W - 12, "Frequency",   frequencyLabel);
  td_ui::Build::addRow(box1, DASH_COL_W - 12, "Bandwidth",   bandwidthLabel);
  td_ui::Build::addRow(box1, DASH_COL_W - 12, "Spread Fctr", spreadingFactorLabel);
  td_ui::Build::addRow(box1, DASH_COL_W - 12, "Coding Rate", codingRateLabel);
  td_ui::Build::addRow(box1, DASH_COL_W - 12, "TX Power",    txPowerLabel);

  // ── status box (right column) ────────────────────────────────────
  lv_obj_t *box2 = lv_obj_create(container);
  lv_obj_add_style(box2, &td_theme::style_card, 0);
  lv_obj_set_size(box2, DASH_COL_W, DASH_COL_H);
  lv_obj_align(box2, LV_ALIGN_RIGHT_MID, DASH_COL_XR, DASH_COL_Y);
  lv_obj_set_flex_flow(box2, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box2, PAD_ROW_SM, 0);

  td_ui::Build::addRow(box2, DASH_COL_W - 12, "State",       stateLabel);
  td_ui::Build::addRow(box2, DASH_COL_W - 12, "Uptime",      uptimeLabel);
  td_ui::Build::addRow(box2, DASH_COL_W - 12, "Air Util",    airUtilLabel);
  td_ui::Build::addRow(box2, DASH_COL_W - 12, "Packets RX",  packetsRxLabel);
  td_ui::Build::addRow(box2, DASH_COL_W - 12, "Packets TX",  packetsTxLabel);
  td_ui::Build::addRow(box2, DASH_COL_W - 12, "Battery",     batteryLabel);
  td_ui::Build::addRow(box2, DASH_COL_W - 12, "Current",     currentLabel);
  td_ui::Build::addRow(box2, DASH_COL_W - 12, "Temp",        temperatureLabel);

  startMs = millis();
  lastUpdateMs = 0; // force first‑tick update
}

void DashboardScreen::update() {
  if (!isCreated()) return;

  uint32_t now = millis();
  bool fastTick = (now - lastUpdateMs >= UPDATE_MS_FAST);
  bool slowTick = (now - lastUpdateMs >= UPDATE_MS_SLOW) || (lastUpdateMs == 0);

  if (!fastTick && !slowTick) return;
  if (fastTick) lastUpdateMs = now;

  char buf[32];

  // ── radio params (fast) ──────────────────────────────────────────
  if (lastFreq != lora_freq || lastUpdateMs == 0) {
    lastFreq = lora_freq;
    snprintf(buf, sizeof(buf), "%.4f MHz", lora_freq);
    lv_label_set_text(frequencyLabel, buf);
  }

  if (lastBw != lora_bw || lastUpdateMs == 0) {
    lastBw = lora_bw;
    snprintf(buf, sizeof(buf), "%.0f kHz", lora_bw);
    lv_label_set_text(bandwidthLabel, buf);
  }

  if (lastSf != lora_sf || lastUpdateMs == 0) {
    lastSf = lora_sf;
    snprintf(buf, sizeof(buf), "SF%u", lora_sf);
    lv_label_set_text(spreadingFactorLabel, buf);
  }

  if (lastCr != lora_cr || lastUpdateMs == 0) {
    lastCr = lora_cr;
    snprintf(buf, sizeof(buf), "4/%u", lora_cr);
    lv_label_set_text(codingRateLabel, buf);
  }

  if (lastTxp != lora_txp || lastUpdateMs == 0) {
    lastTxp = lora_txp;
    snprintf(buf, sizeof(buf), "%d dBm", lora_txp);
    lv_label_set_text(txPowerLabel, buf);
  }

  if (lastRadioOnline != radio_online || lastUpdateMs == 0) {
    lastRadioOnline = radio_online;
    lv_label_set_text(stateLabel, radio_online ? "Online" : "Offline");
    lv_obj_set_style_text_color(stateLabel, radio_online ? td_theme::SUCCESS : td_theme::DANGER, 0);
  }

  // ── uptime (fast) ────────────────────────────────────────────────
  uint32_t sec = (now - startMs) / 1000;
  uint32_t h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
  snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
  lv_label_set_text(uptimeLabel, buf);

  // ── air utilization (slow) ───────────────────────────────────────
  if (slowTick) {
    if (lora_airtime_limit > 0) {
      snprintf(buf, sizeof(buf), "%u/%u s", 0, lora_airtime_limit);
    } else {
      snprintf(buf, sizeof(buf), "N/A");
    }
    lv_label_set_text(airUtilLabel, buf);

    if (lastPacketsRx != lora_packet_rx) {
      lastPacketsRx = lora_packet_rx;
      snprintf(buf, sizeof(buf), "%u", lora_packet_rx);
      lv_label_set_text(packetsRxLabel, buf);
    }

    if (lastPacketsTx != lora_packet_tx) {
      lastPacketsTx = lora_packet_tx;
      snprintf(buf, sizeof(buf), "%u", lora_packet_tx);
      lv_label_set_text(packetsTxLabel, buf);
    }

    // battery
    snprintf(buf, sizeof(buf), "%.0f%%", battery_percent());
    lv_label_set_text(batteryLabel, buf);

    // current
    float cur = read_battery_current();
    if (cur > -1000) {
      snprintf(buf, sizeof(buf), "%.0f mA", cur);
    } else {
      snprintf(buf, sizeof(buf), "N/A");
    }
    lv_label_set_text(currentLabel, buf);

    // temperature
    float temp = read_chip_temperature();
    if (temp > -100) {
      snprintf(buf, sizeof(buf), "%.1f °C", temp);
    } else {
      snprintf(buf, sizeof(buf), "N/A");
    }
    lv_label_set_text(temperatureLabel, buf);
  }

  // board model (static; set once)
  if (slowTick) {
    snprintf(buf, sizeof(buf), "T-Deck ESP32-S3");
    lv_label_set_text(boardLabel, buf);
  }
}

void DashboardScreen::enter() {
  // Reset last‑tick timestamp to force full refresh
  lastUpdateMs = 0;
  if (titleLabel) lv_label_set_text(titleLabel, "RNode T-Deck");
}

#endif // BOARD_TDECK