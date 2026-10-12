# Stock Ticker

A low-power stock and crypto dashboard for the **Xteink X3**. The reader shows up to four symbols, such as Intel and Bitcoin, refreshes them over Wi-Fi on a schedule, and spends the time in between in deep sleep. E-ink keeps the image on screen without power.

![Stock Ticker overview: Intel and Bitcoin in the portrait grid, and Bitcoin in the landscape rotate layout](images/stock-ticker/overview.png)

> **X3 only.** The ticker wakes itself on a timer. That works on the X3 because its sleep mode keeps the chip in real deep sleep. The X4 uses the same "sleep" to cut battery power, so a timer can't wake it, and the app refuses to start ticker mode there.

## What it shows

Each symbol gets:

- **Price**, in large digits that stay readable across a room
- **Change today** versus the previous close, with an up or down arrow
- **Intraday chart**, with a dashed line at the previous close
- **Day range, previous close and volume** (single-symbol layout)
- **Logo** for Intel (`INTC`) and Bitcoin (`BTC-…`); other symbols show their ticker

A status line at the bottom shows when the data was fetched, when the next update is due, and the battery level. If a fetch fails, the status line switches to **Offline** with the time of the data still on screen.

## Layouts

**All on one screen** draws one card per symbol: a grid of up to 4, stacked in portrait and 2 × 2 in landscape.

| Portrait, 2 symbols | Portrait, 3 symbols |
| --- | --- |
| ![Grid, portrait, 2 symbols](images/stock-ticker/grid-portrait-2.png) | ![Grid, portrait, 3 symbols](images/stock-ticker/grid-portrait-3.png) |

| Landscape, 2 symbols | Landscape, 3 symbols |
| --- | --- |
| ![Grid, landscape, 2 symbols](images/stock-ticker/grid-landscape-2.png) | ![Grid, landscape, 3 symbols](images/stock-ticker/grid-landscape-3.png) |

**Rotate** gives each symbol the full screen and steps through them. The page marker (`1 / 2`) shows which one you're on.

| Portrait | Landscape |
| --- | --- |
| ![Rotate, portrait, Intel](images/stock-ticker/rotate-portrait-intc.png) | ![Rotate, landscape, Intel](images/stock-ticker/rotate-landscape-intc.png) |
| ![Rotate, portrait, Bitcoin](images/stock-ticker/rotate-portrait-btc.png) | ![Rotate, landscape, Bitcoin](images/stock-ticker/rotate-landscape-btc.png) |

## Setup

1. **Install the firmware.** See the [installation guide](stock-ticker-install.md).
2. **Connect Wi-Fi once.** The ticker reuses networks the reader already knows. If you've never joined one, open **File Transfer → Join Network**, pick your network and enter the password. That saves it.
3. **Open the app:** **Settings → System → Stock Ticker**.

## Settings

