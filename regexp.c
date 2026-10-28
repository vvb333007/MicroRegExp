/*
 * MicroRegExp — A lightweight regular expression library for C for microcontrollers
 *
 * LIMITATIONS:
 *
 *  1. Lookarounds are not supported.
 *  2. Matching operates on bytes: UTF-8 character ranges such as [à-ÿ] are not supported.
 *
 * A patched and extended fork of SLRE, originally based on the work
 * of Sergey Lyubka and subsequently maintained by Aquefir Consulting.
 * (https://github.com/aquefir/slre)
 *
 * The library provides three APIs, all available for direct use:
 *
 *   1. PHP preg-style API
 *      A well-known preg_* interface inspired by PHP's 
 *      regular expression functions.
 *
 *   2. PCRE-like API
 *      An additional API with pl_* function and structure names
 *
 *   3. Native SLRE API
 *      The original low-level matching interface - slre_match()
 *
 * The PHP preg-style API is the primary high-level interface.
 *
 *
 * Copyright (C) 2004-2013 Sergey Lyubka.
 * Copyright (C) 2013 Cesanta Software Limited.
 * Copyright (C) 2025 Aquefir Consulting LLC.
 * Copyright (C) 2026 Viacheslav Logunov.
 *
 * Released under the GNU General Public License, version 2.
 */
#include "regexp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MAX_DEPTH 32
#define PL_MAX_SLRE_CAPS 64 /* total '(' in the translated pattern, incl. wrapper */
#define PL_MAX_REPEAT 1000
#define PL_MAX_PATTERN (64 * 1024)

/* Compiled REGEX, PCRE-style API */
struct pl_regex {
  char *pat;    /* translated pattern, wrapped in one extra group: "(" ... ")" */
  int flags;    /* SLRE flags */
  int anchored; /* pattern started with ^ or \A (stripped from pat) */
  int ncaps;    /* number of '(' in pat, incl. wrapper */
  int nuser;    /* user-visible groups */
  unsigned char map[PL_MAX_GROUPS + 1]; /* user group i -> index in SLRE caps[] */
};

/* Compiled REGEX, PREG-stype API */
struct preg_re {
  pl_regex *re;  /* NULL for the empty pattern "//" */
  int anchored;  /* modifier A */
  int ngroups;
};


/* Rubbery array of spans */
typedef struct {
  pl_span *p;
  int n;
  int cap;
  int oom;
} spans_t;


/* Rubbery string buffer (can grow)
*/
typedef struct {
  char  *p;
  size_t len;
  size_t cap;
  int    oom;
} sbuf;

/* Pattern translator: PCRE -> SLRE
*/
typedef struct {

  sbuf out;
  const char *p;

  struct {
    int pos;
    int caps;
    int user;
  } stk[PL_MAX_DEPTH]; /* open groups: where '(' is in out, counters before it */

  int depth;
  int atom_pos;
  int atom_caps;
  int atom_user; /* last complete atom, needed for {n,m} */
  int ncaps, nuser;
  int top_alt;   /* saw a '|' outside any group */
  int class_neg; /* currently inside a negated [^...] */

  unsigned char map[PL_MAX_GROUPS + 1];

} tr_t;


/* UTILITY FUNCTIONS USED BY PCRE AND PREG APIs
 *
 */

/* Put data into sbuf, reallocing for bigger size if needed.
 */
void sb_put(sbuf *b, const char *s, size_t n) {

  if (!b->oom) {

    if (b->len + n + 1 > b->cap) {

      size_t nc = b->cap ? b->cap : 64;
      char *np;

      while (nc < b->len + n + 1)
        nc *= 2;

      np = (char *)realloc(b->p, nc);

      if (np == NULL) {
        b->oom = 1;
        return;
      }

      b->p = np;
      b->cap = nc;
    }

    if (n > 0)
      memcpy(b->p + b->len, s, n);

    b->len += n;
    b->p[b->len] = '\0';
  }
}

static inline void sb_putc(sbuf *b, char c) { 
  sb_put(b, &c, 1);
}

static inline void sb_puts(sbuf *b, const char *s) {
  sb_put(b, s, strlen(s));
}

static void put_hex(sbuf *b, unsigned char c) {
  char tmp[8];
  snprintf(tmp, sizeof tmp, "\\x%02x", c);
  sb_puts(b, tmp);
}


static void expand(sbuf *o, const char *s, const pl_match *m, const char *t) {
  while (*t != '\0') {
    char c = *t;
    if ((c == '$' || c == '\\') && t[1] == c) { /* $$  or  \\ */
      sb_putc(o, c);
      t += 2;
    } else if ((c == '$' || c == '\\') && isdigit((unsigned char) t[1])) {
      int n = t[1] - '0';
      t += 2;
      if (isdigit((unsigned char) *t)) n = n * 10 + (*t++ - '0');
      if (n <= m->ngroups && m->g[n].start >= 0)
        sb_put(o, s + m->g[n].start, (size_t) (m->g[n].end - m->g[n].start));
    } else if (c == '$' && t[1] == '{' && isdigit((unsigned char) t[2])) {
      const char *q = t + 2;
      int n = 0;
      while (isdigit((unsigned char) *q) && n < 1000) n = n * 10 + (*q++ - '0');
      if (*q == '}') {
        if (n <= m->ngroups && m->g[n].start >= 0)
          sb_put(o, s + m->g[n].start, (size_t) (m->g[n].end - m->g[n].start));
        t = q + 1;
      } else {
        sb_putc(o, c);
        t++;
      }
    } else {
      sb_putc(o, c);
      t++;
    }
  }
}


/*
 * Inside [...] SLRE runs every element through match_op(), so a literal '.' becomes a
 * wildcard, '$' never matches and '|' never matches (and [^|] matches '|').
 * Such characters are emitted as \xHH instead.
 */
static inline int is_set_special(unsigned char c) {
  return c == '.' || c == '$' || c == '|';
}


static void begin_atom(tr_t *t) {
  t->atom_pos = (int) t->out.len;
  t->atom_caps = t->ncaps;
  t->atom_user = t->nuser;
}

/* t->p points at the backslash.
*/
static int tr_escape(tr_t *t, int in_class) {

  unsigned char c = (unsigned char) t->p[1];
  char tmp[3];

  if (c == '\0')
    return PL_E_SYNTAX; /* trailing backslash */

  t->p += 2;

  switch (c) {

    case 'S':
#ifndef PL_SLRE_PATCHED
      /* stock SLRE: in a positive set \S would also match the terminating NUL past the end */
      if (in_class && !t->class_neg)
        return PL_E_UNSUPPORTED;
#endif
      sb_puts(&t->out, "\\S");
      return PL_OK;

    case 'd':
    case 's':
    case 'n':
    case 'r':
    case 't':
    case 'f':
    case 'v':
      tmp[0] = '\\';
      tmp[1] = (char) c;
      tmp[2] = '\0';
      sb_puts(&t->out, tmp);
      return PL_OK;

    case 'b':
      /* PCRE: word boundary. SLRE: backspace. Only the class meaning is compatible. */
      if (!in_class)
        return PL_E_UNSUPPORTED;

      sb_puts(&t->out, "\\b");
      return PL_OK;

    case 'w':
      sb_puts(&t->out, in_class ? "a-zA-Z0-9_" : "[a-zA-Z0-9_]");
      return PL_OK;

    case 'W':
      if (in_class)
        return PL_E_UNSUPPORTED;

      sb_puts(&t->out, "[^a-zA-Z0-9_]");
      return PL_OK;

    case 'D':
      if (in_class)
        return PL_E_UNSUPPORTED;

      sb_puts(&t->out, "[^0-9]");
      return PL_OK;

    case 'z':
      if (in_class)
        return PL_E_UNSUPPORTED;

      sb_putc(&t->out, '$');
      t->atom_pos = -1;
      return PL_OK;

    case 'x':
      if (!isxdigit((unsigned char) t->p[0]) ||
          !isxdigit((unsigned char) t->p[1]))
        return PL_E_UNSUPPORTED; /* \x{...} etc. */
#ifndef PL_SLRE_PATCHED
      if (in_class && t->p[0] == '0' &&
          t->p[1] == '0')
        return PL_E_UNSUPPORTED; /* see comment above */
#endif
      sb_puts(&t->out, "\\x");
      sb_put(&t->out, t->p, 2);
      t->p += 2;
      return PL_OK;

    default:
      /* \B \1 \p{..} \h \Z etc */
      if (isalnum(c))
        return PL_E_UNSUPPORTED;

      /* Escaped punctuation: emit as \xHH, which SLRE always accepts. */
      put_hex(&t->out, c);
      return PL_OK;
  }
}


