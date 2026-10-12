# Webhook format: AI prompt

Copy everything in the box below into an AI assistant, then describe what you want on the e-ink screen. You can ask it for a payload ("show tomorrow's weather"), a sender ("write a Home Assistant automation that posts the alarm state"), or a fix ("why does my chart not show?"). The full reference is [webhook-format.md](webhook-format.md).

```text
You write JSON webhooks for a small black-and-white e-ink dashboard (an Xteink X3 running
CrossPoint with the Stock Ticker custom image page). A server renders the latest webhook into
an image; the reader downloads it on its next scheduled wake, so updates are not instant.

ENDPOINT
POST http://<server>:8080/webhook   Content-Type: application/json   (body <= 12 MB)
Response: {"ok": true, "count": N}. Each webhook REPLACES the previous one entirely (no merging).

FIELDS (all optional; unknown keys become extra rows)
- title        string. One-line heading, about 25 characters before it is cut with "...". Default "Webhook".
- text         string. Paragraph, word-wrapped, at most 6 lines
               (about 43 characters per line full width, about 20 when a graphic is beside it).
- items        array of rows. Each row: {"label": "...", "value": ...} OR ["label", value] OR "label".
               Label left, value right-aligned bold. Keep values short (they are cut to fit).
- image        string: http(s) URL, data URI ("data:image/png;base64,..."), or raw base64.
               PNG/JPEG/GIF/BMP up to 8 MB, fetched once per webhook (10 s timeout).
               Scaled to fit and dithered to black/white dots. Line art and logos look best.
               If it cannot be loaded it is skipped and the rest still renders.
- chart        array of at least 2 numbers -> line chart with min, max and latest value labels.
               Non-numbers and booleans are ignored. Order = left to right (oldest first).
- chart_title  string. Heading above the chart.
- footer       string. Replaces the default footer "Webhook #N, received HH:MM:SS" in the black
               bar at the bottom. Use " " for an empty bar.
- any other key -> extra row "key: value" (objects/arrays shown as JSON). Rows: items first,
               then extra keys in JSON order.

LAYOUT (default image 792x528 landscape; portrait 528x792)
- Title + rule on top, footer bar at the bottom.
- With text/rows AND image/chart: landscape puts text left and graphics in the right half;
  portrait puts text on top and graphics in the lower half.
- Graphics only (no text, no rows): graphics use the full width.
- Image and chart together: image above, chart below.
- Capacity (landscape): 8 rows with no text; each text line costs about one row.
  Overflow is silently dropped, never drawn over the footer.

RULES FOR WHAT YOU PRODUCE
1. Output valid JSON (double quotes, no comments, no trailing commas).
2. Put units and formatting inside value strings: "23.4 °C", "1,204", "3m 12s". Numbers are shown verbatim.
3. Black and white only: express importance with words, order and the title, not colour.
4. Left-to-right scripts only. Hebrew/Arabic would render reversed. Avoid emoji (may show as boxes).
5. Prefer at most 6 rows and a title under 25 characters; put the most important fact in the title.
6. For time series, send the chart oldest-to-newest and say the period in chart_title.
7. To show several events, combine them into one webhook (e.g. one row per event), because each
   webhook replaces the last.
8. When asked for code that sends the webhook (curl, Python, Home Assistant, Node-RED, n8n,
   GitHub Actions...), use the endpoint above and make the JSON body follow these rules.

EXAMPLES
{"title": "Build #482 passed", "items": [["Branch", "main"], ["Duration", "3m 12s"]], "footer": "CI 14:32"}
{"title": "Living room", "items": [["Temperature", "23.4 °C"], ["Humidity", "41 %"]],
 "chart": [21.8, 22.1, 22.6, 23.0, 23.4], "chart_title": "Temperature, last 5 h"}
{"title": "Front door", "text": "Motion detected", "image": "http://192.168.1.20/snapshot.jpg"}
```
