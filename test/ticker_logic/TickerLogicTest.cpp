#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "TickerLogic.h"

using namespace ticker;

namespace {

time_t utc(int y, unsigned mo, unsigned d, int h, int mi) {
  return static_cast<time_t>(daysFromCivil(y, mo, d)) * 86400 + h * 3600 + mi * 60;
}

Config stocksOnly(uint8_t intervalMin = 15, bool marketHoursOnly = true) {
  Config c;
  c.intervalMin = intervalMin;
  c.marketHoursOnly = marketHoursOnly;
  return c;
}

std::string readFixture(const char* name) {
  std::ifstream in(std::string(TICKER_FIXTURE_DIR) + "/" + name);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// 2026-10-12 is a Monday; New York is on EDT (UTC-4): open 13:30 UTC, close 20:00 UTC.
constexpr int MON = 12;

}  // namespace

// ---- calendar and New York time ---------------------------------------------

TEST(TickerCalendar, DaysFromCivil) {
  EXPECT_EQ(daysFromCivil(1970, 1, 1), 0);
  EXPECT_EQ(daysFromCivil(2000, 3, 1), 11017);
  EXPECT_EQ(daysFromCivil(2026, 10, 12), 20738);
}

TEST(TickerCalendar, UsDaylightSavingDates) {
  EXPECT_EQ(nthSunday(2026, 3, 2), 8);
  EXPECT_EQ(nthSunday(2026, 11, 1), 1);
  EXPECT_EQ(nthSunday(2027, 3, 2), 14);
  EXPECT_EQ(nthSunday(2027, 11, 1), 7);
}

TEST(TickerCalendar, NewYorkOffsetSwitchesAtTwoAmLocal) {
  // Spring forward: 2026-03-08 02:00 EST = 07:00 UTC.
  EXPECT_EQ(nyOffsetSeconds(utc(2026, 3, 8, 6, 59)), -5 * 3600);
  EXPECT_EQ(nyOffsetSeconds(utc(2026, 3, 8, 7, 0)), -4 * 3600);
  // Fall back: 2026-11-01 02:00 EDT = 06:00 UTC.
  EXPECT_EQ(nyOffsetSeconds(utc(2026, 11, 1, 5, 59)), -4 * 3600);
  EXPECT_EQ(nyOffsetSeconds(utc(2026, 11, 1, 6, 0)), -5 * 3600);
}

TEST(TickerCalendar, MarketOpen) {
  EXPECT_FALSE(marketOpenAt(utc(2026, 10, MON, 13, 28)));  // 09:28 NY
  EXPECT_TRUE(marketOpenAt(utc(2026, 10, MON, 13, 29)));   // 09:29: one-minute early-wake tolerance
  EXPECT_TRUE(marketOpenAt(utc(2026, 10, MON, 18, 0)));    // 14:00
  EXPECT_FALSE(marketOpenAt(utc(2026, 10, MON, 20, 0)));   // 16:00 close
  EXPECT_FALSE(marketOpenAt(utc(2026, 10, 17, 16, 0)));    // Saturday
  EXPECT_TRUE(marketOpenAt(utc(2026, 1, 12, 19, 0)));      // winter, 14:00 EST
  EXPECT_FALSE(marketOpenAt(0));                           // unknown clock
}

// ---- refresh schedule -----------------------------------------------------------

TEST(TickerSchedule, StocksDuringMarketUseTheInterval) {
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, MON, 18, 0), false), 15u * 60);
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(5), utc(2026, 10, MON, 18, 0), false), 5u * 60);
}

TEST(TickerSchedule, LastIntervalLandsOnTheAfterCloseRefresh) {
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, MON, 19, 50), false), 15u * 60);  // 15:50 -> 16:05
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, MON, 20, 2), false), 3u * 60);    // 16:02 -> 16:05
}

TEST(TickerSchedule, StocksSleepToTheNextOpen) {
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, MON, 20, 5), false), (17 * 60 + 25) * 60u);  // Tue 09:30
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, MON, 12, 0), false), 90u * 60);              // same-day open
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, 16, 20, 5), false),
            ((2 * 24 + 17) * 60 + 25) * 60u);  // Friday after close -> Monday 09:30
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, 17, 16, 0), false),
            ((24 + 21) * 60 + 30) * 60u);  // Saturday noon -> Monday 09:30
}

TEST(TickerSchedule, CryptoRefreshesHourlyOffHours) {
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, MON, 18, 0), true), 15u * 60);
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, MON, 20, 5), true), 3600u);
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(), utc(2026, 10, 13, 13, 0), true), 1800u);  // capped at the 09:30 open
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(60), utc(2026, 10, 17, 16, 0), true), 3600u);
}

