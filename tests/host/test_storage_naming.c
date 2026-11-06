#include <stddef.h>

#include "storage_naming.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse(void)
{
    TEST_ASSERT_EQUAL(42, storage_parse_photo_number("IMG_0042.JPG"));
    TEST_ASSERT_EQUAL(7, storage_parse_photo_number("img_0007.jpg"));
    TEST_ASSERT_EQUAL(-1, storage_parse_photo_number("IMG_0042.TMP"));
    TEST_ASSERT_EQUAL(-1, storage_parse_photo_number("DSC_0001.JPG"));
    TEST_ASSERT_EQUAL(-1, storage_parse_photo_number("IMG_42.JPG"));
    TEST_ASSERT_EQUAL(-1, storage_parse_photo_number("IMG_0000.JPG"));
    TEST_ASSERT_EQUAL(-1, storage_parse_photo_number("IMG_00a2.JPG"));
    TEST_ASSERT_EQUAL(-1, storage_parse_photo_number("IMG_0042.JPGX"));
    TEST_ASSERT_EQUAL(-1, storage_parse_photo_number(NULL));
}

static void test_next_number_empty(void)
{
    TEST_ASSERT_EQUAL(1, storage_next_photo_number(NULL, 0));
}

static void test_tmp_and_foreign_files_ignored(void)
{
    const char *n[] = {"IMG_0003.JPG", "IMG_0010.TMP", "notes.txt", "IMG_0001.JPG"};
    TEST_ASSERT_EQUAL(4, storage_next_photo_number(n, 4));
}

static void test_next_number_gap_uses_max(void)
{
    const char *n[] = {"IMG_0001.JPG", "IMG_0050.JPG"};
    TEST_ASSERT_EQUAL(51, storage_next_photo_number(n, 2));
}

static void test_next_number_exhausted(void)
{
    const char *n[] = {"IMG_9999.JPG"};
    TEST_ASSERT_EQUAL(-1, storage_next_photo_number(n, 1));
}

static void test_name(void)
{
    char b[13];
    storage_photo_name(42, b);
    TEST_ASSERT_EQUAL_STRING("IMG_0042.JPG", b);
    storage_photo_name(9999, b);
    TEST_ASSERT_EQUAL_STRING("IMG_9999.JPG", b);
}

static void test_remaining(void)
{
    TEST_ASSERT_EQUAL(10, storage_photos_remaining(20u * 1024 * 1024, 2u * 1024 * 1024));
    TEST_ASSERT_EQUAL(5, storage_photos_remaining(10u * 1024 * 1024, 0));
}

static void test_remaining_zero_when_free_below_estimate(void)
{
    TEST_ASSERT_EQUAL(0, storage_photos_remaining(1024 * 1024, 2u * 1024 * 1024));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse);
    RUN_TEST(test_next_number_empty);
    RUN_TEST(test_tmp_and_foreign_files_ignored);
    RUN_TEST(test_next_number_gap_uses_max);
    RUN_TEST(test_next_number_exhausted);
    RUN_TEST(test_name);
    RUN_TEST(test_remaining);
    RUN_TEST(test_remaining_zero_when_free_below_estimate);
    return UNITY_END();
}
