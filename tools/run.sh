#!/usr/bin/env bash
# Build, flash and capture N seconds of console output.  Usage: tools/run.sh [seconds]
set -e
. "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:ED:A0:2A:28:60-if00
cd "$(dirname "$0")/.."
idf.py build > build/last-build.log 2>&1 || { grep -E ': error|Error:' build/last-build.log | grep -v 'gcc ' | cut -c1-220 | head -20; exit 1; }
idf.py -p "$PORT" flash > build/last-flash.log 2>&1 || { tail -20 build/last-flash.log; exit 1; }
sleep 1.5
python tools/serlog.py "${1:-6}"
