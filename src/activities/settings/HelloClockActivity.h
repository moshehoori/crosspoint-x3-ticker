#pragma once
#include <string>

#include "activities/Activity.h"

// Minimal demo "app": shows "Hello World" and the current time, refreshing
// once a minute. Back (or a tap) returns to Settings.
class HelloClockActivity final : public Activity {
 public:
  explicit HelloClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HelloClock", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  void readTime();
  std::string timeText_;
  std::string dateText_;
  int lastMinute_ = -1;
  unsigned long lastPollMs_ = 0;
};
