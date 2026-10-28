/* 
 * This is the SLRE of 2025 (a fork from Aquefir) which was patched where possible against
 * known bugs and then wrapped in a PCRE-like API ("pl_...." functions).
 *
 * A month later, when author realized that his pl_ functions are not quite PHP-ready, an alternative
 * was introduced ("preg_..." functions)
 * 
 * All three API are available: the eaiest is "slre_..." (which is just 1 function)
 *
 * No lookarounds, no UTF8 (only byte matching, no ranges e.g. [?])
 *
 * Copyright (C) 2004-2013 Sergey Lyubka.
 * Copyright (C) 2013 Cesanta Software Limited.
 * Copyright (C) 2025 Aquefir Consulting LLC.
 * Copyright (C) 2026 Viacheslav Logunov (bug fixes, preg and pcre-like compatibility syntax extensions)
 * Released under GNU General Public License v2
 *
 */

#pragma once

#ifndef PH7_PRIVATE
#  define  PH7_PRIVATE
#endif

#define PL_SLRE_PATCHED 1       /* do not change. SLRE library variant (patched or not) */
#define PL_SLRE_FIXED_GROUPS 1  /* do not change. SLRE library variant (patched or not) */


#define PL_MAX_GROUPS   31 /* max user-visible capture groups */


/* pl_match() etc failure codes 
 * slre_match() failure codes 
 */
enum {
  PL_OK = 0,
  PL_NO_MATCH = -1,
  PL_E_UNSUPPORTED = -100, /* construct SLRE cannot express: lookaround, \b, backrefs, ... */
  PL_E_SYNTAX = -101,
  PL_E_NOMEM = -102,
  PL_E_LIMITS = -103, /* too many groups / repeat count or pattern too large */
  PL_E_ENGINE = -104, /* SLRE itself reported an error while matching */
  PL_E_GROUPS = -105, /* SLRE would mis-number these groups: nested / quantified / alternated
                        groups followed by other groups, e.g. (a)?(b)  (a)|(b)  ((a)b)(c) */

  PREG_E_DELIMITER = -110, /* missing / bad delimiter */
  PREG_E_MODIFIER  = -111, /* unsupported modifier */
  PREG_E_OFFSET    = -112, /* offset is beyond the end of the subject */
  PREG_E_ARGS      = -113,  /* NULL argument */

  SLRE_NO_MATCH = PL_NO_MATCH,
  SLRE_UNEXPECTED_QUANTIFIER = -2,
  SLRE_UNBALANCED_BRACKETS = -3,
  SLRE_INTERNAL_ERROR = -4,
  SLRE_INVALID_CHARACTER_SET = -5,
  SLRE_INVALID_METACHARACTER = -6,
  SLRE_CAPS_ARRAY_TOO_SMALL = -7,
  SLRE_TOO_MANY_BRANCHES = -8,
  SLRE_TOO_MANY_BRACKETS = -9,
};

/* PL / SLRE flags */
enum {
  PL_ICASE = 1,                 /* Ignore case */
  SLRE_IGNORE_CASE = PL_ICASE,
};

/* Same numeric values as in PHP; Do not change these numeric values as they must be kept in sync with
 * PH8 engine code
 */
enum {
  PREG_PATTERN_ORDER        = 1,
  PREG_SET_ORDER            = 2,
  PREG_OFFSET_CAPTURE       = 256,
  PREG_UNMATCHED_AS_NULL    = 512,

  PREG_SPLIT_NO_EMPTY       = 1,
  PREG_SPLIT_DELIM_CAPTURE  = 2,
  PREG_SPLIT_OFFSET_CAPTURE = 4,

  PREG_GREP_INVERT          = 1
};

typedef struct pl_regex pl_regex; /* opaque: compiled regex in PCRE-format */
typedef struct preg_re  preg_re;  /* opaque: compiled in PHP's PREG-format "/pattern/flags" */

/* Captures for PCRE
 */
typedef struct {

  int start;
  int end; /* byte offsets into the subject; -1/-1 if the group did not participate */

} pl_span;

