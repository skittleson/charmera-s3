#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_harness_runs(void)
{
    TEST_ASSERT_EQUAL(1, 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_harness_runs);
    return UNITY_END();
}
