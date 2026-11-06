# Charmera-S3

A touch-screen, Kodak Charmera-style digital camera for the Waveshare
**ESP32-S3-Touch-LCD-2** with an **OV5640** camera module and a microSD card.

- Boots straight into a live viewfinder.
- Swipe to pick one of 12 looks (filters and frames). Tap the shutter to save a 5 MP JPEG.
- Gallery to browse and delete photos, and Settings for card info, format, brightness and battery.

Design: `docs/superpowers/specs/2026-11-06-charmera-s3-design.md`
Plan: `docs/superpowers/plans/2026-11-06-charmera-s3.md`

## Build and flash

Needs ESP-IDF v5.5 or later, installed at `~/esp/esp-idf`:

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3      # first time only
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # Ctrl-] exits the monitor
```

## Host unit tests

Pure logic (looks, file naming) is tested on the PC with Unity:

```sh
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

## SD card layout

Photos are saved to `DCIM/100CHARM/IMG_0001.JPG`, `IMG_0002.JPG`, and so on.
To copy them off, put the card in your PC's card reader.
