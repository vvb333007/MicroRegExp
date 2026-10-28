/* MicroRegExp: PHP preg_* API tests
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra preg_test.c -o preg_test
 *
 * This test file includes regexp.c directly, matching the original test setup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regexp.h"
#include "regexp.c"

static int tests_run;
static int tests_failed;

#define CHECK(condition) do {                                           \
    tests_run++;                                                         \
    if (!(condition)) {                                                  \
        tests_failed++;                                                  \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
    }                                                                    \
} while (0)

#define CHECK_STRING(actual, expected) do {                              \
    const char *actual_ = (actual);                                      \
    const char *expected_ = (expected);                                   \
    tests_run++;                                                         \
    if (actual_ == NULL || strcmp(actual_, expected_) != 0) {            \
        tests_failed++;                                                  \
        printf("FAIL %s:%d: got \"%s\", expected \"%s\"\n",             \
               __FILE__, __LINE__,                                       \
               actual_ ? actual_ : "(null)", expected_);                 \
    }                                                                    \
} while (0)

#define CHECK_SPAN(subject, span, expected)                              \
    CHECK(span_equals((subject), (span), (expected)))

static int span_equals(const char *subject, pl_span span,
                       const char *expected)
{
    int length = PREG_SPAN_LEN(span);
    size_t expected_length = strlen(expected);

    return length >= 0
        && (size_t)length == expected_length
        && (length == 0
            || memcmp(subject + span.start, expected, (size_t)length) == 0);
}

static void test_compile(void)
{
    preg_re *re;
    int error;

    re = preg_compile("abc", &error);
    CHECK(re == NULL && error == PREG_E_DELIMITER);

    re = preg_compile("/abc", &error);
    CHECK(re == NULL && error == PREG_E_DELIMITER);

    re = preg_compile("/abc/u", &error);
    CHECK(re == NULL && error == PREG_E_MODIFIER);

    re = preg_compile("/a(?=b)/", &error);
    CHECK(re == NULL && error == PL_E_UNSUPPORTED);

    re = preg_compile("/(/", &error);
    CHECK(re == NULL && error == PL_E_SYNTAX);

    re = preg_compile("  /ab/iDS", &error);
    CHECK(re != NULL && error == PL_OK);
    preg_re_free(re);

    CHECK(preg_strerror(PREG_E_DELIMITER) != NULL);
    CHECK(preg_match(NULL, "x", -1, NULL, 0, 0) == PREG_E_ARGS);
    CHECK(preg_match("/x/", NULL, -1, NULL, 0, 0) == PREG_E_ARGS);
}

static void test_match(void)
{
    const char *subject = "tel 123-4567 x";
    preg_match_t match;
    preg_re *re;
    int error;
    int rc;

    rc = preg_match("/(\\d+)-(\\d+)/", subject, -1, &match, 0, 0);
    CHECK(rc == 1);
    CHECK(match.count == 3);
    CHECK_SPAN(subject, match.g[0], "123-4567");
    CHECK_SPAN(subject, match.g[1], "123");
    CHECK_SPAN(subject, match.g[2], "4567");
    CHECK(match.g[0].start == 4);

    CHECK(preg_match("/xyz/", "abc", -1, &match, 0, 0) == 0);
    CHECK(match.count == 0);

    rc = preg_match("/hello/i", "Say HELLO", -1, &match, 0, 0);
    CHECK(rc == 1 && match.g[0].start == 4 && match.g[0].end == 9);

    CHECK(preg_match("/hello/", "Say HELLO", -1, &match, 0, 0) == 0);

    rc = preg_match("#a/b#", "xa/b", -1, &match, 0, 0);
    CHECK(rc == 1 && match.g[0].start == 1);

    rc = preg_match("/a\\/b/", "a/b", -1, &match, 0, 0);
    CHECK(rc == 1 && match.g[0].start == 0 && match.g[0].end == 3);

    rc = preg_match("{b+}", "abbc", -1, &match, 0, 0);
    CHECK(rc == 1);
    CHECK_SPAN("abbc", match.g[0], "bb");

    CHECK(preg_match("/^a/", "ba", -1, &match, 0, 0) == 0);

    rc = preg_match("/a$/", "ba", -1, &match, 0, 0);
    CHECK(rc == 1 && match.g[0].start == 1);

    /* Only the first two bytes of the subject are searched. */
    CHECK(preg_match("/c/", "abcdef", 2, &match, 0, 0) == 0);

    rc = preg_match("/\\d/", "1a2", -1, &match, 0, 1);
    CHECK(rc == 1 && match.g[0].start == 2);

    rc = preg_match("/\\d/", "1a2", -1, &match, 0, -1);
    CHECK(rc == 1 && match.g[0].start == 2);

    rc = preg_match("/\\d/", "1a2", -1, &match, 0, -100);
    CHECK(rc == 1 && match.g[0].start == 0);

    CHECK(preg_match("/\\d/", "1a2", -1, &match, 0, 4) == PREG_E_OFFSET);
    CHECK(preg_match("/^a/", "ba", -1, &match, 0, 1) == 0);

    CHECK(preg_match("/b/A", "ab", -1, &match, 0, 0) == 0);
    rc = preg_match("/b/A", "ab", -1, &match, 0, 1);
    CHECK(rc == 1 && match.g[0].start == 1);

    rc = preg_match("//", "abc", -1, &match, 0, 1);
    CHECK(rc == 1 && match.g[0].start == 1
          && match.g[0].end == 1 && match.count == 1);

    rc = preg_match("/(a)(x)?/", "a", -1, &match, 0, 0);
    CHECK(rc == 1 && match.count == 2);
    CHECK_SPAN("a", match.g[1], "a");

    rc = preg_match("/(a)(x)?/", "a", -1, &match,
                    PREG_UNMATCHED_AS_NULL, 0);
    CHECK(rc == 1 && match.count == 3 && match.g[2].start == -1);

    CHECK(preg_match("/b/", "abc", -1, NULL, 0, 0) == 1);

    re = preg_compile("/(\\d)/", &error);
    CHECK(re != NULL);
    if (re != NULL) {
        CHECK(preg_match_re(re, "a1", -1, &match, 0, 0) == 1);
        CHECK_SPAN("a1", match.g[1], "1");
        CHECK(preg_match_re(re, "ab", -1, &match, 0, 0) == 0);
        preg_re_free(re);
    }
}

