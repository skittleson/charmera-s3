# Acceptance — 2026-11-06

Board: Waveshare ESP32-S3-LCD-2 (no touch), OV2640 camera, 16 GB "SC16G" card with a 264 MB
FAT partition. Firmware 0.1.0, branch `v1`.

The spec's six criteria, adjusted for revision 2 (OV2640, 1200×1600 portrait photos):

| # | Criterion | Result | Evidence |
|---|---|---|---|
| 1 | Boots straight into a live viewfinder within a few seconds | **Pass** | Serial: `ui: screen 0` at 1.13 s after reset; viewfinder ~20 fps. Owner confirmed. |
| 2 | Pressing the shutter saves a full-res JPEG to `/DCIM/100CHARM/`, readable on the PC | **Pass (via USB)** | 40+ photos saved as `IMG_####.JPG`. Files pulled back over USB (`tools/dumpfiles.py`) decode on the PC as 1200×1600 RGB JPEGs, 0.4–0.8 MB each. Not yet read in a PC card reader; owner skipped that step. |
| 3 | Every look shows live and the saved photo matches it, no strip seams | **Pass** | 11 of the 12 looks checked in saved photos (Normal, Cool, Warm, B&W, Pixel Blue/Yellow, Film, Charmera, Sticker, PC). No seams at the 240-row strip boundaries. Owner confirmed the live looks, including two-tone pixel looks in a dim room. Pixel Red and Gray share Pixel Blue/Yellow's code path and palette table, and are covered by the host tests. |
| 4 | Gallery browses and deletes photos correctly | **Pass** | Owner: opens on newest, steps older with counter, wraps, delete-this with confirm. Delete-all only removes `IMG_####.JPG`; that is host-tested, not exercised on the board. |
| 5 | Settings shows correct card size / free / photo count; format works | **Partial** | Card size and free space match the storage driver's numbers (264 MB partition), photo count matches the gallery, brightness persists, level toggle works. **Format was not run on the board** (owner skipped). |
| 6 | Pulling the card or having no card never crashes or freezes | **Pass** | Boot with no card, then insert: mounts within 2 s. Pull while running: shows "No card", the shutter says "No card", and it recovers after reinsert (owner). |

Also checked on the board: BOOT short/long/double press, idle dim (30 s) and screen-off
(3 min) with BOOT wake (5 s/15 s test build), on-screen level direction and green when
level, and presses during a save being ignored.

## Measured timings (per photo)

| Step | Time |
|---|---|
| Shutter press → photo captured (camera re-init + 1 frame) | ~2.2 s |
| Rotate + look + JPEG encode | ~1.3 s |
| SD write (card-limited, SPI 20 MHz) | ~2.4–3.4 s |
| **Total until "Saved"** | **~6–7 s** |

Host tests: `ctest` 5/5 suites pass (smoke, input_logic, storage_naming, looks, power_logic).
