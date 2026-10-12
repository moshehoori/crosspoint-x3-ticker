# Webhook format

The format accepted by `tools/stock-ticker/webhook_server.py`, which renders webhooks into the image shown on the reader's [custom image page](stock-ticker.md#custom-images-and-webhooks).

To have an AI assistant write these payloads for you, paste [webhook-ai-prompt.md](webhook-ai-prompt.md) into it.

## How it works

1. Something sends `POST /webhook` with a JSON body.
2. The server stores it. **Each webhook replaces the previous one completely.** Fields are never merged.
3. On its next wake, the reader downloads `GET /dashboard.png`, a 1-bit black-and-white image of the latest webhook.

The reader shows the new content at its next scheduled refresh, not immediately.

## Request

```http
POST /webhook
Content-Type: application/json

{"title": "Front door", "text": "Package delivered"}
```

| Body | Result |
| --- | --- |
| JSON object | Normal case; fields below |
| Other JSON (array, number, string) | Shown as one row labelled `value` |
| Not JSON (plain text) | Shown as the `text` paragraph |
| Larger than 12 MB | Rejected with `413` |

The response is `{"ok": true, "count": <webhook number>}`.

## Fields

All fields are optional.

| Field | Type | Shown as |
| --- | --- | --- |
| `title` | string | Large heading on one line, with a line under it. Defaults to "Webhook". Cut off with "..." if too wide. |
| `text` | string | Paragraph, wrapped, at most 6 lines. |
| `items` | array | Rows with the label on the left and the value right-aligned in bold. Each entry may be `{"label": "...", "value": ...}`, a `["label", value]` pair, or a plain string (a label with no value). |
| `image` | string | A picture. Give an `http://` or `https://` URL, a data URI (`data:image/png;base64,...`) or raw base64. |
| `chart` | array of numbers | Line chart showing high, low and latest values. Needs at least 2 numbers. Anything else in the array, including `true`/`false`, is ignored. |
| `chart_title` | string | Heading above the chart. |
| `footer` | string | Replaces the default footer text, "Webhook #N, received HH:MM:SS", in the black bar at the bottom. |
| any other key | any | An extra row, `key` on the left and the value on the right. Objects and arrays are shown as JSON. |

**Values:** numbers and booleans in rows are shown as text, for example `23.5` or `True`. Format them yourself if you want units or rounding: `"23.5 °C"`.

**Row order:** `items` first, then the extra keys in the order they appear in the JSON.

## Images

- **Formats:** anything Pillow can open, including PNG, JPEG, GIF and BMP. Up to 8 MB.
- **URLs:** fetched once per webhook, with a 10-second timeout. Sending the same URL in a new webhook fetches it again, so camera snapshot URLs stay current.
- **Transparency** becomes white.
- **Rendering:** scaled to fit its box without cropping, then dithered (Floyd–Steinberg) to black and white. Photos come out as dot patterns. Logos, line art and black text on white look sharpest.
- **Errors:** an image that can't be fetched or decoded is skipped, and the rest of the webhook still shows.

## Layout

```
┌──────────────────────────────────────────────┐
│ Title                                        │
│ ──────────────────────────────────────────── │
│ text paragraph          │                    │
│ label            value  │   image            │
│ label            value  │   ── chart ──      │
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓ footer ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │
└──────────────────────────────────────────────┘
```

| Content | Landscape (wide image) | Portrait (tall image) |
| --- | --- | --- |
| Text or rows, plus graphics | Text and rows left; graphics in the right half | Text and rows on top; graphics in the lower half |
| Graphics only | Graphics use the full width | Graphics use the full width |
| Image and chart together | Image above, chart below, inside the graphics area | Same |

**How much fits** at the default 792 × 528 landscape size: 8 rows with no `text`. Each line of `text` takes about one row's space, so a 6-line paragraph leaves room for 2 rows. A paragraph wraps at about 43 characters per line across the full width, or about 20 beside a graphic. Content that doesn't fit is left out, never drawn over the footer.

## Getting the image

| Request | Returns |
| --- | --- |
| `GET /dashboard.png?w=792&h=528` | 1-bit PNG. The default size is the X3 landscape screen. |
| `GET /dashboard.png?w=528&h=792` | Portrait |
| `GET /dashboard.bmp?...` | Same, as a 1-bit BMP |
| `GET /` | Status: webhook count, time and stored payload, as JSON |

Width and height must be whole numbers from 64 to 2048, otherwise the response is `400`. Match the reader's **Screen orientation** setting: landscape is `w=792&h=528`, portrait is `w=528&h=792`.

## Limits

- **Black and white only.** Colours become dot patterns, so use contrast rather than colour to stand out.
- **Left to right only.** Hebrew and Arabic text are drawn left to right, which reverses words. Emoji may show as boxes.
- **One message.** The page always shows the latest webhook only. To show a list of events, send them together as `items`.
- **No security.** There's no authentication, so run the server only on a network you trust.

## Examples

**Minimal:**

```json
{"title": "Hello", "text": "First message"}
```

**Build status:**

```json
{
  "title": "Build #482 passed",
  "items": [
    {"label": "Branch", "value": "main"},
    {"label": "Duration", "value": "3m 12s"},
    {"label": "Tests", "value": "1,204 passed"}
  ],
  "footer": "CI, updated 14:32"
}
```

**Home sensor with a chart:**

```json
{
  "title": "Living room",
  "items": [["Temperature", "23.4 °C"], ["Humidity", "41 %"], ["CO2", "612 ppm"]],
  "chart": [21.8, 22.1, 22.6, 23.0, 23.4],
  "chart_title": "Temperature, last 5 hours"
}
```

**Camera snapshot:**

```json
{"title": "Front door", "text": "Motion detected", "image": "http://192.168.1.20/snapshot.jpg"}
```

**Image only:**

```json
{"title": "Today", "image": "https://example.com/calendar.png", "footer": " "}
```

**From the command line:**

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"title":"Backup","text":"Nightly backup finished","items":[["Size","42 GB"]]}' \
  http://192.168.1.10:8080/webhook
```

**From Home Assistant** (`configuration.yaml`):

```yaml
rest_command:
  eink_dashboard:
    url: "http://192.168.1.10:8080/webhook"
    method: POST
    content_type: "application/json"
    payload: >
      {"title": "Home",
       "items": [["Outside", "{{ states('sensor.outdoor_temperature') }} °C"],
                 ["Front door", "{{ states('binary_sensor.front_door') }}"]],
       "footer": "Home Assistant, {{ now().strftime('%H:%M') }}"}
```

**From a GitHub Actions step:**

```yaml
- name: Update e-ink dashboard
  if: always()
  run: |
    curl -s -X POST -H "Content-Type: application/json" \
      -d "{\"title\":\"${{ github.workflow }}: ${{ job.status }}\",\"items\":[[\"Branch\",\"${{ github.ref_name }}\"],[\"Commit\",\"${GITHUB_SHA::7}\"]]}" \
      http://dashboard.example.lan:8080/webhook
```

## Testing

```bash
python3 -m unittest -v tools/stock-ticker/test_webhook_server.py
```

The tests cover request parsing, every field and accepted form, layout in both orientations, clipping and overflow, image loading and re-fetching, and the HTTP errors.