typedef struct {

  int     ngroups;                /* number of capture groups in the pattern */
  pl_span g[PL_MAX_GROUPS + 1]; /* g[0] = whole match, g[1..ngroups] = groups */

} pl_match;

/* Captures for SLRE
 * Result of slre_match() : captured groups. Pointers point to the source buffer
 * no free() is needed
 */
struct slre_cap {

  const char * ptr;
  int          len;

};


/* Result of preg_match(): g[0] = whole match, g[1..count-1] = groups.
 * trailing unmatched groups are not counted, unless PREG_UNMATCHED_AS_NULL is set
 * (-> count == groups + 1). Unset group: start == -1.
 */
typedef struct {

  int     count;
  pl_span g[PL_MAX_GROUPS + 1];

} preg_match_t;

/* Result of preg_match_all(): flat array, PREG_ALL_SPAN(a, match, group).
 * PREG_PATTERN_ORDER view- iterate match for a fixed `group`.
 * PREG_SET_ORDER view    - iterate group < counts[match] for a fixed `mtach`
 *                          (counts[] is the PHP-trimmed size; all ngroups+1 are valid too).
 * Release with preg_all_free().
 */
typedef struct {

  int      nmatches;
  int      ngroups;
  pl_span *spans;    /* nmatches * (ngroups + 1) */
  int     *counts;   /* nmatches */

} preg_all_t;

#define PREG_SPAN_LEN(sp) ((sp).start < 0 ? 0 : (sp).end - (sp).start)
#define PREG_ALL_SPAN(a, i, g) ((a)->spans[(size_t)(i) * (size_t)((a)->ngroups + 1) + (size_t)(g)])

/* Matcher callback type, for pl_... functions */
typedef int (*pl_each_fn)(const pl_match *m, void *userdata);



/* Compile PHP PREG pattern (e.g. "/^hello/i". 
 * Returns NULL and stores an error code in *err (may be NULL).
 */
PH7_PRIVATE preg_re    *preg_compile(const char *pattern, int *err);  // function preg_compile(string $pattern): resource;

/* Free memory occupied by the compiled expression
 */
PH7_PRIVATE void        preg_re_free(preg_re *re);

/* Free memory (result of operation, returned by preg_match / preg_match_all)
 */
PH7_PRIVATE void        preg_all_free(preg_all_t *all);

/* Error code to a human-readable string
*/
PH7_PRIVATE const char *preg_strerror(int code);

/* Classic PHP preg_ functions:
 * Take the pattern, compile it, run the regex operation, free compiled pattern and return the result
 */
PH7_PRIVATE int   preg_match(const char *pattern, const char *s, int len, preg_match_t *m, int flags, int offset);
PH7_PRIVATE int   preg_match_all(const char *pattern, const char *s, int len, preg_all_t *all, int flags, int offset);
PH7_PRIVATE char *preg_replace(const char *pattern, const char *repl, const char *s, int len, int limit, int *out_len, int *count);
PH7_PRIVATE int   preg_split(const char *pattern, const char *s, int len, int limit, int flags, pl_span **out, int *nout);
PH7_PRIVATE int   preg_grep(const char *pattern, const char *const *subjects, const int *lens, int n, int flags, int **out_idx);

/* Classic PHP preg_ functions (precompiled pattern):
 * Precompiled variants (compile once, use many times).
 * In all functions `len` < 0 means strlen(s).
 */

/* Returns 1 (match), 0 (no match) or a negative error code.
 * `m` may be NULL. flags: PREG_UNMATCHED_AS_NULL. offset < 0 counts from the end. 
 */
PH7_PRIVATE int preg_match_re(const preg_re *re, const char *s, int len, preg_match_t *m, int flags, int offset);

/* Returns the number of matches or a negative error code.
 * `all` may be NULL (count only). flags: PREG_PATTERN_ORDER/SET_ORDER (storage is the same)
 * and PREG_UNMATCHED_AS_NULL. 
 */
PH7_PRIVATE int preg_match_all_re(const preg_re *re, const char *s, int len, preg_all_t *all, int flags, int offset);

