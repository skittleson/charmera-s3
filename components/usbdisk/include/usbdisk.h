#pragma once

#include <stdbool.h>

/* Read-only USB mass-storage mode: the host sees a small FAT32 volume whose DCIM
   folder holds the gallery files; file bytes stream straight from the SD card.
   Entry freezes the card (FatFs unmounted, raw sdmmc handle kept) — no other code
   may touch the card while active. Exit remounts and rescans.

   While active the native USB pads carry TinyUSB instead of USB-Serial-JTAG:
   no logs, no flashing — exit must always be possible from the device (touch)
   or BLE. */

/* False with a reason when entry is refused (no card, busy...). */
bool usbdisk_can_start(const char **reason);
/* Start serving the card read-only over USB. true on success. */
bool usbdisk_start(void);
/* Stop, reattach USB-Serial-JTAG, remount + rescan the card. */
void usbdisk_stop(void);
bool usbdisk_is_active(void);
/* Called from the USB task when the host unplugs: exits drive mode by itself. */
void usbdisk_notify_unplug(void);
