#!/usr/bin/env python3
"""Webhook-to-image server for the Stock Ticker's custom image page.

The reader sleeps most of the time, so it cannot receive webhooks itself. This
server receives them, keeps the latest one, and renders it as a 1-bit PNG the
reader pulls on each wake.

    POST /webhook        JSON body, e.g. {"title": "Build", "text": "All tests passed",
                         "items": [{"label": "Branch", "value": "main"}],
                         "image": "https://.../photo.png" or "data:image/png;base64,...",
                         "chart": [3.1, 3.4, 2.9, 4.2], "chart_title": "Latency (s)"}
                         Images are dithered to 1-bit; charts are drawn as lines.
                         Any other JSON is shown as key/value rows.
    GET  /dashboard.png  The rendered image. ?w=&h= picks the size (default 792x528,
                         X3 landscape; use w=528&h=792 for portrait).
    GET  /dashboard.bmp  Same, as a 1-bit BMP.
    GET  /               Status.

Usage: webhook_server.py [--port 8080] [--out DIR]   (Pillow required)
Reader setting: Custom image URL = http://<this-computer>:8080/dashboard.png?w=792&h=528
"""
import argparse
import base64
import io
import json
import os
import textwrap
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
from urllib.request import Request, urlopen

from PIL import Image, ImageDraw, ImageFont

FONT_DIRS = ["/usr/share/fonts/truetype/dejavu", "/Library/Fonts", "C:/Windows/Fonts"]
LOCK = threading.Lock()
STATE = {"payload": {"title": "Waiting for a webhook", "text": "POST JSON to /webhook"}, "received": None, "count": 0}
IMAGE_CACHE = {}  # source string -> decoded PIL image, so each webhook fetches its image once


def font(size, bold=False):
    names = ["DejaVuSans-Bold.ttf", "Arial Bold.ttf", "arialbd.ttf"] if bold else ["DejaVuSans.ttf", "Arial.ttf", "arial.ttf"]
    for d in FONT_DIRS:
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def load_image(src):
    """Image from an http(s) URL or a (data:) base64 string; None if unusable."""
    if src in IMAGE_CACHE:
        return IMAGE_CACHE[src]
    try:
        if src.startswith(("http://", "https://")):
            req = Request(src, headers={"User-Agent": "TickerWebhook/1.0"})
            with urlopen(req, timeout=10) as r:
                data = r.read(8 * 1024 * 1024)
        else:
            data = base64.b64decode(src.split(",", 1)[1] if src.startswith("data:") else src)
        im = Image.open(io.BytesIO(data))
        im.load()
        if im.mode in ("RGBA", "LA", "P"):
            im = im.convert("RGBA")
            bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
            im = Image.alpha_composite(bg, im)
        im = im.convert("L")
    except Exception as e:  # bad URL, network error, not an image
        print(f"image load failed: {e}", flush=True)
        im = None
    IMAGE_CACHE[src] = im
    return im


