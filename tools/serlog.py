# Usage: python tools/serlog.py [seconds]  (run inside the ESP-IDF env for pyserial)
import serial, sys, time
# Read the console without touching DTR/RTS (so the chip is not reset).
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 6
s = None
for _ in range(50):
    try:
        s = serial.Serial(); s.port = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:ED:A0:2A:28:60-if00'; s.baudrate = 115200; s.timeout = 0.2
        s.dtr = False; s.rts = False; s.open(); break
    except Exception:
        s = None; time.sleep(0.1)
if s is None:
    sys.exit("port not available")
end = time.time() + dur
while time.time() < end:
    try:
        d = s.read(4096)
    except Exception:
        time.sleep(0.1); continue
    if d:
        sys.stdout.write(d.decode(errors='replace')); sys.stdout.flush()