static int tr_class(tr_t *t) {

  int rc;

  begin_atom(t);
  sb_putc(&t->out, '[');
  t->p++;
  t->class_neg = 0;
  if (*t->p == '^') {
    sb_putc(&t->out, '^');
    t->p++;
    t->class_neg = 1;
#ifndef PL_SLRE_PATCHED
    /*
     * Stock SLRE does not bound check sets - at the end of the subject a negated set matches
     * the terminating NUL as if it was a char
     */
    sb_puts(&t->out, "\\x00");
#endif
  }

  if (*t->p == ']') { /* leading ']' is literal in PCRE */
    put_hex(&t->out, ']');
    t->p++;
  }

  while (*t->p != '\0' && *t->p != ']') {

    unsigned char c = (unsigned char) *t->p;

    if (c == '\\') {

      char e = t->p[1];
      int shorthand = (e == 'w' || e == 'd' || e == 's' || e == 'S');

      rc = tr_escape(t, 1);

      if (rc != PL_OK)
        return rc;

      if ( *t->p == '-' &&
            t->p[1] != ']' &&
            t->p[1] != '\0') {
        /* SLRE ranges need plain-character endpoints, so "[\x41-Z]" would silently become
         * three literals. For "[\w-.]" PCRE treats '-' as literal: make that explicit. */
        if (!shorthand)
          return PL_E_UNSUPPORTED;

        put_hex(&t->out, '-');
        t->p++;
      }
    } else if ( c == '[' &&
                t->p[1] == ':') {
      return PL_E_UNSUPPORTED; /* POSIX classes [:alpha:] */

    } else if ( c != '-' &&
                t->p[1] == '-' &&
                t->p[2] != ']' &&
                t->p[2] != '\0') {

      unsigned char hi = (unsigned char) t->p[2]; /* range c-hi */

      if (hi == '\\' ||
          is_set_special(c) ||
          is_set_special(hi))
        return PL_E_UNSUPPORTED;

      sb_putc(&t->out, (char) c);
      sb_putc(&t->out, '-');
      sb_putc(&t->out, (char) hi);

      t->p += 3;

    } else if (is_set_special(c)) {

      put_hex(&t->out, c);
      t->p++;

    } else {

      sb_putc(&t->out, (char) c);
      t->p++;

    }
  }

  if (*t->p != ']') {
    //puts("regex: expecting ']'");
    return PL_E_SYNTAX;
  }

  sb_putc(&t->out, ']');
  t->p++;

  return PL_OK;
}

static int tr_open(tr_t *t) {

  int hidden = 0;

  if (t->p[1] == '?') {
    if (t->p[2] != ':')
      return PL_E_UNSUPPORTED; /* lookaround, named groups, inline flags */
    hidden = 1;
    t->p += 2;
  }

  if (t->depth >= PL_MAX_DEPTH ||
      t->ncaps >= PL_MAX_SLRE_CAPS)
    return PL_E_LIMITS;

  if (!hidden && t->nuser >= PL_MAX_GROUPS)
    return PL_E_LIMITS;

  t->stk[t->depth].pos = (int) t->out.len;
  t->stk[t->depth].caps = t->ncaps;
  t->stk[t->depth].user = t->nuser;
  t->depth++;

  if (!hidden)
    t->map[++t->nuser] = (unsigned char) t->ncaps;

  t->ncaps++;
  sb_putc(&t->out, '(');
  t->p++;
  t->atom_pos = -1;
  return PL_OK;
}

static int tr_close(tr_t *t) {

  if (t->depth == 0)
    return PL_E_SYNTAX;

  if (t->out.len > 0 &&
      t->out.p[t->out.len - 1] == '(')
    return PL_E_UNSUPPORTED; /* "()": SLRE never matches it */

  t->depth--;
  sb_putc(&t->out, ')');

  t->atom_pos = t->stk[t->depth].pos;
  t->atom_caps = t->stk[t->depth].caps;
  t->atom_user = t->stk[t->depth].user;

  t->p++;

  return PL_OK;
}

static int tr_quant(tr_t *t) {

  char q = *t->p++;

  if (t->atom_pos < 0)
    return PL_E_SYNTAX; /* nothing to repeat */

  sb_putc(&t->out, q);

  if (*t->p == '?') { /* lazy: *? +? are supported, ?? is not */
    if (q == '?')
      return PL_E_UNSUPPORTED;
    sb_putc(&t->out, '?');
    t->p++;

  } else if (*t->p == '+') {
    return PL_E_UNSUPPORTED; /* possessive */
  }
  t->atom_pos = -1;
  return PL_OK;
}

/* {n}  {n,} and  {n,m} are expanded by repeating the previous atom */
static int tr_brace(tr_t *t) {

  const char *q = t->p + 1;
  int n = 0, m, digits = 0, inner, inst, i;
  size_t alen;
  char *atom;

  while (isdigit((unsigned char) *q)) {
    n = n * 10 + (*q++ - '0');
    digits++;
    if (n > PL_MAX_REPEAT) return PL_E_LIMITS;
  }

  m = n;

  if (digits && *q == ',') {
    q++;
    if (isdigit((unsigned char) *q)) {
      m = 0;
      while (isdigit((unsigned char) *q)) {
        m = m * 10 + (*q++ - '0');
        if (m > PL_MAX_REPEAT)
          return PL_E_LIMITS;
      }
    } else {
      m = -1; /* unbounded */
    }
  }
  if (!digits || *q != '}') { /* not a quantifier! literal '{' (as in PCRE) */
    begin_atom(t);
    put_hex(&t->out, '{');
    t->p++;
    return PL_OK;
  }

  q++;

  if (*q == '?' || *q == '+')
    return PL_E_UNSUPPORTED; /* lazy / possessive range */

  if (t->atom_pos < 0)
    return PL_E_SYNTAX;

  if (m != -1 && m < n)
    return PL_E_SYNTAX;

  /* Repeating a capgroup wwill create a number of SLRE groups for just one PCRE group */
  if (t->nuser != t->atom_user)
    return PL_E_UNSUPPORTED;

  inner = t->ncaps - t->atom_caps; /* hidden groups inside the atom */
  inst = (m == -1) ? (n == 0 ? 1 : n) : m; /* how many copies will be emitted */
  alen = t->out.len - (size_t) t->atom_pos;

  if (alen * (size_t) (inst > 0 ? inst : 1) > PL_MAX_PATTERN)
    return PL_E_LIMITS;

  atom = (char *) malloc(alen ? alen : 1);

  if (atom == NULL)
    return PL_E_NOMEM;

  memcpy(atom, t->out.p + t->atom_pos, alen);

  t->out.len = (size_t) t->atom_pos;
  t->out.p[t->out.len] = '\0';

  if (m == -1) {
    if (n == 0) {
      sb_put(&t->out, atom, alen);
      sb_putc(&t->out, '*');
    } else {
      for (i = 0; i < n - 1; i++) sb_put(&t->out, atom, alen);
      sb_put(&t->out, atom, alen);
      sb_putc(&t->out, '+');
    }
  } else {
    for (i = 0; i < n; i++) sb_put(&t->out, atom, alen);
    for (i = 0; i < m - n; i++) {
      sb_put(&t->out, atom, alen);
      sb_putc(&t->out, '?');
    }
  }
  free(atom);

  t->ncaps += inner * (inst - 1);
  if (t->ncaps > PL_MAX_SLRE_CAPS) return PL_E_LIMITS;
  t->atom_pos = -1;
  t->p = q;
  return PL_OK;
}

