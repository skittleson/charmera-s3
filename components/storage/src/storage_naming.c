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
    static const char suffix[] = ".JPG";
    for (int i = 0; i < 4; i++) {
        if (toupper((unsigned char)name[i]) != prefix[i] ||
            toupper((unsigned char)name[8 + i]) != suffix[i]) {
            return -1;
        }
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

uint32_t storage_photos_remaining(uint64_t free_bytes, uint32_t avg_photo_bytes)
{
    if (avg_photo_bytes == 0) {
        avg_photo_bytes = STORAGE_DEFAULT_PHOTO_BYTES;
    }
    return (uint32_t)(free_bytes / avg_photo_bytes);
}
