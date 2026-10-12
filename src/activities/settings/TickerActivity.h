#pragma once
#include <string>

#include "activities/Activity.h"
#include "activities/UiListActivity.h"

// Stock ticker app: settings rows plus "Start ticker mode", which hands off to
// the low-power wake cycle in main.cpp (fetch, draw, deep sleep, repeat).
class TickerActivity final : public UiListActivity {
 public:
  explicit TickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 10;

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void refreshNow();

  bool usedWifi = false;
  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};

// Full-screen preview of the ticker screen. Confirm/Down pages through the pages
// (symbols in the Rotate layout, plus the custom image); Back returns.
class TickerPreviewActivity final : public Activity {
 public:
  TickerPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fetchFailed)
      : Activity("TickerPreview", renderer, mappedInput), fetchFailed(fetchFailed) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  bool fetchFailed;
  int page = 0;
};