static int tr_step(tr_t *t) {
  unsigned char c = (unsigned char) *t->p;
  switch (c) {
    case '[': return tr_class(t);
    case '(': return tr_open(t);
    case ')': return tr_close(t);
    case '*': 
    case '+': 
    case '?': return tr_quant(t);
    case '{': return tr_brace(t);
    case '^': return PL_E_UNSUPPORTED; /* only supported at the very start of the pattern */
    case '|':
      if (t->depth == 0) t->top_alt = 1;
      sb_putc(&t->out, '|');
      t->atom_pos = -1;
      t->p++;
      return PL_OK;
    case '$':
      sb_putc(&t->out, '$');
      t->atom_pos = -1;
      t->p++;
      return PL_OK;
    case '.': /* PCRE '.' does not match \n; SLRE's does */
      begin_atom(t);
#ifdef PL_SLRE_PATCHED
      sb_puts(&t->out, "[^\\n]");
#else
      sb_puts(&t->out, "[^\\x00\\n]");
#endif
      t->p++;
      return PL_OK;
    case '\\':
      begin_atom(t);
      return tr_escape(t, 0);
    case ']': case '}':
      begin_atom(t);
      put_hex(&t->out, c);
      t->p++;
      return PL_OK;
    default:
      begin_atom(t);
      sb_putc(&t->out, (char) c);
      t->p++;
      return PL_OK;
  }
}

/*
 * Final safety net over the translated pattern. This SLRE numbers groups in two places
 * with shortcuts that only work for "flat" patterns:
 *   parse time (foo):  on ')' or '|' after a child group closed it uses the nesting DEPTH
 *                      as the bracket index, right only if no group closed before it opened;
 *   match time (bar):  '(' takes bracket index = (direct child groups seen so far in this
 *                      branch) + 1, ignoring descendants, quantified groups and earlier '|'
 *                      branches.
 * Where the shortcuts are wrong SLRE silently matches the wrong sub-pattern, e.g.
 * "((a)?(b))" cannot match "b" and "((a)|(b))" cannot match "b". We mirror both rules here
 * and refuse such patterns. Define PL_SLRE_FIXED_GROUPS if you patched SLRE to look
 * brackets up by position; the layout check is then skipped.
 * Also enforces SLRE's MAX_BRANCHES (100).
 */
static int check_slre_pattern(const char *p) {
  struct {
    int index, total, direct;
  } st[PL_MAX_DEPTH + 2];
  int sp = 0, nopen = 0, nbranch = 0, i = 0, n = (int) strlen(p);

  st[0].index = st[0].total = st[0].direct = 0;
  while (i < n) {
    char c = p[i];
    if (c == '\\') {
      i += (p[i + 1] == 'x') ? 4 : 2;
    } else if (c == '[') {
      i++;
      while (i < n && p[i] != ']') i += (p[i] == '\\') ? ((p[i + 1] == 'x') ? 4 : 2) : 1;
      i++;
    } else if (c == '(') {
      int k;
      if (sp + 1 >= PL_MAX_DEPTH + 2) return PL_E_LIMITS;
#ifndef PL_SLRE_FIXED_GROUPS
      if (st[sp].total != st[sp].direct) return PL_E_GROUPS;
#endif
      for (k = 0; k <= sp; k++) st[k].total++;
      st[sp].direct++;
      sp++;
      st[sp].index = ++nopen;
      st[sp].total = st[sp].direct = 0;
      i++;
    } else if (c == ')') {
      if (sp == 0) return PL_E_SYNTAX;
#ifndef PL_SLRE_FIXED_GROUPS
      if (st[sp].total > 0 && st[sp].index != sp)
        return PL_E_GROUPS;
#endif
      sp--;
      if (p[i + 1] == '*' || p[i + 1] == '+' || p[i + 1] == '?') st[sp].direct--;
      i++;
    } else if (c == '|') {
      if (++nbranch > 100) return PL_E_LIMITS;
#ifndef PL_SLRE_FIXED_GROUPS
      if (st[sp].total > 0 && st[sp].index != sp)
        return PL_E_GROUPS;
#endif
      st[sp].direct = 0;
      i++;
    } else {
      i++;
    }
  }
  return PL_OK;
}

/* PCRE-like API
 *
 */
PH7_PRIVATE pl_regex *pl_compile(const char *pattern, int flags, int *err) {
  tr_t t;
  pl_regex *re = NULL;
  int rc = PL_OK, anchored = 0;
  const char *p = pattern;

  memset(&t, 0, sizeof t);
  if (strncmp(p, "(?i)", 4) == 0) {
    flags |= PL_ICASE;
    p += 4;
  }
  if (p[0] == '^') {
    anchored = 1;
    p++;
  } else if (p[0] == '\\' && p[1] == 'A') {
    anchored = 1;
    p += 2;
  }
  if (*p == '\0') rc = PL_E_SYNTAX; /* empty patterns are not supported */

  /*
   * The whole pattern is wrapped in a group so that SLRE reports where the match
   * starts (SLRE itself only returns the end offset). This group is caps[0].
   */
  t.p = p;
  t.atom_pos = -1;
  sb_putc(&t.out, '(');
  t.ncaps = 1;

  while (rc == PL_OK && *t.p != '\0') rc = tr_step(&t);
  if (rc == PL_OK && t.depth != 0) rc = PL_E_SYNTAX;
  if (rc == PL_OK) sb_putc(&t.out, ')');
  if (rc == PL_OK && t.out.oom) rc = PL_E_NOMEM;
  if (rc == PL_OK && t.out.len > PL_MAX_PATTERN) rc = PL_E_LIMITS;
  if (rc == PL_OK && anchored && t.top_alt) rc = PL_E_UNSUPPORTED; /* "^a|b": ^ binds to one branch */
  if (rc == PL_OK) rc = check_slre_pattern(t.out.p);
  if (rc == PL_OK) {
    re = (pl_regex *) calloc(1, sizeof *re);
    if (re == NULL) {
      rc = PL_E_NOMEM;
    } else {
      re->pat = t.out.p;
      t.out.p = NULL;
      re->flags = (flags & PL_ICASE) ? SLRE_IGNORE_CASE : 0;
      re->anchored = anchored;
      re->ncaps = t.ncaps;
      re->nuser = t.nuser;
      memcpy(re->map, t.map, sizeof re->map);
    }
  }
  free(t.out.p);
  if (err != NULL) *err = rc;
  return re;
}

PH7_PRIVATE void pl_free(pl_regex *re) {
  if (re != NULL) {
    free(re->pat);
    free(re);
  }
}

/* utility functions (a couple of getters) as pl_regex is opaque struct 
*/
PH7_PRIVATE const char *pl_slre_pattern(const pl_regex *re) {
  return re->pat;
}

PH7_PRIVATE int pl_ngroups(const pl_regex *re) {
  return re->nuser;
}

