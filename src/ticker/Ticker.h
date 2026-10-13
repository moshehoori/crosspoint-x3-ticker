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
// Custom image page: a PNG/BMP pulled from a URL (e.g. a server that turns webhooks into an image).
enum CustomMode : uint8_t { CUSTOM_OFF = 0, CUSTOM_ONLY = 1, CUSTOM_EXTRA_PAGE = 2, CUSTOM_MODE_COUNT };
constexpr int CUSTOM_URL_LEN = 160;
constexpr char CUSTOM_IMAGE_PATH[] = "/.crosspoint/ticker_custom.bmp";

struct Config {
  char symbols[MAX_SYMBOLS][SYMBOL_LEN] = {"INTC", "BTC-USD"};
  uint8_t symbolCount = 2;
  uint8_t layout = LAYOUT_GRID;
  uint8_t rotateMin = 0;
  uint8_t intervalMin = 15;
  bool marketHoursOnly = true;
  uint8_t orientation = PORTRAIT;
  bool active = false;  // ticker mode armed: timer wakes refresh instead of booting the UI
  uint8_t customMode = CUSTOM_OFF;
  char customUrl[CUSTOM_URL_LEN] = "";
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
void setLastError(FetchError e);
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

// One CSV line per wake: epoch,battery,fetched,ok,awake_ms,event. event says what the wake did
// ("start", "fetch", "rotate", "early", "watchdog", "config-rtc", "inactive", "lowbat").
void appendLog(time_t now, int battery, bool fetched, bool ok, uint32_t awakeMs, const char* event);

// Ticker mode is also remembered in RTC memory (with a copy of the config), so a timer
// wake never depends on reading the SD card to know it should keep running.
void rememberActive(const Config& cfg);
bool activeInRtc();
// Restores the remembered config into config(); false if none.
bool restoreFromRtc();
void forgetActive();

// Pages shown in turn: the grid (1) or one per symbol, plus the custom image page if enabled.
int pageCount(const Config& cfg);
bool customEnabled(const Config& cfg);
bool isCustomPage(const Config& cfg, int page);
// Quote data is needed unless the custom image is the only page.
bool wantsQuotes(const Config& cfg);
const char* customModeLabel(uint8_t m);
void setCustomUrl(const char* url);

// Two steps so the image is decoded with Wi-Fi off: the PNG decoder and 1-bit ditherer
// need buffers that do not fit while the radio holds its memory.
// downloadCustomImage(): with Wi-Fi up, fetch the URL to a temporary file on SD.
// prepareCustomImage(): with Wi-Fi off, validate/convert it and replace the current image.
bool downloadCustomImage(const Config& cfg);
bool prepareCustomImage(const Config& cfg);
bool hasCustomImage();

// Draws page status.pageIndex: the grid, one symbol, or the custom image.
void drawScreen(GfxRenderer& renderer, const Config& cfg, const ScreenStatus& status);

}  // namespace ticker

// Implemented in main.cpp: asks the main loop to enter ticker sleep on its next pass.
void requestTickerSleep();
