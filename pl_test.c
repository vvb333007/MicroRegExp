/* LLM WARNING: Claude-generated test (fuzzing)
 *
 * Differential fuzz: random patterns over {a,b,c}. Whenever pcre lite accepts a pattern,
 * the result of pl_exec(start=0) is compared with POSIX regexec (glibc, ERE):
 *   - match / no match
 *   - match START (leftmost start is the same for leftmost-first and leftmost-longest engines)
 * Ends are not compared: greedy-backtracking and POSIX-longest legitimately differ.
 * Build like test.c (add -DPL_SLRE_PATCHED for the patched SLRE), then:
 * ./fuzz [iters] [allow_optional_quantifiers 0/1] [quantify_only_simple_atoms 0/1] [seed]
 */
#define _GNU_SOURCE
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regexp.h"
#include "regexp.c"

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(unsigned n) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned) (rs % n); }

static char *P, *E; /* pcre-ish pattern and equivalent POSIX ERE being built */
static int allow_q;
static int simple_only; /* quantify only single-character atoms, never groups */
static int last_was_group;

static void app(const char *pcre, const char *ere) { strcat(P, pcre); strcat(E, ere ? ere : pcre); }

static void gen_expr(int depth);

static void gen_atom(int depth) {
  switch (rnd(depth > 0 ? 9 : 6)) {
    case 0: case 1: app("a", 0); break;
    case 2: app("b", 0); break;
    case 3: app("c", 0); break;
    case 4: app("[ab]", 0); break;
    case 5: rnd(2) ? app(".", 0) : app("[^a]", 0); break;
    case 6: case 7: app("(", 0); gen_expr(depth - 1); app(")", 0); last_was_group = 1; return;
    default: app("(?:", "("); gen_expr(depth - 1); app(")", 0); last_was_group = 1; return;
  }
  last_was_group = 0;
}

static void gen_piece(int depth) {
  gen_atom(depth);
  if (simple_only && last_was_group) return;
  switch (rnd(allow_q ? 7 : 6)) {
    case 0: app("*", 0); break;
    case 1: app("+", 0); break;
    case 2: app("{2}", 0); break;
    case 3: allow_q ? app("{1,2}", 0) : (void) 0; break;
    case 6: app("?", 0); break;
    default: break;
  }
}

static void gen_expr(int depth) {
  int nb = 1 + (rnd(4) == 0), b, k, np;
  for (b = 0; b < nb; b++) {
    if (b) app("|", 0);
    np = 1 + (int) rnd(3);
    for (k = 0; k < np; k++) gen_piece(depth);
  }
}

int main(int argc, char **argv) {
  long iters = argc > 1 ? atol(argv[1]) : 100000, it;
  long accepted = 0, rejected = 0, mism = 0, engine_err = 0, shown = 0;
  long rej_by[8] = {0};
  P = calloc(1, 4096); E = calloc(1, 4096);
  allow_q = argc > 2 ? atoi(argv[2]) : 1;
  simple_only = argc > 3 ? atoi(argv[3]) : 0;
  if (argc > 4) rs ^= (unsigned long long) atol(argv[4]) * 0x9E3779B97F4A7C15ULL;

  for (it = 0; it < iters; it++) {
    pl_regex *re; regex_t rx; int err = 0, s, ok;
    P[0] = E[0] = 0;
    gen_expr(2);
    re = pl_compile(P, 0, &err);
    if (!re) { rejected++; if (err <= -100 && err >= -105) rej_by[-err - 100]++; continue; }
    accepted++;
    if (regcomp(&rx, E, REG_EXTENDED)) { pl_free(re); continue; }
    for (s = 0; s < 12; s++) {
      char subj[16]; int len = (int) rnd(7), i, r; regmatch_t pm[1]; pl_match m; int posix;
      for (i = 0; i < len; i++) subj[i] = (char) ('a' + rnd(3));
      subj[len] = 0;
      pm[0].rm_so = 0; pm[0].rm_eo = len;
      posix = regexec(&rx, subj, 1, pm, REG_STARTEND) == 0;
      r = pl_exec(re, subj, len, 0, &m);
      ok = 1;
      if (r == PL_E_ENGINE) { engine_err++; ok = 0; }
      else if (posix != (r == PL_OK)) ok = 0;
      else if (posix && pm[0].rm_so != m.g[0].start) ok = 0;
      if (!ok) {
        mism++;
        if (shown < 25) {
          shown++;
          printf("MISMATCH  /%s/ on \"%s\"  posix=%d(%d) pl=%d(%d)  slre=%s\n", P, subj, posix,
                 posix ? (int) pm[0].rm_so : -1, r, r == PL_OK ? m.g[0].start : -1, pl_slre_pattern(re));
        }
        break;
      }
    }
    regfree(&rx);
    pl_free(re);
  }
  printf("iters=%ld accepted=%ld rejected=%ld (unsupported=%ld syntax=%ld limits=%ld groups=%ld) "
         "mismatching_patterns=%ld (of which slre engine errors=%ld)\n",
         iters, accepted, rejected, rej_by[0], rej_by[1], rej_by[3], rej_by[5], mism, engine_err);
  return 0;
}