/* Matching
*/
PH7_PRIVATE int pl_exec(const pl_regex *re, const char *s, int len, int start, pl_match *m) {
  struct slre_cap caps[PL_MAX_SLRE_CAPS];
  int r, i, ms, me;

  if (start < 0 || start > len) return PL_NO_MATCH;
  if (re->anchored && start > 0) return PL_NO_MATCH;

  memset(caps, 0, sizeof caps);
  r = slre_match(re->pat, s + start, len - start, caps, re->ncaps, re->flags);
  if (r == SLRE_NO_MATCH) return PL_NO_MATCH;
  if (r < 0) return PL_E_ENGINE;

  /*
   * caps[0] is the wrapper group = whole match. SLRE does not fill in groups that
   * matched the empty string, so ptr == NULL means "empty match ending at r"
   * (r is the end offset relative to s + start).
   */
  if (caps[0].ptr != NULL) {
    ms = (int) (caps[0].ptr - s);
    me = ms + caps[0].len;
  } else {
    ms = me = start + r;
  }
  if (re->anchored && ms != 0) return PL_NO_MATCH;

  if (m != NULL) {
    m->ngroups = re->nuser;
    m->g[0].start = ms;
    m->g[0].end = me;
    for (i = 1; i <= re->nuser; i++) {
      const struct slre_cap *c = &caps[re->map[i]];
      /*
       * SLRE never clears captures of failed attempts, so a group that did not take part in
       * the final match may still hold text from an earlier attempt. Anything outside the
       * match is certainly stale. (Stale captures from another branch at the same start
       * position cannot be detected.)
       */
      if (c->ptr != NULL && c->ptr >= s + ms && c->ptr + c->len <= s + me) {
        m->g[i].start = (int) (c->ptr - s);
        m->g[i].end = m->g[i].start + c->len;
      } else {
        m->g[i].start = m->g[i].end = -1;
      }
    }
  }
  return PL_OK;
}

PH7_PRIVATE int pl_match_all(const pl_regex *re, const char *s, int len, pl_each_fn fn, void *ud) {
  int pos = 0, count = 0, r;
  pl_match m;

  while (pos <= len) {
    r = pl_exec(re, s, len, pos, &m);
    if (r == PL_NO_MATCH) break;
    if (r < 0) return r;
    count++;
    if (fn != NULL && fn(&m, ud) != 0) break;
    /* after an empty match step one byte forward, otherwise we would loop forever */
    pos = (m.g[0].end == m.g[0].start) ? m.g[0].end + 1 : m.g[0].end;
  }
  return count;
}



/*
 * Replace
 */
PH7_PRIVATE char *pl_replace(const pl_regex *re, const char *s, int len, const char *tmpl, int limit,
                 int *out_len) {
  sbuf o;
  pl_match m;
  int pos = 0, last = 0, n = 0, r;

  memset(&o, 0, sizeof o);
  sb_put(&o, "", 0); /* make sure o.p is allocated and NUL-terminated */

  while (pos <= len && (limit <= 0 || n < limit)) {
    r = pl_exec(re, s, len, pos, &m);
    if (r == PL_NO_MATCH) break;
    if (r < 0) {
      free(o.p);
      if (out_len != NULL) *out_len = r;
      return NULL;
    }
    sb_put(&o, s + last, (size_t) (m.g[0].start - last));
    expand(&o, s, &m, tmpl);
    last = m.g[0].end;
    n++;
    pos = (m.g[0].end == m.g[0].start) ? m.g[0].end + 1 : m.g[0].end;
  }
  sb_put(&o, s + last, (size_t) (len - last));

  if (o.oom) {
    free(o.p);
    if (out_len != NULL) *out_len = PL_E_NOMEM;
    return NULL;
  }
  if (out_len != NULL) *out_len = (int) o.len;
  return o.p;
}



/*
 * Split
 */
PH7_PRIVATE int pl_split(const pl_regex *re, const char *s, int len, pl_span *out, int max_out) {
  pl_match m;
  int pos = 0, last = 0, n = 0, r;

  while (pos <= len) {
    r = pl_exec(re, s, len, pos, &m);
    if (r == PL_NO_MATCH) break;
    if (r < 0) return r;
    if (m.g[0].end == m.g[0].start) { /* empty matches never split */
      pos = m.g[0].end + 1;
      continue;
    }
    if (out != NULL && n < max_out) {
      out[n].start = last;
      out[n].end = m.g[0].start;
    }
    n++;
    last = pos = m.g[0].end;
  }
  if (out != NULL && n < max_out) {
    out[n].start = last;
    out[n].end = len;
  }
  return n + 1;
}

/* A Patched SLRE
 *
 */

#define MAX_BRANCHES 100
#define MAX_BRACKETS 100
#define FAIL_IF( c, e ) \
  if( c ) \
  return ( e )

#ifndef ARRAY_SIZE
#define ARRAY_SIZE( ar ) ( sizeof( ar ) / sizeof( ( ar )[0] ) )
#endif

#ifdef SLRE_DEBUG
#define DBG( x ) printf x
#else
#define DBG( x )
#endif

struct bracket_pair
{
  /* Points to the first char after '(' in regex */
  const char * ptr;
  /* Length of the text between '(' and ')' */
  int len;
  /* Index in the branches array for this pair */
  int branches;
  /* Number of '|' in this bracket pair */
  int num_branches;
};

struct branch
{
  /* index for 'struct bracket_pair brackets'
   * array defined below */
  int bracket_index;
  /* points to the '|' character in the regex */
  const char * schlong;
};

struct regex_info
{
  /* Describes all bracket pairs in the regular expression.
   * First entry is always present, and grabs the whole regex.
   */
  struct bracket_pair brackets[MAX_BRACKETS];
  int num_brackets;

  /* Describes alternations ('|' operators) in the regular
   * expression. Each branch falls into a specific branch pair.
   */
  struct branch branches[MAX_BRANCHES];
  int num_branches;

  /* Array of captures provided by the user */
  struct slre_cap * caps;
  int num_caps;

  /* E.g. SLRE_IGNORE_CASE. See enum below */
  int flags;
};

static int is_metacharacter( const unsigned char * s )
{
  static const char * metacharacters = "^$().[]*+?|\\Ssdbfnrtv";
  return strchr( metacharacters, *s ) != NULL;
}

static int op_len( const char * re )
{
  return re[0] == '\\' && re[1] == 'x' ? 4
    : re[0] == '\\'              ? 2
               : 1;
}

static int set_len( const char * re, int re_len )
{
  int len = 0;

  while( len < re_len && re[len] != ']' )
  {
    len += op_len( re + len );
  }

  return len <= re_len ? len + 1 : -1;
}

static int get_op_len( const char * re, int re_len )
{
  return re[0] == '[' ? set_len( re + 1, re_len - 1 ) + 1
          : op_len( re );
}

static int is_quantifier( const char * re )
{
  return re[0] == '*' || re[0] == '+' || re[0] == '?';
}

static int toi( int x ) { return isdigit( x ) ? x - '0' : x - 'W'; }

static int hextoi( const unsigned char * s )
{
  return ( toi( tolower( s[0] ) ) << 4 ) | toi( tolower( s[1] ) );
}

static int match_op( const unsigned char * re,
  const unsigned char * s,
  struct regex_info * info )
{
  int result = 0;
  switch( *re )
  {
  case '\\':
    /* Metacharacters */
    switch( re[1] )
    {
    case 'S':
      FAIL_IF( isspace( *s ), SLRE_NO_MATCH );
      result++;
      break;
    case 's':
      FAIL_IF( !isspace( *s ), SLRE_NO_MATCH );
      result++;
      break;
    case 'd':
      FAIL_IF( !isdigit( *s ), SLRE_NO_MATCH );
      result++;
      break;
    case 'b':
      FAIL_IF( *s != '\b', SLRE_NO_MATCH );
      result++;
      break;
    case 'f':
      FAIL_IF( *s != '\f', SLRE_NO_MATCH );
      result++;
      break;
    case 'n':
      FAIL_IF( *s != '\n', SLRE_NO_MATCH );
      result++;
      break;
    case 'r':
      FAIL_IF( *s != '\r', SLRE_NO_MATCH );
      result++;
      break;
    case 't':
      FAIL_IF( *s != '\t', SLRE_NO_MATCH );
      result++;
      break;
    case 'v':
      FAIL_IF( *s != '\v', SLRE_NO_MATCH );
      result++;
      break;

    case 'x':
      /* Match byte, \xHH where HH is hexadecimal byte
       * representaion */
      FAIL_IF(
        hextoi( re + 2 ) != *s, SLRE_NO_MATCH );
      result++;
      break;

    default:
      /* Valid metacharacter check is done in bar() */
      FAIL_IF( re[1] != s[0], SLRE_NO_MATCH );
      result++;
      break;
    }
    break;

  case '|':
    FAIL_IF( 1, SLRE_INTERNAL_ERROR );
    break;
  case '$':
    FAIL_IF( 1, SLRE_NO_MATCH );
    break;
  case '.':
    result++;
    break;

  default:
    if( info->flags & SLRE_IGNORE_CASE )
    {
      FAIL_IF( tolower( *re ) != tolower( *s ),
        SLRE_NO_MATCH );
    }
    else
    {
      FAIL_IF( *re != *s, SLRE_NO_MATCH );
    }
    result++;
    break;
  }