TEST(TickerSchedule, MarketHoursOffOrUnknownClockUseTheInterval) {
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(15, false), utc(2026, 10, 17, 16, 0), false), 15u * 60);
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(30), 0, false), 30u * 60);
}

TEST(TickerSchedule, ImageOnlyIgnoresMarketHours) {
  Config c = stocksOnly(10);
  c.customMode = CUSTOM_ONLY;
  strcpy(c.customUrl, "http://example.com/a.png");
  EXPECT_EQ(nextRefreshSeconds(c, utc(2026, 10, 17, 16, 0), false), 10u * 60);  // Saturday
}

TEST(TickerSchedule, LongIntervals) {
  static_assert(INTERVALS[INTERVAL_COUNT - 1] == 240, "4 h is the longest interval");
  // 10:00 NY + 4 h = 14:00, still before the close.
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(240), utc(2026, 10, MON, 14, 0), false), 240u * 60);
  // 13:00 NY + 4 h would pass the close: land on the 16:05 refresh instead.
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(240), utc(2026, 10, MON, 17, 0), false), (3 * 60 + 5) * 60u);
  // Crypto off-hours: the 4 h interval is longer than the hourly default, so it wins...
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(240), utc(2026, 10, MON, 20, 5), true), 240u * 60);
  // ...but never skips the next open (Tue 07:00 NY -> 09:30).
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(240), utc(2026, 10, 13, 11, 0), true), 150u * 60);
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(180, false), utc(2026, 10, 17, 16, 0), false), 180u * 60);
  EXPECT_EQ(nextRefreshSeconds(stocksOnly(120, false), 0, false), 120u * 60);
}

TEST(TickerSchedule, RotationRunsAllDayWithMarketHoursOff) {
  Config c = stocksOnly(30, false);
  c.symbolCount = 6;  // two grid pages
  c.rotateMin = 5;
  const time_t saturday = utc(2026, 10, 17, 16, 0);
  ASSERT_GT(pageCount(c), 1);
  EXPECT_TRUE(rotationActive(c, saturday, false));
  EXPECT_EQ(secondsUntilNextWake(c, saturday, saturday + 30 * 60, 0, true), 5u * 60);
}

TEST(TickerSchedule, RotationPausesOffHours) {
  EXPECT_TRUE(rotationActive(stocksOnly(), utc(2026, 10, MON, 18, 0), false));
  EXPECT_FALSE(rotationActive(stocksOnly(), utc(2026, 10, 17, 16, 0), true));
  EXPECT_TRUE(rotationActive(stocksOnly(15, false), utc(2026, 10, 17, 16, 0), false));
}

// ---- separate webhook image schedule ------------------------------------------------

namespace {
Config withImage(uint8_t stockMin, uint8_t imageMin, uint8_t mode = CUSTOM_EXTRA_PAGE) {
  Config c = stocksOnly(stockMin);
  c.customMode = mode;
  c.customIntervalMin = imageMin;
  strcpy(c.customUrl, "https://example.com/a.png");
  return c;
}
}  // namespace

TEST(TickerCustomSchedule, OwnIntervalAndRetry) {
  Config c = withImage(30, 10);
  EXPECT_EQ(nextCustomRefreshSeconds(c, true), 10u * 60);
  EXPECT_EQ(nextCustomRefreshSeconds(c, false), 5u * 60);  // retry sooner after a failure
  c.customIntervalMin = 5;
  EXPECT_EQ(nextCustomRefreshSeconds(c, false), 5u * 60);
}

TEST(TickerCustomSchedule, WakeAtTheEarlierSchedule) {
  const time_t now = utc(2026, 10, MON, 18, 0);
  const Config c = withImage(30, 10);
  EXPECT_EQ(secondsUntilNextWake(c, now, now + 30 * 60, now + 10 * 60, false), 10u * 60);
  EXPECT_EQ(secondsUntilNextWake(c, now, now + 4 * 60, now + 10 * 60, false), 4u * 60);
}

TEST(TickerCustomSchedule, MarketHoursDoNotDelayTheImage) {
  // Saturday: stocks sleep until Monday's open, the image keeps its 15-minute schedule.
  const time_t now = utc(2026, 10, 17, 16, 0);
  const Config c = withImage(15, 15);
  const time_t nextQuote = now + nextRefreshSeconds(c, now, false);
  EXPECT_GT(nextQuote - now, 24 * 3600);
  EXPECT_EQ(secondsUntilNextWake(c, now, nextQuote, now + nextCustomRefreshSeconds(c, true), false), 15u * 60);
}

TEST(TickerCustomSchedule, DisabledScheduleIsIgnored) {
  const time_t now = utc(2026, 10, MON, 18, 0);
  Config c = withImage(30, 10);
  c.customMode = CUSTOM_OFF;
  EXPECT_EQ(secondsUntilNextWake(c, now, now + 30 * 60, now + 60, false), 30u * 60);
  Config imageOnly = withImage(30, 10, CUSTOM_ONLY);
  EXPECT_EQ(secondsUntilNextWake(imageOnly, now, now + 60, now + 10 * 60, false), 10u * 60);
}

