#include "Ticker.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <WiFi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"

namespace ticker {
namespace {

constexpr char CONFIG_PATH[] = "/.crosspoint/ticker.json";
constexpr char CACHE_PATH[] = "/.crosspoint/ticker_cache.json";
constexpr char LOG_PATH[] = "/.crosspoint/ticker_log.csv";
constexpr size_t LOG_MAX_BYTES = 64 * 1024;
constexpr time_t MIN_VALID_EPOCH = 1700000000;  // Nov 2023: anything earlier is an unset clock
constexpr uint32_t MAX_SLEEP_S = 3 * 3600;      // RC slow-clock drift: re-check against the RTC at least this often
constexpr uint32_t RETRY_S = 5 * 60;
constexpr uint8_t MAX_RETRIES = 2;
constexpr uint8_t FULL_REFRESH_EVERY = 8;

// RTC slow memory survives deep sleep (zeroed on power-on), so the wake cycle
// keeps its schedule, rotation, failure backoff and fast-join hints without SD writes.
RTC_DATA_ATTR time_t rtcWakeTarget = 0;
RTC_DATA_ATTR time_t rtcNextFetch = 0;
RTC_DATA_ATTR time_t rtcLastNtp = 0;
RTC_DATA_ATTR uint8_t rtcRotation = 0;
RTC_DATA_ATTR uint8_t rtcFailCount = 0;
RTC_DATA_ATTR uint8_t rtcRefreshCount = 0;
RTC_DATA_ATTR bool rtcHaveBssid = false;
RTC_DATA_ATTR uint8_t rtcBssid[6] = {};
RTC_DATA_ATTR int32_t rtcChannel = 0;
RTC_DATA_ATTR char rtcBssidSsid[33] = "";

Config gConfig;
Quote gQuotes[MAX_SYMBOLS];  // static: ~350 bytes each, far over the stack budget
Quote gScratch;              // parse target, so a bad entry never clobbers a cached slot
FetchError gLastError = FetchError::None;

time_t epochOf(JsonVariantConst v) { return static_cast<time_t>(v.as<long long>()); }

void copySymbol(char* dst, size_t dstSize, const char* src) {
  size_t n = 0;
  for (; src && *src && n + 1 < dstSize; ++src) {
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(*src)));
    if (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '^' || c == '=') dst[n++] = c;
  }
  dst[n] = '\0';
}

template <size_t N>
bool inList(const uint8_t (&list)[N], uint8_t v) {
  for (const uint8_t x : list) {
    if (x == v) return true;
  }
  return false;
}

// UTC calendar date -> days since 1970-01-01 (Howard Hinnant's days_from_civil).
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
}

// Day of month of the n-th Sunday of a month (n >= 1).
int nthSunday(int year, unsigned month, int n) {
  const long days = daysFromCivil(year, month, 1);
  const int weekday = static_cast<int>((days + 4) % 7);  // 1970-01-01 was a Thursday (4)
  const int firstSunday = 1 + (7 - weekday) % 7;
  return firstSunday + 7 * (n - 1);
}

// New York local time = UTC-5, or UTC-4 from the 2nd Sunday of March 02:00
// to the 1st Sunday of November 02:00 (US rules since 2007).
time_t nyOffsetSeconds(time_t utc) {
  struct tm t;
  gmtime_r(&utc, &t);
  const int year = t.tm_year + 1900;
  const time_t dstStart = daysFromCivil(year, 3, nthSunday(year, 3, 2)) * 86400L + 7 * 3600;  // 02:00 EST
  const time_t dstEnd = daysFromCivil(year, 11, nthSunday(year, 11, 1)) * 86400L + 6 * 3600;  // 02:00 EDT
  return (utc >= dstStart && utc < dstEnd) ? -4 * 3600 : -5 * 3600;
}

constexpr int OPEN_MIN = 9 * 60 + 30;
constexpr int CLOSE_MIN = 16 * 60;
constexpr int AFTER_CLOSE_MIN = 16 * 60 + 5;  // one stock refresh shortly after the close

struct NyClock {
  time_t nyNow;       // now shifted into NY-local epoch space
  time_t nyMidnight;  // start of today, same space
  int minuteOfDay;
  int wday;
  bool weekday;
};

