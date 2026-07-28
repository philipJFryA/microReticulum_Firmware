// Copyright (C) 2024, Mark Qvist
// Abstract base class for all T-Deck LVGL screens

#pragma once
#ifdef BOARD_TDECK

#include <lvgl.h>

class BaseScreen {
public:
  virtual ~BaseScreen() = default;

  // Called once when the screen object is first created.
  // Must set `container` to a valid lv_obj_t* before returning.
  virtual void create(lv_obj_t *parent) = 0;

  // Called every loop tick while this screen is active.
  virtual void update() {}

  // Called when the screen becomes visible (after tab switch).
  virtual void enter() {}

  // Called when the screen is hidden (tab switch away).
  virtual void leave() {}

  // ── lifecycle helpers ───────────────────────────────────────────
  bool isCreated()  const { return _created && container != nullptr && lv_obj_is_valid(container); }
  bool isVisible()  const { return isCreated() && !lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN); }

  // Safe create — skips if already created
  void createIfNeeded(lv_obj_t *parent) {
    if (!isCreated()) { create(parent); _created = true; }
  }

  // Show / hide with lifecycle callbacks
  void show() {
    if (!isCreated()) return;
    lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
    enter();
  }
  void hide() {
    if (!isCreated()) return;
    leave();
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
  }

  // The top-level LVGL object for this screen.
  lv_obj_t *container = nullptr;

private:
  bool _created = false;
};

#endif // BOARD_TDECK