TEST(TickerCustomSchedule, RotationStepAndFloors) {
  const time_t now = utc(2026, 10, MON, 18, 0);
  Config c = withImage(30, 60);
  c.rotateMin = 5;
  EXPECT_EQ(secondsUntilNextWake(c, now, now + 30 * 60, now + 60 * 60, true), 5u * 60);
  EXPECT_EQ(secondsUntilNextWake(c, now, now + 30 * 60, now + 60 * 60, false), 30u * 60);
  EXPECT_EQ(secondsUntilNextWake(c, now, now - 100, now + 60 * 60, false), 60u);  // overdue -> 60 s floor
  EXPECT_EQ(secondsUntilNextWake(c, 0, 0, 0, false), 30u * 60);                   // unknown clock: shortest interval
}

// ---- symbols ----------------------------------------------------------------------

TEST(TickerSymbols, CopySymbolCleansAndUppercases) {
  char out[SYMBOL_LEN];
  copySymbol(out, sizeof(out), " btc-usd ");
  EXPECT_STREQ(out, "BTC-USD");
  copySymbol(out, sizeof(out), "^gspc");
  EXPECT_STREQ(out, "^GSPC");
  copySymbol(out, sizeof(out), "brk.b");
  EXPECT_STREQ(out, "BRK.B");
  copySymbol(out, sizeof(out), "ab$c<d>");
  EXPECT_STREQ(out, "ABCD");
  copySymbol(out, sizeof(out), "ABCDEFGHIJKLMNOP");
  EXPECT_STREQ(out, "ABCDEFGHIJK");  // SYMBOL_LEN - 1
}

TEST(TickerSymbols, ParseListSeparatorsAndDuplicates) {
  char out[MAX_SYMBOLS][SYMBOL_LEN] = {};
  ASSERT_EQ(parseSymbolList("intc, btc-usd;NVDA  intc", out), 3);
  EXPECT_STREQ(out[0], "INTC");
  EXPECT_STREQ(out[1], "BTC-USD");
  EXPECT_STREQ(out[2], "NVDA");
}

TEST(TickerSymbols, ParseListCapsAtMaxSymbols) {
  char out[MAX_SYMBOLS][SYMBOL_LEN] = {};
  EXPECT_EQ(parseSymbolList("A,B,C,D,E,F,G,H,I,J", out), MAX_SYMBOLS);
  EXPECT_EQ(MAX_SYMBOLS, 8);
  EXPECT_STREQ(out[MAX_SYMBOLS - 1], "H");
}

TEST(TickerSymbols, ParseListEmptyOrJunk) {
  char out[MAX_SYMBOLS][SYMBOL_LEN] = {};
  EXPECT_EQ(parseSymbolList("", out), 0);
  EXPECT_EQ(parseSymbolList(" , ;; ", out), 0);
  EXPECT_EQ(parseSymbolList("$$$", out), 0);
}

// ---- pages and custom image -----------------------------------------------------

TEST(TickerPages, GridAndRotate) {
  Config c;  // INTC, BTC-USD, grid
  EXPECT_EQ(pageCount(c), 1);
  c.layout = LAYOUT_ROTATE;
  EXPECT_EQ(pageCount(c), 2);
  c.symbolCount = 1;
  EXPECT_EQ(pageCount(c), 1);
}

TEST(TickerPages, GridIsPagedByFour) {
  Config c;
  char syms[MAX_SYMBOLS][SYMBOL_LEN] = {};
  for (int n : {1, 4, 5, 6, 8}) {
    c.symbolCount = static_cast<uint8_t>(parseSymbolList("A,B,C,D,E,F,G,H", syms) >= n ? n : 0);
    EXPECT_EQ(pageCount(c), (n + 3) / 4) << n << " symbols";
  }
  c.symbolCount = 6;
  c.customMode = CUSTOM_EXTRA_PAGE;
  strcpy(c.customUrl, "http://example.com/a.png");
  ASSERT_EQ(pageCount(c), 3);  // grid page 1, grid page 2, image
  EXPECT_FALSE(isCustomPage(c, 1));
  EXPECT_TRUE(isCustomPage(c, 2));
  c.layout = LAYOUT_ROTATE;
  EXPECT_EQ(pageCount(c), 7);  // one page per symbol, then the image
}

TEST(TickerPages, CustomImageNeedsAUrl) {
  Config c;
  c.customMode = CUSTOM_EXTRA_PAGE;
  EXPECT_FALSE(customEnabled(c));
  EXPECT_EQ(pageCount(c), 1);
  strcpy(c.customUrl, "http://example.com/a.png");
  EXPECT_TRUE(customEnabled(c));
}

