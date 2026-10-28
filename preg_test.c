/* LLM WARNING: Claude-generated test
 *
 * Build:  gcc -std=c99 -Wall -Wextra  preg_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "regexp.h"
#include "regexp.c"


static int g_total, g_fail;

#define CHECK(cond) \
  do { \
    g_total++; \
    if (!(cond)) { \
      g_fail++; \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while (0)

#define CHECK_STR(got, want) \
  do { \
    const char *g_ = (got); \
    g_total++; \
    if (g_ == NULL || strcmp(g_, (want)) != 0) { \
      g_fail++; \
      printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
             g_ ? g_ : "(null)", (want)); \
    } \
  } while (0)

/* span == want ? */
static int span_is(const char *s, pl_span sp, const char *want) {
  int l = PREG_SPAN_LEN(sp);
  return (size_t) l == strlen(want) && (l == 0 || memcmp(s + sp.start, want, (size_t) l) == 0);
}

/* "a|b|c" from an array of spans */
static const char *join(char *buf, size_t cap, const char *s, const pl_span *sp, int n) {
  size_t k = 0;
  int i, l;

  buf[0] = '\0';

  for (i = 0; i < n; i++) {
    l = PREG_SPAN_LEN(sp[i]);
    if (i > 0 && k + 1 < cap)
      buf[k++] = '|';
    if (l > 0 && k + (size_t) l + 1 < cap) {
      memcpy(buf + k, s + sp[i].start, (size_t) l);
      k += (size_t) l;
    }
    buf[k] = '\0';
  }
  return buf;
}

/* Run preg_split and return joined pieces (or "ERR") */
static const char *split_str(char *buf, size_t cap, const char *pat, const char *s,
                             int limit, int flags, int *n_out) {
  pl_span *sp = NULL;
  int n = 0, rc = preg_split(pat, s, -1, limit, flags, &sp, &n);

  if (rc != PL_OK) {
    strcpy(buf, "ERR");
    return buf;
  }
  if (n_out != NULL)
    *n_out = n;
  join(buf, cap, s, sp, n);
  free(sp);
  return buf;
}

