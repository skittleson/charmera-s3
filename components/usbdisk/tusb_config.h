#pragma once

/* TinyUSB configuration for drive mode only: one MSC interface, no CDC, no console.
   CFG_TUSB_MCU comes from the tinyusb component's compile flags. */

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

/* One sector at a time keeps the DMA buffer small; host retries are rare. */
#define CFG_TUD_MSC_BUFSIZE 512
