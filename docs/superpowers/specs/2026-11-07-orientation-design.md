# Charmera-S3: orientation setting (design)

**Date:** 2026-11-07
**Status:** Draft for owner review. Builder-check answers are marked as designer calls.

## Intent

Today the camera is fixed to one grip: upright, with USB-C at the bottom, and every photo is portrait. The owner wants to choose in Settings how the camera is held. The owner also wants an **Auto** option that follows the camera like a phone does.

## Goals (owner)

- A Settings row, **Orientation**, with these options:
  - **Portrait (USB-C down):** today's behaviour.
  - **Landscape (USB-C right)**
  - **Landscape (USB-C left)**
  - **Auto:** picks one of the three above from the motion sensor.
- The whole camera follows the orientation: the screen, viewfinder, menus, gallery and settings read upright, and photos are saved in that shape.

## Designer calls

- **Default:** Portrait. Nobody's camera changes until they choose to change it.
- **Applies at once:** no restart. The setting is remembered (NVS key `orient`).
- **Bluetooth status:** two fields.
  - `orientation` is what's in effect now: `"portrait"`, `"landscape-right"` or `"landscape-left"`.
  - `orientationSetting` is the setting itself, which can also be `"auto"`.

  With both, a remote can see which way an Auto camera is actually held.
- **No upside-down portrait.** USB-C up isn't offered, and Auto ignores it.
- **Photo size per orientation:**

  | Orientation | OV5640 (sensor JPEG) | OV2640 (raw) |
  |---|---|---|
  | Portrait | 2560×1920 stored, EXIF Orientation 8, viewed 1920×2560, as today | 1200×1600, rotated in strips, as today |
  | Landscape | 2560×1920 stored, EXIF Orientation 1 or 3 by side | 1600×1200, stored as the sensor gives it, flipped 180° in strips if needed |

  Landscape photos need no rotation work in the pipeline beyond an optional 180° flip, so they're as fast as portrait or faster.
- **Frames:** the four frame looks draw their borders on the photo as the viewer sees it (landscape frames on landscape photos), using the same artwork rules.
- **Same look in either orientation:** pixel-cell size and frame thickness are measured from the image's **short edge**, so a landscape photo looks just like a portrait one turned sideways. Today they come from the width, which *is* the short edge in portrait, so portrait photos are unchanged. Tones don't depend on orientation.
- **Viewfinder:**
  - **Landscape:** the viewfinder is 320×240, filling the rotated screen, from the sensor's 320×240 preview, which is native or flipped 180°.
  - **Portrait:** 240×320, as today.
- **Screen rotation:**
  - The display is rotated at runtime (LVGL display rotation, with the ST7789 swapping axes), and every screen is laid out from the current resolution instead of fixed 240×320 positions.
  - Menus, toasts and confirm dialogs re-centre.
  - The Settings list scrolls, as today.
- **Gallery:** each photo is shown upright for the current screen orientation, using its own EXIF orientation and size, scaled to fit with black bars. Old portrait photos and new landscape photos both look right in either screen orientation.
- **Level line:** it uses the axis along the screen's current horizontal edge, so it stays correct in every orientation.
- **Auto:**
  - **Where it rotates:** only on the camera screen, live. The Gallery and Settings screens lay out in whatever orientation was in effect when they opened, and don't rotate while open. That way a rotation never has to preserve scroll position, selection or the current photo.
  - **While the screen is dim or off:** tilt never wakes the camera, which is today's rule. Auto doesn't track while the screen is off; on wake it re-checks with the same 0.7 s dwell.
  - It decides from the accelerometer's gravity direction in the screen plane.
  - It switches only after the camera has been clearly in a new grip (more than 30° past the 45° boundary) for **0.7 s**, so framing wobble and quick tilts don't flip the screen.
  - It never switches while a photo is being saved, while a time-lapse is running (a run keeps the orientation it started with, so every frame matches), or while a menu or dialog is open.
  - With the camera lying flat (face up or down), it keeps the current orientation.
  - A switch is shown with a short toast ("Landscape"), like the look name.
- **Bluetooth:** a new command, `orientation <portrait|landscape-right|landscape-left|auto>`. Like `look`, it's refused during a time-lapse run.
- **Settings during a run:** the on-camera Settings screen can't be opened during a run, because the menu is blocked. So the setting can only change between runs. The web page gets a matching picker.
- **EXIF:** Orientation, ExifImageWidth and ExifImageHeight follow the photo actually stored. The UserComment adds `orientation=…`.

## Non-goals

- Upside-down portrait (USB-C up).
- Re-rotating photos already on the card.

## Constraints

- **Logic placement:** the pure logic is host-tested, as elsewhere in the repo:
  - the orientation transforms: pixel mapping between sensor, photo and screen, and EXIF value per orientation and sensor
  - the frames-on-any-orientation equivalence (strip-by-strip must equal whole-image)
  - the Auto decision state machine (thresholds, dwell, lock)
- **Memory:** the viewfinder buffer is 240×320 or 320×240 RGB565, the same 150 KB either way, so no new large allocation is needed.
- **Hardware unknown:** which landscape side needs the 180° flip must be measured on the board in the first task, as the portrait grip was. The tilt axes for each grip are measured at the same time.

## Left to the builder

- **LVGL mechanism:** the exact rotation approach (`lv_display_set_rotation` with hardware axis swap, or software rotation), provided the screen redraws correctly and the touch-free UI keeps its layout.
- **Toast wording:** the exact toast wording and the Settings row labels.
