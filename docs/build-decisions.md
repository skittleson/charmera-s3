# Decisions made while building v1

Every call made on the owner's behalf during the build, in order, from the build ledger.

## Rulings

- Setup: Ruling: work on branch `v1` in the fresh repo instead of a separate worktree — repo has no other work; main holds only docs — cost if wrong: none, branch can be merged/rebased.
- - T7 ui_note_activity ↔ T10 power_activity — T7 must call power_activity once T10 exists; Ruling: ui_note_activity is a weak no-op until T10 wires it — cost if wrong: trivial.
- Task 0: Ruling: console logs re-enumerate the USB port; use /dev/serial/by-id/... path and tools/serlog.py (no DTR/RTS) instead of idf.py monitor — cost if wrong: none
- Task 1: Ruling: backlight ramp not visually confirmed (I can't see the screen); it's confirmed by the owner during Task 2's display check — cost if wrong: caught in Task 2
- Task 2: Ruling: CST816D sleeps and NACKs when untouched (no RST/INT wired), so esp_lcd_touch_cst816s fails its ID read at init. Replaced with a tiny polling LVGL indev in display.c (reg 0x02..0x06, NACK = released) — cost if wrong: small; swap back to the driver if needed.
- Task 2: Ruling: no board_spi_lock in the LVGL flush — ESP-IDF's SPI master already arbitrates the bus between the LCD and SD devices; board_spi_lock is kept for multi-call SD sequences — cost if wrong: display glitches during SD writes, fix by locking in flush.
- Task 2: Ruling: (supersedes the CST816 polling ruling) owner says the board has NO touchscreen (it's the ESP32-S3-LCD-2). Spec + plan revised with owner approval: BOOT + tilt controls, new Task 2b `input`. Touch code removed — cost if wrong: none.
- Task 2b: Ruling: tilt thresholds lowered to ON 18° / OFF 8° (plan said 30/15) — owner's natural "45°" tilt measured ~21°; spec leaves thresholds to the builder — cost if wrong: accidental look changes; retune constants.
- Task 2b: Ruling: tilt_state_t gained a `stepped` flag (plan struct had dir/since/last_step/armed) — needed to distinguish hold vs repeat — cost: none.
- Task 2b: Ruling: added input_flush(), input_roll_deg(), input_accel_raw() beyond the plan's interface — UI needs flush after long saves; others are diagnostics — cost: none.
- Task 3: Ruling: files < 64 KB are excluded from the avg photo size estimate — a 1 KB test file made "remaining" 208k — cost: none.
- Task 3: Ruling: added storage_numbers_left() so the UI can tell "numbering exhausted" from "card full" (Review Focus 4) — cost: none.
- Task 4: Ruling: added test_strip_with_partial_last_strip and test_rgb565_matches_rgb888 beyond the plan (1944 % 32 != 0; viewfinder must equal photo maths) — cost: none.
- Task 5: Ruling: sensor is OV2640 (2 MP) — owner chose "use OV2640 at 2MP"; OV5640 still auto-detected — cost: none.
- Task 5: Ruling: OV2640 JPEG broken (NO-SOI at 20/16/10 MHz, QVGA..UXGA, quality 8/12, esp32-camera 2.0.15 and 2.1.8) and YUV422 green; RGB565 clean → capture RGB565, encode in firmware — cost: 16-bit colour depth in photos (slight banding).
- Task 5: Ruling: owner switched grip to PORTRAIT, USB-C down (after seeing preview); UI 240x320; photos 1200x1600 rotated in firmware — cost: none.
- Task 5: Ruling: raw-mode fb sizes fixed at init → camera re-inits for each capture (~3.2 s) — cost: shutter lag; Task 7 hides it with freeze-frame + flash.
- Task 6: Ruling: Cool/Warm/B&W moved from sensor to software (OV2640 wb/effect presets unusable with our RGB565 path; JPEG passthrough impossible anyway). look_is_software() now true for all but Normal; host tests updated RED→GREEN — cost: none.
- Task 6: Ruling: preview exposure/gain carried into the full-res capture (manual AEC 1:1) — auto-exposure restarting after re-init gave a bright/dark band mid-photo and brightness mismatch — cost: scenes that change brightness during the 2 s capture won't re-adapt.
- Task 6: Ruling: full-res grab uses CAMERA_GRAB_WHEN_EMPTY (LATEST let the driver overwrite the single buffer mid-encode) — cost: none.
- Task 6: Ruling: pipeline buffers (strip 1.4 MB, JPEG out 1.6 MB, thumb, 32 KB DMA chunk) allocated once at boot via pipeline_init() — per-shot mallocs fragmented PSRAM so the camera's 3.84 MB buffer failed on the 2nd shot — cost: ~3 MB PSRAM held permanently.
- Task 6: Ruling: 4:2:0, quality 92, Huffman task on core 1. Measured per photo: capture 2.2 s, process 1.3 s, SD write 2.4-3.4 s (card limited at 20 MHz SPI) ≈ 6-7 s total — over the plan's 8 s alarm? no; reported to owner.
- Task 7: Ruling (owner): menus = press moves down, HOLD chooses, double press closes; tilt ignored in menus (slight tilt carouseled) — supersedes plan's tilt/short/double scheme.
- Task 7: Ruling (owner): tilt never changes looks; looks chosen from hold-menu Camera/Looks/Gallery/Settings; motion sensor drives an optional on-screen level (default OFF, toggle in Settings). UI drops PREV/NEXT events globally.
- Task 7: Ruling: pixel filters threshold at the scene's mean luma (owner saw solid colour in a dim room); photo reuses the viewfinder-thumbnail threshold; host tests RED→GREEN.
- Task 7: Ruling: input roll axes changed for portrait grip (roll=Y over gravity=X); tilt reads 0 when lying flat — cost: level may need sign flip, verified in Task 9 with the level toggle.
- Task 7: Ruling: power.c stubbed (no-op) until Task 10; gallery/settings placeholders until Tasks 8/9.
- Task 8: Ruling: gallery controls per owner scheme: press = older (wraps), hold = menu (Delete this / Delete all / Back to camera / Cancel), double = camera; confirm opens on Cancel. Preview decode uses esp_new_jpeg 1/5 scale into the pipeline's JPEG buffer (no extra 1.6 MB) — cost: gallery can't run during a save (never happens: single UI task).
- Task 9: Ruling: Settings adds "Level On/Off" (owner) and "Back to camera"; brightness edit = press +10% wrapping 10..100, hold/double to finish (fits the one-button scheme).
- Task 10: Ruling: idle timing pulled into host-tested power_logic (30 s dim to 20 %, 3 min dark + camera PWDN); tilt is filtered before activity so it never wakes; idle restarts after a save/format. Test timers via compile defs, removed after the check.
- Task 11: Ruling: acceptance criterion 2 verified by pulling JPEGs over USB rather than a PC card reader (owner skipped the card-reader step); criterion 5 format not run (owner skipped) — recorded as Partial in docs/acceptance — cost: an untested format path.
- Final: Ruling (owner request): auto-detect OV2640 vs OV5640 at boot with per-sensor best mode. OV5640: 2560x1920 sensor JPEG q12, AF single at full res, EXIF orientation 8, looks via block decode → look_apply_rgb888_sensor (host-tested vs rotate-then-look) → q92 re-encode. Pipeline buffers sized after detection — cost: OV2640 path NOT re-verified on device after the refactor (owner skipped the swap test).
- Final: Ruling: OV5640 q6-9 produced truncated frames after 1-2 shots (all full-res frames NO-EOI); q12 reliable over 10 consecutive shots; quality steps down on failure — cost: OV5640 photos ~0.4-1.0 MB rather than the 1.0+ MB q6 would give.
- Final: Ruling: AF in viewfinder (single or continuous) broke later full-res switches; AF single now runs at full res right before the grab (~1.2 s added shutter lag).
- Final: Ruling (owner: "camera only on while shown"): sensor PWDN whenever the camera screen isn't showing (gallery/settings/screen off), back on when it is; OV5640 XCLK 24→20 MHz, viewfinder PLL cut to a third (SYSCLK 15 MHz, ~7 fps), AF lens released after each shot. Module still very hot per owner; owner investigating a heat sink — cost: choppier viewfinder; quality q12 retested only for single shots at 20 MHz (2 shots ok).

## Time-lapse (owner: "just do it"; all designer calls)

- Ruling: menu entry "Time-lapse" -> interval menu (5 s / 10 s / 30 s / 1 min / 5 min, default 10 s, saved in NVS); press starts; press or hold while running opens "Stop time-lapse?" with Cancel first. "Camera" in the menu returns to plain photos.
- Ruling: each run writes full-quality frames (current look) to a new DCF folder DCIM/NNNLAPSE (101..999) as IMG_0001.JPG...; frames are not in the gallery or the photo count. Video is assembled on a PC (ffmpeg recipe in README) - no on-device AVI in this version.
- Ruling: missed slots (save longer than the interval) are skipped, not caught up; the next frame starts at once.
- Ruling: keeps shooting while the screen dims or sleeps, and while a dialog is open. Intervals >= 30 s power the sensor down between frames (heat); the last frame stays on screen.
- Ruling: stops on No card / Card full / write failure, or after 3 consecutive capture failures, with a toast giving the frame count and folder.
- On the board: a 10 s run saved 10 frames to 101LAPSE on schedule (saves 3-6 s, screen dimmed mid-run); a 30 s run saved 3 frames to 102LAPSE with the sensor off between frames; stop confirmed by the owner.
- Fixed: with the sensor asleep between frames the viewfinder loop spun without yielding and tripped the task watchdog every 5 s (the run itself kept working). It now waits 20 ms per loop; built and flashed, but not re-run on the board (owner skipped).
- Dev note: opening the serial log (tools/serlog.py) restarts the board, which ends a running time-lapse; its frames already on the card are kept.

## Bluetooth remote (owner: nRF Connect + GitHub Pages web page; designer calls below)

- Ruling: one custom service c4a30000-7b6e-4f0a-9d2e-5a1c8e3f6b20 with Status (read/notify JSON), Command (write text), Info (read JSON), plus the standard Battery Service; no pairing; one connection.
- Ruling: changing look/mode/interval or shooting during a time-lapse is refused with an error (the run continues); `shoot` takes one photo in any mode; `lapse start` switches to time-lapse if needed; `lapse stop` needs no on-camera confirm (the page confirms). Mode is not kept across a reboot.
- Ruling: Bluetooth on by default, Settings toggle; Off fully deinitialises NimBLE. NimBLE host memory is in PSRAM, but the controller still takes ~40 KB of internal RAM (free internal 95 -> 56 KB, largest block 54 -> 31 KB). Photos (sensor JPEG, re-encoded look) and time-lapse frames all saved while connected.
- Ruling: Settings list now scrolls (5 rows no longer fit).
- Board: tools/ble_test.py 17/17 from the PC (bleak); tools/web_test.py 14/14 in a headless Chrome with Web Bluetooth enabled. One earlier web_test run failed the look-dropdown check once (2/14 failed); three later runs passed 14/14 and it didn't reproduce.
- Known: the "photos left" estimate drifts after time-lapse runs because small frames lower the average photo size.

## Deferred minor issues (from the final review)

- "card full" estimate can still fail mid-write on the last photo (now returns Card full via free-space check before writing).
- settings paint() calls storage_status() under the display lock.
- photo numbers can be reused after deleting the newest + reboot.
- 350 ms double-press window delays the shutter; quick double tap on camera screen takes no photo; no debounce.
- gallery doesn't refresh on card pull/insert.
- delete-all doesn't flush input.
- tilt events still generated though unused (dropped in UI).
- ui_toast/ui_menu_open continue after display_lock timeout; menu_close/switch_to may leave a menu box.
- boot ~6.4 s with OV5640 (AF firmware upload).