static void test_match_all(void)
{
    preg_all_t all;
    int count;

    memset(&all, 0, sizeof all);
    count = preg_match_all("/\\d+/", "a1b22c333", -1,
                           &all, PREG_PATTERN_ORDER, 0);
    CHECK(count == 3 && all.nmatches == 3 && all.ngroups == 0);
    CHECK_SPAN("a1b22c333", PREG_ALL_SPAN(&all, 0, 0), "1");
    CHECK_SPAN("a1b22c333", PREG_ALL_SPAN(&all, 1, 0), "22");
    CHECK_SPAN("a1b22c333", PREG_ALL_SPAN(&all, 2, 0), "333");
    preg_all_free(&all);
    CHECK(all.spans == NULL && all.nmatches == 0);

    CHECK(preg_match_all("/\\d+/", "a1b22c333", -1, NULL, 0, 0) == 3);

    CHECK(preg_match_all("/x/", "abc", -1, &all, 0, 0) == 0);
    preg_all_free(&all);

    count = preg_match_all("/(\\w)(\\d)/", "a1 b2", -1,
                            &all, PREG_SET_ORDER, 0);
    CHECK(count == 2 && all.ngroups == 2);
    CHECK_SPAN("a1 b2", PREG_ALL_SPAN(&all, 0, 1), "a");
    CHECK_SPAN("a1 b2", PREG_ALL_SPAN(&all, 1, 0), "b2");
    CHECK_SPAN("a1 b2", PREG_ALL_SPAN(&all, 1, 2), "2");
    CHECK(all.counts[0] == 3 && all.counts[1] == 3);
    preg_all_free(&all);

    count = preg_match_all("/(x)(y)/", "abc", -1, &all, 0, 0);
    CHECK(count == 0 && all.ngroups == 2);
    preg_all_free(&all);

    CHECK(preg_match_all("/\\d/", "1 2 3", -1, NULL, 0, 2) == 2);
    CHECK(preg_match_all("//", "ab", -1, NULL, 0, 0) == 3);
    CHECK(preg_match_all("/\\d/", "123", -1, NULL, 0, 9)
          == PREG_E_OFFSET);
}

static void test_replace(void)
{
    char *result;
    int length;
    int count;

    result = preg_replace("/(\\w+)@(\\w+)/", "$2:${1}",
                          "joe@home", -1, -1, &length, &count);
    CHECK_STRING(result, "home:joe");
    CHECK(length == 8 && count == 1);
    free(result);

    result = preg_replace("/(\\w+)@(\\w+)/", "\\2-\\1",
                          "joe@home", -1, -1, &length, &count);
    CHECK_STRING(result, "home-joe");
    free(result);

    result = preg_replace("/b/", "[\\\\]", "abc", -1, -1, NULL, NULL);
    CHECK_STRING(result, "a[\\]c");
    free(result);

    result = preg_replace("/b/", "\\$1", "abc", -1, -1, NULL, NULL);
    CHECK_STRING(result, "a$1c");
    free(result);

    result = preg_replace("/(b)/", "$$1", "abc", -1, -1, NULL, NULL);
    CHECK_STRING(result, "a$bc");
    free(result);

    result = preg_replace("/b/", "[$0|$5]", "abc", -1, -1, NULL, NULL);
    CHECK_STRING(result, "a[b|]c");
    free(result);

    result = preg_replace("/b/", "$x", "abc", -1, -1, NULL, NULL);
    CHECK_STRING(result, "a$xc");
    free(result);

    result = preg_replace("/a/", "b", "aaaa", -1, -1, NULL, &count);
    CHECK_STRING(result, "bbbb");
    CHECK(count == 4);
    free(result);

    result = preg_replace("/a/", "b", "aaaa", -1, 2, NULL, &count);
    CHECK_STRING(result, "bbaa");
    CHECK(count == 2);
    free(result);

    result = preg_replace("/a/", "b", "aaaa", -1, 0, NULL, &count);
    CHECK_STRING(result, "aaaa");
    CHECK(count == 0);
    free(result);

    result = preg_replace("/x/", "y", "abc", -1, -1, &length, &count);
    CHECK_STRING(result, "abc");
    CHECK(length == 3 && count == 0);
    free(result);

    result = preg_replace("/B/i", "-", "abc", -1, -1, NULL, NULL);
    CHECK_STRING(result, "a-c");
    free(result);

    result = preg_replace("//", "-", "ab", -1, -1, NULL, &count);
    CHECK_STRING(result, "-a-b-");
    CHECK(count == 3);
    free(result);

    result = preg_replace("/a*/", "X", "baaac", -1, -1, NULL, &count);
    CHECK_STRING(result, "XbXXcX");
    CHECK(count == 4);
    free(result);

    result = preg_replace("/b/", "x", "abcb", 3, -1, NULL, &count);
    CHECK_STRING(result, "axc");
    free(result);

    result = preg_replace("nodelim", "x", "abc", -1, -1, &length, &count);
    CHECK(result == NULL && length == PREG_E_DELIMITER && count == 0);
}

