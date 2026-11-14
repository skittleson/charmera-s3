# USB Drive Mode — Design

Date: 2026-11-13 · Status: approved by Spencer

## Intent

Plug the camera into a laptop over USB-C and the **photos folder appears as a
read-only drive** — copy photos off without opening the camera or removing the
card. A card reader built into the camera, for the "grab a few shots now"
moment where a card reader isn't handy (a whole card is still faster on a reader).

## Why read-only

Mass storage exposes raw blocks; the host would write FAT structures the
firmware also owns — two masters, one FAT, guaranteed corruption sooner or
later. Serving reads only lets the firmware freeze its own writes and stay
safe. Pulling photos off never needs host writes. Deletion from the host is a
non-goal (desyncs the camera's number sequence; rescan-on-exit could support
it later, v1 doesn't).

## Goals

- **Entry**: Settings screen item "USB Drive" (touch), and a BLE command
  `usbdisk on` / `usbdisk off`. Entry requires: card mounted, no time-lapse
  running, no clip recording, no photo save busy — refuse otherwise with a
  reason (same refusal vocabulary as lapse).
- **What the host sees**: one drive whose root contains a `DCIM` folder with
  only the gallery files (`IMG_####.JPG`, plus `.THM` sidecars — harmless,
  and lets the laptop preview match). No other card contents.
- **Read-only**: MSC write-protect set; host writes refused.
- **While active**: the screen shows a dedicated page titled **USB Mode** —
  subtitle like "Photos on your computer · read-only" — with a large
  **Disconnect** button that returns the camera to its normal mode; shutter
  and card-touching settings disabled; clock/BLE keep working (they don't
  touch the card); charging continues.
- **Exit**: Disconnect button (touch), `usbdisk off`, or unplug. On exit the
  card is remounted and rescanned so counts/next-number are true again.
- **Format from the web app**: a `format` BLE command wipes the card
  (full FAT format, all folders). Gated: refused while `usbdisk` active,
  lapse running, or save busy; requires an explicit second argument
  (`format CONFIRM`) so a stray write can't wipe a card. The web page shows
  it as a Settings-row danger button behind a type-to-confirm dialog
  ("CONFIRM" typed exactly).

## Non-goals

- Host writes or host deletes.
- Exposing the whole card partition (only the photos folder).
- MTP, PTP, or webcam modes.
- Auto-entering drive mode on plug-in (explicit entry only — the same port is
  used for flashing, see constraints).

## Anti-goals

- Losing photos or corrupting the card because FAT was written from two sides.
- A stuck drive mode that also kills flashing and serial logs with no way out
  from the device itself.
- A format reachable by accident.

## Constraints

- **One USB-C, two identities.** The board's USB-C is the ESP32-S3's native
  USB pads, currently used as USB-Serial-JTAG for flashing and logs. MSC uses
  the same pads via the USB-OTG/TinyUSB device stack: while drive mode is on
  there are no serial logs and no flashing. Exit must therefore always be
  possible from the device (touch Disconnect on the USB Mode page) and from
  BLE — never only from the host.
- FATFS must unmount (or be frozen completely) before MSC starts; all SD
  access is under the existing SPI bus lock — the MSC block reads take it too.
- **Photos-folder-only = synthetic FAT.** Serve a small FAT16/FAT32 image
  built in PSRAM: synthesized directory + FAT for `DCIM/IMG_*.JPG`, and a
  virtual-cluster → real-card-sector remap table so file bytes stream
  directly from the card (never copied; remap for a full card is tens of KB).
  Host may read any LBA at any time — every read goes through the remap;
  writes refused. Build must stay in PSRAM (internal heap is scarce — the
  standing project rule).
- TinyUSB must come in as an managed component; console/USB-JTAG runtime
  detach before TinyUSB init, reattach on exit so `idf.py flash` and logs
  work again.
- Format = full `f_mkfs` on the card (not quick): wipes **everything** on the
  card including time-lapse folders, not just DCIM. The dialog says so.

## Decisions (Spencer's)

- Photos folder only, read-only acceptable.
- Format belongs in the web UX too (danger button, typed confirmation).

## Left to the builder

- Virtual-FAT layout details (FAT16 vs FAT32, volume size choice, how `.THM`
  are included), the exact TinyUSB wiring/component, and whether the drive
  mode page replaces the camera screen or overlays it.
- Whether the web app also gets an "USB Drive" toggle button (the BLE command
  exists either way; the page change is cheap but the page is owned by the
  coordinating session — decide at implementation time).
- Host-side validation: mount the LUN in a Linux test (loop/tinyusb host or a
  real laptop) and byte-compare 2-3 photos; document as the verification bar.