  return result;
}

static int match_set( const char * re,
  int re_len,
  const char * s,
  struct regex_info * info )
{
  int len = 0, result = -1, invert = re[0] == '^';

  if( invert )
    re++, re_len--;

  while( len <= re_len && re[len] != ']' && result <= 0 )
  {
    /* Support character range */
    if( re[len] != '-' && re[len + 1] == '-' &&
      re[len + 2] != ']' && re[len + 2] != '\0' )
    {
      result = info->flags & SLRE_IGNORE_CASE
        ? tolower( *s ) >= tolower( re[len] ) &&
          tolower( *s ) <=
            tolower( re[len + 2] )
        : *s >= re[len] && *s <= re[len + 2];
      len += 3;
    }
    else
    {
      result = match_op(
        (const unsigned char *)re + len,
        (const unsigned char *)s,
        info );
      len += op_len( re + len );
    }
  }
  return ( !invert && result > 0 ) || ( invert && result <= 0 )
    ? 1
    : -1;
}

static int doh(
  const char * s, int s_len, struct regex_info * info, int bi );

/* Index of the bracket whose '(' is right before p (i.e. brackets[k].ptr == p). */
static int bracket_at( const struct regex_info * info, const char * p )
{
  int k;
  for( k = 1; k < info->num_brackets; k++ )
    if( info->brackets[k].ptr == p )
      return k;
  return -1;
}

static int bar( const char * re,
  int re_len,
  const char * s,
  int s_len,
  struct regex_info * info,
  int bi )
{
  /* i is offset in re, j is offset in s, bi is brackets index */
  int i, j, n, step, bk = 0;

  for( i = j = 0; i < re_len && j <= s_len; i += step )
  {

    /* Handle quantifiers. Get the length of the chunk. */
    if( re[i] == '(' )
    {
      /* FIX: look the bracket up by position instead of counting */
      bk = bracket_at( info, re + i + 1 );
      FAIL_IF( bk < 0, SLRE_INTERNAL_ERROR );
    }
    step = re[i] == '(' ? info->brackets[bk].len + 2
            : get_op_len( re + i, re_len - i );

    DBG( ( "%s [%.*s] [%.*s] re_len=%d step=%d i=%d j=%d\n",
      __func__,
      re_len - i,
      re + i,
      s_len - j,
      s + j,
      re_len,
      step,
      i,
      j ) );

    FAIL_IF( is_quantifier( &re[i] ),
      SLRE_UNEXPECTED_QUANTIFIER );
    FAIL_IF( step <= 0, SLRE_INVALID_CHARACTER_SET );

    if( i + step < re_len &&
      is_quantifier( re + i + step ) )
    {
      DBG( ( "QUANTIFIER: [%.*s]%c [%.*s]\n",
        step,
        re + i,
        re[i + step],
        s_len - j,
        s + j ) );
      if( re[i + step] == '?' )
      {
        int result = bar( re + i,
          step,
          s + j,
          s_len - j,
          info,
          bi );
        /* FIX: try "atom taken" together with the rest of the regex first;
         * if the rest fails, fall back to "atom skipped" (backtrack). */
        if( result > 0 )
        {
          int rest = bar( re + i + step + 1,
            re_len - ( i + step + 1 ),
            s + j + result,
            s_len - ( j + result ),
            info,
            bi );
          if( rest >= 0 )
            return j + result + rest;
        }
        i++;
      }
      else if( re[i + step] == '+' ||
        re[i + step] == '*' )
      {
        int j2 = j, nj = j, n1, n2 = -1, ni,
            non_greedy = 0;

        /* Points to the regexp code after the
         * quantifier */
        ni = i + step + 1;
        if( ni < re_len && re[ni] == '?' )
        {
          non_greedy = 1;
          ni++;
        }

        do
        {
          if( ( n1 = bar( re + i,
                  step,
                  s + j2,
                  s_len - j2,
                  info,
                  bi ) ) > 0 )
          {
            j2 += n1;
          }
          if( re[i + step] == '+' &&
            n1 < 0 )
            break;

          if( ni >= re_len )
          {
            /* After quantifier,
             * there is nothing */
            nj = j2;
          }
          else if( ( n2 = bar( re + ni,
                 re_len - ni,
                 s + j2,
                 s_len - j2,
                 info,
                 bi ) ) >= 0 )
          {
            /* Regex after
             * quantifier matched */
            nj = j2 + n2;
          }
          if( nj > j && non_greedy )
            break;
        } while( n1 > 0 );

        /*
         * Even if we found one or more pattern,
         * this branch will be executed,
         * changing the next captures.
         */
        if( n1 < 0 && n2 < 0 &&
          re[i + step] == '*' &&
          ( n2 = bar( re + ni,
              re_len - ni,
              s + j,
              s_len - j,
              info,
              bi ) ) > 0 )
        {
          nj = j + n2;
        }

        DBG( ( "STAR/PLUS END: %d %d %d %d %d\n",
          j,
          nj,
          re_len - ni,
          n1,
          n2 ) );
        FAIL_IF( re[i + step] == '+' && nj == j,
          SLRE_NO_MATCH );

        /* If while loop body above was not
         * executed for the * quantifier,  */
        /* make sure the rest of the regex
         * matches                          */
        FAIL_IF( nj == j && ni < re_len &&
            n2 < 0,
          SLRE_NO_MATCH );

        /* Returning here cause we've matched
         * the rest of RE already */
        return nj;
      }
      continue;
    }

    if( re[i] == '[' )
    {
      /* FIX: sets must respect s_len like every other operator */
      FAIL_IF( j >= s_len, SLRE_NO_MATCH );
      n = match_set( re + i + 1,
        re_len - ( i + 2 ),
        s + j,
        info );
      DBG( ( "SET %.*s [%.*s] -> %d\n",
        step,
        re + i,
        s_len - j,
        s + j,
        n ) );
      FAIL_IF( n <= 0, SLRE_NO_MATCH );
      j += n;
    }
    else if( re[i] == '(' )
    {
      n = SLRE_NO_MATCH;
      DBG( ( "CAPTURING [%.*s] [%.*s] [%s]\n",
        step,
        re + i,
        s_len - j,
        s + j,
        re + i + step ) );

      if( re_len - ( i + step ) <= 0 )
      {
        /* Nothing follows brackets */
        n = doh( s + j, s_len - j, info, bk );
      }
      else
      {
        int j2;
        for( j2 = 0; j2 <= s_len - j; j2++ )
        {
          if( ( n = doh( s + j,
                  s_len - ( j + j2 ),
                  info,
                  bk ) ) >= 0 &&
            bar( re + i + step,
              re_len -
                ( i + step ),
              s + j + n,
              s_len - ( j + n ),
              info,
              bi ) >= 0 )
            break;
        }
      }

      DBG( ( "CAPTURED [%.*s] [%.*s]:%d\n",
        step,
        re + i,
        s_len - j,
        s + j,
        n ) );
      FAIL_IF( n < 0, n );
      if( info->caps != NULL && n > 0 )
      {
        info->caps[bk - 1].ptr = s + j;
        info->caps[bk - 1].len = n;
      }
      j += n;
    }
    else if( re[i] == '^' )
    {
      FAIL_IF( j != 0, SLRE_NO_MATCH );
    }
    else if( re[i] == '$' )
    {
      FAIL_IF( j != s_len, SLRE_NO_MATCH );
    }
    else
    {
      FAIL_IF( j >= s_len, SLRE_NO_MATCH );
      n = match_op( (const unsigned char *)( re + i ),
        (const unsigned char *)( s + j ),
        info );
      FAIL_IF( n <= 0, n );
      j += n;
    }
  }

  return j;
}

