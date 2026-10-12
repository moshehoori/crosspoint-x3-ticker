#!/bin/bash
# Flashes a CrossPoint app image to an Xteink X3 over USB and makes it the boot image.
# The X3 only stays on USB while awake, so this waits for the port and acts immediately.
# Usage: flash_x3.sh firmware.bin
set -u
FW="${1:?usage: flash_x3.sh firmware.bin}"
command -v esptool >/dev/null || { echo "install esptool: pip install esptool"; exit 1; }
echo "Wake the X3 and plug it in. Waiting up to 10 minutes..."
for _ in $(seq 1 12000); do PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1); [ -n "$PORT" ] && break; sleep 0.05; done
[ -n "${PORT:-}" ] || { echo "No device found"; exit 1; }
echo "Found $PORT"
# Reset into the ROM bootloader and stay there, so the port no longer disappears.
esptool --chip esp32c3 --port "$PORT" --before default-reset --after no-reset chip-id
sleep 1.5
PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1)
out=$(esptool --chip esp32c3 --port "$PORT" --before no-reset --after no-reset --baud 460800 write-flash 0x10000 "$FW" 2>&1)
echo "$out" | tail -3
if ! echo "$out" | grep -q "Hash of data verified"; then
  # Never switch the boot slot after a failed write: the reader keeps booting what it has.
  echo "Write failed; boot selection left unchanged. Wake the reader and run this again."
  exit 1
fi
# Erase OTA data so the bootloader runs app0 (where we just wrote), not a previous image in app1.
esptool --chip esp32c3 --port "$PORT" --before no-reset --after no-reset erase-region 0xe000 0x2000
esptool --chip esp32c3 --port "$PORT" --before no-reset --after hard-reset chip-id
echo "Done. The reader restarts into the new firmware."
