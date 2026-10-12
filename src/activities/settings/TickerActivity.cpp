#include "TickerActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <memory>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "ticker/Ticker.h"

namespace fui = freeink::ui;

namespace {
enum Row {
  ROW_START = 0,
  ROW_REFRESH,
  ROW_SYMBOLS,
  ROW_LAYOUT,
  ROW_ROTATE,
  ROW_INTERVAL,
  ROW_MARKET_HOURS,
  ROW_ORIENTATION,
  ROW_CUSTOM_MODE,
  ROW_CUSTOM_URL,
};

constexpr StrId ROW_NAMES[TickerActivity::MENU_ITEMS] = {StrId::STR_TICKER_START,        StrId::STR_TICKER_REFRESH,
                                                         StrId::STR_TICKER_SYMBOLS,      StrId::STR_TICKER_LAYOUT,
                                                         StrId::STR_TICKER_ROTATE_EVERY, StrId::STR_TICKER_INTERVAL,
                                                         StrId::STR_TICKER_MARKET_HOURS, StrId::STR_TICKER_SCREEN,
                                                         StrId::STR_TICKER_CUSTOM,       StrId::STR_TICKER_CUSTOM_URL};

template <size_t N>
uint8_t nextInList(const uint8_t (&list)[N], uint8_t current) {
  for (size_t i = 0; i < N; i++) {
    if (list[i] == current) return list[(i + 1) % N];
  }
  return list[0];
}

time_t latestFetch() {
  time_t t = 0;
  const auto& cfg = ticker::config();
  for (int i = 0; i < cfg.symbolCount; i++) {
    if (ticker::quotes()[i].valid && ticker::quotes()[i].fetchedAt > t) t = ticker::quotes()[i].fetchedAt;
  }
  return t;
}
}  // namespace

TickerActivity::TickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Ticker", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(ROW_NAMES[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* TickerActivity::headerTitle() const { return tr(STR_TICKER); }

void TickerActivity::onEnter() {
  ticker::loadConfig();
  ticker::loadCache();
  UiListActivity::onEnter();
}

void TickerActivity::onExit() {
  UiListActivity::onExit();
  // Wi-Fi + TLS fragment the heap; reboot back into Settings like the other Wi-Fi activities do.
  if (usedWifi) silentRestartToSettings();
}

void TickerActivity::activateIndex(const int index) {
  app.clearTapFlash();
  auto& cfg = ticker::config();
  switch (index) {
    case ROW_START:
      if (!gpio.deviceIsX3()) {
        LOG_ERR("TICK", "Timer wake needs the X3 (X4 sleep cuts battery power)");
        return;
      }
      cfg.active = true;
      ticker::saveConfig();
      requestTickerSleep();
      return;
    case ROW_REFRESH:
      refreshNow();
      return;
    case ROW_SYMBOLS: {
      char current[ticker::MAX_SYMBOLS * (ticker::SYMBOL_LEN + 2)];
      ticker::formatSymbols(current, sizeof(current), cfg);
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TICKER_SYMBOLS),
                                                                     current, sizeof(current) - 1, InputType::Text),
                             [](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               ticker::setSymbols(std::get<KeyboardResult>(result.data).text.c_str());
                               ticker::saveConfig();
                               ticker::loadCache();  // re-map cached quotes onto the new symbol slots
                             });
      return;
    }
    case ROW_LAYOUT:
      cfg.layout = static_cast<uint8_t>((cfg.layout + 1) % ticker::LAYOUT_COUNT);
      break;
    case ROW_ROTATE:
      cfg.rotateMin = nextInList(ticker::ROTATE_STEPS, cfg.rotateMin);
      break;
    case ROW_INTERVAL:
      cfg.intervalMin = nextInList(ticker::INTERVALS, cfg.intervalMin);
      break;
    case ROW_MARKET_HOURS:
      cfg.marketHoursOnly = !cfg.marketHoursOnly;
      break;
    case ROW_ORIENTATION:
      cfg.orientation = static_cast<uint8_t>((cfg.orientation + 1) % ticker::ORIENTATION_COUNT);
      break;
    case ROW_CUSTOM_MODE:
      cfg.customMode = static_cast<uint8_t>((cfg.customMode + 1) % ticker::CUSTOM_MODE_COUNT);
      break;
    case ROW_CUSTOM_URL:
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TICKER_CUSTOM_URL),
                                                                     cfg.customUrl[0] ? cfg.customUrl : "http://",
                                                                     ticker::CUSTOM_URL_LEN - 1, InputType::Url),
                             [](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               ticker::setCustomUrl(std::get<KeyboardResult>(result.data).text.c_str());
                               ticker::saveConfig();
                             });
      return;
    default:
      return;
  }
  ticker::saveConfig();
  requestUpdate();
}