NyClock nyClock(time_t now) {
  NyClock c;
  c.nyNow = now + nyOffsetSeconds(now);
  struct tm ny;
  gmtime_r(&c.nyNow, &ny);
  c.minuteOfDay = ny.tm_hour * 60 + ny.tm_min;
  c.wday = ny.tm_wday;
  c.weekday = ny.tm_wday >= 1 && ny.tm_wday <= 5;
  c.nyMidnight = c.nyNow - (c.minuteOfDay * 60 + ny.tm_sec);
  return c;
}

uint32_t untilNyMinute(const NyClock& c, int dayOffset, int minute) {
  const time_t delta = c.nyMidnight + dayOffset * 86400L + minute * 60L - c.nyNow;
  return delta < 60 ? 60u : static_cast<uint32_t>(delta);
}

bool marketOpen(const NyClock& c) { return c.weekday && c.minuteOfDay >= OPEN_MIN - 1 && c.minuteOfDay < CLOSE_MIN; }

uint32_t untilNextOpen(const NyClock& c) {
  int dayOffset = (c.weekday && c.minuteOfDay < OPEN_MIN) ? 0 : 1;
  for (; dayOffset < 8; dayOffset++) {
    const int wday = (c.wday + dayOffset) % 7;
    if (wday >= 1 && wday <= 5) break;
  }
  return untilNyMinute(c, dayOffset, OPEN_MIN);
}

}  // namespace

Config& config() { return gConfig; }
Quote* quotes() { return gQuotes; }

void setSymbols(const char* list) {
  char parsed[MAX_SYMBOLS][SYMBOL_LEN] = {};
  int count = 0;
  char token[SYMBOL_LEN * 2];
  size_t t = 0;
  for (const char* p = list;; ++p) {
    if (*p == ',' || *p == ' ' || *p == ';' || *p == '\0') {
      token[t] = '\0';
      if (t > 0 && count < MAX_SYMBOLS) {
        copySymbol(parsed[count], SYMBOL_LEN, token);
        bool dup = parsed[count][0] == '\0';
        for (int i = 0; i < count && !dup; i++) dup = strcmp(parsed[i], parsed[count]) == 0;
        if (!dup) count++;
      }
      t = 0;
      if (*p == '\0') break;
    } else if (t + 1 < sizeof(token)) {
      token[t++] = *p;
    }
  }
  if (count == 0) return;
  memcpy(gConfig.symbols, parsed, sizeof(parsed));
  gConfig.symbolCount = static_cast<uint8_t>(count);
  rtcRotation = 0;
}

void formatSymbols(char* buf, size_t n, const Config& cfg) {
  size_t used = 0;
  buf[0] = '\0';
  for (int i = 0; i < cfg.symbolCount && used < n; i++) {
    used += snprintf(buf + used, n - used, "%s%s", i ? ", " : "", cfg.symbols[i]);
  }
}

bool loadConfig() {
  JsonDocument doc;
  if (!Storage.exists(CONFIG_PATH) || !PersistableStoreBase::readDocFromFile(CONFIG_PATH, doc)) return false;
  JsonArrayConst syms = doc["symbols"].as<JsonArrayConst>();
  if (!syms.isNull()) {
    int count = 0;
    for (JsonVariantConst s : syms) {
      if (count >= MAX_SYMBOLS) break;
      copySymbol(gConfig.symbols[count], SYMBOL_LEN, s | "");
      if (gConfig.symbols[count][0]) count++;
    }
    if (count > 0) gConfig.symbolCount = static_cast<uint8_t>(count);
  } else if (doc["symbol"].is<const char*>()) {  // single-symbol config from the first version
    copySymbol(gConfig.symbols[0], SYMBOL_LEN, doc["symbol"] | "INTC");
    gConfig.symbolCount = 1;
  }
  const uint8_t layout = doc["layout"] | 0;
  gConfig.layout = layout < LAYOUT_COUNT ? layout : LAYOUT_GRID;
  const uint8_t rotate = doc["rotateMin"] | 0;
  gConfig.rotateMin = inList(ROTATE_STEPS, rotate) ? rotate : 0;
  const uint8_t interval = doc["intervalMin"] | 15;
  gConfig.intervalMin = inList(INTERVALS, interval) ? interval : 15;
  gConfig.marketHoursOnly = doc["marketHoursOnly"] | true;
  const uint8_t o = doc["orientation"] | 0;
  gConfig.orientation = o < ORIENTATION_COUNT ? o : PORTRAIT;
  gConfig.active = doc["active"] | false;
  return true;
}

