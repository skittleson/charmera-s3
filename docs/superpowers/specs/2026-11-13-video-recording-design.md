# Video Recording — Design

Date: 2026-11-13 · Status: approved by Spencer

## Intent

Add a third capture mode, **Video**, beside Single shot and Time-lapse, with the
Charmera's lo-fi charm: grainy, low-res, short. Spencer watches clips **on the
camera's own screen**. Bluetooth plays no role in this feature.

## Goals

- Video mode selectable on-device (camera UI) and via the Bluetooth remote
  (`mode video`), consistent with how Single/Time-lapse work.
- Press shutter → record up to **10 s** or **~2 MB**, whichever first, at
  **320×240 @ ~8 fps**, MJPEG frames each encoded as a JPEG.
- One file per clip on the card, named like photos (`IMG_xxxx.AVI` in the same
  photo directory; number from the same sequence so photos and clips interleave
  in the gallery).
- Tapping a clip in the on-device gallery plays it on screen, looping, until
  tapped again (tap to stop/advance like the current gallery step).
- Look filters (B&W, etc.) apply to video frames — reuse the existing pipeline.
- Delete (single or all) removes clips too.

## Non-goals

- No Bluetooth video download, thumbnails/streaming of video, or browser
  playback. The web app only sees clips as files in `list` (size shown; its
  `thumb`/`get` may 404 gracefully — acceptable).
- No audio (no mic path on this board's pipeline).
- No H.264 (no hardware encoder on ESP32-S3), no re-timestamping, no edit UI.

## Anti-goals

- A clip that fills the card unexpectedly (hence 10 s / ~2 MB cap).
- Blocking the UI while recording (LVGL lock discipline — see existing code).
- Breaking existing photo/lapse flows or the 26-check BLE suite.

## Constraints

- ESP32-S3, no video encoder: MJPEG-in-AVI. Each frame is a JPEG (reuse
  sensor-JPEG or pipeline encode path used for photos).
- Playback: decode each JPEG with the already-vendored `esp_new_jpeg` decoder
  and blit to the LVGL canvas at ~8 fps; keep decode buffers in PSRAM
  (internal heap is scarce — proven by a past regression: never put new static
  buffers in internal DRAM).
- Write via the existing storage write-chunk path; a power loss mid-clip must
  leave no bogus gallery entry (use the existing `.TMP` rename convention;
  rescan deletes leftovers).
- AVI container minimal: RIFF/AVI header + one MJPEG stream (idx1 optional;
  players on-device don't need seek — write a simple sequential AVI, fix up
  sizes at close, or write a streaming-opened header with placeholder sizes
  patched on stop).
- Time-lapse run rule: while recording video, remote commands that alter mode,
  look, or shutter are refused the same way lapse refuses them.

## Decisions (Spencer's)

- Device-only playback; no BLE video transfer now.
- ~10 s / ~2 MB / 320×240 / 8 fps, looks applied.

## Left to the builder

- Exact AVI writer layout, playback task structure, UI affordances for
  recording state (REC badge, elapsed), and where the clip plays (full-screen
  overlay in gallery). Host-test the AVI writer and parser round-trip where
  practical; verify on hardware with `tools/ble_test.py` plus manual gallery
  playback.