/* Process branch points */
static int doh(
  const char * s, int s_len, struct regex_info * info, int bi )
{
  const struct bracket_pair * b = &info->brackets[bi];
  int i                         = 0, len, result;
  const char * p;

  do
  {
    p   = i == 0
        ? b->ptr
        : info->branches[b->branches + i - 1].schlong +
                                1;
    len = b->num_branches == 0 ? b->len
      : i == b->num_branches
      ? (int)( b->ptr + b->len - p )
      : (int)( info->branches[b->branches + i]
            .schlong -
          p );
    DBG( ( "%s %d %d [%.*s] [%.*s]\n",
      __func__,
      bi,
      i,
      len,
      p,
      s_len,
      s ) );
    result = bar( p, len, s, s_len, info, bi );
    DBG( ( "%s <- %d\n", __func__, result ) );
  } while( result < 0 && /* FIX: was "<= 0": an empty match ended the search of branches wrongly */
    i++ < b->num_branches ); /* At least 1 iteration */

  return result;
}

static int baz( const char * s, int s_len, struct regex_info * info )
{
  int i, result = -1,
         is_anchored = info->brackets[0].ptr[0] == '^';

  for( i = 0; i <= s_len; i++ )
  {
    result = doh( s + i, s_len - i, info, 0 );
    if( result >= 0 )
    {
      result += i;
      break;
    }
    if( is_anchored )
      break;
  }

  return result;
}

static void setup_branch_points( struct regex_info * info )
{
  int i, j;
  struct branch tmp;

  /* First, sort branches. Must be stable, no qsort. Use bubble
   * algo. */
  for( i = 0; i < info->num_branches; i++ )
  {
    for( j = i + 1; j < info->num_branches; j++ )
    {
      if( info->branches[i].bracket_index >
        info->branches[j].bracket_index )
      {
        tmp               = info->branches[i];
        info->branches[i] = info->branches[j];
        info->branches[j] = tmp;
      }
    }
  }

  /*
   * For each bracket, set their branch points. This way, for
   * every bracket (i.e. every chunk of regex) we know all branch
   * points before matching.
   */
  for( i = j = 0; i < info->num_brackets; i++ )
  {
    info->brackets[i].num_branches = 0;
    info->brackets[i].branches     = j;
    while( j < info->num_branches &&
      info->branches[j].bracket_index == i )
    {
      info->brackets[i].num_branches++;
      j++;
    }
  }
}

static int foo( const char * re,
  int re_len,
  const char * s,
  int s_len,
  struct regex_info * info )
{
  int i, step, depth = 0;
  int open_stack[MAX_BRACKETS + 1]; /* FIX: indices of currently open brackets */

  open_stack[0] = 0;

  /* First bracket captures everything */
  info->brackets[0].ptr = re;
  info->brackets[0].len = re_len;
  info->num_brackets    = 1;

  /* Make a single pass over regex string, memorize brackets and
   * branches */
  for( i = 0; i < re_len; i += step )
  {
    step = get_op_len( re + i, re_len - i );

    if( re[i] == '|' )
    {
      FAIL_IF( info->num_branches >=
          (int)ARRAY_SIZE(
            info->branches ),
        SLRE_TOO_MANY_BRANCHES );
      info->branches[info->num_branches]
        .bracket_index = open_stack[depth];
      info->branches[info->num_branches].schlong =
        &re[i];
      info->num_branches++;
    }
    else if( re[i] == '\\' )
    {
      FAIL_IF( i >= re_len - 1,
        SLRE_INVALID_METACHARACTER );
      if( re[i + 1] == 'x' )
      {
        /* Hex digit specification must follow
         */
        FAIL_IF( re[i + 1] == 'x' &&
            i >= re_len - 3,
          SLRE_INVALID_METACHARACTER );
        FAIL_IF( re[i + 1] == 'x' &&
            !( isxdigit( (int)((unsigned )re[i + 2]) ) &&
              isxdigit( (int)((unsigned )re[i + 3]) ) ),
          SLRE_INVALID_METACHARACTER );
      }
      else
      {
        FAIL_IF( !is_metacharacter(
             (const unsigned char *)
               re +
             i + 1 ),
          SLRE_INVALID_METACHARACTER );
      }
    }
    else if( re[i] == '(' )
    {
      FAIL_IF( info->num_brackets >=
          (int)ARRAY_SIZE(
            info->brackets ),
        SLRE_TOO_MANY_BRACKETS );
      depth++; /* Order is important here. Depth
            increments first. */
      info->brackets[info->num_brackets].ptr =
        re + i + 1;
      info->brackets[info->num_brackets].len = -1;
      open_stack[depth] = info->num_brackets;
      info->num_brackets++;
      FAIL_IF( info->num_caps > 0 &&
          info->num_brackets - 1 >
            info->num_caps,
        SLRE_CAPS_ARRAY_TOO_SMALL );
    }
    else if( re[i] == ')' )
    {
      int ind;
      FAIL_IF( depth <= 0, SLRE_UNBALANCED_BRACKETS );
      ind = open_stack[depth];
      info->brackets[ind].len = (int)( &re[i] -
        info->brackets[ind].ptr );
      DBG( ( "SETTING BRACKET %d [%.*s]\n",
        ind,
        info->brackets[ind].len,
        info->brackets[ind].ptr ) );
      depth--;
      FAIL_IF( depth < 0, SLRE_UNBALANCED_BRACKETS );
      FAIL_IF( i > 0 && re[i - 1] == '(',
        SLRE_NO_MATCH );
    }
  }

  FAIL_IF( depth != 0, SLRE_UNBALANCED_BRACKETS );
  setup_branch_points( info );

  return baz( s, s_len, info );
}

/*
 * SLRE PUBLIC API
 *
 */

PH7_PRIVATE int slre_match( const char * regexp,
  const char * s,
  int s_len,
  struct slre_cap * caps,
  int num_caps,
  int flags )
{
  struct regex_info info;

  /* Initialize info structure */
  info.flags        = flags;
  info.num_brackets = info.num_branches = 0;
  info.num_caps                         = num_caps;
  info.caps                             = caps;

  DBG( ( "========================> [%s] [%.*s]\n",
    regexp,
    s_len,
    s ) );
  return foo( regexp, (int)strlen( regexp ), s, s_len, &info );
}



/* Helpers for preg_-style API
*/
static void sp_push(spans_t *a, int start, int end) {

  if (!a->oom) {

    if (a->n == a->cap) {

      int nc = a->cap ? a->cap * 2 : 8;
      pl_span *np = (pl_span *) realloc(a->p, (size_t) nc * sizeof *np);

      if (np == NULL) {
        a->oom = 1;
        return;
      }

      a->p = np;
      a->cap = nc;
    }

    a->p[a->n].start = start;
    a->p[a->n].end = end;
    a->n++;
  }
}


/* "/pattern/flags" -> malloc()ed body + flags
*/
static int parse_pattern(const char *pat, char **body_out, int *flags, int *anchored) {

  const char *p = pat, *b, *e;
  char open, close;
  int depth = 1;
  size_t n;
  char *body;

  while (isspace((unsigned char) *p))
    p++;

  open = *p;

  if (open == '\0' || isalnum((unsigned char) open) || open == '\\')
    return PREG_E_DELIMITER;

  switch (open) {
    case '(': close = ')'; break;
    case '[': close = ']'; break;
    case '{': close = '}'; break;
    case '<': close = '>'; break;
    default:  close = open; break;
  }

  b = ++p;

  while (*p != '\0') {

    if (*p == '\\' && p[1] != '\0') { /* escaped char (incl. escaped delimiter) */
      p += 2;
      continue;
    }

    if (close != open && *p == open)
      depth++;
    else if (*p == close && --depth == 0)
      break;

    p++;
  }

  if (*p == '\0')
    return PREG_E_DELIMITER;

  e = p++;

  for (; *p != '\0'; p++) {
    switch (*p) {
      case 'i': *flags |= PL_ICASE; break;
      case 'A': *anchored = 1; break;
      case 'D':
      case 'S':
      case ' ':
      case '\n':
      case '\r': break;
      default: return PREG_E_MODIFIER;
    }
  }

  n = (size_t) (e - b);
  body = (char *) malloc(n + 1);

  if (body == NULL)
    return PL_E_NOMEM;

  memcpy(body, b, n);
  body[n] = '\0';
  *body_out = body;

  return PL_OK;
}



