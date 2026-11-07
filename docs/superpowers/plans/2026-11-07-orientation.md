# Orientation Setting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hold the camera portrait, landscape USB-C right, landscape USB-C left, or Auto; the screen, viewfinder, looks, saved photos, EXIF, gallery, level and Bluetooth all follow.

**Architecture:** A new pure-C `orient` component, host-tested, owns three things: the orientation types, the pixel mapping between "view" (the photo as seen) and "sensor" (the native landscape frame), and the Auto decision state machine. Looks take an orientation for sensor-orientation strips and size their cells and frames from the image's short edge. The camera, pipeline, display and UI query a single "current orientation", set by the setting or by Auto on the camera screen.

**Tech Stack:** ESP-IDF v5.5, LVGL 9 + esp_lvgl_port (`lv_display_set_rotation`, hardware swap/mirror), esp32-camera, esp_new_jpeg, Unity host tests.

**Spec:** `docs/superpowers/specs/2026-11-07-orientation-design.md`

## Global Constraints

- **Orientations:**
  - `ORIENT_PORTRAIT` is the default: USB-C down, and the photo is the sensor turned 90° counter-clockwise, as today.
  - `ORIENT_LAND_RIGHT` (USB-C right) and `ORIENT_LAND_LEFT` (USB-C left). One of them is the sensor as-is and the other is the sensor turned 180°; Task 3 measures which.
  - The setting adds `ORIENT_SETTING_AUTO`. There is no upside-down portrait.
- **NVS:** key `orient` (u8: 0 portrait, 1 right, 2 left, 3 auto). It's changed in Settings and over Bluetooth, and Bluetooth refuses it during a time-lapse run.
- **Photos:**

  | Path | Portrait | Landscape |
  |---|---|---|
  | OV5640, sensor JPEG | stored 2560×1920 + EXIF Orientation 8, unchanged | same stored size + EXIF 1 or 3 |
  | OV2640, raw | 1200×1600 | 1600×1200, rows in view order |

  ExifImageWidth/Height are the stored size, and the UserComment adds `orientation=<name>`.
- **Looks:** pixel cell = `look_pixel_block(min(w,h))`; frame unit `t = max(4, min(w,h)/32)`. Portrait output must stay byte-identical to today; the existing look tests prove it.
- **Auto:**
  - It runs on the camera screen only.
  - It switches after 0.7 s with the gravity angle more than 30° past the 45° boundary, measured from the current orientation's centre (so more than 75° from it, and within 15° of the target's centre).
  - It ignores a flat camera, when the in-plane gravity is less than 0.5 g.
  - It's locked while busy, during a time-lapse run, while a menu is open, and while the screen is asleep.
  - A switch shows a toast with the orientation's name.
- **Bluetooth:**
  - The command is `orientation <portrait|landscape-right|landscape-left|auto>`.
  - The status adds `"orientation"` (in effect: `portrait`, `landscape-right` or `landscape-left`) and `"orientationSetting"` (those three, plus `auto`).

## Review Focus

- **Flat camera:** a camera lying flat, or tilted towards or away from you, must never flip the screen. (Task 1 `test_auto_ignores_flat`.)
- **Wobble at 45°:** a camera hovering around 45° must not oscillate. (Task 1 `test_auto_hysteresis_no_flip_flop`.)
- **Old photos in the gallery:** existing portrait photos must show upright on a landscape screen, and new landscape photos must show upright on a portrait screen. (Task 4 board check.)
- **Settings changed during a run:** an orientation change during a time-lapse, from Settings over Bluetooth or from Auto, must not change that run's frames. (Task 1 `test_auto_locked`, Task 5 `remote_check` test, Task 5 board check.)
- **Odd image sizes:** strip boundaries and the 180° flip must keep pixel cells aligned in view coordinates for both sensors. (Task 2 `test_sensor_strips_match_view_every_orientation`.)

---

### Task 1: `orient` component — mapping, EXIF value, Auto (host-tested)

**Files:**
- Create: `components/orient/include/orient.h`, `components/orient/orient.c`, `components/orient/CMakeLists.txt` (no deps)
- Test: `tests/host/test_orient.c`; add to `tests/host/CMakeLists.txt` (with `components/orient/include` on the include path)

**Interfaces — Produces:**

