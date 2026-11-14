#include <string.h>

#include "remote_proto.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define LOOKS     12
#define INTERVALS 5

static remote_cmd_t parse_ok(const char *text)
{
    remote_cmd_t c = {.kind = RC_SHOOT, .arg = 99};
    const char *err = remote_parse(text, strlen(text), LOOKS, INTERVALS, &c);
    TEST_ASSERT_NULL_MESSAGE(err, text);
    return c;
}

static const char *parse_err(const char *text)
{
    remote_cmd_t c;
    return remote_parse(text, strlen(text), LOOKS, INTERVALS, &c);
}

static void test_parse_each_command(void)
{
    remote_cmd_t c = parse_ok("look 3");
    TEST_ASSERT_EQUAL(RC_LOOK, c.kind);
    TEST_ASSERT_EQUAL(3, c.arg);
    TEST_ASSERT_EQUAL(RC_MODE_SINGLE, parse_ok("mode single").kind);
    TEST_ASSERT_EQUAL(RC_MODE_VIDEO, parse_ok("mode video").kind);
    TEST_ASSERT_EQUAL_STRING("unknown command", parse_err("mode video 3"));
    c = parse_ok("mode lapse");
    TEST_ASSERT_EQUAL(RC_MODE_LAPSE, c.kind);
    TEST_ASSERT_EQUAL(-1, c.arg);
    c = parse_ok("mode lapse 2");
    TEST_ASSERT_EQUAL(RC_MODE_LAPSE, c.kind);
    TEST_ASSERT_EQUAL(2, c.arg);
    c = parse_ok("interval 4");
    TEST_ASSERT_EQUAL(RC_INTERVAL, c.kind);
    TEST_ASSERT_EQUAL(4, c.arg);
    TEST_ASSERT_EQUAL(RC_SHOOT, parse_ok("shoot").kind);
    TEST_ASSERT_EQUAL(RC_LAPSE_START, parse_ok("lapse start").kind);
    TEST_ASSERT_EQUAL(RC_LAPSE_STOP, parse_ok("lapse stop").kind);
}

static void test_parse_tolerates_whitespace_and_case(void)
{
    remote_cmd_t c = parse_ok("  LOOK 3\r\n");
    TEST_ASSERT_EQUAL(RC_LOOK, c.kind);
    TEST_ASSERT_EQUAL(3, c.arg);
    TEST_ASSERT_EQUAL(RC_SHOOT, parse_ok("Shoot\n").kind);
    TEST_ASSERT_EQUAL(RC_LAPSE_STOP, parse_ok("lapse   stop").kind);
    /* len is honoured: no NUL terminator needed */
    const char raw[] = {'s', 'h', 'o', 'o', 't', 'X', 'Y'};
    TEST_ASSERT_NULL(remote_parse(raw, 5, LOOKS, INTERVALS, &c));
    TEST_ASSERT_EQUAL(RC_SHOOT, c.kind);
}

static void test_parse_rejects_bad_numbers(void)
{
    TEST_ASSERT_EQUAL_STRING("bad look", parse_err("look 12"));
    TEST_ASSERT_EQUAL_STRING("bad look", parse_err("look -1"));
    TEST_ASSERT_EQUAL_STRING("bad look", parse_err("look x"));
    TEST_ASSERT_EQUAL_STRING("bad look", parse_err("look 3x"));
    TEST_ASSERT_EQUAL_STRING("bad look", parse_err("look"));
    TEST_ASSERT_EQUAL_STRING("bad interval", parse_err("interval 5"));
    TEST_ASSERT_EQUAL_STRING("bad interval", parse_err("mode lapse 9"));
    TEST_ASSERT_EQUAL_STRING("unknown command", parse_err("dance"));
    TEST_ASSERT_EQUAL_STRING("unknown command", parse_err(""));
    TEST_ASSERT_EQUAL_STRING("unknown command", parse_err("shoot now"));
}

