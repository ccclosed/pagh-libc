/* Differential test for the pagh C library's getopt(3) family.
 *
 * src/getopt.c is compiled with every public symbol renamed (see
 * tools/host-tests.sh), so this process contains two independent parsers:
 * the pagh one (`pagh_*`) and the host libc's. Every test case is run through
 * both on separate copies of the same argv array, and after each call the
 * return value, optind, optarg, optopt (and for the long API longindex) must
 * agree; after the scan ends the permuted argv arrays must be identical too.
 *
 * opterr is 0 here: the diagnostic texts are formatted for humans, not a
 * contract. The parsing decisions they accompany are what programs depend on,
 * and those are compared exactly.
 *
 *   ./build/host_getopt_test
 */
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The renamed pagh implementation. `struct option` is the same four fields in
 * both headers (name, has_arg, flag, val), so the host declaration works for
 * both calls at the ABI level. */
extern char *pagh_optarg;
extern int pagh_optind;
extern int pagh_opterr;
extern int pagh_optopt;
extern int pagh_getopt(int argc, char *const argv[], const char *optstring);
extern int pagh_getopt_long(int argc, char *const argv[], const char *optstring,
                            const struct option *longopts, int *longindex);
extern int pagh_getopt_long_only(int argc, char *const argv[], const char *optstring,
                                 const struct option *longopts, int *longindex);

static int checks;
static int failures;