/* Returns malloc()ed NUL-terminated string (free() it) or NULL on error.
 * *out_len: result length, or a negative error code if NULL is returned (out_len may be NULL).
 * *count:   number of replacements (may be NULL).
 * limit < 0: unlimited (PHP default is -1); limit == 0: nothing is replaced. 
 */
PH7_PRIVATE char *preg_replace_re(const preg_re *re, const char *s, int len, const char *repl, int limit, int *out_len, int *count);

/* Returns 0 or a negative error code. On success *out is a malloc()ed array of *nout spans
 * (NULL if *nout == 0) - free() it. limit: -1 or 0 = no limit.
 * flags: PREG_SPLIT_NO_EMPTY, PREG_SPLIT_DELIM_CAPTURE. 
 */
PH7_PRIVATE int preg_split_re(const preg_re *re, const char *s, int len, int limit, int flags, pl_span **out, int *nout);

/* subjects[i] / lens[i]; lens may be NULL (then strlen). Returns the number of selected
 * elements or a negative error code. *out_idx = malloc()ed array of their indices
 * (may be NULL if only the count is needed). flags: PREG_GREP_INVERT. 
 */
PH7_PRIVATE int preg_grep_re(const preg_re *re, const char *const *subjects, const int *lens, int n, int flags, int **out_idx);



/* internal helper: pl_regex is opaque struct, thats why
 * 
*/
PH7_PRIVATE int pl_ngroups(const pl_regex *re);

/* Compile PCRE pattern.
 * Returns NULL on error and stores the PL_E_* code in *err (err may be NULL)
 */
PH7_PRIVATE pl_regex *pl_compile( const char *pattern,
                      int flags,  /* So far only 1 flag is supported : ignore case*/
                      int *err);

/* Release the compiled regexp
*/
PH7_PRIVATE void pl_free(pl_regex *re);


/* Search for the leftmost match starting at offset `start` (like pcre_exec).
 * '^' still means "start of the whole subject", not "start offset".
 * Returns PL_OK, PL_NO_MATCH or a negative PL_E_* code.
 */
PH7_PRIVATE int pl_exec(const pl_regex *re,
            const char *s,
            int len,
            int start,
            pl_match *m
            );

/* Iterate over all non-overlapping matches. `fn` is called for each match;
 * if it returns non-zero the iteration stops. `fn` may be NULL (just count).
 * Returns the number of matches, or a negative PL_E_* code.
 */
PH7_PRIVATE int pl_match_all( const pl_regex *re,
                  const char *s,
                  int len,
                  pl_each_fn fn,
                  void *userdata
                );

/* Replace matches with a template. Template syntax:
 *   $0..$99, ${N}, \0..\99  - whole match / group N (unset group -> empty) TODO: real max is 31, see .h file
 *   $$  and  \\             - literal '$' and '\'
 * limit: max number of replacements, 0 = all.
 * Returns a malloc()ed NUL-terminated string (must be free()ed) and stores its length in
 * *out_len (NULL is ok). On failure returns NULL and, if out_len != NULL,
 * stores a negative PL_E_* code in *out_len.
 */
PH7_PRIVATE char *pl_replace( const pl_regex *re,
                  const char *s, 
                  int len,
                  const char *tmpl,
                  int limit,
                  int *out_len
                 );

/*
 * Split the subject on matches. Empty matches never split.
 * Writes up to max_out pieces to out (out may be NULL) and returns the TOTAL
 * number of pieces (may be > max_out), or a negative PL_E_* code.
 */
PH7_PRIVATE int pl_split( const pl_regex *re,
              const char *s,
              int len,
              pl_span *out,
              int max_out
             );


/* The translated pattern that is actually handed to SLRE (for debugging).
*/
PH7_PRIVATE const char *pl_slre_pattern(const pl_regex *re);


/* Native PH7 lightweright regex engine, only 1 function:
 */
PH7_PRIVATE int slre_match( const char * regexp,
                const char * buf,
                int buf_len,
                struct slre_cap * caps,
                int num_caps,
                int flags
              );