static void test_parse_time(void)
{
    remote_cmd_t c = parse_ok("time 1793072525 -480");
    TEST_ASSERT_EQUAL(RC_TIME, c.kind);
    TEST_ASSERT_TRUE(c.epoch == 1793072525LL);
    TEST_ASSERT_EQUAL(-480, c.offset_min);
    c = parse_ok("time 1793072525");
    TEST_ASSERT_EQUAL(0, c.offset_min);
    TEST_ASSERT_EQUAL_STRING("bad time", parse_err("time"));
    TEST_ASSERT_EQUAL_STRING("bad time", parse_err("time 12345"));        /* before 2020 */
    TEST_ASSERT_EQUAL_STRING("bad time", parse_err("time abc"));
    TEST_ASSERT_EQUAL_STRING("bad time", parse_err("time 1793072525 900")); /* beyond UTC+14 */
    TEST_ASSERT_EQUAL_STRING("bad time", parse_err("time 1793072525 -721"));
}

static void test_parse_orientation(void)
{
    remote_cmd_t c = parse_ok("orientation landscape-left");
    TEST_ASSERT_EQUAL(RC_ORIENTATION, c.kind);
    TEST_ASSERT_EQUAL(2, c.arg);
    TEST_ASSERT_EQUAL(3, parse_ok("Orientation AUTO\n").arg);
    TEST_ASSERT_EQUAL(0, parse_ok("orientation portrait").arg);
    TEST_ASSERT_EQUAL(1, parse_ok("orientation landscape-right").arg);
    TEST_ASSERT_EQUAL_STRING("bad orientation", parse_err("orientation up"));
    TEST_ASSERT_EQUAL_STRING("bad orientation", parse_err("orientation"));
}

static void test_parse_file_service(void)
{
    remote_cmd_t c = parse_ok("list");
    TEST_ASSERT_EQUAL(RC_LIST, c.kind);
    c = parse_ok("thumb 7");
    TEST_ASSERT_EQUAL(RC_THUMB, c.kind);
    TEST_ASSERT_EQUAL(7, c.arg);
    c = parse_ok("get 1234");
    TEST_ASSERT_EQUAL(RC_GET, c.kind);
    TEST_ASSERT_EQUAL(1234, c.arg);
    TEST_ASSERT_EQUAL_STRING("bad photo number", parse_err("thumb 0"));
    TEST_ASSERT_EQUAL_STRING("bad photo number", parse_err("get 10000"));
    TEST_ASSERT_EQUAL_STRING("bad photo number", parse_err("thumb x"));
    TEST_ASSERT_EQUAL_STRING("unknown command", parse_err("list extra"));
    TEST_ASSERT_EQUAL_STRING("unknown command", parse_err("thumb"));
}

static void test_check_refuses_changes_while_running(void)
{
    const remote_cmd_kind_t refused[] = {RC_LOOK,       RC_MODE_SINGLE, RC_MODE_LAPSE, RC_MODE_VIDEO,
                                         RC_INTERVAL,   RC_SHOOT,       RC_LAPSE_START, RC_ORIENTATION};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        remote_cmd_t c = {.kind = refused[i], .arg = 1};
        TEST_ASSERT_EQUAL_STRING("time-lapse running", remote_check(&c, RUN_LAPSE));
        TEST_ASSERT_EQUAL_STRING("recording", remote_check(&c, RUN_VIDEO));
        TEST_ASSERT_NULL(remote_check(&c, RUN_NONE));
    }
    remote_cmd_t t = {.kind = RC_TIME};
    TEST_ASSERT_NULL(remote_check(&t, RUN_LAPSE)); /* setting the clock never disturbs a run */
    TEST_ASSERT_NULL(remote_check(&t, RUN_VIDEO));
    remote_cmd_t stop = {.kind = RC_LAPSE_STOP, .arg = -1};
    TEST_ASSERT_NULL(remote_check(&stop, RUN_LAPSE));
    TEST_ASSERT_NULL(remote_check(&stop, RUN_VIDEO)); /* "lapse stop" also stops a clip */
    TEST_ASSERT_EQUAL_STRING("not running", remote_check(&stop, RUN_NONE));
}