bool saveConfig() {
  JsonDocument doc;
  JsonArray syms = doc["symbols"].to<JsonArray>();
  for (int i = 0; i < gConfig.symbolCount; i++) syms.add(gConfig.symbols[i]);
  doc["layout"] = gConfig.layout;
  doc["rotateMin"] = gConfig.rotateMin;
  doc["intervalMin"] = gConfig.intervalMin;
  doc["marketHoursOnly"] = gConfig.marketHoursOnly;
  doc["orientation"] = gConfig.orientation;
  doc["active"] = gConfig.active;
  return PersistableStoreBase::writeDocToFile(CONFIG_PATH, doc);
}

const char* orientationLabel(uint8_t o) {
  switch (o) {
    case LANDSCAPE_LEFT:
      return tr(STR_LANDSCAPE_CCW);
    case LANDSCAPE_RIGHT:
      return tr(STR_LANDSCAPE_CW);
    case PORTRAIT:
    default:
      return tr(STR_PORTRAIT);
  }
}

const char* layoutLabel(uint8_t l) {
  return l == LAYOUT_ROTATE ? tr(STR_TICKER_LAYOUT_ROTATE) : tr(STR_TICKER_LAYOUT_GRID);
}

bool isCrypto(const char* symbol) {
  for (const Quote& q : gQuotes) {
    if (q.valid && strcmp(q.symbol, symbol) == 0) return q.crypto;
  }
  return strchr(symbol, '-') != nullptr;  // Yahoo crypto pairs look like BTC-USD
}

bool anyCrypto(const Config& cfg) {
  for (int i = 0; i < cfg.symbolCount; i++) {
    if (isCrypto(cfg.symbols[i])) return true;
  }
  return false;
}

// Cache slots follow cfg.symbols; entries for symbols no longer configured are dropped.
bool loadCache() {
  for (Quote& q : gQuotes) q = Quote{};
  JsonDocument doc;
  if (!Storage.exists(CACHE_PATH) || !PersistableStoreBase::readDocFromFile(CACHE_PATH, doc)) return false;
  bool any = false;
  for (JsonObjectConst o : doc["quotes"].as<JsonArrayConst>()) {
    char sym[SYMBOL_LEN];
    copySymbol(sym, sizeof(sym), o["symbol"] | "");
    int slot = -1;
    for (int i = 0; i < gConfig.symbolCount; i++) {
      if (strcmp(gConfig.symbols[i], sym) == 0) slot = i;
    }
    if (slot < 0) continue;
    Quote& q = gQuotes[slot];
    memcpy(q.symbol, sym, sizeof(sym));
    q.crypto = o["crypto"] | false;
    q.price = o["price"] | 0.0f;
    q.prevClose = o["prevClose"] | 0.0f;
    q.dayHigh = o["dayHigh"] | 0.0f;
    q.dayLow = o["dayLow"] | 0.0f;
    q.volume = o["volume"] | 0.0f;
    q.marketTime = epochOf(o["marketTime"]);
    q.sessionStart = epochOf(o["sessionStart"]);
    q.sessionEnd = epochOf(o["sessionEnd"]);
    q.fetchedAt = epochOf(o["fetchedAt"]);
    for (JsonArrayConst p : o["points"].as<JsonArrayConst>()) {
      if (q.pointCount >= MAX_POINTS) break;
      q.pointMinute[q.pointCount] = p[0] | 0;
      q.pointPrice[q.pointCount] = p[1] | 0.0f;
      q.pointCount++;
    }
    q.valid = q.price > 0;
    any |= q.valid;
  }
  return any;
}

