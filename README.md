# Charmera-S3

A one-button, Kodak Charmera-style digital camera for the Waveshare **ESP32-S3-LCD-2**
(2" 240×320 screen, no touch) with an **OV2640** camera and a microSD card.

Hold it like a phone, with USB-C at the bottom.

## Using it

| On the camera screen | |
|---|---|
| **Press** BOOT | take a photo (white flash, "Hold still…", then the photo freezes while it saves) |
| **Hold** BOOT | menu: Camera / Looks / Gallery / Settings |

| In menus, the gallery and settings | |
|---|---|
| **Press** | move down / next (older) photo |
| **Hold** | choose / open the photo menu |
| **Double press** | back / close |

- **Looks** (12): Normal, Cool Tone, Warm Tone, Black & White, Pixel Red / Yellow / Blue /
  Gray, and Sticker, PC, Film and Charmera frames. What you see in the viewfinder is what's saved.
- **Gallery:** newest first. Hold for Delete this / Delete all. Confirm dialogs open on Cancel.
- **Settings:** card size, free and used space, photo count, battery, version; Brightness;
  on-screen **Level** (off by default); Format card.
- The screen dims after 30 s idle and turns off after 3 min. Press BOOT to wake; the waking
  press doesn't take a photo.
- Don't hold BOOT while plugging in or pressing RESET. That starts the chip in download mode.

## Photos

`DCIM/100CHARM/IMG_0001.JPG`, … — 1200×1600 portrait JPEGs (quality 92). Numbering
continues from the highest number on the card and never overwrites. Put the card in your
PC's reader to copy them off.

A photo takes about 6–7 s from press to "Saved": about 2 s to capture, 1 s to process and
2–3 s to write to the card.

## Hardware notes

- **OV2640:** its JPEG output is broken on this module and its YUV output is green, so the
  firmware captures raw RGB565 and encodes JPEG itself. An OV5640 is detected
  automatically and would shoot 1944×2592.
- Pins: `components/board/include/board.h`, `components/camera/camera.c`.

## Build and flash

Needs ESP-IDF v5.5 or later at `~/esp/esp-idf`.

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3      # first time only
idf.py build
idf.py -p /dev/ttyACM0 flash
tools/run.sh 10                # build + flash + 10 s of serial log
```

## Host unit tests

Looks, file naming, button/tilt logic and idle timing are tested on the PC with Unity:

```sh
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

## Layout

| Path | What |
|---|---|
| `components/board` | SPI/I2C buses, backlight, battery ADC |
| `components/display` | ST7789 panel + LVGL |
| `components/input` | BOOT press + motion-sensor tilt |
| `components/storage` | SD card, DCF naming, card info, atomic writes |
| `components/looks` | the 12 looks (pure C, host-tested) |
| `components/camera` | OV2640 preview, full-res capture, rotation |
| `components/pipeline` | capture → look → JPEG → card |
| `main/ui` | camera, gallery and settings screens, menus |
| `main/power.c` | idle dim / screen-off |
| `tools/` | `run.sh`, serial log reader, and `dumpfiles.py` / `dump2png.py` to pull images over USB |

Not in v1: video, date stamp, USB-drive mode, Wi-Fi.

Design: `docs/superpowers/specs/2026-11-06-charmera-s3-design.md` ·
Plan: `docs/superpowers/plans/2026-11-06-charmera-s3.md` ·
Acceptance: `docs/acceptance-2026-11-06.md`