static remote_status_t sample(void)
{
    remote_status_t s = {
        .photos = 91, .left = 120, .battery_pct = 82, .usb = false, .camera = "OV5640",
        .look = 3, .look_name = "Black & White", .lapse_mode = true, .interval = 1, .interval_name = "10 s",
        .lapse_running = true, .frames = 12, .elapsed_s = 75, .card = REMOTE_CARD_OK, .busy = false,
        .clock_set = true, .orientation = "portrait", .orientation_setting = "auto",
    };
    strcpy(s.folder, "101LAPSE");
    strcpy(s.last, "IMG_0093");
    return s;
}

static void test_status_json_fields(void)
{
    remote_status_t s = sample();
    char out[512];
    const size_t n = remote_status_json(&s, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0 && n < 400);
    TEST_ASSERT_EQUAL(n, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('{', out[0]);
    TEST_ASSERT_EQUAL_CHAR('}', out[n - 1]);
    const char *want[] = {"\"photos\":91", "\"left\":120", "\"battery\":82", "\"usb\":false",
                          "\"camera\":\"OV5640\"", "\"look\":3", "\"lookName\":\"Black & White\"",
                          "\"mode\":\"lapse\"", "\"interval\":1", "\"intervalName\":\"10 s\"", "\"running\":true",
                          "\"recording\":false", "\"frames\":12", "\"elapsed\":75", "\"folder\":\"101LAPSE\"",
                          "\"card\":\"ok\"", "\"busy\":false", "\"last\":\"IMG_0093\"", "\"error\":\"\"", "\"clock\":true",
                          "\"orientation\":\"portrait\"", "\"orientationSetting\":\"auto\""};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, want[i]), want[i]);
    }
    s.lapse_mode = false;
    s.lapse_running = false;
    s.card = REMOTE_CARD_FULL;
    remote_status_json(&s, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"mode\":\"single\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"card\":\"full\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"running\":false"));
    /* Video mode: mode=video, recording=true, running=true. */
    s.lapse_mode = false;
    s.video_mode = true;
    s.video_recording = true;
    remote_status_json(&s, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"mode\":\"video\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"recording\":true"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"running\":true"));
}

static void test_status_json_escapes_quotes(void)
{
    remote_status_t s = sample();
    s.look_name = "a\"b\\c";
    char out[512];
    TEST_ASSERT_TRUE(remote_status_json(&s, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"lookName\":\"a\\\"b\\\\c\""));
}

static void test_status_json_too_small_buffer_returns_0(void)
{
    remote_status_t s = sample();
    char out[40];
    TEST_ASSERT_EQUAL(0, remote_status_json(&s, out, sizeof(out)));
}

static void test_info_json(void)
{
    const char *looks[] = {"Normal", "Cool Tone"};
    const char *iv[] = {"5 s", "10 s"};
    char out[256];
    TEST_ASSERT_TRUE(remote_info_json("0.1.0", looks, 2, iv, 2, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"version\":\"0.1.0\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"looks\":[\"Normal\",\"Cool Tone\"]"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"intervals\":[\"5 s\",\"10 s\"]"));
    TEST_ASSERT_EQUAL(0, remote_info_json("0.1.0", looks, 2, iv, 2, out, 20));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_each_command);
    RUN_TEST(test_parse_tolerates_whitespace_and_case);
    RUN_TEST(test_parse_rejects_bad_numbers);
    RUN_TEST(test_parse_time);
    RUN_TEST(test_parse_orientation);
    RUN_TEST(test_parse_file_service);
    RUN_TEST(test_check_refuses_changes_while_running);
    RUN_TEST(test_status_json_fields);
    RUN_TEST(test_status_json_escapes_quotes);
    RUN_TEST(test_status_json_too_small_buffer_returns_0);
    RUN_TEST(test_info_json);
    return UNITY_END();
}