bool saveCache() {
  JsonDocument doc;
  JsonArray arr = doc["quotes"].to<JsonArray>();
  for (int i = 0; i < gConfig.symbolCount; i++) {
    const Quote& q = gQuotes[i];
    if (!q.valid) continue;
    JsonObject o = arr.add<JsonObject>();
    o["symbol"] = q.symbol;
    o["crypto"] = q.crypto;
    o["price"] = q.price;
    o["prevClose"] = q.prevClose;
    o["dayHigh"] = q.dayHigh;
    o["dayLow"] = q.dayLow;
    o["volume"] = q.volume;
    o["marketTime"] = static_cast<long long>(q.marketTime);
    o["sessionStart"] = static_cast<long long>(q.sessionStart);
    o["sessionEnd"] = static_cast<long long>(q.sessionEnd);
    o["fetchedAt"] = static_cast<long long>(q.fetchedAt);
    JsonArray pts = o["points"].to<JsonArray>();
    for (int k = 0; k < q.pointCount; k++) {
      JsonArray p = pts.add<JsonArray>();
      p.add(q.pointMinute[k]);
      p.add(q.pointPrice[k]);
    }
  }
  return PersistableStoreBase::writeDocToFile(CACHE_PATH, doc);
}

time_t nowUtc() {
  struct tm local;
  if (halClock.localTime(local)) {
    const time_t t = mktime(&local);  // inverse of the localtime_r() HalClock applied
    if (t >= MIN_VALID_EPOCH) return t;
  }
  const time_t sys = time(nullptr);
  return sys >= MIN_VALID_EPOCH ? sys : 0;
}

bool ntpDue(time_t now) { return now == 0 || rtcLastNtp == 0 || now - rtcLastNtp > 86400; }

void markNtpSynced(time_t now) { rtcLastNtp = now; }

bool connectWifi(uint32_t timeoutMs) {
  // The radio does not start at the idle low-power CPU clock; setPowerSaving() keeps
  // full speed for as long as Wi-Fi stays on.
  powerManager.setPowerSaving(false);
  gLastError = FetchError::Wifi;  // cleared by a successful fetch
  WIFI_STORE.loadFromFile();
  const size_t count = WIFI_STORE.getCredentialCount();
  if (count == 0) {
    LOG_ERR("TICK", "No saved Wi-Fi networks");
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  // Last-connected network first, then the rest; no scan, which saves ~2s of radio time per wake.
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  for (size_t pass = 0; pass <= count; pass++) {
    std::optional<WifiCredential> cred;
    if (pass == 0) {
      if (last.empty()) continue;
      cred = WIFI_STORE.findCredential(last);
    } else {
      cred = WIFI_STORE.getCredentialAt(pass - 1);
      if (cred && cred->ssid == last) continue;
    }
    if (!cred) continue;

    const bool useHint = rtcHaveBssid && cred->ssid == rtcBssidSsid;
    LOG_INF("TICK", "Joining %s%s", cred->ssid.c_str(), useHint ? " (cached BSSID)" : "");
    if (useHint) {
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str(), rtcChannel, rtcBssid);
    } else {
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    }
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(50);

    if (WiFi.status() == WL_CONNECTED) {
      const uint8_t* bssid = WiFi.BSSID();
      if (bssid) {
        memcpy(rtcBssid, bssid, sizeof(rtcBssid));
        rtcChannel = WiFi.channel();
        snprintf(rtcBssidSsid, sizeof(rtcBssidSsid), "%s", cred->ssid.c_str());
        rtcHaveBssid = true;
      }
      LOG_INF("TICK", "Wi-Fi up in %lu ms", millis() - start);
      return true;
    }
    if (useHint) rtcHaveBssid = false;  // AP moved channel or changed; next wake joins normally
    WiFi.disconnect();
  }
  LOG_ERR("TICK", "Wi-Fi join failed");
  return false;
}

void wifiOff() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

