#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "storage_naming.h"

#define STORAGE_MOUNT     "/sdcard"
#define STORAGE_PHOTO_DIR STORAGE_MOUNT "/DCIM/100CHARM"

typedef enum { STORAGE_OK, STORAGE_NO_CARD, STORAGE_ERROR } storage_state_t;

typedef struct {
    uint64_t total_bytes, free_bytes;
    uint32_t photo_count;
} storage_info_t;

/* Mounts lazily; notices a pulled card and re-mounts a reinserted one. */
storage_state_t storage_status(void);
esp_err_t storage_info(storage_info_t *out);

/* Photo numbers in ascending order. Caller frees *numbers. */
esp_err_t storage_list_photos(int **numbers, size_t *count);
esp_err_t storage_photo_path(int number, char *out, size_t len);

/* Open the next IMG_####.TMP for writing. NULL if no card or numbering is used up. */
FILE *storage_begin_photo(int *out_number);
/* Flush, close and rename TMP -> JPG. */
esp_err_t storage_commit_photo(FILE *f, int number);
/* Close and delete the TMP. */
void storage_abort_photo(FILE *f, int number);

esp_err_t storage_delete_photo(int number);
/* Only IMG_*.JPG in the photo folder. */
esp_err_t storage_delete_all_photos(void);
esp_err_t storage_format(void);

/* Sidecar thumbnail (120x160 JPEG, IMG_####.THM) written beside the photo. NULL when
   unavailable. Paths for a photo's JPEG and THM; both return ESP_OK on success. */
esp_err_t storage_thumb_path(int number, char *out, size_t len);
bool storage_file_size(const char *path, size_t *out);
/* Write the sidecar for `number` from jpeg data. Best effort: on failure the photo
   still saves; the THM is simply absent. */
void storage_thumb_write(int number, const uint8_t *jpeg, size_t len);

uint32_t storage_photos_remaining_now(void);
/* True when the card has photo numbers left (IMG_9999 not reached). */
bool storage_numbers_left(void);

/* Send photos to a new DCIM/NNNLAPSE folder (numbered from IMG_0001) until
   storage_lapse_end(). They don't appear in the gallery or the photo count.
   out_folder gets the folder name, e.g. "101LAPSE". */
esp_err_t storage_lapse_begin(char *out_folder, size_t len);
void storage_lapse_end(void);
