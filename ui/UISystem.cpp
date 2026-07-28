// Copyright (C) 2024, Mark Qvist
// T-Deck LVGL UI system orchestrator

#include "UISystem.h"
#include "UITheme.h"
#include "UIDisplay.h"
#include "UIInput.h"
#include "Screen/DashboardScreen.h"
#include "Screen/MessagesScreen.h"
#include "Screen/SettingsScreen.h"
#include "Screen/LogScreen.h"

#ifdef BOARD_TDECK

#include <Arduino.h>

namespace td_ui {

// ── screen instances (created once, built lazily) ────────────────────
static DashboardScreen  dashboard;
static MessagesScreen   messages;
static SettingsScreen   settings;
static LogScreen        logScreen;

static BaseScreen *screens[PAGE_COUNT] = {
  &dashboard,
  &messages,
  &settings,
  &logScreen
};

static uint8_t     currentPage  = PAGE_DASHBOARD;
static lv_obj_t   *rootScreen   = nullptr;

// ── forward declare key event handler ─────────────────────────────────
static void keyEventHandler(lv_event_t *e);

// ── ensure a screen is built ─────────────────────────────────────────
static void buildScreenIfNeeded(uint8_t idx) {
  if (!screens[idx]) return;
  if (!rootScreen)  return;

  screens[idx]->createIfNeeded(rootScreen);

  if (!screens[idx]->isCreated()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "[UI] Screen %d create failed", idx);
    Serial.println(buf);
    LogScreen::pushLine(buf);
  }
}

// ── initialisation ────────────────────────────────────────────────────
void begin() {
  // 1. Display
  if (!td_display::init()) {
    Serial.println("[UI] Display init failed — retrying once");
    delay(200);
    if (!td_display::init()) {
      Serial.println("[UI] Display init definitively failed");
      return;
    }
  }

  // 2. Theme (idempotent — safe to call again after a soft reset)
  td_theme::init();

  if (!td_theme::isInitialized()) {
    Serial.println("[UI] Theme init failed");
    return;
  }

  // 3. Input (trackball + keyboard)
  td_input::init();

  // 4. Create the root screen
  rootScreen = lv_scr_act();
  if (!rootScreen) {
    Serial.println("[UI] LVGL active screen unavailable");
    return;
  }
  lv_obj_add_style(rootScreen, &td_theme::style_screen, 0);
  lv_obj_clear_flag(rootScreen, LV_OBJ_FLAG_SCROLLABLE);

  // 5. Build only the first page (dashboard); others are lazy
  buildScreenIfNeeded(PAGE_DASHBOARD);
  for (uint8_t i = 1; i < PAGE_COUNT; i++) {
    // Explicitly hide any placeholder container if it exists
    if (screens[i]->container) {
      lv_obj_add_flag(screens[i]->container, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // 6. Register global key event handler
  lv_obj_add_event_cb(rootScreen, [](lv_event_t *e) {
    keyEventHandler(e);
  }, LV_EVENT_KEY, nullptr);

  // 7. Set initial focus
  lv_group_t *group = lv_group_get_default();
  if (group && screens[PAGE_DASHBOARD]->container) {
    lv_group_add_obj(group, screens[PAGE_DASHBOARD]->container);
  }

  // 8. Seed log
  LogScreen::pushLine("T-Deck UI ready");

  Serial.println("[UI] Initialised");
}

// ── tick ──────────────────────────────────────────────────────────────
void update() {
  // Poll I²C keyboard
  td_input::poll();

  // Let LVGL handle timers and input devices
  lv_timer_handler();

  // Update only the currently visible screen
  if (screens[currentPage] && screens[currentPage]->isVisible()) {
    screens[currentPage]->update();
  }

  // Backlight is driven by td_display::setBacklight (set in settings screen)
}

// ── page switching ────────────────────────────────────────────────────
void showPage(int pageIndex) {
  if (pageIndex < 0 || pageIndex >= PAGE_COUNT) return;
  if (pageIndex == currentPage) return;

  // Ensure new page is built before showing it
  buildScreenIfNeeded(pageIndex);
  if (!screens[pageIndex]->isCreated()) return;

  // Hide current page
  if (screens[currentPage] && screens[currentPage]->isCreated()) {
    screens[currentPage]->hide();
  }

  // Switch
  currentPage = pageIndex;

  // Show new page
  screens[currentPage]->show();

  // Re-assign input group
  lv_group_t *group = lv_group_get_default();
  if (group && screens[currentPage]->container) {
    lv_group_remove_all_objs(group);
    lv_group_add_obj(group, screens[currentPage]->container);
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "Switched to page %d", pageIndex);
  LogScreen::pushLine(buf);
}

// ── global key handler ────────────────────────────────────────────────
static void keyEventHandler(lv_event_t *e) {
  uint32_t key = lv_event_get_key(e);

  // F1–F4: direct page switching
  if (key == td_input::KEY_F1) {
    showPage(PAGE_DASHBOARD);
    return;
  }
  if (key == td_input::KEY_F2) {
    showPage(PAGE_MESSAGES);
    return;
  }
  if (key == td_input::KEY_F3) {
    showPage(PAGE_SETTINGS);
    return;
  }
  if (key == td_input::KEY_F4) {
    showPage(PAGE_LOG);
    return;
  }

  // ESC: back to dashboard (or nested-back later)
  if (key == td_input::KEY_ESC || key == LV_KEY_ESC) {
    showPage(PAGE_DASHBOARD);
    return;
  }

  // TAB: cycle pages forward
  if (key == td_input::KEY_TAB || key == LV_KEY_NEXT) {
    showPage((currentPage + 1) % PAGE_COUNT);
    return;
  }
}

} // namespace td_ui

#endif // BOARD_TDECK