namespace {
// Fills gScratch from one spark result entry. Points are evenly downsampled to MAX_POINTS.
bool parseSparkEntry(JsonObjectConst entry) {
  JsonObjectConst resp = entry["response"][0];
  JsonObjectConst meta = resp["meta"];
  if (meta.isNull()) return false;

  Quote& q = gScratch;
  q = Quote{};
  copySymbol(q.symbol, sizeof(q.symbol), entry["symbol"] | (meta["symbol"] | ""));
  q.crypto = strcmp(meta["instrumentType"] | "", "CRYPTOCURRENCY") == 0;
  q.price = meta["regularMarketPrice"] | 0.0f;
  q.prevClose = meta["chartPreviousClose"] | (meta["previousClose"] | 0.0f);
  q.dayHigh = meta["regularMarketDayHigh"] | 0.0f;
  q.dayLow = meta["regularMarketDayLow"] | 0.0f;
  q.volume = meta["regularMarketVolume"] | 0.0f;
  q.marketTime = epochOf(meta["regularMarketTime"]);
  q.sessionStart = epochOf(meta["currentTradingPeriod"]["regular"]["start"]);
  q.sessionEnd = epochOf(meta["currentTradingPeriod"]["regular"]["end"]);

  JsonArrayConst ts = resp["timestamp"];
  JsonArrayConst closes = resp["indicators"]["quote"][0]["close"];
  const size_t n = ts.size() < closes.size() ? ts.size() : closes.size();
  const size_t stride = n > static_cast<size_t>(MAX_POINTS) ? (n + MAX_POINTS - 1) / MAX_POINTS : 1;
  for (size_t i = 0; i < n && q.pointCount < MAX_POINTS; i++) {
    const bool last = i + 1 == n;
    if (i % stride != 0 && !last) continue;  // keep the latest point even when striding
    if (closes[i].isNull()) continue;        // Yahoo emits null for intervals without trades
    const time_t t = epochOf(ts[i]);
    if (q.sessionStart && t < q.sessionStart) continue;
    q.pointMinute[q.pointCount] = static_cast<uint16_t>(q.sessionStart ? (t - q.sessionStart) / 60 : i * 15);
    q.pointPrice[q.pointCount] = closes[i] | 0.0f;
    q.pointCount++;
  }
  q.valid = q.price > 0;
  return q.valid;
}
}  // namespace