```c
typedef enum { ORIENT_PORTRAIT, ORIENT_LAND_RIGHT, ORIENT_LAND_LEFT, ORIENT_COUNT } orient_t;
#define ORIENT_SETTING_AUTO 3
/* Which landscape side is the sensor as-is (the other is 180°). Measured on the board in Task 3. */
#ifndef ORIENT_SENSOR_NATIVE
#define ORIENT_SENSOR_NATIVE ORIENT_LAND_RIGHT
#endif
const char *orient_name(int setting_or_orient);  /* "portrait","landscape-right","landscape-left","auto" */
int orient_parse(const char *name);             /* inverse; -1 if unknown */
void orient_view_size(orient_t o, int sw, int sh, int *vw, int *vh);
/* View pixel (vx,vy) of a sw x sh sensor image -> sensor pixel. */
void orient_view_to_sensor(orient_t o, int vx, int vy, int sw, int sh, int *sx, int *sy);
uint16_t orient_exif(orient_t o);               /* EXIF Orientation for an image stored in sensor orientation: 8 / 1 / 3 */
float orient_angle_deg(float g_down, float g_side); /* gravity angle in the screen plane; 0 = portrait */
float orient_centre_deg(orient_t o);                /* 0, +90 or -90 (sign per Task 3) */
typedef struct { orient_t current; int candidate; int64_t since_ms; } orient_auto_t;
void orient_auto_init(orient_auto_t *a, orient_t start);
/* Feed one accelerometer sample (milli-g, screen-plane axes + face axis). Returns true when `current` changes. */
bool orient_auto_update(orient_auto_t *a, float g_down, float g_side, float g_face, bool locked, int64_t now_ms);
float orient_roll_deg(orient_t o, float g_down, float g_side); /* level-line roll for that grip */
```

- [ ] **Step 1: Write the failing tests:**
  - `test_names_roundtrip`: all four names, plus `orient_parse("sideways") == -1`.
  - `test_view_size`: portrait 1920×2560 → 1920×2560 view of a 2560×1920 sensor (i.e. vw=sh, vh=sw); landscape 2560×1920.
  - `test_view_to_sensor_corners`:
    - portrait (0,0) → (sw-1, 0), as today's `portrait_to_land`
    - native landscape is the identity
    - the other landscape side (0,0) → (sw-1, sh-1)
  - `test_exif_values`: portrait 8, native 1, the other 3.
  - `test_auto_switches_after_dwell`: feed the landscape angle (centre ±0°) for 600 ms → no switch; at 700 ms → switch.
  - `test_auto_hysteresis_no_flip_flop`: alternate 40° and 50° samples for 5 s → stays portrait. A steady 80° switches.
  - `test_auto_ignores_flat`: in-plane magnitude 300 mg with face 950 mg at a landscape angle → no switch.
  - `test_auto_locked`: `locked=true` at a steady landscape angle for 2 s → no switch. After the unlock, it switches only after a fresh 0.7 s.
  - `test_roll_relative`: in portrait, `orient_roll_deg` equals today's roll formula (`atan2(side, down)`). In landscape it's measured from ±90°.
- [ ] **Step 2: Run the tests.** Expect a build failure or test failures against stubs.
- [ ] **Step 3: Implement.** Angles use `atan2f`. The candidate must hold for 700 ms with |angle − candidate centre| < 30° and in-plane magnitude ≥ 0.5 × total.
- [ ] **Step 4: Run all host tests.** All pass.
- [ ] **Step 5: Commit** `feat(orient): orientation mapping, EXIF values and Auto state machine (host-tested)`.

### Task 2: looks in any orientation (host-tested)

**Files:**
- Modify: `components/looks/include/looks.h`, `components/looks/src/looks.c`, `components/looks/src/frames.c`, `components/looks/src/looks_internal.h`, `components/looks/CMakeLists.txt` (REQUIRES orient)
- Test: `tests/host/test_looks.c`; add `orient.c` to the looks test sources in CMake

