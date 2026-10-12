# Installing the Stock Ticker firmware on an Xteink X3

This is CrossPoint 1.6.5 with the [Stock Ticker](stock-ticker.md) app added. Everything else, including books, fonts and settings, works as in stock CrossPoint.

**What you need:**

- An Xteink X3
- A USB-C **data** cable (some cables only charge)
- The firmware file `crosspoint-1.6.5-ticker.2-x3.bin` from the [Releases page](https://github.com/moshehoori/crosspoint-x3-ticker/releases)

Flashing writes only the firmware. Books, fonts, Wi-Fi networks and settings on the SD card are untouched.

> **USB-locked units.** Some X3s sold by third-party stores ship with USB flashing locked. If your reader never shows up on USB, read CrossPoint's [USB-locked devices](../README.md#usb-locked-devices-xteink-unlocker) notes before doing anything else.

## Option 1: Web flasher (easiest)

1. Wake the reader and plug it into your computer.
2. In Chrome or Edge, open <https://crosspointreader.com/#flash-tools>.
3. Select **X3**, choose **Custom .bin**, and pick `crosspoint-1.6.5-ticker.2-x3.bin`.
4. Keep the reader awake until flashing finishes.

## Option 2: Command line (Linux and macOS)

Install esptool:

```bash
pip install esptool
```

Then run the helper script from this repo:

```bash
tools/stock-ticker/flash_x3.sh crosspoint-1.6.5-ticker.2-x3.bin
```

The script waits for the reader, writes the firmware, and makes it the boot image. Wake the reader (hold the power button) while it's plugged in.

### Doing it by hand

```bash
PORT=/dev/ttyACM0   # macOS: /dev/cu.usbmodem*
# 1. Reset into the bootloader and stay there
esptool --chip esp32c3 --port $PORT --before default-reset --after no-reset chip-id
# 2. Write the firmware to the first app slot
esptool --chip esp32c3 --port $PORT --before no-reset --after no-reset --baud 460800 write-flash 0x10000 crosspoint-1.6.5-ticker.2-x3.bin
# 3. Clear the boot selector so the reader boots that slot
esptool --chip esp32c3 --port $PORT --before no-reset --after no-reset erase-region 0xe000 0x2000
# 4. Restart
esptool --chip esp32c3 --port $PORT --before no-reset --after hard-reset chip-id
```

## Problems we hit, and the fixes

**The reader appears on USB for a second, then disappears.**
The X3 sleeps whenever it's idle, and a sleeping X3 isn't on USB. Hold the power button to wake it fully. A short press wakes it for about a second, then it sleeps again. Step 1 above keeps it in the bootloader, so it stays connected for the rest of the flash.

**Flashing said "Hash of data verified", but the old firmware still boots.**
The X3 has two firmware slots, and a boot selector picks one. A unit that was updated over the air may be booting the second slot. Writing to the first slot alone changes nothing. Step 3 erases the selector, and the bootloader then falls back to the first slot.

**"A fatal error occurred: The chip stopped responding" at 921600 baud.**
Use `--baud 460800`. The X3's USB serial link isn't reliable at the higher speed.

**"Could not open port, the port is busy".**
Something else has the port open: a browser tab with the web flasher, a serial monitor, or ModemManager on Linux. Close it, or run `sudo systemctl stop ModemManager` while flashing.

**The device shows up as `/dev/ttyACM1`.**
That happens when the previous connection hasn't been released yet. Use whichever `ttyACM` exists. The helper script picks it automatically.

## After installing

1. Make sure the reader knows your Wi-Fi: **File Transfer → Join Network** (one time only).
2. Open **Settings → System → Stock Ticker → Refresh now** to check that quotes load.
3. Choose your symbols and layout, then **Start ticker mode**.

To go back to stock CrossPoint, flash an official release the same way, or use the web flasher's official builds.