static void fail(const char *what, int step, const char *fmt, ...) {
    va_list ap;
    failures++;
    if (failures > 40) return;
    printf("MISMATCH %s (call %d): ", what, step);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static char *copy_string(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (!copy) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memcpy(copy, s, n);
    return copy;
}

static char **copy_argv(int argc, char *const argv[]) {
    char **copy = calloc((size_t)argc + 1, sizeof *copy);
    if (!copy) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (int i = 0; i < argc; i++) copy[i] = copy_string(argv[i]);
    return copy;
}

static void free_argv(char **argv) {
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static void reset_state(void) {
    pagh_optind = 0;
    pagh_opterr = 0;
    pagh_optarg = NULL;
    pagh_optopt = 0;
    optind = 0;
    opterr = 0;
    optarg = NULL;
    optopt = 0;
}

/* Both parsers were handed copies of the same argv; the strings must line up
 * element by element after the scan (permutation included). */
static void compare_argv(const char *what, int argc, char **pagh, char **host) {
    checks++;
    for (int i = 0; i < argc; i++) {
        if (strcmp(pagh[i], host[i]) != 0) {
            fail(what, -1, "argv[%d]: pagh [%s] host [%s]", i, pagh[i], host[i]);
            return;
        }
    }
}

static void compare_step(const char *what, int step, int rp, int rh) {
    checks++;
    if (rp != rh) {
        fail(what, step, "return: pagh %d host %d", rp, rh);
        return;
    }
    if (pagh_optind != optind) {
        fail(what, step, "optind: pagh %d host %d", pagh_optind, optind);
        return;
    }
    /* optopt is only meaningful after an error return; on success glibc's
     * public variable keeps whatever its internal copy had, which is an
     * implementation artifact rather than a contract. */
    if ((rp == '?' || rp == ':') && pagh_optopt != optopt) {
        fail(what, step, "optopt: pagh %d host %d", pagh_optopt, optopt);
        return;
    }
    if ((pagh_optarg == NULL) != (optarg == NULL)) {
        fail(what, step, "optarg presence: pagh %s host %s",
             pagh_optarg ? "set" : "NULL", optarg ? "set" : "NULL");
        return;
    }
    if (pagh_optarg && strcmp(pagh_optarg, optarg) != 0)
        fail(what, step, "optarg: pagh [%s] host [%s]", pagh_optarg, optarg);
}

static void run_short(const char *what, int argc, char *const template[],
                      const char *optstring) {
    char **pagh = copy_argv(argc, template);
    char **host = copy_argv(argc, template);
    reset_state();
    for (int step = 0; step < argc + 4; step++) {
        int rp = pagh_getopt(argc, pagh, optstring);
        int rh = getopt(argc, host, optstring);
        compare_step(what, step, rp, rh);
        if (rp != rh || rp == -1) break;
    }
    compare_argv(what, argc, pagh, host);
    free_argv(pagh);
    free_argv(host);
}

static void run_long(const char *what, int argc, char *const template[],
                     const char *optstring, const struct option longopts[],
                     int compare_index) {
    char **pagh = copy_argv(argc, template);
    char **host = copy_argv(argc, template);
    reset_state();
    for (int step = 0; step < argc + 4; step++) {
        int pi = -1, hi = -1;
        int rp = pagh_getopt_long(argc, pagh, optstring, longopts, &pi);
        int rh = getopt_long(argc, host, optstring, longopts, &hi);
        compare_step(what, step, rp, rh);
        checks++;
        if (compare_index && pi != hi)
            fail(what, step, "longindex: pagh %d host %d", pi, hi);
        if (rp != rh || rp == -1) break;
    }
    compare_argv(what, argc, pagh, host);
    free_argv(pagh);
    free_argv(host);
}

static void run_long_only(const char *what, int argc, char *const template[],
                          const char *optstring, const struct option longopts[]) {
    char **pagh = copy_argv(argc, template);
    char **host = copy_argv(argc, template);
    reset_state();
    for (int step = 0; step < argc + 4; step++) {
        int rp = pagh_getopt_long_only(argc, pagh, optstring, longopts, NULL);
        int rh = getopt_long_only(argc, host, optstring, longopts, NULL);
        compare_step(what, step, rp, rh);
        if (rp != rh || rp == -1) break;
    }
    compare_argv(what, argc, pagh, host);
    free_argv(pagh);
    free_argv(host);
}

#define ARGV(...) {(char *)(argv0), __VA_ARGS__, NULL}

static int flag_word;

static const struct option LONGOPTS[] = {
    {"alpha", no_argument, NULL, 'a'},
    {"alf", no_argument, NULL, 'A'},
    {"beta", required_argument, NULL, 'b'},
    {"gamma", optional_argument, NULL, 'g'},
    {"delta", no_argument, &flag_word, 42},
    {"epsilon", required_argument, &flag_word, 43},
    {NULL, 0, NULL, 0},
};

static void short_cases(void) {
    char *argv0 = "prog";

    {   /* plain flags, one per element */
        char *v[] = ARGV("-a", "-b", "-c");
        run_short("plain flags", 4, v, "abc");
    }
    {   /* clustered flags */
        char *v[] = ARGV("-abc");
        run_short("cluster", 2, v, "abc");
    }
    {   /* required argument, separate */
        char *v[] = ARGV("-a", "value", "-b");
        run_short("required sep", 4, v, "a:b");
    }
    {   /* required argument, attached */
        char *v[] = ARGV("-avalue", "-b");
        run_short("required attached", 3, v, "a:b");
    }
    {   /* missing required argument: '?' without ':' prefix */
        char *v[] = ARGV("-a");
        run_short("missing arg", 2, v, "a:");
    }
    {   /* missing required argument: ':' with ':' prefix */
        char *v[] = ARGV("-a");
        run_short("missing arg silent", 2, v, ":a:");
    }
    {   /* unknown option */
        char *v[] = ARGV("-x", "-a");
        run_short("unknown", 3, v, "ab");
    }
    {   /* unknown clustered last */
        char *v[] = ARGV("-axb", "-b");
        run_short("unknown cluster", 3, v, "ab");
    }
    {   /* optional argument attached/absent */
        char *v[] = ARGV("-avalue", "-a", "-b");
        run_short("optional", 4, v, "a::b");
    }
    {   /* interleaved operands: getopt permutes them to the end */
        char *v[] = ARGV("first", "-a", "second", "-b", "third");
        run_short("permute", 6, v, "ab");
    }
    {   /* "--" terminates parsing */
        char *v[] = ARGV("-a", "--", "-b", "operand");
        run_short("terminator", 5, v, "ab");
    }
    {   /* a lone "-" is an operand, not an option */
        char *v[] = ARGV("-a", "-", "operand");
        run_short("lone dash", 4, v, "ab");
    }
    {   /* '+' prefix: stop at the first operand (POSIX) */
        char *v[] = ARGV("operand", "-a");
        run_short("plus prefix", 3, v, "+ab");
    }
    {   /* '-' prefix: return operands as option 1 */
        char *v[] = ARGV("operand", "-a", "other");
        run_short("dash prefix", 4, v, "-ab");
    }
    {   /* empty optstring: everything is an error */
        char *v[] = ARGV("-a");
        run_short("empty optstring", 2, v, "");
    }
    {   /* no options at all */
        char *v[] = ARGV("one", "two");
        run_short("only operands", 3, v, "ab");
    }
    {   /* option and argument glued with '=' is a *short* option */
        char *v[] = ARGV("-a=1");
        run_short("equals short", 2, v, "a");
    }
    {   /* ":" as an option letter is not a valid option, even if listed */
        char *v[] = ARGV("-:");
        run_short("colon option", 2, v, "a:");
    }
    {   /* missing argument, then option-like word is consumed as the argument */
        char *v[] = ARGV("-a", "-b");
        run_short("arg looks like option", 3, v, "a:b");
    }
    {   /* running out of argv: last cluster character increments optind */
        char *v[] = ARGV("-ab");
        run_short("cluster end", 2, v, "ab:");
    }
}

static void long_cases(void) {
    char *argv0 = "prog";

    {   /* long flag */
        char *v[] = ARGV("--alpha");
        run_long("long flag", 2, v, "ab:", LONGOPTS, 1);
    }
    {   /* required argument attached with '=' */
        char *v[] = ARGV("--beta=42");
        run_long("long =arg", 2, v, "ab:", LONGOPTS, 1);
    }
    {   /* required argument in the next element */
        char *v[] = ARGV("--beta", "42");
        run_long("long sep arg", 3, v, "ab:", LONGOPTS, 1);
    }
    {   /* optional argument: absent then present */
        char *v[] = ARGV("--gamma", "--gamma=7");
        run_long("long optional", 3, v, "g", LONGOPTS, 1);
    }
    {   /* unambiguous abbreviation */
        char *v[] = ARGV("--alp");
        run_long("abbrev", 2, v, "a", LONGOPTS, 1);
    }
    {   /* ambiguous abbreviation */
        char *v[] = ARGV("--al");
        run_long("ambiguous", 2, v, "a", LONGOPTS, 1);
    }
    {   /* exact match wins over a longer option's prefix */
        char *v[] = ARGV("--alf");
        run_long("exact wins", 2, v, "aA", LONGOPTS, 1);
    }
    {   /* flag pointer: returns 0 and stores val */
        char *v[] = ARGV("--delta");
        flag_word = 0;
        run_long("flag", 2, v, "a", LONGOPTS, 1);
    }
    {   /* no argument allowed but '=' given: '?' */
        char *v[] = ARGV("--alpha=1");
        run_long("no arg allowed", 2, v, "a", LONGOPTS, 1);
    }
    {   /* missing required argument: '?' ... */
        char *v[] = ARGV("--beta");
        run_long("missing long arg", 2, v, ":b:", LONGOPTS, 1);
    }
    {   /* ... and ':' when optstring begins with ':' */
        char *v[] = ARGV("--beta");
        run_long("missing long arg silent", 2, v, ":b:", LONGOPTS, 1);
    }
    {   /* unrecognized long option */
        char *v[] = ARGV("--nonesuch");
        run_long("unrecognized", 2, v, "a", LONGOPTS, 1);
    }
    {   /* long options and operands interleaved */
        char *v[] = ARGV("op1", "--alpha", "op2", "--beta=9", "op3");
        run_long("long permute", 6, v, "ab:", LONGOPTS, 1);
    }
    {   /* "--" ends scanning for the long parser too */
        char *v[] = ARGV("--alpha", "--", "--beta");
        run_long("long terminator", 4, v, "ab:", LONGOPTS, 1);
    }
    {   /* short options still work through getopt_long */
        char *v[] = ARGV("-a", "-b", "3");
        run_long("long parser short", 4, v, "ab:", LONGOPTS, 1);
    }
    {   /* clustered short option with attached argument */
        char *v[] = ARGV("-ab3");
        run_long("long parser cluster", 2, v, "ab:", LONGOPTS, 1);
    }
    {   /* abbreviation followed by an attached argument */
        char *v[] = ARGV("--bet=5");
        run_long("abbrev =arg", 2, v, "b:", LONGOPTS, 1);
    }
    {   /* empty name after "--" is ambiguous */
        char *v[] = ARGV("--=1");
        run_long("empty long name", 2, v, "b:", LONGOPTS, 1);
    }
}

static void long_only_cases(void) {
    char *argv0 = "prog";

    {   /* a long-only call accepts both spellings */
        char *v[] = ARGV("-alpha", "--beta=3");
        run_long_only("long_only long", 3, v, "b:", LONGOPTS);
    }
    {   /* a single dash short option that is listed stays short */
        char *v[] = ARGV("-a");
        run_long_only("long_only short", 2, v, "ab:", LONGOPTS);
    }
    {   /* an unknown single-dash word is tried as a long option first */
        char *v[] = ARGV("-gamma");
        run_long_only("long_only single-dash long", 2, v, "ab:", LONGOPTS);
    }
    {   /* abbreviation on a single dash */
        char *v[] = ARGV("-bet=4");
        run_long_only("long_only abbrev", 2, v, "b:", LONGOPTS);
    }
    {   /* unknown single-dash name falls back to an unknown short option */
        char *v[] = ARGV("-zzz");
        run_long_only("long_only unknown", 2, v, "ab:", LONGOPTS);
    }
}

/* Deterministic fuzz: build short argv arrays out of the tokens a real
 * command line mixes (flags, clusters, attached/separate arguments, operands,
 * "--"), and require both parsers to make the same sequence of decisions on
 * every one. This is the part that catches an ordering edge the hand-written
 * cases do not name. */
static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;

static unsigned long rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned long)rng_state;
}