**Interfaces:**
- **Consumes:** Task 1's `orient_t`, `orient_view_to_sensor` and `orient_view_size`.
- **Produces:**
  - `look_apply_rgb888_sensor(look_t l, uint8_t *rgb, int sensor_w, int rows, int y0, int img_sensor_w, int img_sensor_h, orient_t o)` gains a trailing `o`. Its contract: once the image is mapped to view orientation, it equals `look_apply_rgb888` on the whole view image.
  - `look_pixel_block(int short_edge)` is unchanged in body; callers now pass `min(w,h)`.
  - `look_strip_align(orient_t o, int img_sensor_h, int b)` returns the row offset that strips must be multiples of, measured from row 0 (or from the bottom for flips).

- [ ] **Step 1: Write the failing tests:**
  - `test_sensor_strips_match_view_every_orientation`: for each orientation, for both 2560×1920 and 1600×1200 random images, and for every software look, apply in sensor strips of the pipeline's row count, map to view, and compare with the whole view image. This replaces `test_sensor_strips_match_portrait_for_every_look`.
  - `test_landscape_view_uses_short_edge`: the cell size for a 320×240 view equals the cell size for 240×320.
  - All existing portrait tests stay as they are, which proves portrait output is unchanged.
- [ ] **Step 2: Run the tests.** Expect failures.
- [ ] **Step 3: Implement.**
  - Generalise `pixelate_sensor` and `frame_apply_sensor` to walk view coordinates through `orient_view_to_sensor`. Keep the current portrait fast path's memory order: walk sensor rows, and compute the view coordinate per pixel.
  - `frame_apply`, `look_apply_rgb888` and `look_apply_rgb565` use `min(img_w,img_h)` for `b` and `t`.
- [ ] **Step 4: Run all host tests.** All pass.
- [ ] **Step 5: Commit** `feat(looks): looks and frames in any orientation, sized from the short edge`.

### Task 3: screen, viewfinder, level and setting at runtime (+ board calibration)

**Files:**
- Modify:
  - `components/display/display.c` + `display.h`: `void display_set_orientation(orient_t o)` calls `lv_display_set_rotation`, with esp_lvgl_port doing the hardware swap/mirror. `int display_width(void)` and `int display_height(void)` are also added.
  - `components/camera/camera.c` + `camera.h`: `camera_set_orientation(orient_t o)`. The preview is view-oriented with `camera_preview_w()/h()` (240×320 or 320×240, same buffer). `camera_photo_width()/height()`, `camera_photo_rows_rgb888` and `camera_photo_thumbnail` follow the orientation, through `orient_view_to_sensor`. `camera_rotate_to_portrait` is replaced by `camera_rotate_to_view`.
  - `components/input/input.c` + `input.h`: `void input_gravity(float *down, float *side, float *face)`, in milli-g on the portrait-calibrated axes. `input_roll_deg()` is removed, and callers use `orient_roll_deg`.
  - `main/settings.{h,c}`: `int settings_orientation(void)` (0–3, default 0) and `settings_set_orientation(int)`.
  - `main/orientation.{c,h}` (new): `orient_t orientation_current(void)`, `void orientation_apply(orient_t o)`, which sets display, camera, toast and the camera-screen relayout, and `void orientation_setting_changed(void)`.
  - `main/ui/screen_camera.c`: the image descriptor and level use the current view size, and the overlays are aligned relative to the screen. It rebuilds on an orientation change through the existing enter/leave via `ui_show_now(UI_SCREEN_CAMERA)`.
  - `main/ui/screen_settings.c`: a new **Orientation** row that cycles Portrait, Landscape right, Landscape left, Auto on hold, saves, and calls `orientation_setting_changed()`.
  - `main/CMakeLists.txt`.

- [ ] **Step 1: Implement the above.** The fixed orientations work in this task; Auto is a no-op for now and behaves as Portrait.
- [ ] **Step 2: Build.** Run `idf.py build`, with no warnings from `main/` or `components/`, and run the host tests.
- [ ] **Step 3: Board calibration (owner).**
  - Choose Landscape (USB-C right) in Settings.
  - Owner reports whether the viewfinder is upright. If it's upside down, swap `ORIENT_SENSOR_NATIVE` to `ORIENT_LAND_LEFT`.
  - Record the accelerometer readings (`input_gravity`) in all three grips and fix the sign of `orient_centre_deg` for landscape-right. The owner holds each grip for 4 s while I log.
  - Commit the measured constants, with a comment saying how they were measured.