void TickerActivity::refreshNow() {
  {
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_UPDATING));
  }
  usedWifi = true;
  bool ok = false;
  bool customDownloaded = false;
  if (ticker::connectWifi(12000)) {
    const auto& cfg = ticker::config();
    ok = true;
    if (ticker::wantsQuotes(cfg)) {
      ok = ticker::fetchQuotes(cfg, ticker::nowUtc());
      if (ok) ticker::saveCache();
    }
    if (ticker::customEnabled(cfg)) customDownloaded = ticker::downloadCustomImage(cfg);
  }
  ticker::wifiOff();
  if (ticker::customEnabled(ticker::config())) {
    ok = customDownloaded && ticker::prepareCustomImage(ticker::config()) && ok;
  }
  auto preview = makeUniqueNoThrow<TickerPreviewActivity>(renderer, mappedInput, !ok);
  if (!preview) {
    LOG_ERR("TICK", "OOM: TickerPreviewActivity");
    return;
  }
  startActivityForResult(std::move(preview), [](const ActivityResult&) {});
}

void TickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const auto& cfg = ticker::config();
  char buf[64];
  rowValues_[ROW_START].clear();
  const time_t fetched = latestFetch();
  if (fetched > 0) {
    struct tm local;
    localtime_r(&fetched, &local);
    strftime(buf, sizeof(buf), "%d/%m %H:%M", &local);
    rowValues_[ROW_REFRESH] = buf;
  } else {
    rowValues_[ROW_REFRESH] = tr(STR_NEVER);
  }
  ticker::formatSymbols(buf, sizeof(buf), cfg);
  rowValues_[ROW_SYMBOLS] = buf;
  rowValues_[ROW_LAYOUT] = ticker::layoutLabel(cfg.layout);
  if (cfg.rotateMin == 0) {
    rowValues_[ROW_ROTATE] = tr(STR_TICKER_EACH_UPDATE);
  } else {
    snprintf(buf, sizeof(buf), tr(STR_TICKER_MINUTES), cfg.rotateMin);
    rowValues_[ROW_ROTATE] = buf;
  }
  snprintf(buf, sizeof(buf), tr(STR_TICKER_MINUTES), cfg.intervalMin);
  rowValues_[ROW_INTERVAL] = buf;
  rowValues_[ROW_MARKET_HOURS] = cfg.marketHoursOnly ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  rowValues_[ROW_ORIENTATION] = ticker::orientationLabel(cfg.orientation);
  rowValues_[ROW_CUSTOM_MODE] = ticker::customModeLabel(cfg.customMode);
  rowValues_[ROW_CUSTOM_URL] = cfg.customUrl[0] ? cfg.customUrl : tr(STR_NOT_SET);
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

void TickerPreviewActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void TickerPreviewActivity::loop() {
  const auto& cfg = ticker::config();
  const bool paged = ticker::pageCount(cfg) > 1;
  if (paged && (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                mappedInput.wasReleased(MappedInputManager::Button::Right))) {
    page = (page + 1) % ticker::pageCount(cfg);
    requestUpdate();
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      (!paged && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void TickerPreviewActivity::render(RenderLock&&) {
  const auto savedOrientation = renderer.getOrientation();
  ticker::ScreenStatus status;
  status.offline = fetchFailed;
  status.battery = powerManager.getBatteryPercentage();
  status.banner = fetchFailed ? ticker::errorMessage(ticker::lastError()) : nullptr;
  status.pageIndex = page;
  ticker::drawScreen(renderer, ticker::config(), status);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  renderer.setOrientation(savedOrientation);
}
