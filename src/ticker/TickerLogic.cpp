#include "TickerLogic.h"

#include <cctype>
#include <cstring>

// Pure ticker logic: no Arduino, storage, display or network calls, so it runs
// unchanged in the host unit tests (test/ticker_logic).

namespace ticker {

time_t epochOf(JsonVariantConst v) { return static_cast<time_t>(v.as<long long>()); }

void copySymbol(char* dst, size_t dstSize, const char* src) {
  size_t n = 0;
  for (; src && *src && n + 1 < dstSize; ++src) {
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(*src)));
    if (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '^' || c == '=') dst[n++] = c;
  }
  dst[n] = '\0';
}

int parseSymbolList(const char* list, char out[MAX_SYMBOLS][SYMBOL_LEN]) {
  int count = 0;
  char token[SYMBOL_LEN * 2];
  size_t t = 0;
  for (const char* p = list;; ++p) {
    if (*p == ',' || *p == ' ' || *p == ';' || *p == '\0') {
      token[t] = '\0';
      if (t > 0 && count < MAX_SYMBOLS) {
        copySymbol(out[count], SYMBOL_LEN, token);
        bool dup = out[count][0] == '\0';
        for (int i = 0; i < count && !dup; i++) dup = strcmp(out[i], out[count]) == 0;
        if (!dup) count++;
      }
      t = 0;
      if (*p == '\0') break;
    } else if (t + 1 < sizeof(token)) {
      token[t++] = *p;
    }
  }
  return count;
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

namespace {
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

bool marketOpenAt(time_t now) { return now != 0 && marketOpen(nyClock(now)); }

bool customEnabled(const Config& cfg) { return cfg.customMode != CUSTOM_OFF && cfg.customUrl[0] != '\0'; }

bool wantsQuotes(const Config& cfg) { return !(customEnabled(cfg) && cfg.customMode == CUSTOM_ONLY); }

int pageCount(const Config& cfg) {
  if (!wantsQuotes(cfg)) return 1;
  const int base = (cfg.layout == LAYOUT_ROTATE && cfg.symbolCount > 1) ? cfg.symbolCount : 1;
  return base + (customEnabled(cfg) ? 1 : 0);
}

bool isCustomPage(const Config& cfg, int page) {
  if (!customEnabled(cfg)) return false;
  return cfg.customMode == CUSTOM_ONLY || page == pageCount(cfg) - 1;
}

uint32_t nextRefreshSeconds(const Config& cfg, time_t now, bool hasCrypto) {
  const uint32_t interval = static_cast<uint32_t>(cfg.intervalMin) * 60u;
  // An image-only setup has no market to follow.
  if (!cfg.marketHoursOnly || now == 0 || !wantsQuotes(cfg)) return interval;

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

uint32_t nextCustomRefreshSeconds(const Config& cfg, bool ok) {
  const uint32_t interval = static_cast<uint32_t>(cfg.customIntervalMin) * 60u;
  constexpr uint32_t RETRY_S = 5 * 60;
  return ok ? interval : (interval < RETRY_S ? interval : RETRY_S);
}

uint32_t secondsUntilNextWake(const Config& cfg, time_t now, time_t nextQuoteFetch, time_t nextCustomFetch,
                              bool rotationStepActive) {
  constexpr uint32_t MIN_S = 60;
  constexpr uint32_t NONE = 0xFFFFFFFFu;
  uint32_t best = NONE;
  auto consider = [&](uint32_t s) {
    if (s < MIN_S) s = MIN_S;
    if (s < best) best = s;
  };
  auto until = [&](time_t at, uint8_t intervalMin) -> uint32_t {
    if (now == 0) return static_cast<uint32_t>(intervalMin) * 60u;  // unknown clock: plain interval
    return at > now ? static_cast<uint32_t>(at - now) : MIN_S;
  };
  if (wantsQuotes(cfg)) consider(until(nextQuoteFetch, cfg.intervalMin));
  if (customEnabled(cfg)) consider(until(nextCustomFetch, cfg.customIntervalMin));
  if (rotationStepActive && cfg.rotateMin > 0) consider(static_cast<uint32_t>(cfg.rotateMin) * 60u);
  return best == NONE ? static_cast<uint32_t>(cfg.intervalMin) * 60u : best;
}

// Fills q from one spark result entry. Points are evenly downsampled to MAX_POINTS.
bool parseSparkEntry(JsonObjectConst entry, Quote& q) {
  JsonObjectConst resp = entry["response"][0];
  JsonObjectConst meta = resp["meta"];
  if (meta.isNull()) return false;

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

}  // namespace ticker
