# Charmera-S3 — Design

**Date:** 2026-11-06
**Status:** Approved 2026-11-06 (owner said "proceed"; calls 12–14 are designer defaults).
**Revised 2026-11-06:** the board has **no touchscreen**. Controls are the BOOT button + tilt (owner approved).

## Revision 2 (hardware findings, owner approved) — overrides anything below

- **Camera is an OV2640 (2 MP), not an OV5640.** "Best quality" = **1600×1200** sensor
  frames. Code detects an OV5640 and would use 2592×1944 if one is fitted.
- **This OV2640's JPEG output is broken** (NO-SOI at every clock/size) and its YUV422 is
  green-tinted; **RGB565 is clean**. Preview and capture are both RGB565, and the
  firmware encodes JPEG itself (`esp_new_jpeg`). There is no "sensor JPEG straight to
  card" path.
- **Grip is portrait, USB-C at the bottom** (owner's choice after seeing it). UI is
  **240×320 portrait**. The sensor's long axis runs top-to-bottom in this grip, so the
  viewfinder fills the screen and **photos are portrait 1200×1600**, rotated in firmware
  (not with an EXIF tag).
- Raw-mode frame buffers can't change size, so the camera is re-initialised at full size
  for each shot and back to 320×240 after (~3 s capture). The UI must cover this with
  instant feedback (freeze frame + flash at the press).
- No autofocus (OV2640).

## Intent

Turn a Waveshare **ESP32-S3-LCD-2** (no touch) with an **OV5640** camera into a small
handheld digital camera that *feels like a Kodak Charmera* to use. The Charmera's
user experience — pick a look, press the shutter, flip back through shots — is the
thing to copy. Its specs are not: the owner wants **the best image quality the
hardware can give**.

There is no touchscreen. The BOOT button plus tilting the camera (via the on-board motion
sensor) replace the Charmera's physical buttons.

Reference material in this repo:
- `docs/reference/kodak-charmera-quick-start.pdf` — Kodak's official quick start guide
  (source for the Charmera flow below)
- `docs/reference/ESP32-S3-Touch-LCD-2_V2-schematic.pdf` — schematic of the touch sibling board
  (same pin-out for everything this project uses)

## Goals (v1)

1. **Take a photo.** Live viewfinder → press BOOT → JPEG saved to the SD card.
2. **Browse photos.** Gallery of what's on the card; step forward/back; delete.
3. **Settings.** SD card total size + free space, number of photos, format card
   (with confirmation), plus small device settings (see below).
4. **Charmera looks.** The 12-ish selectable "looks" (filters + frames) the Charmera
   offers, chosen by tilting the camera left/right in the viewfinder. What you see in the viewfinder is
   what lands on the card.
5. **Best quality.** Photos at the OV5640's full 5 MP (2592×1944).

## Secondary goal (not v1, but design for it)

- **Video recording.** The mode structure must leave a clean slot for a Video mode
  (press shutter to start/stop, file saved to the card). Do not build it in v1; do not
  paint the architecture into a corner that prevents it (e.g. camera ownership should
  sit behind one module that a video recorder could also use).

## Non-goals (v1)

- USB mass-storage mode. The owner moves the SD card to a PC card reader.
- Date/time stamp on photos. The board has no battery-backed clock. (A future
  version could set time over Wi-Fi/NTP.)
- Wi-Fi, Bluetooth, phone apps, cloud anything.
- Multiple languages. English only.
- Audio. The board has no speaker; the "shutter sound" becomes a visual flash.

## Anti-goals (it fails even if it "works")

- Fiddly or slow to shoot. The Charmera is instant and dumb-simple; a menu maze or
  a multi-second lag with no feedback betrays the point.
- Making the owner pick a mode on every power-up. Reviewers' #1 Charmera complaint.
  Boot straight into the camera.
- Corrupting the card or losing a photo silently. Every save either succeeds or
  shows an error.
- Viewfinder look ≠ saved look.

## Hardware (confirmed)

Board: **Waveshare ESP32-S3-LCD-2**, the non-touch version (verified over USB: ESP32-S3 rev v0.2,
16 MB flash, 8 MB octal PSRAM, native USB-Serial/JTAG on `/dev/ttyACM0`).

Pin map, taken from Waveshare's ESP-IDF demos for the touch sibling board
(`ESP32-S3-Touch-LCD-2-Demo.zip`, examples 01/04/05/06); display, I2C and battery pins were
confirmed on this board:

| Function | Pins / details |
|---|---|
| Shared SPI bus (SPI2_HOST) | SCLK 39, MOSI 38, MISO 40 |
| LCD ST7789, 240×320, SPI 80 MHz | CS 45, DC 42, RST none, backlight 1 (LEDC PWM); `invert_color=true`, RGB order RGB, 16 bpp, LVGL needs 16-bit color swap |
| SD card (SPI mode, same bus) | CS 41 |
| I2C bus 0 (400 kHz) | SDA 48, SCL 47 — QMI8658 IMU at 0x6B (only device on the bus) |
| Touch | **none** on this board |
| OV5640 (DVP) | XCLK 8 (20 MHz), SIOD 21, SIOC 16, D0–D7 = 12,13,15,11,14,10,7,2, VSYNC 6, HREF 4, PCLK 9, PWDN 17, RESET none |
| Battery voltage | ADC1 channel 4, divider ×3 |
| Buttons | BOOT (GPIO0, active low; holding it during reset enters download mode), RESET |

Note: the LCD and SD card share one SPI bus. Display flushes and file writes must be
serialized (bus locking), and long SD writes will pause the screen briefly.

## How it gets made (owner's decision)

- **Language/framework:** C on **ESP-IDF** (Waveshare requires v5.5.0+ for this
  board; use current v5.5.x).
- **UI:** **LVGL**.
- **Camera:** Espressif **`esp32-camera`** component.
- **Location:** `~/repos/charmera-s3`, a git repo.

Waveshare's demos use LVGL 8.4 and the legacy I2C driver; the builder may use LVGL 9
and the new I2C master driver if that's cleaner. Pins and panel settings above are
what matter.

## The experience

The UI is **landscape, 320×240**, matching the camera's 4:3 frame and the Charmera's
horizontal body, held with the **USB-C port on the right** (confirmed on the board).

### Controls (owner approved)

Two inputs only:

- **Tilt** (QMI8658): roll the camera left or right past ~30° and hold briefly
  (~250 ms) = **previous / next**. Held tilt auto-repeats (~every 600 ms). Must return
  near level (< ~15°) to re-arm. Ordinary hand wobble while framing never triggers it.
- **BOOT button:**
  - **short press** = shutter (camera screen) / OK / select (menus)
  - **long press** (≥ ~800 ms) = open menu
  - **double press** (two presses within ~350 ms) = back / cancel

| Charmera (buttons) | Charmera-S3 |
|---|---|
| Hold power 3 s, pick Photo/Video/Settings icon every boot | Boots straight into the **Camera** screen |
| Up/Down cycles filters & frames | **Tilt** left/right; look name shown briefly |
| Shutter button, flash + shutter sound | **Short press**; white screen flash + brief "Saved" |
| Top-right shows photos remaining | Top-right shows **photos remaining** |
| On/Off button → mode icons | **Long press** → menu with Camera / Gallery / Settings icons; tilt to highlight, short press to pick |
| Up/Down to step through files | **Tilt** between photos |
| Long-press Play → delete current / delete all | **Long press** in gallery → Delete this / Delete all / Cancel |
| On/Off acts as "back" | **Double press** = back, from anywhere |
| Auto power-off after 3 min idle | Dim, then screen off after 3 min idle; **BOOT** wakes (tilt does not) |

Confirmation dialogs (delete, format) always open with **Cancel** highlighted; tilt to
move to the action, short press to choose. On-screen hints show what the button does
on each screen (e.g. "● shoot   ● hold: menu").

### Screens

**1. Camera (home)**
- Full-screen live viewfinder with the current look applied in real time.
- Overlays: photos remaining (top-right), battery level, current look name (fades
  after a tilt), a small control hint.
- No card / card full / card error → clear message on the viewfinder; shutter
  disabled. (The Charmera can hold 2 shots without a card; we don't need that.)
- Shutter: freeze frame, white flash, save, "Saved" toast, back to live view. The
  owner must always see that the camera is busy while a full-res photo is saved.

**2. Gallery**
- Opens on the newest photo. Tilt for older/newer. Shows "12 / 87" position.
- Long press → Delete this / Delete all / Cancel → confirm.
- Empty card → friendly "No photos yet".
- Photos are shown scaled to fit 320×240 (decode a downscaled preview; never try to
  hold a full 5 MP decode in RAM).

**3. Settings**
- A list; tilt moves the highlight, short press activates. Brightness: press, tilt to
  adjust, press to finish.
- SD card: total size, free space, used space.
- Number of photos on the card.
- **Format card** → "This erases every photo. Format?" → confirm → progress → done.
- Screen brightness.
- Battery voltage / level.
- About: firmware version.
- Room for future items (date/time, video settings).

### Looks (Charmera's set)

From Kodak's guide, the original Charmera has 7 filters and 4 frames. v1 ships a
plain look plus all of these:

| # | Look | Behaviour |
|---|---|---|
| 0 | Normal | No processing |
| 1 | Cool Tone | Shift white balance blue (sensor-side) |
| 2 | Warm Tone | Shift white balance amber (sensor-side) |
| 3 | Black & White | Monochrome, contrast lift (sensor-side) |
| 4–7 | Pixel Red / Yellow / Blue / Gray | Pixelate into large blocks, then map luminance to a 2-tone palette in that colour |
| 8–11 | Frames: Sticker, PC Window, Film Strip, Charmera | Decorative border drawn over the image |

Frames combine with Normal in v1 (one look at a time, like the Charmera). Frame
artwork is **original**, inspired by the Charmera's frame themes, without Kodak logos
or trademarks.

## Image pipeline (the hard part)

**Requirement:** full 5 MP JPEG output; the applied look must match the
viewfinder.

**Constraints:**
- 2592×1944 in RGB565 is ~10 MB, more than the 8 MB PSRAM. A full decoded 5 MP frame
  cannot sit in memory.
- The OV5640 can output hardware JPEG at full resolution, which fits comfortably
  (a few hundred KB to ~1.5 MB).
- The viewfinder needs a small, fast, uncompressed stream (e.g. 320×240 RGB565).

**Design:**
- **Viewfinder:** camera at 320×240 (QVGA) RGB565. Apply the look per frame in
  software (cheap at this size) and push to the display.
- **Capture:** reconfigure the sensor to full-res JPEG, grab one frame, then switch
  back to the viewfinder. Some shutter lag is acceptable as long as it's covered by
  clear visual feedback.
- **Normal look:** write the sensor's JPEG straight to the card. Best quality, fastest.
- **Sensor-side looks (Cool, Warm, B&W):** done with the OV5640's own white-balance /
  special-effect settings, so they apply identically to the viewfinder and the full-res
  JPEG, and save as fast as Normal with no re-encode. *(Designer's call.)*
- **Orientation:** the sensor can mirror/flip 180° itself. If the sensor turns out to
  sit 90° to the UI, keep the landscape UI, rotate the small viewfinder in software,
  and mark full-res photos with an EXIF orientation tag rather than rotating 5 MP of
  pixels. The builder confirms orientation on the real board in the first milestone.
- **Software looks (Pixel ×4, Frames ×4):** process the 5 MP JPEG in horizontal **strips**: decode a
  band of rows, apply the same filter code the viewfinder uses (scaled for
  resolution, e.g. pixel block size, frame thickness), and encode that band to the
  output JPEG, without ever holding the whole image decoded.
- **One implementation of each look,** written as pure functions over a pixel buffer,
  shared by the viewfinder and the capture path. This guarantees "what you see is
  what you get" and makes the looks unit-testable on the host.
- **Fallback (builder may use only if strip encoding proves infeasible):** save
  filtered looks at the largest resolution that fits a single in-memory pass (e.g.
  2048×1536), keep Normal at full 5 MP, and document the choice in the README. This
  is a quality compromise; flag it to the owner before settling on it.

**Saving:**
- Write to a temp file, then rename to the final name. A pulled card or power loss
  never leaves a half-written `IMG_xxxx.JPG`.
- Check free space before capture.

## Storage layout

- FAT32 card (exFAT if the IDF FatFS config supports it; formatting from the device
  produces whatever the device can mount).
- Photos in `/DCIM/100CHARM/IMG_0001.JPG`, `IMG_0002.JPG`, … (DCF-style, so PCs and
  photo apps recognise it).
- Numbering continues from the highest existing number on the card; never overwrite.
- Photo count = number of `IMG_*.JPG` files in that folder.
- Gallery and "delete all" only ever touch `IMG_*.JPG` in `/DCIM/100CHARM/`. Other
  files on the card are left alone; Format is the only way to wipe everything.
  *(Designer's call.)*
- "Photos remaining" = free space ÷ a running average of recent file sizes (seeded
  with a sensible default).

## Device settings persistence

Brightness and last-used look are stored in NVS and survive reboots.

## Idle / power

After ~30 s with no button press or tilt step, dim the backlight; after 3 min turn the
backlight off and power down the camera (PWDN pin). A **BOOT press** wakes back into
the live viewfinder (the waking press does nothing else). Tilt never wakes it, so it
can't wake in a pocket. No deep sleep in v1. *(Designer's call: instant wake.)*

## Architecture (guidance)

Small modules with one job each, for example:

- `board` — pins, SPI/I2C bus setup, backlight, battery ADC
- `display` — panel + LVGL port
- `input` — BOOT press classifier (short / long / double) + tilt classifier; pure
  state machines (host-testable) fed by a GPIO poll and IMU reads
- `camera` — owns the sensor; preview mode, still capture; later also video
- `looks` — pure pixel filters + frame drawing (host-testable)
- `pipeline` — strip decode → look → encode → file
- `storage` — mount/unmount, card info, format, file numbering, listing, delete
- `ui/` — camera, gallery, settings screens
- `power` — idle dim/sleep

## Testing & verification

- **Host unit tests** (ESP-IDF linux target or plain CMake + Unity) for: each look on
  small synthetic buffers, file-number allocation, "photos remaining" maths, strip
  boundary handling (no seams between strips), button press classification and tilt
  stepping/re-arm/auto-repeat.
- **On-device checks** on the real board for each milestone: flash, open serial
  monitor, exercise the screen.
- **Acceptance (v1 done when):**
  1. Boots straight into a live viewfinder in a few seconds.
  2. Pressing the shutter saves a 2592×1944 JPEG to `/DCIM/100CHARM/`, readable on
     the PC.
  3. Every look shows live and the saved photo matches it, with no visible strip seams.
  4. Gallery browses and deletes photos correctly.
  5. Settings shows correct card size/free/photo count (cross-checked on the PC), and
     format works and leaves a usable card.
  6. Pulling the card or having no card never crashes or freezes the device.

## Calls made by the designer (owner please check)

1. **Landscape UI at 320×240**, matching the camera's 4:3 frame.
2. **Plain "Normal" look added** as look #0 (the Charmera's filters are all effects;
   a best-quality camera needs an untouched option).
3. **Frames are original artwork,** no Kodak logos.
4. **No shutter sound** (no speaker on the board); a visual flash replaces it.
5. **Idle behaviour:** dim, then screen off after 3 minutes; wake on BOOT only.
6. **File layout** `DCIM/100CHARM/IMG_####.JPG`.
7. **Brightness and battery** added to Settings (cheap, useful).
8. **Cool/Warm/B&W done by the sensor**, so they save at full quality as fast as Normal.
9. **Gallery/delete-all scoped** to the camera's own folder.
10. **No deep sleep;** screen-off + camera power-down, BOOT to wake.
11. **Wrong-way sensor** handled with EXIF orientation, not pixel rotation.
12. **Saving blocks.** Software looks show a "Saving…" indicator and the shutter
    waits until the file is safely written. No background save queue in v1.
13. **Autofocus if present.** If the module answers to the OV5640 AF firmware load,
    run a single focus before each full-res capture (with a brief "focusing" cue);
    if not, treat it as fixed-focus. Never block boot on AF.
14. **Lens faces away from the screen** (normal camera). Builder verifies on the
    real board in milestone 1; if it's a selfie-facing mount, mirror the viewfinder.

## Left to the builder

- Exact tilt/press thresholds and timings (values above are starting points; tune on
  the board for a deliberate-but-easy feel). Which IMU axis is "roll" for the
  landscape, USB-right grip.

- LVGL 8 vs 9; exact widget styling and frame artwork.
- JPEG decode/encode libraries for the strip pipeline (e.g. `esp_new_jpeg`, TJpgDec).
- Exact filter maths (colour shifts, pixel block size, palettes), as long as the
  results read as the Charmera looks.
- Task/core layout, buffer counts, SPI bus arbitration.
- Partition table and sdkconfig details.
