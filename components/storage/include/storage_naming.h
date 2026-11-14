#pragma once

#include <stddef.h>
#include <stdint.h>

#define STORAGE_MAX_NUMBER 9999
#define STORAGE_DEFAULT_PHOTO_BYTES (2u * 1024 * 1024)

/* "IMG_0042.JPG" -> 42 (case-insensitive, 0001..9999). Anything else -> -1. */
int storage_parse_photo_number(const char *name);

/* Highest photo number among names, plus one. 1 when there are none.
   -1 when the highest is already STORAGE_MAX_NUMBER. Non-photo names are ignored. */
int storage_next_photo_number(const char *const *names, size_t n);

/* 42 -> "IMG_0042.JPG" */
void storage_photo_name(int number, char out[13]);

/* 42 -> "IMG_0042.AVI" (a video clip; same number space as photos) */
void storage_clip_name(int number, char out[13]);

/* free / avg. avg == 0 uses STORAGE_DEFAULT_PHOTO_BYTES. */
uint32_t storage_photos_remaining(uint64_t free_bytes, uint32_t avg_photo_bytes);

/* DCF folder "NNNXXXXX" (100..999 + 5 letters/digits/underscore) -> NNN. Anything else -> -1. */
int storage_parse_folder_number(const char *name);

/* Highest DCF folder number plus one, never below 101 (100 is the photo folder).
   -1 when 999 is already used. */
int storage_next_folder_number(const char *const *names, size_t n);

/* 103 -> "103LAPSE" */
void storage_lapse_folder_name(int number, char out[9]);
