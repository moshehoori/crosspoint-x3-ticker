#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "Ticker.h"
#include "fontIds.h"
#include "images/BitcoinLogo.h"
#include "images/IntelLogo.h"

namespace ticker {
namespace {

constexpr int MARGIN = 24;

// ---- logos ---------------------------------------------------------------

struct Logo {
  const uint8_t* bits;
  int w, h;
};

// Only symbols with a bundled logo get one; the rest show their ticker text.
bool logoFor(const char* symbol, Logo& out) {
  if (strcmp(symbol, "INTC") == 0) {
    out = {IntelLogo, INTEL_LOGO_W, INTEL_LOGO_H};
    return true;
  }
  if (strncmp(symbol, "BTC-", 4) == 0) {
    out = {BitcoinLogo, BITCOIN_LOGO_W, BITCOIN_LOGO_H};
    return true;
  }
  return false;
}

// Logos are MSB-first with bit==1 as ink; plotted per pixel (div = downscale factor)
// so they follow the renderer orientation, which the byte blit in drawImage() does not.
void drawLogo(const GfxRenderer& r, const Logo& logo, int x, int y, int div) {
  const int rowBytes = (logo.w + 7) / 8;
  for (int row = 0; row < logo.h; row += div) {
    for (int col = 0; col < logo.w; col += div) {
      if (logo.bits[row * rowBytes + (col >> 3)] & (0x80 >> (col & 7))) r.drawPixel(x + col / div, y + row / div, true);
    }
  }
}

// ---- big seven-segment digits ------------------------------------------------

// bit0..6 = a(top) b(top-right) c(bottom-right) d(bottom) e(bottom-left) f(top-left) g(middle).
constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

struct BigMetrics {
  int digitW, thick, gap;
};

BigMetrics bigMetrics(int h) {
  const int thick = h / 9 < 3 ? 3 : h / 9;
  return {h * 11 / 20, thick, thick + thick / 2};
}

// Per-glyph advance: "1" and "." are narrow so "101.07" doesn't read as "1 0 1".
int glyphWidth(char c, const BigMetrics& m) {
  if (c == '.') return m.thick * 2;
  if (c == '1') return m.thick * 2;
  return m.digitW;
}

int bigTextWidth(const char* s, int h) {
  const BigMetrics m = bigMetrics(h);
  int w = 0;
  for (; *s; ++s) w += glyphWidth(*s, m) + m.gap;
  return w > 0 ? w - m.gap : 0;
}

// Largest digit height <= maxH whose text fits maxW.
int fitBigHeight(const char* s, int maxW, int maxH) {
  int h = maxH;
  while (h > 18 && bigTextWidth(s, h) > maxW) h -= 2;
  return h;
}

// Built-in fonts top out at 18pt, too small for a price read across a room.
void drawBigText(const GfxRenderer& r, int x, int y, int h, const char* s) {
  const BigMetrics m = bigMetrics(h);
  const int half = h / 2;
  const int t = m.thick;
  for (; *s; ++s) {
    if (*s == '.') {
      r.fillRect(x, y + h - t, t, t, true);
      x += t * 2 + m.gap;
      continue;
    }
    if (*s < '0' || *s > '9') continue;
    if (*s == '1') {  // single full-height bar, no hanging space
      r.fillRect(x + t / 2, y, t, h, true);
      x += glyphWidth('1', m) + m.gap;
      continue;
    }
    const uint8_t seg = SEGMENTS[*s - '0'];
    const int w = m.digitW;
    if (seg & 0x01) r.fillRect(x, y, w, t, true);
    if (seg & 0x02) r.fillRect(x + w - t, y, t, half + t / 2, true);
    if (seg & 0x04) r.fillRect(x + w - t, y + half - t / 2, t, h - half + t / 2, true);
    if (seg & 0x08) r.fillRect(x, y + h - t, w, t, true);
    if (seg & 0x10) r.fillRect(x, y + half - t / 2, t, h - half + t / 2, true);
    if (seg & 0x20) r.fillRect(x, y, t, half + t / 2, true);
    if (seg & 0x40) r.fillRect(x, y + half - t / 2, w, t, true);
    x += w + m.gap;
  }
}

// ---- formatting -----------------------------------------------------------------

// Fewer decimals for large prices (BTC ~ 82410) so the digits stay big.
void formatPrice(char* buf, size_t n, float v) {
  const float a = fabsf(v);
  snprintf(buf, n, a >= 10000.0f ? "%.0f" : a >= 1000.0f ? "%.1f" : "%.2f", v);
}

void formatChange(char* buf, size_t n, float change, float pct) {
  const float a = fabsf(change);
  snprintf(buf, n, a >= 1000.0f ? "%+.0f  (%+.2f%%)" : "%+.2f  (%+.2f%%)", change, pct);
}

void formatVolume(char* buf, size_t n, float v) {
  if (v >= 1e9f) {
    snprintf(buf, n, "%.1fB", v / 1e9f);
  } else if (v >= 1e6f) {
    snprintf(buf, n, "%.1fM", v / 1e6f);
  } else if (v >= 1e3f) {
    snprintf(buf, n, "%.1fK", v / 1e3f);
  } else {
    snprintf(buf, n, "%.0f", v);
  }
}

void formatClock(char* buf, size_t n, time_t t) {
  struct tm local;
  localtime_r(&t, &local);  // TZ comes from the user's timezone setting (timezones::applyToClock)
  strftime(buf, n, SETTINGS.clockFormat == 1 ? "%I:%M %p" : "%H:%M", &local);
}

GfxRenderer::Orientation toRendererOrientation(uint8_t o) {
  switch (o) {
    case LANDSCAPE_LEFT:
      return GfxRenderer::LandscapeCounterClockwise;
    case LANDSCAPE_RIGHT:
      return GfxRenderer::LandscapeClockwise;
    case PORTRAIT:
    default:
      return GfxRenderer::Portrait;
  }
}

void drawTriangle(const GfxRenderer& r, int x, int y, int size, bool up) {
  const int xs[3] = {x, x + size, x + size / 2};
  const int ys[3] = {up ? y + size : y, up ? y + size : y, up ? y : y + size};
  r.fillPolygon(xs, ys, 3, true);
}

// ---- chart ------------------------------------------------------------------------

void drawChart(const GfxRenderer& r, const Quote& q, int x, int y, int w, int h, bool labels) {
  r.drawLine(x, y + h, x + w, y + h, true);
  if (q.pointCount == 0 || w < 20 || h < 16) return;

  float lo = q.pointPrice[0], hi = q.pointPrice[0];
  for (int i = 0; i < q.pointCount; i++) {
    lo = fminf(lo, q.pointPrice[i]);
    hi = fmaxf(hi, q.pointPrice[i]);
  }
  if (q.prevClose > 0) {
    lo = fminf(lo, q.prevClose);
    hi = fmaxf(hi, q.prevClose);
  }
  float pad = (hi - lo) * 0.08f;
  if (pad <= 0.0f) pad = hi * 0.001f + 0.01f;
  lo -= pad;
  hi += pad;

  const int labelH = labels ? r.getLineHeight(SMALL_FONT_ID) : 0;
  const int plotTop = y + labelH;
  const int plotH = h - labelH - 2;
  const float sessionMin = (q.sessionEnd > q.sessionStart) ? (q.sessionEnd - q.sessionStart) / 60.0f : 390.0f;
  auto px = [&](float minute) { return x + static_cast<int>(minute / sessionMin * w); };
  auto py = [&](float price) { return plotTop + static_cast<int>((hi - price) / (hi - lo) * plotH); };

  for (int i = 0; i + 1 < q.pointCount; i++) {
    const int x0 = px(q.pointMinute[i]), x1 = px(q.pointMinute[i + 1]);
    const int y0 = py(q.pointPrice[i]), y1 = py(q.pointPrice[i + 1]);
    for (int cx = x0; cx < x1; cx++) {
      const int cy = y0 + (y1 - y0) * (cx - x0) / (x1 - x0);
      r.fillRectDither(cx, cy, 1, y + h - cy, LightGray);
    }
  }
  const int lineW = h > 120 ? 3 : 2;
  for (int i = 0; i + 1 < q.pointCount; i++) {
    r.drawLine(px(q.pointMinute[i]), py(q.pointPrice[i]), px(q.pointMinute[i + 1]), py(q.pointPrice[i + 1]), lineW,
               true);
  }
  if (q.pointCount == 1) r.fillRect(px(q.pointMinute[0]) - 2, py(q.pointPrice[0]) - 2, 5, 5, true);

  if (q.prevClose > 0) {
    const int yc = py(q.prevClose);
    for (int dx = x; dx < x + w; dx += 12) r.drawLine(dx, yc, dx + 6 < x + w ? dx + 6 : x + w, yc, true);
  }
  if (!labels) return;
  char buf[24];
  if (q.prevClose > 0) {
    const int yc = py(q.prevClose);
    formatPrice(buf, sizeof(buf), q.prevClose);
    const int tw = r.getTextWidth(SMALL_FONT_ID, buf);
    const int ly = yc - labelH - 2 < y ? yc + 3 : yc - labelH - 2;
    r.fillRect(x + w - tw - 4, ly, tw + 4, labelH, false);
    r.drawText(SMALL_FONT_ID, x + w - tw - 2, ly, buf, true);
  }
  if (q.prevClose <= 0 || py(q.prevClose) - plotTop > labelH * 2) {
    formatPrice(buf, sizeof(buf), hi - pad);
    r.drawText(SMALL_FONT_ID, x, y, buf, true);
  }
}

// ---- single-symbol page --------------------------------------------------------

// Logo (or nothing) on the left; symbol and one status line ("Market open   1 / 2") on the right.
// Returns the header height so the price block starts below whichever side is taller.
int drawHeader(const GfxRenderer& r, const Quote& q, const char* symbol, int x, int y, int w, int page, int pages) {
  Logo logo;
  int leftH = 0;
  int maxSymbolW = w;
  if (logoFor(symbol, logo)) {
    drawLogo(r, logo, x, y, 1);
    leftH = logo.h;
    maxSymbolW = w - logo.w - 16;
  }
  const int font = r.getTextWidth(NOTOSANS_18_FONT_ID, symbol, EpdFontFamily::BOLD) <= maxSymbolW ? NOTOSANS_18_FONT_ID
                                                                                                  : UI_12_FONT_ID;
  const int symW = r.getTextWidth(font, symbol, EpdFontFamily::BOLD);
  r.drawText(font, x + w - symW, y, symbol, true, EpdFontFamily::BOLD);
  int rightH = r.getLineHeight(font);

  char info[48];
  int n = 0;
  info[0] = '\0';
  if (q.valid) {
    const bool open = q.fetchedAt >= q.sessionStart && q.fetchedAt < q.sessionEnd;
    n += snprintf(info, sizeof(info), "%s",
                  q.crypto ? tr(STR_TICKER_CRYPTO)
                  : open   ? tr(STR_TICKER_LIVE)
                           : tr(STR_TICKER_AT_CLOSE));
  }
  if (pages > 1 && n < static_cast<int>(sizeof(info))) {
    snprintf(info + n, sizeof(info) - n, "%s%d / %d", n ? "   " : "", page + 1, pages);
  }
  if (info[0]) {
    const int tw = r.getTextWidth(UI_10_FONT_ID, info);
    if (tw <= maxSymbolW) {
      r.drawText(UI_10_FONT_ID, x + w - tw, y + rightH + 2, info, true);
      rightH += 2 + r.getLineHeight(UI_10_FONT_ID);
    } else {  // narrow column (landscape): the line goes under the logo instead of beside it
      r.drawText(UI_10_FONT_ID, x, y + leftH + 6, info, true);
      leftH += 6 + r.getLineHeight(UI_10_FONT_ID);
    }
  }
  return leftH > rightH ? leftH : rightH;
}

// "$" + big price, then the change line. Returns the bottom y.
int drawPriceBlock(const GfxRenderer& r, const Quote& q, int x, int y, int w, int maxBigH) {
  char buf[32];
  formatPrice(buf, sizeof(buf), q.price);
  const int dollarW = r.getTextWidth(NOTOSANS_18_FONT_ID, "$", EpdFontFamily::BOLD);
  const int bigH = fitBigHeight(buf, w - dollarW - 6, maxBigH);
  r.drawText(NOTOSANS_18_FONT_ID, x, y, "$", true, EpdFontFamily::BOLD);
  drawBigText(r, x + dollarW + 6, y, bigH, buf);
  y += bigH + 18;

  if (q.prevClose > 0) {
    const float change = q.price - q.prevClose;
    const int lineH = r.getLineHeight(NOTOSANS_18_FONT_ID);
    const int tri = lineH / 2;
    drawTriangle(r, x, y + (lineH - tri) / 2, tri, change >= 0);
    formatChange(buf, sizeof(buf), change, change / q.prevClose * 100.0f);
    r.drawText(NOTOSANS_18_FONT_ID, x + tri + 10, y, buf, true, EpdFontFamily::BOLD);
    y += lineH;
  }
  return y;
}

void statRow(const GfxRenderer& r, int x, int y, const char* label, const char* v) {
  r.drawText(UI_10_FONT_ID, x, y, label, true);
  r.drawText(UI_12_FONT_ID, x, y + r.getLineHeight(UI_10_FONT_ID), v, true, EpdFontFamily::BOLD);
}

int statBlockH(const GfxRenderer& r) { return r.getLineHeight(UI_10_FONT_ID) + r.getLineHeight(UI_12_FONT_ID) + 6; }

// Prev close, day range, volume. Returns the bottom y.
int drawStats(const GfxRenderer& r, const Quote& q, int x, int y, int w, bool oneRow) {
  char range[40], prev[16], vol[16], lo[16], hi[16];
  formatPrice(lo, sizeof(lo), q.dayLow);
  formatPrice(hi, sizeof(hi), q.dayHigh);
  snprintf(range, sizeof(range), "%s - %s", lo, hi);
  formatPrice(prev, sizeof(prev), q.prevClose);
  formatVolume(vol, sizeof(vol), q.volume);
  if (oneRow) {
    statRow(r, x, y, tr(STR_TICKER_PREV_CLOSE), prev);
    statRow(r, x + w * 30 / 100, y, tr(STR_TICKER_DAY_RANGE), range);
    statRow(r, x + w * 78 / 100, y, tr(STR_TICKER_VOLUME), vol);
    return y + statBlockH(r);
  }
  statRow(r, x, y, tr(STR_TICKER_DAY_RANGE), range);
  y += statBlockH(r);
  statRow(r, x, y, tr(STR_TICKER_PREV_CLOSE), prev);
  statRow(r, x + w / 2, y, tr(STR_TICKER_VOLUME), vol);
  return y + statBlockH(r);
}

void drawSinglePage(const GfxRenderer& r, const Quote& q, const char* symbol, int page, int pages, int W,
                    int statusTop) {
  const bool landscape = W > r.getScreenHeight();
  if (!q.valid) {
    drawHeader(r, q, symbol, MARGIN, MARGIN, W - 2 * MARGIN, page, pages);
    r.drawCenteredText(NOTOSANS_18_FONT_ID, r.getScreenHeight() / 2 - 20, tr(STR_TICKER_NO_DATA), true,
                       EpdFontFamily::BOLD);
    return;
  }
  if (landscape) {
    const int leftW = W * 44 / 100;
    int y = MARGIN + drawHeader(r, q, symbol, MARGIN, MARGIN, leftW - MARGIN, page, pages) + 28;
    y = drawPriceBlock(r, q, MARGIN, y, leftW - MARGIN, 84);
    drawStats(r, q, MARGIN, y + 24, leftW - MARGIN, false);
    const int chartX = leftW + 24;
    drawChart(r, q, chartX, MARGIN, W - MARGIN - chartX, statusTop - MARGIN - 20, true);
  } else {
    const int w = W - 2 * MARGIN;
    int y = MARGIN + drawHeader(r, q, symbol, MARGIN, MARGIN, w, page, pages) + 36;
    y = drawPriceBlock(r, q, MARGIN, y, w, 104);
    const int statsY = statusTop - statBlockH(r) - 16;
    drawChart(r, q, MARGIN, y + 28, w, statsY - (y + 28) - 24, true);
    drawStats(r, q, MARGIN, statsY, w, true);
  }
}

// ---- grid card --------------------------------------------------------------------

void drawCard(const GfxRenderer& r, const Quote& q, const char* symbol, int x, int y, int w, int h) {
  constexpr int PAD = 10;
  x += PAD;
  y += PAD;
  w -= 2 * PAD;
  h -= 2 * PAD;

  // Top line: logo (half size) or ticker text on the left, ticker text on the right.
  Logo logo;
  const int topH = 34;
  const int symW = r.getTextWidth(UI_12_FONT_ID, symbol, EpdFontFamily::BOLD);
  if (logoFor(symbol, logo)) {
    drawLogo(r, logo, x, y + (topH - logo.h / 2) / 2, 2);
    r.drawText(UI_12_FONT_ID, x + w - symW, y + 4, symbol, true, EpdFontFamily::BOLD);
  } else {
    r.drawText(NOTOSANS_18_FONT_ID, x, y, symbol, true, EpdFontFamily::BOLD);
  }
  y += topH + 14;
  h -= topH + 14;

  if (!q.valid) {
    r.drawText(UI_10_FONT_ID, x, y, tr(STR_TICKER_NO_DATA), true);
    return;
  }

  // Change column is measured first (right-aligned); the price gets the space left of it.
  char pctBuf[16], absBuf[16], buf[32];
  int changeW = 0, tri = 0;
  const int lineH = r.getLineHeight(UI_12_FONT_ID);
  const bool hasChange = q.prevClose > 0;
  float change = 0;
  if (hasChange) {
    change = q.price - q.prevClose;
    snprintf(pctBuf, sizeof(pctBuf), "%+.2f%%", change / q.prevClose * 100.0f);
    snprintf(absBuf, sizeof(absBuf), fabsf(change) >= 1000.0f ? "%+.0f" : "%+.2f", change);
    tri = lineH / 2;
    const int pctW = tri + 8 + r.getTextWidth(UI_12_FONT_ID, pctBuf, EpdFontFamily::BOLD);
    const int absW = tri + 8 + r.getTextWidth(UI_10_FONT_ID, absBuf);
    changeW = pctW > absW ? pctW : absW;
  }
  formatPrice(buf, sizeof(buf), q.price);
  const int priceMaxH = h * 40 / 100 > 76 ? 76 : h * 40 / 100;
  const int priceMaxW = w - changeW - (hasChange ? 24 : 0);
  const int bigH = fitBigHeight(buf, priceMaxW, priceMaxH);
  drawBigText(r, x, y, bigH, buf);

  if (hasChange) {
    const int cx = x + w - changeW;
    drawTriangle(r, cx, y + (lineH - tri) / 2, tri, change >= 0);
    r.drawText(UI_12_FONT_ID, cx + tri + 8, y, pctBuf, true, EpdFontFamily::BOLD);
    r.drawText(UI_10_FONT_ID, cx + tri + 8, y + lineH + 2, absBuf, true);
  }
  const int priceBottom = y + bigH + 12;

  // Sparkline fills the rest of the card.
  const int chartH = y + h - priceBottom;
  if (chartH > 24) drawChart(r, q, x, priceBottom, w, chartH, false);
}

void drawGrid(const GfxRenderer& r, const Config& cfg, int W, int statusTop) {
  const int n = cfg.symbolCount;
  const bool landscape = W > r.getScreenHeight();
  const int cols = (landscape && n > 1) ? 2 : 1;
  const int rows = (n + cols - 1) / cols;
  const int top = 8;
  const int areaH = statusTop - 12 - top;
  const int cellW = W / cols;
  const int cellH = areaH / rows;
  for (int i = 0; i < n; i++) {
    const int cx = (i % cols) * cellW;
    const int cy = top + (i / cols) * cellH;
    drawCard(r, quotes()[i], cfg.symbols[i], cx + 8, cy, cellW - 16, cellH);
    if (i / cols < rows - 1) r.drawLine(cx + MARGIN, cy + cellH, cx + cellW - MARGIN, cy + cellH, true);
    if (cols == 2 && i % 2 == 0) r.drawLine(cx + cellW, cy + 16, cx + cellW, cy + cellH - 16, true);
  }
}

// ---- status line -----------------------------------------------------------------

time_t latestFetch(const Config& cfg) {
  time_t t = 0;
  for (int i = 0; i < cfg.symbolCount; i++) {
    if (quotes()[i].valid && quotes()[i].fetchedAt > t) t = quotes()[i].fetchedAt;
  }
  return t;
}

void drawStatus(const GfxRenderer& r, const Config& cfg, const ScreenStatus& s, int bottomY) {
  char line[128], t1[16], t2[16];
  int n = 0;
  const time_t fetched = latestFetch(cfg);
  if (fetched > 0) {
    formatClock(t1, sizeof(t1), fetched);
    n += snprintf(line + n, sizeof(line) - n, "%s %s", s.offline ? tr(STR_TICKER_OFFLINE) : tr(STR_TICKER_UPDATED), t1);
  }
  if (s.nextUpdate > 0 && n < static_cast<int>(sizeof(line))) {
    formatClock(t2, sizeof(t2), s.nextUpdate);
    n += snprintf(line + n, sizeof(line) - n, "%s%s %s", n ? "   |   " : "", tr(STR_TICKER_NEXT), t2);
  }
  if (s.battery >= 0 && n < static_cast<int>(sizeof(line))) {
    snprintf(line + n, sizeof(line) - n, "%s%s %d%%", n ? "   |   " : "", tr(STR_BATTERY), s.battery);
  }
  if (n == 0 && s.battery < 0) line[0] = '\0';

  const int smallH = r.getLineHeight(SMALL_FONT_ID);
  int y = bottomY - smallH;
  if (s.nextUpdate > 0) {
    r.drawCenteredText(SMALL_FONT_ID, y, tr(STR_TICKER_EXIT_HINT), true);
    y -= smallH + 2;
  }
  r.drawCenteredText(SMALL_FONT_ID, y, line, true, s.offline ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  r.drawLine(MARGIN, y - 8, r.getScreenWidth() - MARGIN, y - 8, true);
}

int statusHeight(const GfxRenderer& r, const ScreenStatus& s) {
  const int smallH = r.getLineHeight(SMALL_FONT_ID);
  return (s.nextUpdate > 0 ? 2 * smallH + 2 : smallH) + 12;
}

// Full-screen custom image, centered; a small "Offline" badge when the last fetch failed.
void drawCustomPage(const GfxRenderer& r, const ScreenStatus& s) {
  const int W = r.getScreenWidth();
  const int H = r.getScreenHeight();
  bool drawn = false;
  {
    HalFile file;
    if (Storage.openFileForRead("TICK", CUSTOM_IMAGE_PATH, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int x = bitmap.getWidth() < W ? (W - bitmap.getWidth()) / 2 : 0;
        const int y = bitmap.getHeight() < H ? (H - bitmap.getHeight()) / 2 : 0;
        drawn = r.drawBitmap(bitmap, x, y, W, H);
      }
    }
  }
  if (!drawn) {
    r.drawCenteredText(NOTOSANS_18_FONT_ID, H / 2 - 20, tr(STR_TICKER_CUSTOM_NONE), true, EpdFontFamily::BOLD);
  }
  if (s.offline) {
    const char* text = tr(STR_TICKER_OFFLINE);
    const int tw = r.getTextWidth(SMALL_FONT_ID, text, EpdFontFamily::BOLD);
    const int th = r.getLineHeight(SMALL_FONT_ID);
    r.fillRect(W - tw - 16, H - th - 10, tw + 12, th + 6, true);
    r.drawText(SMALL_FONT_ID, W - tw - 10, H - th - 7, text, false, EpdFontFamily::BOLD);
  }
}

}  // namespace

void drawScreen(GfxRenderer& r, const Config& cfg, const ScreenStatus& s) {
  r.setOrientation(toRendererOrientation(cfg.orientation));
  r.clearScreen();
  const int W = r.getScreenWidth();
  const int H = r.getScreenHeight();
  const int bottom = H - MARGIN + 8;
  const int statusTop = bottom - statusHeight(r, s);

  const int pages = pageCount(cfg);
  const int page = pages > 0 ? s.pageIndex % pages : 0;
  if (isCustomPage(cfg, page)) {
    drawCustomPage(r, s);
    return;  // the image owns the whole screen
  }
  if (cfg.layout == LAYOUT_ROTATE || cfg.symbolCount == 1) {
    const int sym = cfg.symbolCount > 0 ? page % cfg.symbolCount : 0;
    drawSinglePage(r, quotes()[sym], cfg.symbols[sym], sym, cfg.symbolCount, W, statusTop);
  } else {
    drawGrid(r, cfg, W, statusTop);
  }
  drawStatus(r, cfg, s, bottom);

  if (s.banner) {
    const int lineH = r.getLineHeight(NOTOSANS_18_FONT_ID);
    const int by = statusTop - lineH - 20;
    r.fillRect(0, by - 8, W, lineH + 16, false);
    r.drawCenteredText(NOTOSANS_18_FONT_ID, by, s.banner, true, EpdFontFamily::BOLD);
  }
}

}  // namespace ticker