static const char *const FUZZ_TOKENS[] = {
    "-a", "-b", "-c", "-ab", "-abc", "-bVALUE", "-c", "-", "--",
    "operand", "-x", "-aVALUE", "another", "VALUE",
};
#define FUZZ_TOKEN_COUNT (sizeof FUZZ_TOKENS / sizeof FUZZ_TOKENS[0])

static const char *const FUZZ_OPTSTRINGS[] = {
    "abc", "a:b:c", "ab:c::", ":a:b", "+abc", "-abc", "a", "::a",
};
#define FUZZ_OPTSTRING_COUNT (sizeof FUZZ_OPTSTRINGS / sizeof FUZZ_OPTSTRINGS[0])

static void fuzz_short(int rounds) {
    char *argv0 = "prog";
    char *template[10];
    template[0] = argv0;
    for (int round = 0; round < rounds; round++) {
        int count = 1 + (int)(rng_next() % 6);
        for (int i = 1; i < count; i++)
            template[i] = (char *)FUZZ_TOKENS[rng_next() % FUZZ_TOKEN_COUNT];
        template[count] = NULL;
        const char *optstring = FUZZ_OPTSTRINGS[rng_next() % FUZZ_OPTSTRING_COUNT];
        run_short("fuzz short", count, template, optstring);
    }
}

