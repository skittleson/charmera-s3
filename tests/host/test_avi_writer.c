#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "avi_writer.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *PATH = "/tmp/charmera_avi_test.avi";

/* Frames of known bytes: distinct content and an odd length to exercise padding. */
static uint8_t frame0[100], frame1[201], frame2[50];

static void make_frames(void)
{
    for (size_t i = 0; i < sizeof(frame0); i++) {
        frame0[i] = (uint8_t)(i * 7 + 1);
    }
    for (size_t i = 0; i < sizeof(frame1); i++) {
        frame1[i] = (uint8_t)(i * 3 + 5);
    }
    for (size_t i = 0; i < sizeof(frame2); i++) {
        frame2[i] = (uint8_t)(i * 11 + 2);
    }
}

typedef struct {
    int count;
    size_t sizes[8];
    uint8_t data[8][512]; /* the parse buffer is reused per frame: copy it */
} captured_t;

static bool capture(void *ctx, const uint8_t *jpeg, size_t len)
{
    captured_t *c = (captured_t *)ctx;
    if (c->count < 8 && len <= sizeof(c->data[0])) {
        c->sizes[c->count] = len;
        memcpy(c->data[c->count], jpeg, len);
    }
    c->count++;
    return true;
}

static void test_round_trip_three_frames(void)
{
    make_frames();
    FILE *f = fopen(PATH, "wb");
    TEST_ASSERT_NOT_NULL(f);
    avi_writer_t w;
    TEST_ASSERT_TRUE(avi_writer_open(&w, f, 320, 240));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame0, sizeof(frame0)));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame1, sizeof(frame1)));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame2, sizeof(frame2)));
    TEST_ASSERT_TRUE(avi_writer_close(&w));

    uint8_t buf[512];
    captured_t c = {0};
    bool truncated = true;
    const int n = avi_writer_parse(PATH, buf, sizeof(buf), 0, capture, &c, &truncated);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_FALSE(truncated);
    TEST_ASSERT_EQUAL(3, c.count);
    TEST_ASSERT_EQUAL(sizeof(frame0), c.sizes[0]);
    TEST_ASSERT_EQUAL(sizeof(frame1), c.sizes[1]);
    TEST_ASSERT_EQUAL(sizeof(frame2), c.sizes[2]);
    /* The frames must come back byte-for-byte. */
    TEST_ASSERT_EQUAL_MEMORY(frame0, c.data[0], sizeof(frame0));
    TEST_ASSERT_EQUAL_MEMORY(frame1, c.data[1], sizeof(frame1));
    TEST_ASSERT_EQUAL_MEMORY(frame2, c.data[2], sizeof(frame2));
}

static void test_header_sizes_patched(void)
{
    make_frames();
    FILE *f = fopen(PATH, "wb");
    avi_writer_t w;
    TEST_ASSERT_TRUE(avi_writer_open(&w, f, 320, 240));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame0, sizeof(frame0)));
    TEST_ASSERT_TRUE(avi_writer_close(&w));

    /* RIFF size = file size - 8; movi size = frame data (100 + 0 padding). */
    FILE *r = fopen(PATH, "rb");
    TEST_ASSERT_NOT_NULL(r);
    fseek(r, 0, SEEK_END);
    const long size = ftell(r);
    uint8_t v[4];
    fseek(r, 4, SEEK_SET);
    TEST_ASSERT_EQUAL(4, fread(v, 1, 4, r));
    TEST_ASSERT_EQUAL((uint32_t)(size - 8), (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24));
    /* strh scale/rate: ms-per-frame = scale/rate must be 125000/1000000 (8 fps),
       matching avih dwMicroSecPerFrame. */
    uint32_t scale, rate;
    fseek(r, 116, SEEK_SET);
    TEST_ASSERT_EQUAL(4, fread(v, 1, 4, r));
    scale = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    fseek(r, 120, SEEK_SET);
    TEST_ASSERT_EQUAL(4, fread(v, 1, 4, r));
    rate = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    TEST_ASSERT_EQUAL(125000, scale);
    TEST_ASSERT_EQUAL(1000000, rate);
    fclose(r);
}

static void test_truncated_file_stops_cleanly(void)
{
    make_frames();
    FILE *f = fopen(PATH, "wb");
    avi_writer_t w;
    TEST_ASSERT_TRUE(avi_writer_open(&w, f, 320, 240));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame0, sizeof(frame0)));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame1, sizeof(frame1)));
    /* Cut the file mid-second-frame: close without patching, then truncate. */
    fflush(f);
    const long cut = ftell(f) - 20; /* chop into frame1's data */
    fclose(f);
    /* Reopen and truncate to simulate a power loss mid-clip. */
    FILE *t = fopen(PATH, "r+b");
    TEST_ASSERT_NOT_NULL(t);
    fseek(t, cut, SEEK_SET);
    TEST_ASSERT_EQUAL(0, ftruncate(fileno(t), cut));
    fclose(t);

    uint8_t buf[512];
    captured_t c = {0};
    bool truncated = false;
    const int n = avi_writer_parse(PATH, buf, sizeof(buf), 0, capture, &c, &truncated);
    TEST_ASSERT_EQUAL(1, n); /* only the first frame survived */
    TEST_ASSERT_TRUE(truncated);
}

static void test_start_frame_skips(void)
{
    make_frames();
    FILE *f = fopen(PATH, "wb");
    TEST_ASSERT_NOT_NULL(f);
    avi_writer_t w;
    TEST_ASSERT_TRUE(avi_writer_open(&w, f, 320, 240));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame0, sizeof(frame0)));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame1, sizeof(frame1)));
    TEST_ASSERT_TRUE(avi_writer_frame(&w, frame2, sizeof(frame2)));
    TEST_ASSERT_TRUE(avi_writer_close(&w));

    uint8_t buf[512];
    captured_t c = {0};
    bool truncated = true;
    /* Skip the first two frames: only the third is delivered. */
    const int n = avi_writer_parse(PATH, buf, sizeof(buf), 2, capture, &c, &truncated);
    TEST_ASSERT_EQUAL(3, n); /* total complete frames in the file */
    TEST_ASSERT_FALSE(truncated);
    TEST_ASSERT_EQUAL(1, c.count);
    TEST_ASSERT_EQUAL(sizeof(frame2), c.sizes[0]);
    TEST_ASSERT_EQUAL_MEMORY(frame2, c.data[0], sizeof(frame2));
}

static void test_not_an_avi(void)
{
    FILE *f = fopen(PATH, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("hello", 1, 5, f);
    fclose(f);
    uint8_t buf[64];
    bool truncated = false;
    TEST_ASSERT_EQUAL(-1, avi_writer_parse(PATH, buf, sizeof(buf), 0, NULL, NULL, &truncated));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_three_frames);
    RUN_TEST(test_header_sizes_patched);
    RUN_TEST(test_truncated_file_stops_cleanly);
    RUN_TEST(test_start_frame_skips);
    RUN_TEST(test_not_an_avi);
    remove(PATH);
    return UNITY_END();
}