| Row | What it does |
| --- | --- |
| **Start ticker mode** | Fetches, draws, and starts the sleep-and-refresh cycle. |
| **Refresh now** | Fetches once and shows a full-screen preview. In the Rotate layout, Confirm or Down flips to the next symbol, and Back returns. |
| **Symbols** | A comma list of up to 4 [Yahoo Finance](https://finance.yahoo.com) symbols, for example `INTC, BTC-USD, NVDA`. Crypto uses pairs like `BTC-USD` or `ETH-USD`. |
| **Layout** | *All on one screen* or *Rotate*. |
| **Rotate every** | *Each update*, or every 1, 2, 5 or 10 minutes. Rotation steps redraw from saved data without Wi-Fi. |
| **Refresh interval** | 5, 10, 15 (default), 30 or 60 minutes. |
| **Market hours only** | On by default. See [Schedule](#schedule). |
| **Screen orientation** | Portrait, Landscape CCW, or Landscape CW. |
| **Custom image** | *Off*, *Only the image*, or *Extra page* (shown in turn with your stocks). See [Custom images and webhooks](#custom-images-and-webhooks). |
| **Custom image URL** | An `http://` or `https://` address that returns a PNG or BMP. |

### From a browser

All of these settings, except **Start ticker mode** and **Refresh now**, also appear in CrossPoint's web settings page under a **Stock Ticker** section. Start **File Transfer** on the reader, open the address it shows, go to **Settings**, edit, and save. Symbols use the same comma list, for example `INTC, BTC-USD`.

Scripts can use the same API. These keys go in `POST /api/settings`, and enum values are option indexes:

| Key | Values |
| --- | --- |
| `tickerSymbols` | `"INTC, BTC-USD"` |
| `tickerLayout` | 0 = All on one screen, 1 = Rotate |
| `tickerRotateMin` | index into Each update, 1, 2, 5, 10 min |
| `tickerIntervalMin` | index into 5, 10, 15, 30, 60 min |
| `tickerMarketHoursOnly` | 0 = Off, 1 = On |
| `tickerOrientation` | 0 = Portrait, 1 = Landscape CCW, 2 = Landscape CW |
| `tickerCustomMode` | 0 = Off, 1 = Only the image, 2 = Extra page |
| `tickerCustomUrl` | `"http://192.168.1.10:8080/dashboard.png?w=792&h=528"` |

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"tickerSymbols":"INTC, BTC-USD, NVDA","tickerLayout":1}' \
  http://crosspoint.local/api/settings
```

**To leave ticker mode, hold the power button** until the reader wakes. It returns to normal, and your usual sleep screen applies again.

## Custom images and webhooks

The ticker can also show **any image from a URL**. On each update, the reader downloads a PNG or BMP and shows it full-screen. It can be the only page, or an extra page that rotates with your stocks. Use it for anything a server can draw: notifications, a calendar, home sensors, build status.

![A webhook with text, the Intel logo and a chart, as shown on the reader](images/stock-ticker/custom-webhook-intel.png)

**Webhooks:** the reader sleeps between updates, so it can't receive a webhook directly. Something always-on receives the webhook and serves the latest result as an image, and the reader pulls it on its next wake. `tools/stock-ticker/webhook_server.py` does exactly that. Run it on any always-on computer:

```bash
pip install pillow
python3 tools/stock-ticker/webhook_server.py --port 8080
```

Set **Custom image URL** to `http://<that-computer>:8080/dashboard.png?w=792&h=528`. For portrait, use `w=528&h=792`. Then send webhooks:

```bash
curl -X POST -H "Content-Type: application/json" http://<that-computer>:8080/webhook -d '{
  "title": "Front door",
  "text": "Package delivered",
  "items": [{"label": "Temperature", "value": "23 C"}],
  "image": "https://example.com/camera-snapshot.png",
  "chart": [21.5, 22.0, 22.8, 23.1],
  "chart_title": "Temperature today"
}'
```

| Field | Shown as |
| --- | --- |
| `title` | Large heading |
| `text` | Wrapped paragraph |
| `items` | Label and value rows |
| `image` | A picture, given as an http(s) URL or base64 (`data:image/png;base64,...`), dithered into black-and-white dots |
| `chart` + `chart_title` | Line chart with low, high and latest values |
| any other key | An extra label and value row |

| Text, picture and chart | Picture from a URL and chart |
| --- | --- |
| ![Intel webhook on the reader](images/stock-ticker/custom-webhook-intel.png) | ![Bitcoin webhook on the reader](images/stock-ticker/custom-webhook-bitcoin.png) |

These screenshots were taken from the reader. They match the server's image pixel for pixel.

**Other image sources:** anything that serves a PNG or BMP works, for example Home Assistant, n8n, a Cloudflare Worker, or a static file. Images are scaled down to fit and converted to black and white on the reader. For the sharpest result, serve a 1-bit image at the screen size: 792 × 528, or 528 × 792 in portrait.

**Schedule:** with *Only the image*, the reader refreshes every interval around the clock. With *Extra page*, the image follows the crypto rule: every interval during market hours, hourly otherwise.

**Failures:** if the server can't be reached or returns something that isn't an image, the reader keeps the last good image and shows a small **Offline** badge in the corner.

## Schedule

With **Market hours only** on:

- **Stocks** refresh every interval on weekdays from 09:30 to 16:00 New York time, plus once at 16:05 for the closing price. Nights and weekends need no wakes.
- **Crypto** never closes. If your list contains a crypto pair, the reader refreshes every interval while the stock market is open, and **hourly** the rest of the time.
- **Rotation steps** pause outside stock-market hours. The page still advances on each hourly crypto refresh.

With it off, everything refreshes every interval, around the clock.

New York time and US daylight-saving time are calculated on the device, so the schedule is correct wherever you live. Exchange holidays aren't modelled, so a holiday costs a few unnecessary wakes. The times on screen use the reader's own timezone setting.

## Battery

The reader is asleep almost all the time. A wake that fetches turns on Wi-Fi for about 2 to 3 seconds and refreshes the screen. A rotation-only wake just refreshes the screen.

These are **estimates, not measurements**:

| Setup (15-minute interval) | Fetches per weekday | Estimated use |
| --- | --- | --- |
| Stocks only, market hours | ~27 | ~4–7 mAh/day |
| Stocks + crypto, market hours | ~45 | ~7–11 mAh/day |
| Market hours off | 96 | ~15–25 mAh/day |

To measure your own: each wake appends a line to `/.crosspoint/ticker_log.csv` on the SD card with `epoch,battery%,fetched,ok,awake_ms`. After a day, download it with File Transfer and compare the battery column.

Other battery protections:

- **Low-battery pause:** below 8% battery, and not charging, ticker mode stops itself and shows *Ticker paused: battery low*.
- **Clock checks:** long sleeps are split into pieces of at most 3 hours and re-checked against the X3's clock chip, so drift in the chip's internal timer can't pile up.
- **Failed fetches:** retried after 5 minutes, at most twice, then the normal schedule resumes.

## Data source

Quotes come from Yahoo Finance's public chart endpoint (`/v7/finance/spark`), one HTTPS request for all symbols with no API key. That endpoint is unofficial and can change or rate-limit without notice. Prices may be delayed, depending on the exchange. Don't trade on them.

## Troubleshooting

| On screen | Cause and fix |
| --- | --- |
| *Wi-Fi connection failed* | No saved network is reachable. Join one once from **File Transfer → Join Network**. |
| *Low memory: reopen Stock Ticker* | A fetch in this menu visit left memory fragmented. Back out to Settings, which restarts the reader cleanly, and open Stock Ticker again. Ticker mode is not affected, because every wake starts fresh. |
| *No quotes for these symbols* | Yahoo returned nothing usable. Check the spelling, for example `BTC-USD`, not `BTC`. |
| *No stock data yet* on one card | That symbol hasn't been fetched yet. Run **Refresh now**. |
| **Offline** in the status line | The last fetch failed. The screen keeps the previous data and retries in 5 minutes. |

## Updates

The fork's version is `1.6.5-ticker.3`. CrossPoint's built-in **Check for Updates** compares only the first three numbers. It won't offer stock 1.6.5, but it will offer a future official release such as 1.6.6. Installing that replaces this firmware, and the ticker goes away.

## Trademarks

Intel and the Intel logo are trademarks of Intel Corporation. The Bitcoin logo is in the public domain. This project isn't affiliated with or endorsed by Intel, Yahoo, or Xteink.