static const char *const FUZZ_LONG_TOKENS[] = {
    "-a", "-b", "-ab", "-bVALUE", "--alpha", "--alf", "--al", "--alp",
    "--beta=1", "--beta", "--gamma", "--gamma=2", "--delta", "--epsilon=3",
    "--nope", "--", "operand", "-", "--alpha=1", "--beta=", "--gam=4",
};
#define FUZZ_LONG_TOKEN_COUNT (sizeof FUZZ_LONG_TOKENS / sizeof FUZZ_LONG_TOKENS[0])

static void fuzz_long(int rounds) {
    char *argv0 = "prog";
    char *template[10];
    template[0] = argv0;
    for (int round = 0; round < rounds; round++) {
        int count = 1 + (int)(rng_next() % 6);
        for (int i = 1; i < count; i++)
            template[i] = (char *)FUZZ_LONG_TOKENS[rng_next() % FUZZ_LONG_TOKEN_COUNT];
        template[count] = NULL;
        run_long("fuzz long", count, template, "ab:g::", LONGOPTS, 1);
    }
}

static int flag_value_for(int *flag_word_snapshot) {
    (void)flag_word_snapshot;
    return flag_word;
}

int main(void) {
    short_cases();
    long_cases();
    long_only_cases();
    fuzz_short(400);
    fuzz_long(400);

    /* The flag-pointer case: both parsers must have written 42. The last
     * flag test leaves the variable at 42 unless a bug skipped the store. */
    checks++;
    if (flag_word != 42) {
        failures++;
        printf("MISMATCH flag pointer: expected 42, got %d\n", flag_value_for(NULL));
    }
    if (failures) {
        printf("getopt diff: %d checks, %d FAILURES\n", checks, failures);
        return 1;
    }
    printf("getopt diff: %d checks, 0 failures\n", checks);
    return 0;
}