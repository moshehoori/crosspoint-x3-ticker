#include "HelloClockActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void HelloClockActivity::onEnter() {
  Activity::onEnter();
  readTime();
  requestUpdate();
}

void HelloClockActivity::readTime() {
  struct tm t{};
  bool ok = halClock.localTime(t);
  if (!ok) {
    // No RTC (or not available): fall back to the system clock, which NTP sets after a Wi-Fi join.
    const time_t now = time(nullptr);
    localtime_r(&now, &t);
    ok = (t.tm_year + 1900) >= 2020;
  }
  char buf[48];
  if (ok) {
    strftime(buf, sizeof(buf), SETTINGS.clockFormat == 1 ? "%I:%M %p" : "%H:%M", &t);
    timeText_ = buf;
    strftime(buf, sizeof(buf), "%A, %d %B %Y", &t);
    dateText_ = buf;
    lastMinute_ = t.tm_min;
  } else {
    timeText_ = "--:--";
    dateText_ = "Clock not set (connect to Wi-Fi once)";
    lastMinute_ = -1;
  }
}

void HelloClockActivity::loop() {
  int x = 0, y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
    return;
  }
  // Poll once a second; redraw only when the displayed minute changes.
  const unsigned long now = millis();
  if (now - lastPollMs_ < 1000) return;
  lastPollMs_ = now;
  const int prev = lastMinute_;
  readTime();
  if (lastMinute_ != prev) requestUpdate();
}

void HelloClockActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HELLO_CLOCK), nullptr);

  const int helloH = renderer.getLineHeight(NOTOSERIF_18_FONT_ID);
  const int timeH = renderer.getLineHeight(NOTOSERIF_18_FONT_ID);
  const int dateH = renderer.getLineHeight(UI_10_FONT_ID);
  const int gap = metrics.verticalSpacing * 2;
  const int total = helloH + gap + timeH + gap / 2 + dateH;
  int y = (pageHeight - total) / 2;

  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, y, "Hello World", true, EpdFontFamily::BOLD);
  y += helloH + gap;
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, y, timeText_.c_str(), true, EpdFontFamily::REGULAR);
  y += timeH + gap / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, y, dateText_.c_str(), true, EpdFontFamily::REGULAR);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
