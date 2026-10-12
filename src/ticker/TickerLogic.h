#pragma once
// Pure ticker logic shared by the firmware and the host unit tests (test/ticker_logic).
// No Arduino, storage, display or network dependencies; ArduinoJson is header-only.

#include <ArduinoJson.h>

#include <cstddef>
#include <ctime>

#include "Ticker.h"

namespace ticker {

time_t epochOf(JsonVariantConst v);

// Uppercases and keeps only [A-Z0-9.^=-], truncated to dstSize - 1.
void copySymbol(char* dst, size_t dstSize, const char* src);

// Splits "INTC, btc-usd;NVDA" on commas, spaces or semicolons into at most MAX_SYMBOLS
// cleaned, de-duplicated symbols. Returns how many were written.
int parseSymbolList(const char* list, char out[MAX_SYMBOLS][SYMBOL_LEN]);

// Calendar helpers (UTC). daysFromCivil: days since 1970-01-01. nthSunday: day of month.
long daysFromCivil(int y, unsigned m, unsigned d);
int nthSunday(int year, unsigned month, int n);
// New York offset from UTC in seconds: -4 h during US daylight saving time, else -5 h.
time_t nyOffsetSeconds(time_t utc);
// Regular NYSE session (Mon-Fri 09:30-16:00 New York time, holidays not modelled).
bool marketOpenAt(time_t now);

// Fills q from one entry of Yahoo's v7/finance/spark "result" array; false if unusable.
bool parseSparkEntry(JsonObjectConst entry, Quote& q);

}  // namespace ticker