/* Common helpers
*/

/* One search from `start`; handles the empty pattern and modifier A. */
static int re_exec(const preg_re *r, const char *s, int len, int start, pl_match *m) {

  int rc;

  if (r->re == NULL) {
    if (start < 0 || start > len)
      return PL_NO_MATCH;
    m->ngroups = 0;
    m->g[0].start = m->g[0].end = start;
    return PL_OK;
  }

  rc = pl_exec(r->re, s, len, start, m);

  /* leftmost match: if there is one exactly at `start`, it is the one we got */
  if (rc == PL_OK && r->anchored && m->g[0].start != start)
    return PL_NO_MATCH;

  return rc;
}

/* PHP drops trailing groups that did not participate */
static int visible_count(const pl_match *m, int flags) {

  int n = m->ngroups;

  if (!(flags & PREG_UNMATCHED_AS_NULL)) {
    while (n > 0 && m->g[n].start < 0)
      n--;
  }

  return n + 1;
}

static int next_pos(const pl_match *m) {
  /* after an empty match step one byte forward, otherwise we would loop forever */
  return (m->g[0].end == m->g[0].start) ? m->g[0].end + 1 : m->g[0].end;
}

static int norm_offset(int *off, int len) {

  if (*off < 0) {
    *off += len;
    if (*off < 0)
      *off = 0;
  } else if (*off > len) {
    return PREG_E_OFFSET;
  }

  return PL_OK;
}

static int get_backref(const char **pp, int *ref) {

  const char *w = *pp;
  int brace = 0, n;

  if (w[1] == '\0')
    return 0;

  if (*w == '$' && w[1] == '{') {
    brace = 1;
    w++;
  }

  w++;

  if (*w >= '0' && *w <= '9')
    n = *w++ - '0';
  else
    return 0;

  if (*w >= '0' && *w <= '9')
    n = n * 10 + (*w++ - '0');

  if (brace) {
    if (*w != '}')
      return 0;
    w++;
  }

  *ref = n;
  *pp = w;

  return 1;
}

static void expand2(sbuf *o, const char *s, const pl_match *m, const char *t) {

  char last = 0;
  int ref;

  while (*t != '\0') {

    if (*t == '\\' || *t == '$') {

      if (last == '\\') { /* "\\" -> "\"   "\$" -> "$": overwrite the backslash just emitted */
        if (!o->oom)
          o->p[o->len - 1] = *t;
        t++;
        last = 0;
        continue;
      }

      if (get_backref(&t, &ref)) {
        if (ref <= m->ngroups && m->g[ref].start >= 0)
          sb_put(o, s + m->g[ref].start, (size_t) (m->g[ref].end - m->g[ref].start));
        last = 0;
        continue;
      }
    }

    sb_putc(o, *t);
    last = *t;
    t++;
  }
}


/* PREG PUBLIC API
 *  Compile pattern 
 */
PH7_PRIVATE preg_re *preg_compile(const char *pattern, int *err) {

  preg_re *r;
  char *body = NULL;
  int flags = 0, anchored = 0, rc, e = PL_OK;

  if (pattern == NULL) {
    if (err != NULL) *err = PREG_E_ARGS;
    return NULL;
  }

  rc = parse_pattern(pattern, &body, &flags, &anchored);

  if (rc != PL_OK) {
    if (err != NULL) *err = rc;
    return NULL;
  }

  r = (preg_re *) calloc(1, sizeof *r);

  if (r == NULL) {
    free(body);
    if (err != NULL) *err = PL_E_NOMEM;
    return NULL;
  }

  r->anchored = anchored;

  if (*body != '\0') { /* empty pattern is fine in PHP, pl_compile() would refuse it */
    r->re = pl_compile(body, flags, &e);
    if (r->re == NULL) {
      free(body);
      free(r);
      if (err != NULL) *err = e;
      return NULL;
    }
    r->ngroups = pl_ngroups(r->re);
  }

  free(body);

  if (err != NULL) *err = PL_OK;

  return r;
}

/* Free compiled pattern
 *
 */
PH7_PRIVATE void preg_re_free(preg_re *re) {
  if (re != NULL) {
    pl_free(re->re);
    free(re);
  }
}

/* Free all matches returned by preg_match_all()
 *
 */
PH7_PRIVATE void preg_all_free(preg_all_t *all) {
  if (all != NULL) {
    free(all->spans);
    free(all->counts);
    memset(all, 0, sizeof *all);
  }
}

/* Human-readable error code
 *
 */
PH7_PRIVATE const char *preg_strerror(int code) {
  switch (code) {
    case PL_OK:             return "no error";
    case PL_NO_MATCH:       return "no match";
    case PL_E_UNSUPPORTED:  return "unsupported regex construct";
    case PL_E_SYNTAX:       return "regex syntax error";
    case PL_E_NOMEM:        return "out of memory";
    case PL_E_LIMITS:       return "regex limits exceeded";
    case PL_E_ENGINE:       return "regex engine error";
    case PL_E_GROUPS:       return "unsupported group layout";
    case PREG_E_DELIMITER:  return "missing or invalid delimiter";
    case PREG_E_MODIFIER:   return "unsupported modifier";
    case PREG_E_OFFSET:     return "offset is out of range";
    case PREG_E_ARGS:       return "invalid argument";
    default:                return "unknown error";
  }
}


/* preg_match()
*/
PH7_PRIVATE int preg_match_re(const preg_re *re, const char *s, int len, preg_match_t *out, int flags, int offset) {

  pl_match m;
  int rc, i, n;

  if (re == NULL || s == NULL)
    return PREG_E_ARGS;

  if (len < 0)
    len = (int) strlen(s);

  rc = norm_offset(&offset, len);

  if (rc != PL_OK)
    return rc;

  rc = re_exec(re, s, len, offset, &m);

  if (rc == PL_NO_MATCH) {
    if (out != NULL)
      out->count = 0;
    return 0;
  }

  if (rc < 0)
    return rc;

  if (out != NULL) {
    n = visible_count(&m, flags);
    out->count = n;
    for (i = 0; i < n; i++)
      out->g[i] = m.g[i];
  }

  return 1;
}


/* preg_match_all()
*/
PH7_PRIVATE int preg_match_all_re(const preg_re *re, const char *s, int len, preg_all_t *all, int flags, int offset) {

  pl_match m;
  pl_span *sp = NULL;
  int *cnt = NULL;
  int cap = 0, n = 0, pos, rc, i;
  int ng, w;

  if (re == NULL || s == NULL)
    return PREG_E_ARGS;

  if (len < 0)
    len = (int) strlen(s);

  rc = norm_offset(&offset, len);

  if (rc != PL_OK)
    return rc;

  ng = re->ngroups;
  w = ng + 1;
  pos = offset;

  while (pos <= len) {

    rc = re_exec(re, s, len, pos, &m);

    if (rc == PL_NO_MATCH)
      break;

    if (rc < 0)
      goto fail;

    if (all != NULL) {

      if (n == cap) {

        int nc = cap ? cap * 2 : 8;
        pl_span *a = (pl_span *) realloc(sp, (size_t) nc * (size_t) w * sizeof *sp);
        int *b;

        if (a != NULL)
          sp = a;

        b = (int *) realloc(cnt, (size_t) nc * sizeof *cnt);

        if (b != NULL)
          cnt = b;

        if (a == NULL || b == NULL) {
          rc = PL_E_NOMEM;
          goto fail;
        }

        cap = nc;
      }

      for (i = 0; i < w; i++)
        sp[(size_t) n * (size_t) w + (size_t) i] = m.g[i];

      cnt[n] = visible_count(&m, flags);
    }

    n++;
    pos = next_pos(&m);
  }

  if (all != NULL) {
    all->nmatches = n;
    all->ngroups = ng;
    all->spans = sp;
    all->counts = cnt;
  }

  return n;

fail:
  free(sp);
  free(cnt);

  if (all != NULL)
    memset(all, 0, sizeof *all);

  return rc;
}



