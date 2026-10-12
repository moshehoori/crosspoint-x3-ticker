#pragma once
// Stock / crypto ticker "app": config, quote cache, Wi-Fi join, Yahoo quote
// fetch (all symbols in one request), wake scheduling and the e-ink screens.
// The low-power cycle (timer wake -> fetch/rotate -> draw -> deep sleep)
// lives in main.cpp because it owns boot and sleep.

#include <cstddef>
#include <cstdint>
#include <ctime>

class GfxRenderer;

namespace ticker {

constexpr int MAX_SYMBOLS = 4;
constexpr int SYMBOL_LEN = 12;
constexpr int MAX_POINTS = 48;  // intraday points kept per symbol (24h crypto is downsampled)
constexpr uint8_t INTERVALS[] = {5, 10, 15, 30, 60};
constexpr int INTERVAL_COUNT = sizeof(INTERVALS) / sizeof(INTERVALS[0]);
constexpr uint8_t ROTATE_STEPS[] = {0, 1, 2, 5, 10};  // minutes; 0 = advance once per update
constexpr int ROTATE_STEP_COUNT = sizeof(ROTATE_STEPS) / sizeof(ROTATE_STEPS[0]);
constexpr uint32_t CRYPTO_OFF_HOURS_S = 60 * 60;  // crypto refresh while the stock market is closed

enum Orientation : uint8_t { PORTRAIT = 0, LANDSCAPE_LEFT = 1, LANDSCAPE_RIGHT = 2, ORIENTATION_COUNT };
enum Layout : uint8_t { LAYOUT_GRID = 0, LAYOUT_ROTATE = 1, LAYOUT_COUNT };

struct Config {
  char symbols[MAX_SYMBOLS][SYMBOL_LEN] = {"INTC", "BTC-USD"};
  uint8_t symbolCount = 2;
  uint8_t layout = LAYOUT_GRID;
  uint8_t rotateMin = 0;
  uint8_t intervalMin = 15;
  bool marketHoursOnly = true;
  uint8_t orientation = PORTRAIT;
  bool active = false;  // ticker mode armed: timer wakes refresh instead of booting the UI
};

struct Quote {
  bool valid = false;
  bool crypto = false;
  char symbol[SYMBOL_LEN] = "";
  float price = 0, prevClose = 0, dayHigh = 0, dayLow = 0;
  float volume = 0;         // float: crypto volumes exceed uint32
  time_t marketTime = 0;    // last trade time (UTC epoch)
  time_t sessionStart = 0;  // regular session bounds (UTC epoch); a UTC day for crypto
  time_t sessionEnd = 0;
  time_t fetchedAt = 0;
  uint8_t pointCount = 0;
  uint16_t pointMinute[MAX_POINTS] = {};  // minutes after sessionStart
  float pointPrice[MAX_POINTS] = {};
};

struct ScreenStatus {
  bool offline = false;          // last fetch failed; showing cached data
  int battery = -1;              // percent, -1 unknown
  time_t nextUpdate = 0;         // UTC epoch of the next fetch, 0 = none (preview / paused)
  const char* banner = nullptr;  // optional one-line notice
  int pageIndex = 0;             // rotate layout: which symbol is shown
};

Config& config();
bool loadConfig();
bool saveConfig();
// Parses "INTC, btc-usd" into the symbol list (uppercased, max MAX_SYMBOLS); keeps the old list when empty.
void setSymbols(const char* commaList);
void formatSymbols(char* buf, size_t n, const Config& cfg);
const char* orientationLabel(uint8_t o);
const char* layoutLabel(uint8_t l);
// Crypto per the cached quote type, or a "-" pair (BTC-USD) before the first fetch.
bool isCrypto(const char* symbol);
bool anyCrypto(const Config& cfg);

// Cached quotes, one slot per configured symbol (same order as cfg.symbols).
Quote* quotes();
bool loadCache();
bool saveCache();

// Best-known current UTC epoch (DS3231 RTC, then system time); 0 when unknown.
time_t nowUtc();
bool ntpDue(time_t now);
void markNtpSynced(time_t now);

// Join a saved network (last-connected first). Blocks up to ~timeoutMs per network.
bool connectWifi(uint32_t timeoutMs);
void wifiOff();

// One HTTPS request for every configured symbol; updates the cached slots it parsed.
bool fetchQuotes(const Config& cfg, time_t now);

// Why the last connectWifi()/fetchQuotes() failed, for an accurate on-screen message.
enum class FetchError : uint8_t { None, Wifi, Memory, Network, Data };
FetchError lastError();
const char* errorMessage(FetchError e);

// Seconds until the next fetch per config (market-hours and crypto aware), >= 60.
uint32_t nextRefreshSeconds(const Config& cfg, time_t now, bool hasCrypto);
// Whether rotation wakes should run now (they pause while nothing is refreshing).
bool rotationActive(const Config& cfg, time_t now, bool hasCrypto);

// Wake bookkeeping kept in RTC memory across deep sleep.
void setWakeTarget(time_t target);
int64_t secondsUntilTarget(time_t now);
void setNextFetch(time_t at);
time_t nextFetchAt();
bool fetchDue(time_t now);
int rotationIndex();
void advanceRotation(int count);
// Failure backoff: returns retry delay in seconds, or 0 when the normal schedule applies.
uint32_t noteFetchResult(bool ok);
// Every Nth refresh uses a full waveform to clear e-ink ghosting.
bool wantFullRefresh();
// Long sleeps are split so the RC slow clock is re-checked against the RTC.
uint32_t capSleep(uint32_t seconds);

void appendLog(time_t now, int battery, bool fetched, bool ok, uint32_t awakeMs);

// Draws the configured layout (grid of all symbols, or the rotation page in status.pageIndex).
void drawScreen(GfxRenderer& renderer, const Config& cfg, const ScreenStatus& status);

}  // namespace ticker

// Implemented in main.cpp: asks the main loop to enter ticker sleep on its next pass.
void requestTickerSleep();