int main(void) {

  char buf[256];
  preg_match_t m;
  preg_all_t all;
  int err, n, cnt, rc;
  char *r;
  preg_re *re;

  /* ---------------------------------------------------------------- *
   * preg_compile: delimiters, modifiers, errors
   * ---------------------------------------------------------------- */
  re = preg_compile("abc", &err);
  CHECK(re == NULL && err == PREG_E_DELIMITER);       /* no delimiter */

  re = preg_compile("/abc", &err);
  CHECK(re == NULL && err == PREG_E_DELIMITER);       /* no closing delimiter */

  re = preg_compile("/abc/u", &err);
  CHECK(re == NULL && err == PREG_E_MODIFIER);        /* unsupported modifier */

  re = preg_compile("/a(?=b)/", &err);
  CHECK(re == NULL && err == PL_E_UNSUPPORTED);       /* lookahead: from pl_compile */

  re = preg_compile("/(/", &err);
  CHECK(re == NULL && err == PL_E_SYNTAX);

  re = preg_compile("  /ab/iDS", &err);               /* leading space, ok modifiers */
  CHECK(re != NULL && err == PL_OK);
  preg_re_free(re);

  CHECK(preg_strerror(PREG_E_DELIMITER) != NULL);
  CHECK(preg_match(NULL, "x", -1, &m, 0, 0) == PREG_E_ARGS);
  CHECK(preg_match("/x/", NULL, -1, &m, 0, 0) == PREG_E_ARGS);

  /* ---------------------------------------------------------------- *
   * preg_match
   * ---------------------------------------------------------------- */
  rc = preg_match("/(\\d+)-(\\d+)/", "tel 123-4567 x", -1, &m, 0, 0);
  CHECK(rc == 1);
  CHECK(m.count == 3);
  CHECK(span_is("tel 123-4567 x", m.g[0], "123-4567"));
  CHECK(span_is("tel 123-4567 x", m.g[1], "123"));
  CHECK(span_is("tel 123-4567 x", m.g[2], "4567"));
  CHECK(m.g[0].start == 4);                           /* offsets are byte offsets */

  CHECK(preg_match("/xyz/", "abc", -1, &m, 0, 0) == 0);
  CHECK(m.count == 0);

  rc = preg_match("/hello/i", "Say HELLO", -1, &m, 0, 0);
  CHECK(rc == 1 && m.g[0].start == 4 && m.g[0].end == 9);

  rc = preg_match("/hello/", "Say HELLO", -1, &m, 0, 0);   /* case-sensitive */
  CHECK(rc == 0);

  rc = preg_match("#a/b#", "xa/b", -1, &m, 0, 0);          /* other delimiter */
  CHECK(rc == 1 && m.g[0].start == 1);

  rc = preg_match("/a\\/b/", "a/b", -1, &m, 0, 0);         /* escaped delimiter */
  CHECK(rc == 1 && m.g[0].start == 0 && m.g[0].end == 3);

  rc = preg_match("{b+}", "abbc", -1, &m, 0, 0);           /* bracket delimiters */
  CHECK(rc == 1 && span_is("abbc", m.g[0], "bb"));

  rc = preg_match("/^a/", "ba", -1, &m, 0, 0);
  CHECK(rc == 0);

  rc = preg_match("/a$/", "ba", -1, &m, 0, 0);
  CHECK(rc == 1 && m.g[0].start == 1);

  /* explicit length: the subject is not NUL-terminated at len */
  rc = preg_match("/c/", "abcdef", 2, &m, 0, 0);
  CHECK(rc == 0);

  /* offsets */
  rc = preg_match("/\\d/", "1a2", -1, &m, 0, 1);
  CHECK(rc == 1 && m.g[0].start == 2);
  rc = preg_match("/\\d/", "1a2", -1, &m, 0, -1);         /* from the end */
  CHECK(rc == 1 && m.g[0].start == 2);
  rc = preg_match("/\\d/", "1a2", -1, &m, 0, -100);       /* clamped to 0 */
  CHECK(rc == 1 && m.g[0].start == 0);
  rc = preg_match("/\\d/", "1a2", -1, &m, 0, 4);          /* beyond the end */
  CHECK(rc == PREG_E_OFFSET);
  rc = preg_match("/^a/", "ba", -1, &m, 0, 1);            /* '^' is not the offset */
  CHECK(rc == 0);

  /* modifier A: match must start exactly at the offset */
  rc = preg_match("/b/A", "ab", -1, &m, 0, 0);
  CHECK(rc == 0);
  rc = preg_match("/b/A", "ab", -1, &m, 0, 1);
  CHECK(rc == 1 && m.g[0].start == 1);

  /* empty pattern */
  rc = preg_match("//", "abc", -1, &m, 0, 1);
  CHECK(rc == 1 && m.g[0].start == 1 && m.g[0].end == 1 && m.count == 1);

  /* trailing unmatched groups (engine: optional group at the end) */
  rc = preg_match("/(a)(x)?/", "a", -1, &m, 0, 0);
  CHECK(rc == 1 && m.count == 2);                          /* PHP drops the trailing (x) */
  CHECK(span_is("a", m.g[1], "a"));
  rc = preg_match("/(a)(x)?/", "a", -1, &m, PREG_UNMATCHED_AS_NULL, 0);
  CHECK(rc == 1 && m.count == 3 && m.g[2].start == -1);

  /* m == NULL is allowed */
  CHECK(preg_match("/b/", "abc", -1, NULL, 0, 0) == 1);

  /* precompiled */
  re = preg_compile("/(\\d)/", &err);
  CHECK(re != NULL);
  CHECK(preg_match_re(re, "a1", -1, &m, 0, 0) == 1 && span_is("a1", m.g[1], "1"));
  CHECK(preg_match_re(re, "ab", -1, &m, 0, 0) == 0);
  preg_re_free(re);

  /* ---------------------------------------------------------------- *
   * preg_match_all
   * ---------------------------------------------------------------- */
  memset(&all, 0, sizeof all);
  n = preg_match_all("/\\d+/", "a1b22c333", -1, &all, PREG_PATTERN_ORDER, 0);
  CHECK(n == 3 && all.nmatches == 3 && all.ngroups == 0);
  CHECK(span_is("a1b22c333", PREG_ALL_SPAN(&all, 0, 0), "1"));
  CHECK(span_is("a1b22c333", PREG_ALL_SPAN(&all, 1, 0), "22"));
  CHECK(span_is("a1b22c333", PREG_ALL_SPAN(&all, 2, 0), "333"));
  preg_all_free(&all);
  CHECK(all.spans == NULL && all.nmatches == 0);

  CHECK(preg_match_all("/\\d+/", "a1b22c333", -1, NULL, 0, 0) == 3);   /* count only */
  CHECK(preg_match_all("/x/", "abc", -1, &all, 0, 0) == 0);
  preg_all_free(&all);

  n = preg_match_all("/(\\w)(\\d)/", "a1 b2", -1, &all, PREG_SET_ORDER, 0);
  CHECK(n == 2 && all.ngroups == 2);
  CHECK(span_is("a1 b2", PREG_ALL_SPAN(&all, 0, 1), "a"));
  CHECK(span_is("a1 b2", PREG_ALL_SPAN(&all, 1, 0), "b2"));
  CHECK(span_is("a1 b2", PREG_ALL_SPAN(&all, 1, 2), "2"));
  CHECK(all.counts[0] == 3 && all.counts[1] == 3);
  preg_all_free(&all);

  /* zero matches still reports the group count (needs pl_ngroups) */
  n = preg_match_all("/(x)(y)/", "abc", -1, &all, 0, 0);
  CHECK(n == 0 && all.ngroups == 2);
  preg_all_free(&all);

  CHECK(preg_match_all("/\\d/", "1 2 3", -1, NULL, 0, 2) == 2);        /* offset */
  CHECK(preg_match_all("//", "ab", -1, NULL, 0, 0) == 3);              /* empty matches */
  CHECK(preg_match_all("/\\d/", "123", -1, NULL, 0, 9) == PREG_E_OFFSET);

  /* ---------------------------------------------------------------- *
   * preg_replace
   * ---------------------------------------------------------------- */
  r = preg_replace("/(\\w+)@(\\w+)/", "$2:${1}", "joe@home", -1, -1, &n, &cnt);
  CHECK_STR(r, "home:joe");
  CHECK(n == 8 && cnt == 1);
  free(r);

  r = preg_replace("/(\\w+)@(\\w+)/", "\\2-\\1", "joe@home", -1, -1, &n, &cnt);
  CHECK_STR(r, "home-joe");                              /* \N syntax */
  free(r);

  r = preg_replace("/b/", "[\\\\]", "abc", -1, -1, NULL, NULL);
  CHECK_STR(r, "a[\\]c");                                /* "\\" -> "\" */
  free(r);

  r = preg_replace("/b/", "\\$1", "abc", -1, -1, NULL, NULL);
  CHECK_STR(r, "a$1c");                                  /* "\$" -> literal "$" */
  free(r);

  r = preg_replace("/(b)/", "$$1", "abc", -1, -1, NULL, NULL);
  CHECK_STR(r, "a$bc");                                  /* "$$" is NOT special in PHP */
  free(r);

  r = preg_replace("/b/", "[$0|$5]", "abc", -1, -1, NULL, NULL);
  CHECK_STR(r, "a[b|]c");                                /* $0 = whole, missing group = "" */
  free(r);

  r = preg_replace("/b/", "$x", "abc", -1, -1, NULL, NULL);
  CHECK_STR(r, "a$xc");                                  /* not a backref -> literal */
  free(r);

  r = preg_replace("/a/", "b", "aaaa", -1, -1, NULL, &cnt);
  CHECK_STR(r, "bbbb");
  CHECK(cnt == 4);
  free(r);

  r = preg_replace("/a/", "b", "aaaa", -1, 2, NULL, &cnt);   /* limit */
  CHECK_STR(r, "bbaa");
  CHECK(cnt == 2);
  free(r);

  r = preg_replace("/a/", "b", "aaaa", -1, 0, NULL, &cnt);   /* limit 0: nothing */
  CHECK_STR(r, "aaaa");
  CHECK(cnt == 0);
  free(r);

  r = preg_replace("/x/", "y", "abc", -1, -1, &n, &cnt);     /* no match: plain copy */
  CHECK_STR(r, "abc");
  CHECK(n == 3 && cnt == 0);
  free(r);

  r = preg_replace("/B/i", "-", "abc", -1, -1, NULL, NULL);
  CHECK_STR(r, "a-c");
  free(r);

  r = preg_replace("//", "-", "ab", -1, -1, NULL, &cnt);     /* empty pattern */
  CHECK_STR(r, "-a-b-");
  CHECK(cnt == 3);
  free(r);

  r = preg_replace("/a*/", "X", "baaac", -1, -1, NULL, &cnt);  /* engine: empty matches */
  CHECK_STR(r, "XbXXcX");
  CHECK(cnt == 4);
  free(r);

  r = preg_replace("/b/", "x", "abcb", 3, -1, NULL, &cnt);     /* explicit len: "abc" only */
  CHECK_STR(r, "axc");
  free(r);

  r = preg_replace("nodelim", "x", "abc", -1, -1, &n, &cnt);
  CHECK(r == NULL && n == PREG_E_DELIMITER && cnt == 0);

  /* ---------------------------------------------------------------- *
   * preg_split
   * ---------------------------------------------------------------- */
  CHECK_STR(split_str(buf, sizeof buf, "/[\\s:]/", "aa bb cc dddd", -1, 0, &n), "aa|bb|cc|dddd");
  CHECK(n == 4);

  CHECK_STR(split_str(buf, sizeof buf, "/,/", "a,b,,c", -1, 0, &n), "a|b||c");
  CHECK(n == 4);

  CHECK_STR(split_str(buf, sizeof buf, "/,/", "a,b,,c", -1, PREG_SPLIT_NO_EMPTY, &n), "a|b|c");
  CHECK(n == 3);
  CHECK_STR(split_str(buf, sizeof buf, "/,/", "a,b,,c", 0, 0, &n), "a|b||c");   /* 0 = no limit */
  CHECK_STR(split_str(buf, sizeof buf, "/,/", "a,b,,c", 2, 0, &n), "a|b,,c");
  CHECK(n == 2);
  CHECK_STR(split_str(buf, sizeof buf, "/,/", "a,b,,c", 1, 0, &n), "a,b,,c");
  CHECK_STR(split_str(buf, sizeof buf, "/(-)/", "a-b-c", -1, PREG_SPLIT_DELIM_CAPTURE, &n),
            "a|-|b|-|c");
  CHECK(n == 5);
  CHECK_STR(split_str(buf, sizeof buf, "/\\s+/", "a  b\tc", -1, 0, &n), "a|b|c");
  CHECK_STR(split_str(buf, sizeof buf, "/x/", "abc", -1, 0, &n), "abc");
  CHECK(n == 1);
  CHECK_STR(split_str(buf, sizeof buf, "/,/", "", -1, 0, &n), "");           /* [""] */
  CHECK(n == 1);
  CHECK_STR(split_str(buf, sizeof buf, "/,/", "", -1, PREG_SPLIT_NO_EMPTY, &n), "");
  CHECK(n == 0);
  CHECK_STR(split_str(buf, sizeof buf, "/,/", ",a,", -1, 0, &n), "|a|");     /* edge pieces */
  CHECK(n == 3);
  CHECK_STR(split_str(buf, sizeof buf, "/,/", ",a,", -1, PREG_SPLIT_NO_EMPTY, &n), "a");

  /* the classic: empty matches DO split in preg_split */
  CHECK_STR(split_str(buf, sizeof buf, "//", "abc", -1, 0, &n), "|a|b|c|");
  CHECK(n == 5);
  CHECK_STR(split_str(buf, sizeof buf, "//", "abc", -1, PREG_SPLIT_NO_EMPTY, &n), "a|b|c");

  /* ---------------------------------------------------------------- *
   * preg_grep
   * ---------------------------------------------------------------- */
  {
    const char *arr[] = { "apple", "banana", "cherry", "avocado" };
    int *idx = NULL;

    n = preg_grep("/^a/", arr, NULL, 4, 0, &idx);
    CHECK(n == 2 && idx != NULL && idx[0] == 0 && idx[1] == 3);
    free(idx);

    n = preg_grep("/^a/", arr, NULL, 4, PREG_GREP_INVERT, &idx);
    CHECK(n == 2 && idx != NULL && idx[0] == 1 && idx[1] == 2);
    free(idx);

    CHECK(preg_grep("/an/", arr, NULL, 4, 0, NULL) == 1);          /* count only */
    CHECK(preg_grep("/zzz/", arr, NULL, 4, 0, &idx) == 0);
    free(idx);
    CHECK(preg_grep("/a/", arr, NULL, 0, 0, &idx) == 0);           /* empty input */
    free(idx);
    CHECK(preg_grep("bad", arr, NULL, 4, 0, &idx) == PREG_E_DELIMITER);
    CHECK(idx == NULL);

    {
      const int lens[] = { 2, 6, 6, 7 };                           /* "ap" "banana" ... */
      n = preg_grep("/^ap+le/", arr, lens, 4, 0, NULL);
      CHECK(n == 0);                                               /* truncated "ap" */
    }
  }

  printf("%d checks, %d failed\n", g_total, g_fail);
  return g_fail > 255 ? 255 : g_fail;
}
