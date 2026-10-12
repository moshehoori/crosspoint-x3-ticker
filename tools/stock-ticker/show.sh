#!/bin/bash
# Draws a ticker layout on a USB-connected reader (debug build) and saves a PNG.
# Run serial_logger.py in the same directory first.
# Usage: show.sh <layout 0=grid 1=rotate> <orientation 0=portrait 1=landscape-left 2=landscape-right> <page> <symbols> <out.png>
set -e
here="$(cd "$(dirname "$0")" && pwd)"
dir="${LOGDIR:-.}"
n0=$(grep -c "TICKER_SHOWN" "$dir/serial.log" || true)
echo "TICKER SHOW $1 $2 $3 $4" > "$dir/cmd.req"
for _ in $(seq 1 30); do [ "$(grep -c TICKER_SHOWN "$dir/serial.log")" -gt "$n0" ] && break; sleep 0.5; done
s0=$(ls "$dir"/shot_*.bin 2>/dev/null | wc -l)
touch "$dir/shot.req"
for _ in $(seq 1 30); do [ "$(ls "$dir"/shot_*.bin 2>/dev/null | wc -l)" -gt "$s0" ] && break; sleep 0.5; done
modes=(portrait landscape-left landscape-right)
python3 "$here/shot2png.py" "$(ls -t "$dir"/shot_*.bin | head -1)" "$5" "${modes[$2]}"
