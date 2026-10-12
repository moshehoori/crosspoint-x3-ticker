#!/usr/bin/env python3
"""Logs CrossPoint serial output (reconnecting across deep-sleep USB drops) and grabs screenshots on request.

Usage: serial_logger.py [seconds] [output_dir]   (default 1800 s, current directory)
Requires pyserial (PlatformIO's penv has it: ~/.platformio/penv/bin/python).
Touch <dir>/shot.req to capture the framebuffer to shot_<n>.bin; write a command to <dir>/cmd.req to send CMD:<command>."""
import glob, os, sys, time, serial

D = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else ".")
LOG = open(os.path.join(D, "serial.log"), "a", buffering=1)
END = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 1800)
shots = 0

def log(msg):
    LOG.write(f"{time.strftime('%H:%M:%S')} {msg}\n")

while time.time() < END:
    ports = sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        time.sleep(0.2)
        continue
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.dtr, s.rts = ports[0], 115200, 0.3, False, False
        s.open()
        log(f"== port open {ports[0]} ==")
        buf = b""
        while time.time() < END:
            req = os.path.join(D, "shot.req")
            if os.path.exists(req):
                os.remove(req)
                s.write(b"CMD:SCREENSHOT\n")
                log("== screenshot requested ==")
            creq = os.path.join(D, "cmd.req")
            if os.path.exists(creq):
                cmd = open(creq).read().strip()
                os.remove(creq)
                s.write(("CMD:" + cmd + "\n").encode())
                log(f"== sent CMD:{cmd} ==")
            chunk = s.read(4096)
            if not chunk:
                continue
            buf += chunk
            while True:
                i = buf.find(b"SCREENSHOT_START:")
                if i < 0:
                    break
                j = buf.find(b"\n", i)
                if j < 0:
                    break
                size = int(buf[i + 17:j])
                need = j + 1 + size
                while len(buf) < need:
                    more = s.read(8192)
                    if more:
                        buf += more
                shots += 1
                path = os.path.join(D, f"shot_{shots}.bin")
                open(path, "wb").write(buf[j + 1:need])
                log(f"== screenshot saved {path} ({size} bytes) ==")
                buf = buf[:i] + buf[need:]
            *lines, buf = buf.split(b"\n")
            for l in lines:
                log(l.decode("utf-8", "replace").rstrip())
    except Exception as e:  # port vanished (deep sleep) or reset
        log(f"== port closed: {e.__class__.__name__} ==")
        time.sleep(0.5)