TEST(TickerPages, ExtraPageComesLast) {
  Config c;
  c.layout = LAYOUT_ROTATE;
  c.customMode = CUSTOM_EXTRA_PAGE;
  strcpy(c.customUrl, "http://example.com/a.png");
  ASSERT_EQ(pageCount(c), 3);
  EXPECT_FALSE(isCustomPage(c, 0));
  EXPECT_FALSE(isCustomPage(c, 1));
  EXPECT_TRUE(isCustomPage(c, 2));
  EXPECT_TRUE(wantsQuotes(c));
}

TEST(TickerPages, ImageOnlySkipsQuotes) {
  Config c;
  c.customMode = CUSTOM_ONLY;
  strcpy(c.customUrl, "http://example.com/a.png");
  EXPECT_FALSE(wantsQuotes(c));
  EXPECT_EQ(pageCount(c), 1);
  EXPECT_TRUE(isCustomPage(c, 0));
}

// ---- Yahoo spark parsing ----------------------------------------------------------

class SparkParse : public ::testing::Test {
 protected:
  void SetUp() override {
    body = readFixture("spark_intc_btc.json");
    ASSERT_FALSE(body.empty()) << "fixture missing";
    ASSERT_FALSE(deserializeJson(doc, body));
  }
  JsonObjectConst entry(const char* symbol) const {
    for (JsonObjectConst e : doc["spark"]["result"].as<JsonArrayConst>()) {
      if (strcmp(e["symbol"] | "", symbol) == 0) return e;
    }
    return JsonObjectConst();
  }
  std::string body;
  JsonDocument doc;
};

TEST_F(SparkParse, Stock) {
  Quote q;
  ASSERT_TRUE(parseSparkEntry(entry("INTC"), q));
  EXPECT_STREQ(q.symbol, "INTC");
  EXPECT_FALSE(q.crypto);
  EXPECT_NEAR(q.price, 101.35f, 0.001f);
  EXPECT_NEAR(q.prevClose, 104.70f, 0.001f);
  EXPECT_GT(q.dayHigh, q.dayLow);
  EXPECT_GT(q.volume, 1e6f);
  EXPECT_EQ(q.sessionEnd - q.sessionStart, 390 * 60);  // 6.5 h session
  ASSERT_GT(q.pointCount, 0);
  EXPECT_LE(q.pointCount, MAX_POINTS);
  EXPECT_EQ(q.pointMinute[0], 0);
  EXPECT_NEAR(q.pointPrice[q.pointCount - 1], q.price, 0.01f);  // latest close == current price
}

TEST_F(SparkParse, CryptoIsDownsampledAndKeepsTheLatestPoint) {
  Quote q;
  ASSERT_TRUE(parseSparkEntry(entry("BTC-USD"), q));
  EXPECT_TRUE(q.crypto);
  EXPECT_GT(q.volume, 4.0e9f);  // larger than uint32: why volume is a float
  ASSERT_EQ(entry("BTC-USD")["response"][0]["timestamp"].size(), 84u);
  EXPECT_LE(q.pointCount, MAX_POINTS);
  EXPECT_GE(q.pointCount, MAX_POINTS / 2);
  EXPECT_NEAR(q.pointPrice[q.pointCount - 1], q.price, 0.01f);
  for (int i = 1; i < q.pointCount; i++) EXPECT_GT(q.pointMinute[i], q.pointMinute[i - 1]);
}

TEST(SparkParseEdge, NullClosesAreSkipped) {
  JsonDocument doc;
  deserializeJson(doc, R"({"symbol":"X","response":[{"meta":{"symbol":"X","regularMarketPrice":5,
    "currentTradingPeriod":{"regular":{"start":1000,"end":24400}}},
    "timestamp":[1000,1900,2800],"indicators":{"quote":[{"close":[4.0,null,5.0]}]}}]})");
  Quote q;
  ASSERT_TRUE(parseSparkEntry(doc.as<JsonObjectConst>(), q));
  ASSERT_EQ(q.pointCount, 2);
  EXPECT_EQ(q.pointMinute[0], 0);
  EXPECT_EQ(q.pointMinute[1], 30);
}

TEST(SparkParseEdge, MissingMetaOrPriceIsRejected) {
  JsonDocument doc;
  deserializeJson(doc, R"({"symbol":"X","response":[{}]})");
  Quote q;
  EXPECT_FALSE(parseSparkEntry(doc.as<JsonObjectConst>(), q));
  deserializeJson(doc, R"({"symbol":"X","response":[{"meta":{"regularMarketPrice":0}}]})");
  EXPECT_FALSE(parseSparkEntry(doc.as<JsonObjectConst>(), q));
}