- [ ] **Step 4: Board check (owner).** In each of the three fixed orientations:
  - the menus, toasts and Settings read upright and nothing is cut off
  - the viewfinder is upright and fills the screen
  - the level is horizontal when the camera is level
- [ ] **Step 5: Commit** `feat: orientation setting rotates screen, viewfinder and level`.

### Task 4: photos, EXIF and gallery in every orientation

**Files:**
- Modify: `components/pipeline/pipeline.c`:
  - pass `camera_orientation()` to `look_apply_rgb888_sensor`
  - use the strip sizing from `look_pixel_block(min)` and `look_strip_align`
  - EXIF orientation `orient_exif(o)` for sensor JPEG, and 1 for raw
  - width/height from the camera
- Modify: `main/ui/screen_camera.c`: add `orientation=<name>` to both UserComment strings.
- Modify: `main/ui/jpeg_preview.{c,h}`: `jpeg_preview_load(path, out, int out_w, int out_h)` fits any photo upright into the current screen size, using its EXIF orientation (1, 3, 6 or 8) and size.
- Modify: `main/ui/screen_gallery.c`: the image descriptor follows `display_width/height`.

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Build and host tests.** Both pass.
- [ ] **Step 3: Board check.**
  - In each orientation, take a Normal photo and a Film Frame photo.
  - Then the owner moves the card to the laptop's reader, and I run `exiftool -G1 -a -Orientation -ExifImageWidth -ExifImageHeight -UserComment`.
  - Each photo opens upright in an image viewer, the frames sit on the visible edges, and landscape photos are 2560×1920 or 1600×1200.
  - The gallery shows old portrait photos and new landscape photos upright in both screen orientations.
- [ ] **Step 4: Commit** `feat: photos, EXIF and gallery follow the orientation`.

### Task 5: Auto, Bluetooth and the web page

**Files:**
- Modify: `main/orientation.c`: on the camera screen only, feed `orient_auto_update` every input poll from `input_gravity`. `locked` is busy, or lapse running, or `ui_menu_is_open()`, or `power_is_asleep()`. On a change, call `orientation_apply`.
- Modify: `components/remote/include/remote_proto.h` + `src/remote_proto.c`: add `RC_ORIENTATION` (arg 0–3) to the parse rules, so `remote_check` refuses it while running. Add `orientation` and `orientation_setting` status fields (`const char*`).
- Modify: `tests/host/test_remote_proto.c`, `main/remote_bridge.c`, `main/ui/screen_camera.c` (`ui_camera_remote` case, `ui_camera_fill_status`), `docs/app.js`, `docs/index.html` (Orientation select), `docs/sw.js` (cache v3), `tools/ble_test.py`.

- [ ] **Step 1: Write the failing proto tests:**
  - `test_parse_orientation`: `"orientation landscape-left"` → `{RC_ORIENTATION,2}`, `"orientation auto"` → 3, `"orientation up"` → `"bad orientation"`.
  - In `test_check_refuses_changes_while_running`, add `RC_ORIENTATION`.
  - `test_status_json_fields` expects `"orientation":"portrait"` and `"orientationSetting":"auto"`.
- [ ] **Step 2: Run the tests.** Expect failures.
- [ ] **Step 3: Implement.** Do the proto changes, then the bridge, Auto wiring and page.
- [ ] **Step 4: Host tests + build.** Both pass.
- [ ] **Step 5: Board check.**
  - `tools/ble_test.py` is extended with `orientation landscape-right`: status shows it, and it's refused during a run.
  - Owner: set Auto, then turn the camera slowly through the three grips. It switches after a brief hold, doesn't flicker near 45°, ignores the camera lying flat, and doesn't switch during a time-lapse.
- [ ] **Step 6: Commit** `feat: Auto orientation, Bluetooth orientation command and page picker`.

### Task 6: docs, review, publish

- [ ] **Step 1: README.** Add an Orientation section covering the setting, Auto behaviour and photo sizes. Add a Settings feature line, the Bluetooth command and status fields, an EXIF table note, and a status-table row.
- [ ] **Step 2: Decisions log.** Add the rulings to `docs/build-decisions.md`.
- [ ] **Step 3: Final review and fixes.** Run the final whole-branch review, then fix anything it finds.
- [ ] **Step 4: Publish.** Merge into `main` and push as `skittleson`, then switch `gh` back to `spencerkittleson`.
