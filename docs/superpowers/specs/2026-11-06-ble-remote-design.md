# Charmera-S3 — Bluetooth remote — Design

**Date:** 2026-11-06
**Status:** Draft for owner review. Builder-check answers are marked as designer calls.

## Intent

See what the camera is doing and drive it without touching it. The owner connects
two ways:

- **nRF Connect** on a phone, to inspect and poke the camera directly.
- **A web page in this repo**, published with GitHub Pages and opened in Chrome. It connects
  over Web Bluetooth, shows the camera's state live, and controls it.

## Goals (owner)

- **Read live:** photos taken, photos left, battery, which camera is fitted, the current
  look, and the current mode.
- **Change:** the look, and the mode (single shot or time-lapse).
- **Act:** take a photo, start a time-lapse, and stop a time-lapse.

## Non-goals (owner)

- **No photo previews or downloads over Bluetooth for now** ("it can be large"). After a remote
  shot the page shows the file name and the updated counts, not the image.

## Anti-goals (designer)

- The button on the camera must keep working exactly as it does now. Bluetooth is an extra
  way in, never a replacement.
- A Bluetooth command must never corrupt a photo that's being saved, or break a
  time-lapse run.

## Behaviour

### What's exposed

- **Status**, readable and notified. One small UTF-8 JSON object, so nRF Connect shows
  it as text and the page parses it directly. Fields:
  - `photos` (count in `100CHARM`) and `left` (photos-remaining estimate)
  - `battery`: percent, plus `usb: true` on USB power
  - `camera`: `"OV5640"`, `"OV2640"` or `"none"`
  - `look`: index and name
  - `mode`: `"single"` or `"lapse"`, with `interval` (index and name)
  - `lapse`: `running`, `frames`, `elapsed_s`, `folder`
  - `card`: `"ok"`, `"none"` or `"full"`
  - `busy`: true while a photo or frame is being saved
  - `last`: name of the last saved file, e.g. `"IMG_0093"` or `"101LAPSE/IMG_0012"`

  It's sent as a notification whenever any field changes, and every 5 s at most for the
  battery and remaining-count drift.
- **Info**, read-only JSON: the firmware version, the list of look names, and the list of
  interval names, so the page doesn't hard-code them.
- **Command**, writable UTF-8 text, one command per write. Text keeps it typeable in nRF
  Connect:
  - `look <n>`
  - `mode single`
  - `mode lapse` (optionally `mode lapse <interval index>`)
  - `interval <n>`
  - `shoot`
  - `lapse start`
  - `lapse stop`
- **Standard Battery Service (0x180F)** too, so nRF Connect and other generic apps show
  battery without knowing the custom service *(designer call)*.

### Rules (designer calls)

- **Who runs commands.** Commands are queued and carried out by the UI task, which already
  owns the camera, the card and the screen. The Bluetooth task never touches them directly.
- **Busy camera.** A command that arrives during a save waits until the save finishes; it isn't
  dropped. `shoot` and `lapse start` while busy are rejected, and the status shows `busy`.
- **Screen.** If the gallery or settings screen is open, a command switches to the camera
  screen first. A remote command also counts as activity, so it wakes a sleeping screen and
  the photo appears there.
- **Stopping a time-lapse.** `lapse stop` stops at once with no on-camera confirm, since the
  page asks for its own confirmation.
- **Changes while running.** `look`, `mode` and `interval` are **refused with an error**
  while a time-lapse is running, and the run keeps going. The page greys those controls
  out during a run. A remote tap must never end a long run by accident.
- **How `shoot` and `lapse start` relate to the mode:**
  - `shoot` takes one ordinary photo into `100CHARM`, whatever the mode, and doesn't change
    the mode. It's refused while a time-lapse is running, so the run stays on schedule.
  - `lapse start` switches to time-lapse mode with the current interval if needed, then
    starts a run.
  - So the page's shutter and Start buttons don't depend on the mode switch.
- **The mode isn't kept across a reboot**, the same as on the camera: it always boots into
  single shot. The look and the interval are kept, as they are today.
- **Keeping settings.** Settings changed over Bluetooth are saved, the same as changing them
  on the camera.
- **Errors.** A command that can't run (bad value, refused, no card) gets a short reason in
  the status (`"error": "..."`). The reason clears on the next successful command.

### Bluetooth on/off (designer calls)

- **Settings toggle.** Settings gets a **Bluetooth** row (On/Off). The default is **On**, so
  the remote works out of the box. Off fully shuts the Bluetooth stack down and frees its
  memory; On starts it again, at boot or from Settings.
- **Name.** Advertises as `Charmera-S3` with the custom service UUID, so the page's device
  picker can filter on it.
- **Connections.** One connection at a time. Advertising restarts after a disconnect.
- **Pairing.** None. Anyone nearby can connect, which is acceptable for a hobby camera.
- **Sleep.** It keeps advertising while the screen sleeps.

### Web page (owner: GitHub Pages, Chrome)

- **Location.** `docs/index.html` plus its script and style. Plain HTML/JS with no build step
  and no dependencies. GitHub Pages serves it from `main` → `/docs` once the owner turns
  Pages on.
- **Controls.**
  - a Connect button, which opens Chrome's device picker filtered to the camera
  - live status cards
  - a look picker and a mode switch (single or time-lapse) with an interval picker
  - a big shutter button, and Start/Stop time-lapse buttons (Stop asks to confirm)
- **Updates.** The page reflects notifications live, greys out controls while `busy` or
  disconnected, and offers to reconnect after a drop.
- **Browsers.** Web Bluetooth needs Chrome or Edge on desktop or Android. iOS Safari can't use
  it, and the page says so plainly instead of failing silently.

## Constraints

- **Hardware.** ESP32-S3 with 8 MB PSRAM, already running the camera, LVGL and the SD card
  over SPI. Internal RAM is the tight resource. Use **NimBLE**, and let it allocate from
  PSRAM where it can (`BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`). Log the internal free heap
  and the largest internal block at boot, before and after Bluetooth starts. The release gate
  is that, with Bluetooth on and connected, the board still takes photos with every camera
  path (OV5640 sensor JPEG, a re-encoded look, and a time-lapse frame) without allocation
  failures. If it fails, move more NimBLE memory to PSRAM before cutting features.
- **Flash.** The app partition is 6 MB and the current image is about 1 MB, so there's plenty of room.
- **MTU.** The status JSON is about 250–300 bytes. Request a 517-byte MTU (Chrome and
  nRF Connect negotiate high), and keep the JSON compact. Reads must also work at the
  default MTU through long reads; notifications at a small MTU may be truncated, so the
  page re-reads Status after any notification that doesn't parse as JSON.
- **Testing.** The command parser and the status JSON builder are pure C and host-tested,
  like the other logic components.
- **Heat.** No change to camera power rules: the sensor still only runs while the
  viewfinder is showing or a photo is being taken.

## Left to the builder

- **UUIDs.** Pick one random 128-bit UUID base and fix it in the code and the page.
- **Status fields.** Exact JSON field formatting, as long as the fields above are present.
- **Page style.** Page layout and styling, kept simple and readable on a phone.
- **Stack config.** NimBLE configuration details: roles (peripheral only), connection count
  1, and advertising interval.