bool fetchQuotes(const Config& cfg, time_t now) {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("TICK", "Not enough heap for TLS: %u free, %u max", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    gLastError = FetchError::Memory;
    return false;
  }
  char url[192];
  int n = snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/spark?symbols=");
  for (int i = 0; i < cfg.symbolCount && n < static_cast<int>(sizeof(url)); i++) {
    n += snprintf(url + n, sizeof(url) - n, "%s%s", i ? "," : "", cfg.symbols[i]);
  }
  if (n < static_cast<int>(sizeof(url))) snprintf(url + n, sizeof(url) - n, "&range=1d&interval=15m");

  // ~2 KB per stock and ~6 KB per crypto pair; freed before the JSON document is built up.
  std::string body;
  body.reserve(8192);
  if (!HttpDownloader::fetchUrl(url, body)) {
    LOG_ERR("TICK", "Fetch failed");
    gLastError = FetchError::Network;
    return false;
  }

  JsonDocument filter;
  JsonObject fe = filter["spark"]["result"].add<JsonObject>();
  fe["symbol"] = true;
  JsonObject fr = fe["response"].add<JsonObject>();
  JsonObject fm = fr["meta"].to<JsonObject>();
  for (const char* key : {"symbol", "instrumentType", "regularMarketPrice", "chartPreviousClose", "previousClose",
                          "regularMarketDayHigh", "regularMarketDayLow", "regularMarketVolume", "regularMarketTime"}) {
    fm[key] = true;
  }
  fm["currentTradingPeriod"]["regular"]["start"] = true;
  fm["currentTradingPeriod"]["regular"]["end"] = true;
  fr["timestamp"] = true;
  fr["indicators"]["quote"].add<JsonObject>()["close"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  body.clear();
  body.shrink_to_fit();
  if (err) {
    LOG_ERR("TICK", "JSON parse: %s", err.c_str());
    gLastError = FetchError::Data;
    return false;
  }

  int updated = 0;
  for (JsonObjectConst entry : doc["spark"]["result"].as<JsonArrayConst>()) {
    if (!parseSparkEntry(entry)) continue;
    for (int i = 0; i < cfg.symbolCount; i++) {
      if (strcmp(cfg.symbols[i], gScratch.symbol) != 0) continue;
      gScratch.fetchedAt = now;
      gQuotes[i] = gScratch;
      updated++;
      LOG_INF("TICK", "%s %.2f (prev %.2f), %u points%s", gScratch.symbol, gScratch.price, gScratch.prevClose,
              gScratch.pointCount, gScratch.crypto ? ", crypto" : "");
    }
  }
  if (updated == 0) LOG_ERR("TICK", "No usable quotes in response");
  gLastError = updated > 0 ? FetchError::None : FetchError::Data;
  return updated > 0;
}

FetchError lastError() { return gLastError; }

const char* errorMessage(FetchError e) {
  switch (e) {
    case FetchError::Wifi:
      return tr(STR_TICKER_ERR_WIFI);
    case FetchError::Memory:
      return tr(STR_TICKER_ERR_MEMORY);
    case FetchError::Data:
      return tr(STR_TICKER_ERR_DATA);
    case FetchError::Network:
    case FetchError::None:
    default:
      return tr(STR_TICKER_FETCH_FAILED);
  }
}

uint32_t nextRefreshSeconds(const Config& cfg, time_t now, bool hasCrypto) {
  const uint32_t interval = static_cast<uint32_t>(cfg.intervalMin) * 60u;
  if (!cfg.marketHoursOnly || now == 0) return interval;

  const NyClock c = nyClock(now);
  if (marketOpen(c)) {
    const int next = c.minuteOfDay + cfg.intervalMin;
    return next < CLOSE_MIN ? interval : untilNyMinute(c, 0, AFTER_CLOSE_MIN);
  }
  if (c.weekday && c.minuteOfDay >= CLOSE_MIN && c.minuteOfDay < AFTER_CLOSE_MIN) {
    return untilNyMinute(c, 0, AFTER_CLOSE_MIN);
  }
  const uint32_t toOpen = untilNextOpen(c);
  if (hasCrypto) {
    // Crypto never closes: keep it current at a slower off-hours rate, then resume the interval at the open.
    const uint32_t offHours = interval > CRYPTO_OFF_HOURS_S ? interval : CRYPTO_OFF_HOURS_S;
    return offHours < toOpen ? offHours : toOpen;
  }
  return toOpen;
}

// Off-hours, rotation-only wakes are skipped even with crypto in the list: the hourly
// crypto fetch still advances the page, and per-minute flips overnight cost ~10 mAh.
bool rotationActive(const Config& cfg, time_t now, bool /*hasCrypto*/) {
  if (!cfg.marketHoursOnly || now == 0) return true;
  return marketOpen(nyClock(now));
}

void setWakeTarget(time_t target) { rtcWakeTarget = target; }

int64_t secondsUntilTarget(time_t now) {
  if (now == 0 || rtcWakeTarget == 0) return 0;
  return static_cast<int64_t>(rtcWakeTarget) - static_cast<int64_t>(now);
}

void setNextFetch(time_t at) { rtcNextFetch = at; }
time_t nextFetchAt() { return rtcNextFetch; }
bool fetchDue(time_t now) { return now == 0 || rtcNextFetch == 0 || now + 90 >= rtcNextFetch; }

int rotationIndex() { return rtcRotation; }
void advanceRotation(int count) { rtcRotation = count > 0 ? static_cast<uint8_t>((rtcRotation + 1) % count) : 0; }

uint32_t noteFetchResult(bool ok) {
  if (ok) {
    rtcFailCount = 0;
    return 0;
  }
  if (rtcFailCount < MAX_RETRIES) {
    rtcFailCount++;
    return RETRY_S;
  }
  rtcFailCount = 0;
  return 0;
}

bool wantFullRefresh() { return (rtcRefreshCount++ % FULL_REFRESH_EVERY) == 0; }

uint32_t capSleep(uint32_t seconds) { return seconds > MAX_SLEEP_S ? MAX_SLEEP_S : seconds; }

void appendLog(time_t now, int battery, bool fetched, bool ok, uint32_t awakeMs) {
  bool tooBig = false;
  {
    HalFile existing;
    if (Storage.exists(LOG_PATH) && Storage.openFileForRead("TICK", LOG_PATH, existing)) {
      tooBig = existing.fileSize() > LOG_MAX_BYTES;
    }
  }
  if (tooBig) Storage.remove(LOG_PATH);

  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("TICK", "Cannot open %s", LOG_PATH);
    return;
  }
  char line[64];
  const int len = snprintf(line, sizeof(line), "%lld,%d,%d,%d,%lu\n", static_cast<long long>(now), battery,
                           fetched ? 1 : 0, ok ? 1 : 0, static_cast<unsigned long>(awakeMs));
  if (len > 0) file.write(line, static_cast<size_t>(len));
}

}  // namespace ticker
