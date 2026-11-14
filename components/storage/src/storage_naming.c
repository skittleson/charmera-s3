#include "storage_naming.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int storage_parse_photo_number(const char *name)
{
    if (!name || strlen(name) != 12) {
        return -1;
    }
    static const char prefix[] = "IMG_";
    for (int i = 0; i < 4; i++) {
        if (toupper((unsigned char)name[i]) != prefix[i]) {
            return -1;
        }
    }
    /* Photos are .JPG, clips are .AVI; both share the IMG_%04d number space. */
    if (strcasecmp(name + 8, ".JPG") != 0 && strcasecmp(name + 8, ".AVI") != 0) {
        return -1;
    }
    int n = 0;
    for (int i = 4; i < 8; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return -1;
        }
        n = n * 10 + (name[i] - '0');
    }
    return n >= 1 ? n : -1;
}

int storage_next_photo_number(const char *const *names, size_t n)
{
    int max = 0;
    for (size_t i = 0; i < n; i++) {
        int v = storage_parse_photo_number(names[i]);
        if (v > max) {
            max = v;
        }
    }
    return max >= STORAGE_MAX_NUMBER ? -1 : max + 1;
}

void storage_photo_name(int number, char out[13])
{
    snprintf(out, 13, "IMG_%04d.JPG", number);
}

void storage_clip_name(int number, char out[13])
{
    snprintf(out, 13, "IMG_%04d.AVI", number);
}

uint32_t storage_photos_remaining(uint64_t free_bytes, uint32_t avg_photo_bytes)
{
    if (avg_photo_bytes == 0) {
        avg_photo_bytes = STORAGE_DEFAULT_PHOTO_BYTES;
    }
    return (uint32_t)(free_bytes / avg_photo_bytes);
}

int storage_parse_folder_number(const char *name)
{
    if (!name || strlen(name) != 8) {
        return -1;
    }
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return -1;
        }
        n = n * 10 + (name[i] - '0');
    }
    for (int i = 3; i < 8; i++) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') {
            return -1;
        }
    }
    return n >= 100 ? n : -1;
}

int storage_next_folder_number(const char *const *names, size_t n)
{
    int max = 100;
    for (size_t i = 0; i < n; i++) {
        int v = storage_parse_folder_number(names[i]);
        if (v > max) {
            max = v;
        }
    }
    return max >= 999 ? -1 : max + 1;
}

void storage_lapse_folder_name(int number, char out[9])
{
    snprintf(out, 9, "%03dLAPSE", number);
}