/* preg_replace_re(): precompiled preg_replace() variant
 *
 */
PH7_PRIVATE char *preg_replace_re(const preg_re *re, 
                                  const char *s, 
                                  int len, 
                                  const char *repl,
                                  int limit, 
                                  int *out_len, 
                                  int *count) {

  sbuf o;
  pl_match m;
  int pos = 0, last = 0, n = 0, rc;

  if (out_len != NULL) *out_len = 0;
  if (count != NULL) *count = 0;

  if (re == NULL || s == NULL || repl == NULL) {
    if (out_len != NULL) *out_len = PREG_E_ARGS;
    return NULL;
  }

  if (len < 0)
    len = (int) strlen(s);

  memset(&o, 0, sizeof o);
  sb_put(&o, "", 0); /* make sure o.p is allocated and NUL-terminated */

  while (pos <= len && (limit < 0 || n < limit)) {

    rc = re_exec(re, s, len, pos, &m);

    if (rc == PL_NO_MATCH)
      break;

    if (rc < 0) {
      free(o.p);
      if (out_len != NULL) *out_len = rc;
      return NULL;
    }

    sb_put(&o, s + last, (size_t) (m.g[0].start - last));
    expand2(&o, s, &m, repl);
    last = m.g[0].end;
    n++;
    pos = next_pos(&m);
  }

  sb_put(&o, s + last, (size_t) (len - last));

  if (o.oom) {
    free(o.p);
    if (out_len != NULL) *out_len = PL_E_NOMEM;
    return NULL;
  }

  if (out_len != NULL) *out_len = (int) o.len;
  if (count != NULL) *count = n;

  return o.p;
}



/* preg_split(): unlike pl_split(), empty matches DO split (preg_split('//', 'abc')
 * gives "", "a", "b", "c", "").
 */
PH7_PRIVATE int preg_split_re(const preg_re *re,
                              const char *s,
                              int len,
                              int limit,
                              int flags,
                              pl_span **out,
                              int *nout) {

  spans_t a;
  pl_match m;
  int pos = 0, last = 0, rc, i, gc;
  int no_empty = (flags & PREG_SPLIT_NO_EMPTY) != 0;

  if (re == NULL || s == NULL || out == NULL || nout == NULL)
    return PREG_E_ARGS;

  if (len < 0)
    len = (int) strlen(s);

  if (limit == 0)
    limit = -1;

  memset(&a, 0, sizeof a);

  while ((limit == -1 || limit > 1) && pos <= len) {

    rc = re_exec(re, s, len, pos, &m);

    if (rc == PL_NO_MATCH)
      break;

    if (rc < 0) {
      free(a.p);
      return rc;
    }

    if (!no_empty || m.g[0].start != last) {
      sp_push(&a, last, m.g[0].start);
      if (limit != -1)
        limit--;
    }

    if (flags & PREG_SPLIT_DELIM_CAPTURE) {

      gc = visible_count(&m, 0) - 1;

      for (i = 1; i <= gc; i++) {
        if (!no_empty || PREG_SPAN_LEN(m.g[i]) > 0)
          sp_push(&a, m.g[i].start, m.g[i].end);
      }
    }

    last = m.g[0].end;
    pos = next_pos(&m);
  }

  if (!no_empty || last < len)
    sp_push(&a, last, len);

  if (a.oom) {
    free(a.p);
    return PL_E_NOMEM;
  }

  *out = a.p;
  *nout = a.n;

  return PL_OK;
}


/* preg_grep()
*/
PH7_PRIVATE int preg_grep_re(const preg_re *re,
                             const char * const *subjects,
                             const int *lens,
                             int n,
                             int flags,
                             int **out_idx) {

  pl_match m;
  int *idx = NULL;
  int i, cnt = 0, rc, l, matched, invert = (flags & PREG_GREP_INVERT) != 0;

  if (out_idx != NULL)
    *out_idx = NULL;

  if (re == NULL || (n > 0 && subjects == NULL) || n < 0)
    return PREG_E_ARGS;

  if (out_idx != NULL && n > 0) {
    idx = (int *) malloc((size_t) n * sizeof *idx);
    if (idx == NULL)
      return PL_E_NOMEM;
  }

  for (i = 0; i < n; i++) {

    if (subjects[i] == NULL) {
      free(idx);
      return PREG_E_ARGS;
    }

    l = lens != NULL ? lens[i] : (int) strlen(subjects[i]);
    rc = re_exec(re, subjects[i], l, 0, &m);

    if (rc < 0 && rc != PL_NO_MATCH) {
      free(idx);
      return rc;
    }

    matched = (rc == PL_OK);

    if (matched != invert) {
      if (idx != NULL)
        idx[cnt] = i;
      cnt++;
    }
  }

  if (out_idx != NULL)
    *out_idx = idx;
  else
    free(idx);

  return cnt;
}


/* Pattern-string variants: compile/exec/free at once
*/
PH7_PRIVATE int preg_match(const char *pattern,
                           const char *s, 
                           int len, 
                           preg_match_t *m, 
                           int flags, 
                           int offset) {

  int err = PL_OK, rc;
  preg_re *re = preg_compile(pattern, &err);

  if (re == NULL)
    return err;

  rc = preg_match_re(re, s, len, m, flags, offset);
  preg_re_free(re);

  return rc;
}

/*
 *
 */
PH7_PRIVATE int preg_match_all( const char *pattern, 
                                const char *s, 
                                int len, 
                                preg_all_t *all, 
                                int flags, 
                                int offset) {

  int err = PL_OK, rc;
  preg_re *re = preg_compile(pattern, &err);

  if (re == NULL)
    return err;

  rc = preg_match_all_re(re, s, len, all, flags, offset);
  preg_re_free(re);

  return rc;
}

/*
 *
 */
PH7_PRIVATE char *preg_replace( const char *pattern,
                                const char *repl,
                                const char *s,
                                int len,
                                int limit,
                                int *out_len,
                                int *count) {

  int err = PL_OK;
  char *r;
  preg_re *re = preg_compile(pattern, &err);

  if (re == NULL) {
    if (out_len != NULL) *out_len = err;
    if (count != NULL) *count = 0;
    return NULL;
  }

  r = preg_replace_re(re, s, len, repl, limit, out_len, count);
  preg_re_free(re);

  return r;
}

/*
 *
 */
PH7_PRIVATE int preg_split( const char *pattern,
                            const char *s,
                            int len,
                            int limit,
                            int flags,
                            pl_span **out,
                            int *nout) {

  int err = PL_OK, rc;
  preg_re *re = preg_compile(pattern, &err);

  if (re == NULL)
    return err;

  rc = preg_split_re(re, s, len, limit, flags, out, nout);
  preg_re_free(re);

  return rc;
}


/*
 *
 */
PH7_PRIVATE int preg_grep(const char *pattern,
                          const char * const *subjects,
                          const int *lens,
                          int n,
                          int flags,
                          int **out_idx) {

  int err = PL_OK, rc;
  preg_re *re = preg_compile(pattern, &err);

  if (re == NULL) {
    if (out_idx != NULL) *out_idx = NULL;
    return err;
  }

  rc = preg_grep_re(re, subjects, lens, n, flags, out_idx);
  preg_re_free(re);

  return rc;
}