static void check_split(const char *pattern, const char *subject,
                        int limit, int flags, const char *expected,
                        int expected_count)
{
    pl_span *spans = NULL;
    int count = 0;
    int rc;
    size_t used = 0;
    char joined[256];
    int i;

    joined[0] = '\0';
    rc = preg_split(pattern, subject, -1, limit, flags, &spans, &count);

    CHECK(rc == PL_OK);
    if (rc != PL_OK) {
        free(spans);
        return;
    }

    for (i = 0; i < count; i++) {
        int length = PREG_SPAN_LEN(spans[i]);

        if (i > 0 && used + 1 < sizeof joined)
            joined[used++] = '|';

        if (length > 0 && used + (size_t)length < sizeof joined) {
            memcpy(joined + used, subject + spans[i].start, (size_t)length);
            used += (size_t)length;
        }
        joined[used] = '\0';
    }

    CHECK_STRING(joined, expected);
    CHECK(count == expected_count);
    free(spans);
}

static void test_split(void)
{
    check_split("/[\\s:]/", "aa bb cc dddd", -1, 0,
                "aa|bb|cc|dddd", 4);
    check_split("/,/", "a,b,,c", -1, 0, "a|b||c", 4);
    check_split("/,/", "a,b,,c", -1, PREG_SPLIT_NO_EMPTY, "a|b|c", 3);
    check_split("/,/", "a,b,,c", 0, 0, "a|b||c", 4);
    check_split("/,/", "a,b,,c", 2, 0, "a|b,,c", 2);
    check_split("/,/", "a,b,,c", 1, 0, "a,b,,c", 1);
    check_split("/(-)/", "a-b-c", -1, PREG_SPLIT_DELIM_CAPTURE,
                "a|-|b|-|c", 5);
    check_split("/\\s+/", "a  b\tc", -1, 0, "a|b|c", 3);
    check_split("/x/", "abc", -1, 0, "abc", 1);
    check_split("/,/", "", -1, 0, "", 1);
    check_split("/,/", "", -1, PREG_SPLIT_NO_EMPTY, "", 0);
    check_split("/,/", ",a,", -1, 0, "|a|", 3);
    check_split("/,/", ",a,", -1, PREG_SPLIT_NO_EMPTY, "a", 1);
    check_split("//", "abc", -1, 0, "|a|b|c|", 5);
    check_split("//", "abc", -1, PREG_SPLIT_NO_EMPTY, "a|b|c", 3);
}

static void test_grep(void)
{
    const char *items[] = { "apple", "banana", "cherry", "avocado" };
    const int lengths[] = { 2, 6, 6, 7 };
    int *indices = NULL;
    int count;

    count = preg_grep("/^a/", items, NULL, 4, 0, &indices);
    CHECK(count == 2 && indices != NULL
          && indices[0] == 0 && indices[1] == 3);
    free(indices);
    indices = NULL;

    count = preg_grep("/^a/", items, NULL, 4, PREG_GREP_INVERT, &indices);
    CHECK(count == 2 && indices != NULL
          && indices[0] == 1 && indices[1] == 2);
    free(indices);
    indices = NULL;

    CHECK(preg_grep("/an/", items, NULL, 4, 0, NULL) == 1);

    count = preg_grep("/zzz/", items, NULL, 4, 0, &indices);
    CHECK(count == 0);
    free(indices);
    indices = NULL;

    CHECK(preg_grep("/a/", items, NULL, 0, 0, &indices) == 0);
    free(indices);
    indices = NULL;

    CHECK(preg_grep("bad", items, NULL, 4, 0, &indices)
          == PREG_E_DELIMITER);
    CHECK(indices == NULL);

    CHECK(preg_grep("/^ap+le/", items, lengths, 4, 0, NULL) == 0);
}

int main(void)
{
    test_compile();
    test_match();
    test_match_all();
    test_replace();
    test_split();
    test_grep();

    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed > 255 ? 255 : tests_failed;
}
