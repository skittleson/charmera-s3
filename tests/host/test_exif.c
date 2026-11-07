#include <string.h>

#include "exif.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- a tiny independent EXIF reader for the assertions ---- */

static const uint8_t *tiff; /* start of the TIFF header inside the segment */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Entry for tag in the IFD at offset ifd, or NULL. Also checks the IFD is sorted. */
static const uint8_t *entry(uint32_t ifd, uint16_t tag)
{
    const uint16_t n = be16(tiff + ifd);
    uint16_t prev = 0;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *e = tiff + ifd + 2 + 12 * i;
        TEST_ASSERT_TRUE_MESSAGE(be16(e) > prev, "IFD entries must be in ascending tag order");
        prev = be16(e);
        if (be16(e) == tag) {
            return e;
        }
    }
    return NULL;
}

static const uint8_t *value(const uint8_t *e)
{
    static const int size[] = {0, 1, 1, 2, 4, 8, 0, 1};
    const uint32_t bytes = size[be16(e + 2)] * be32(e + 4);
    return bytes <= 4 ? e + 8 : tiff + be32(e + 8);
}

static uint32_t number(uint32_t ifd, uint16_t tag)
{
    const uint8_t *e = entry(ifd, tag);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "missing numeric tag");
    return be16(e + 2) == 3 ? be16(value(e)) : be32(value(e));
}

static const char *text(uint32_t ifd, uint16_t tag)
{
    const uint8_t *e = entry(ifd, tag);
    return e ? (const char *)value(e) : NULL;
}

static uint8_t buf[2048];

static exif_info_t sample(void)
{
    return (exif_info_t){
        .make = "Charmera-S3",
        .model = "Charmera-S3 (OV5640)",
        .software = "charmera-s3 0.1.0",
        .serial = "30EDA02A2860",
        .description = "Time-lapse 101LAPSE frame 12, every 10 s",
        .comment = "mode=timelapse; folder=101LAPSE; frame=12; interval_s=10; look=Normal",
        .datetime = "2026:11:06 19:42:05",
        .offset = "-08:00",
        .orientation = 8,
        .width = 2560,
        .height = 1920,
    };
}