def draw_image(canvas, im, box):
    """Fit, then Floyd-Steinberg dither to 1-bit so photos keep their tones on e-ink."""
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    scale = min(bw / im.width, bh / im.height)
    fitted = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.LANCZOS)
    canvas.paste(fitted.convert("1"), (x0 + (bw - fitted.width) // 2, y0 + (bh - fitted.height) // 2))


def draw_chart(d, values, box, title=None):
    x0, y0, x1, y1 = box
    if title:
        d.text((x0, y0), str(title), font=font(22, True), fill=0)
        y0 += 32
    pts = [float(v) for v in values if isinstance(v, (int, float))]
    d.line((x0, y1, x1, y1), fill=0, width=2)
    if len(pts) < 2:
        return
    lo, hi = min(pts), max(pts)
    span = (hi - lo) or 1.0
    top, bottom = y0 + 26, y1 - 6
    xy = [(x0 + i * (x1 - x0) / (len(pts) - 1), bottom - (p - lo) / span * (bottom - top)) for i, p in enumerate(pts)]
    d.line(xy, fill=0, width=3, joint="curve")
    d.text((x0, y0), f"{hi:g}", font=font(18), fill=0)
    d.text((x0, bottom - 22), f"{lo:g}", font=font(18), fill=0)
    last = f"{pts[-1]:g}"
    d.text((x1 - d.textlength(last, font=font(20, True)), max(y0, xy[-1][1] - 30)), last, font=font(20, True), fill=0)


def render(w, h):
    with LOCK:
        payload, received, count = dict(STATE["payload"]), STATE["received"], STATE["count"]
    im = Image.new("1", (w, h), 1)  # 1 = white
    d = ImageDraw.Draw(im)
    m = 28
    y = m
    title = str(payload.pop("title", "Webhook"))
    d.text((m, y), title, font=font(44, True), fill=0)
    y += 62
    d.line((m, y, w - m, y), fill=0, width=3)
    y += 20

    image = load_image(str(payload.pop("image"))) if payload.get("image") else None
    chart = payload.pop("chart", None)
    chart_title = payload.pop("chart_title", None)
    chart = chart if isinstance(chart, list) and len(chart) >= 2 else None
    has_text = bool(payload.get("text") or payload.get("items") or len(payload) > 0)
    footer_top = h - 24 - 16
    if image is not None or chart is not None:
        # Graphics take the right side (or everything when there is no text).
        gx0 = int(w * 0.48) if has_text else m
        gbox = (gx0, y, w - m, footer_top - 12)
        if image is not None and chart is not None:
            mid = (gbox[1] + gbox[3]) // 2
            draw_image(im, image, (gbox[0], gbox[1], gbox[2], mid - 8))
            draw_chart(d, chart, (gbox[0], mid + 8, gbox[2], gbox[3]), chart_title)
        elif image is not None:
            draw_image(im, image, gbox)
        else:
            draw_chart(d, chart, gbox, chart_title)
        w_text = gx0 - 20  # text column ends before the graphics
    else:
        w_text = w - m

    text = payload.pop("text", None)
    if text:
        chars = max(10, (w_text - m) // 17)
        for line in textwrap.wrap(str(text), chars)[:6]:
            d.text((m, y), line, font=font(30), fill=0)
            y += 40
        y += 10

    items = payload.pop("items", None)
    rows = [(str(i.get("label", "")), str(i.get("value", ""))) for i in items] if isinstance(items, list) else []
    rows += [(str(k), json.dumps(v) if isinstance(v, (dict, list)) else str(v)) for k, v in payload.items()]
    for label, value in rows:
        if y > h - m - 70:
            break
        d.text((m, y), label, font=font(26), fill=0)
        vw = d.textlength(value, font=font(26, True))
        d.text((w_text - vw, y), value, font=font(26, True), fill=0)
        y += 36
        d.line((m, y, w_text, y), fill=0, width=1)
        y += 10

    footer = f"Webhook #{count}, received {time.strftime('%H:%M:%S', time.localtime(received))}" if received else "No webhook received yet"
    fh = 24
    d.rectangle((0, h - fh - 16, w, h), fill=0)
    d.text((m, h - fh - 10), footer, font=font(20, True), fill=1)
    return im


class Handler(BaseHTTPRequestHandler):
    server_version = "TickerWebhook/1.0"

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if urlparse(self.path).path != "/webhook":
            return self._send(404, b"not found\n", "text/plain")
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0))
        try:
            payload = json.loads(raw or b"{}")
            if not isinstance(payload, dict):
                payload = {"value": payload}
        except json.JSONDecodeError:
            payload = {"text": raw.decode("utf-8", "replace")}
        with LOCK:
            STATE["payload"], STATE["received"] = payload, time.time()
            STATE["count"] += 1
            n = STATE["count"]
        self._send(200, json.dumps({"ok": True, "count": n}).encode() + b"\n", "application/json")

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        w = int(q.get("w", [792])[0])
        h = int(q.get("h", [528])[0])
        if u.path in ("/dashboard.png", "/dashboard.bmp"):
            im = render(w, h)
            fmt = "PNG" if u.path.endswith(".png") else "BMP"
            buf = io.BytesIO()
            im.save(buf, fmt)
            if self.server.out_dir:
                im.save(os.path.join(self.server.out_dir, "last_served.png"))
            return self._send(200, buf.getvalue(), "image/png" if fmt == "PNG" else "image/bmp")
        if u.path == "/":
            with LOCK:
                info = {"count": STATE["count"], "received": STATE["received"], "payload": STATE["payload"]}
            return self._send(200, json.dumps(info, indent=2).encode() + b"\n", "application/json")
        self._send(404, b"not found\n", "text/plain")

    def log_message(self, fmt, *args):
        print(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} {fmt % args}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--out", default=None, help="also save each served image here as last_served.png")
    a = ap.parse_args()
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    srv.out_dir = a.out
    print(f"listening on :{a.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