static uint32_t build(const exif_info_t *in)
{
    const size_t n = exif_build(in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_HEX8(0xff, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xe1, buf[1]);
    TEST_ASSERT_EQUAL(n - 2, be16(buf + 2)); /* APP1 length excludes the marker */
    TEST_ASSERT_EQUAL_MEMORY("Exif\0\0MM\0\x2a\0\0\0\x08", buf + 4, 14);
    tiff = buf + 10;
    return be32(entry(8, 0x8769) ? value(entry(8, 0x8769)) : (const uint8_t *)"\0\0\0\0");
}

static void test_ifd0_tags(void)
{
    const exif_info_t in = sample();
    const uint32_t exif_ifd = build(&in);
    TEST_ASSERT_TRUE(exif_ifd > 8);
    TEST_ASSERT_EQUAL_STRING("Charmera-S3", text(8, 0x010f));
    TEST_ASSERT_EQUAL_STRING("Charmera-S3 (OV5640)", text(8, 0x0110));
    TEST_ASSERT_EQUAL_STRING("charmera-s3 0.1.0", text(8, 0x0131));
    TEST_ASSERT_EQUAL_STRING("Time-lapse 101LAPSE frame 12, every 10 s", text(8, 0x010e));
    TEST_ASSERT_EQUAL_STRING("2026:11:06 19:42:05", text(8, 0x0132));
    TEST_ASSERT_EQUAL(8, number(8, 0x0112));
    TEST_ASSERT_EQUAL(2, number(8, 0x0128)); /* inches */
    TEST_ASSERT_EQUAL(1, number(8, 0x0213));
    const uint8_t *xres = value(entry(8, 0x011a));
    TEST_ASSERT_EQUAL(72, be32(xres));
    TEST_ASSERT_EQUAL(1, be32(xres + 4));
}

static void test_exif_ifd_tags(void)
{
    const exif_info_t in = sample();
    const uint32_t e = build(&in);
    TEST_ASSERT_EQUAL_MEMORY("0232", value(entry(e, 0x9000)), 4);
    TEST_ASSERT_EQUAL_STRING("2026:11:06 19:42:05", text(e, 0x9003));
    TEST_ASSERT_EQUAL_STRING("2026:11:06 19:42:05", text(e, 0x9004));
    TEST_ASSERT_EQUAL_STRING("-08:00", text(e, 0x9010));
    TEST_ASSERT_EQUAL_STRING("-08:00", text(e, 0x9011));
    TEST_ASSERT_EQUAL_STRING("-08:00", text(e, 0x9012));
    TEST_ASSERT_EQUAL(0x20, number(e, 0x9209)); /* no flash function */
    const uint8_t *uc = value(entry(e, 0x9286));
    TEST_ASSERT_EQUAL_MEMORY("ASCII\0\0\0mode=timelapse;", uc, 22);
    TEST_ASSERT_EQUAL_MEMORY("0100", value(entry(e, 0xa000)), 4);
    TEST_ASSERT_EQUAL(1, number(e, 0xa001)); /* sRGB */
    TEST_ASSERT_EQUAL(2560, number(e, 0xa002));
    TEST_ASSERT_EQUAL(1920, number(e, 0xa003));
    TEST_ASSERT_EQUAL(2, number(e, 0xa217)); /* one-chip colour sensor */
    TEST_ASSERT_EQUAL(0, number(e, 0xa402));
    TEST_ASSERT_EQUAL(0, number(e, 0xa403));
    TEST_ASSERT_EQUAL_STRING("30EDA02A2860", text(e, 0xa431));
}

static void test_optional_tags_omitted(void)
{
    exif_info_t in = sample();
    in.datetime = NULL;
    in.description = NULL;
    in.comment = NULL;
    in.serial = NULL;
    const uint32_t e = build(&in);
    TEST_ASSERT_NULL(entry(8, 0x0132));
    TEST_ASSERT_NULL(entry(8, 0x010e));
    TEST_ASSERT_NULL(entry(e, 0x9003));
    TEST_ASSERT_NULL(entry(e, 0x9010));
    TEST_ASSERT_NULL(entry(e, 0x9286));
    TEST_ASSERT_NULL(entry(e, 0xa431));
    TEST_ASSERT_EQUAL_STRING("Charmera-S3", text(8, 0x010f)); /* the rest still there */
    TEST_ASSERT_EQUAL(1920, number(e, 0xa003));
}

static void test_data_offsets_are_even_and_inside(void)
{
    const exif_info_t in = sample();
    const size_t n = exif_build(&in, buf, sizeof(buf));
    tiff = buf + 10;
    const uint32_t e = build(&in);
    const uint32_t ifds[] = {8, e};
    for (int k = 0; k < 2; k++) {
        for (uint16_t i = 0; i < be16(tiff + ifds[k]); i++) {
            const uint8_t *en = tiff + ifds[k] + 2 + 12 * i;
            const uint8_t *v = value(en);
            if (v != en + 8) {
                TEST_ASSERT_EQUAL(0, (v - tiff) % 2);
                TEST_ASSERT_TRUE(v - buf < (long)n);
            }
        }
    }
}

static void test_too_small_returns_0(void)
{
    const exif_info_t in = sample();
    TEST_ASSERT_EQUAL(0, exif_build(&in, buf, 100));
}

static void test_format_datetime(void)
{
    char dt[20], off[7];
    exif_format_datetime(1793072525, -480, dt, off); /* 2026-10-27 03:42:05 UTC */
    TEST_ASSERT_EQUAL_STRING("2026:10:26 19:42:05", dt);
    TEST_ASSERT_EQUAL_STRING("-08:00", off);
    exif_format_datetime(1793072525, 330, dt, off);
    TEST_ASSERT_EQUAL_STRING("2026:10:27 09:12:05", dt);
    TEST_ASSERT_EQUAL_STRING("+05:30", off);
    exif_format_datetime(0, 0, dt, off);
    TEST_ASSERT_EQUAL_STRING("1970:01:01 00:00:00", dt);
    TEST_ASSERT_EQUAL_STRING("+00:00", off);
}

static void test_read_orientation_of_our_segment(void)
{
    static uint8_t jpg[2048];
    exif_info_t in = sample(); /* description first, so Orientation is not the first tag */
    for (int o = 1; o <= 8; o++) {
        in.orientation = (uint16_t)o;
        jpg[0] = 0xff;
        jpg[1] = 0xd8;
        const size_t n = exif_build(&in, jpg + 2, sizeof(jpg) - 2);
        TEST_ASSERT_EQUAL(o, exif_read_orientation(jpg, n + 2));
    }
}

static void test_read_orientation_other_files(void)
{
    /* Little-endian TIFF with Orientation 6 as the second entry. */
    static const uint8_t le[] = {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x2e, 'E', 'x', 'i', 'f', 0, 0,
                                 'I', 'I', 0x2a, 0, 8, 0, 0, 0,  /* TIFF, IFD0 at 8 */
                                 2, 0,                          /* 2 entries */
                                 0x0f, 0x01, 2, 0, 1, 0, 0, 0, 'X', 0, 0, 0,
                                 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0,
                                 0, 0, 0, 0};
    TEST_ASSERT_EQUAL(6, exif_read_orientation(le, sizeof(le)));
    static const uint8_t none[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0};
    TEST_ASSERT_EQUAL(1, exif_read_orientation(none, sizeof(none)));
    TEST_ASSERT_EQUAL(1, exif_read_orientation(le, 30)); /* truncated */
    TEST_ASSERT_EQUAL(1, exif_read_orientation(NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ifd0_tags);
    RUN_TEST(test_exif_ifd_tags);
    RUN_TEST(test_optional_tags_omitted);
    RUN_TEST(test_data_offsets_are_even_and_inside);
    RUN_TEST(test_too_small_returns_0);
    RUN_TEST(test_format_datetime);
    RUN_TEST(test_read_orientation_of_our_segment);
    RUN_TEST(test_read_orientation_other_files);
    return UNITY_END();
}
