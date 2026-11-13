/* getopt(3), getopt_long(3) and getopt_long_only(3).
 *
 * This follows the implementation every Unix program is written against:
 * by default options and operands may be interleaved (getopt permutes argv
 * so all options are scanned), a leading '+' in optstring makes scanning
 * stop at the first operand (POSIX behaviour, also selected by the
 * POSIXLY_CORRECT environment variable), and a leading '-' returns each
 * operand as option 1. A leading ':' suppresses the diagnostic messages and
 * makes a missing argument return ':' instead of '?'.
 *
 * The file is deliberately dependency-light (`fprintf`/`stderr` for the
 * messages only) so the host test in tests/getopt_test.c can build it on the
 * build machine and compare every call against the host libc's getopt. */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

enum ordering {
    REQUIRE_ORDER, /* stop at the first operand: optstring[0] == '+' */
    PERMUTE,       /* scan the whole argv, exchange operands to the end */
    RETURN_IN_ORDER /* return every operand as option 1: optstring[0] == '-' */
};

static int ordering;
static int initialized;
static char *nextchar;     /* rest of the current short-option cluster */
static int first_nonopt;   /* first argv element of the pending operand run */
static int last_nonopt;    /* one past the last argv element of that run */

/* An argv element that is not an option. A lone "-" is an operand, not a
 * (nameless) option. */
static int nonoption(const char *arg) {
    return arg[0] != '-' || arg[1] == '\0';
}

/* Exchange the two adjacent blocks [first_nonopt, last_nonopt) (operands) and
 * [last_nonopt, optind) (options) so the options are scanned left to right and
 * the operands keep their relative order at the end. Greedy block swapping:
 * the shorter block moves past the longer one until they line up. */
static void exchange(char **argv) {
    int bottom = first_nonopt;
    int middle = last_nonopt;
    int top = optind;
    while (top > middle && middle > bottom) {
        int i;
        if (top - middle > middle - bottom) {
            int len = middle - bottom;
            for (i = 0; i < len; i++) {
                char *tmp = argv[bottom + i];
                argv[bottom + i] = argv[top - (middle - bottom) + i];
                argv[top - (middle - bottom) + i] = tmp;
            }
            top -= len;
        } else {
            int len = top - middle;
            for (i = 0; i < len; i++) {
                char *tmp = argv[bottom + i];
                argv[bottom + i] = argv[middle + i];
                argv[middle + i] = tmp;
            }
            bottom += len;
        }
    }
    first_nonopt += optind - last_nonopt;
    last_nonopt = optind;
}

/* (Re)start scanning at `optind`, deciding the ordering from optstring and the
 * environment. Called on the first call and whenever optind is set to 0 (the
 * portable way to restart; GNU getopt documents the same reset). */
static void initialize(const char *optstring) {
    if (optind == 0) optind = 1;
    if (optstring[0] == '-') ordering = RETURN_IN_ORDER;
    else if (optstring[0] == '+') ordering = REQUIRE_ORDER;
    else if (getenv("POSIXLY_CORRECT") != NULL) ordering = REQUIRE_ORDER;
    else ordering = PERMUTE;
    first_nonopt = last_nonopt = optind;
    nextchar = NULL;
    initialized = 1;
}

static int getopt_internal(int argc, char **argv, const char *optstring,
                           const struct option *longopts, int *longindex,
                           int long_only) {
    char *nameend;
    const struct option *p;
    const struct option *pfound;
    int exact, ambig, indfound, option_index;
    char c;
    const char *temp;

    if (argc < 1) return -1;
    if (!initialized || optind == 0) initialize(optstring);

    optarg = NULL;

    if (nextchar == NULL || *nextchar == '\0') {
        /* Advance to the next argv element, exchanging the operand run that
         * was collected since the last option. */
        if (last_nonopt > optind) last_nonopt = optind;
        if (first_nonopt > optind) first_nonopt = optind;
        if (ordering == PERMUTE) {
            if (first_nonopt != last_nonopt && last_nonopt != optind)
                exchange(argv);
            else if (last_nonopt != optind)
                first_nonopt = optind;
            while (optind < argc && nonoption(argv[optind])) optind++;
            last_nonopt = optind;
        }

        /* "--" ends option scanning; everything after it is an operand. */
        if (optind != argc && strcmp(argv[optind], "--") == 0) {
            optind++;
            if (first_nonopt != last_nonopt && last_nonopt != optind)
                exchange(argv);
            else if (first_nonopt == last_nonopt)
                first_nonopt = optind;
            last_nonopt = argc;
            optind = argc;
        }

        if (optind == argc) {
            if (first_nonopt != last_nonopt) optind = first_nonopt;
            return -1;
        }

        if (nonoption(argv[optind])) {
            if (ordering == REQUIRE_ORDER) return -1;
            optarg = argv[optind++];
            return 1;
        }

        /* Start decoding: short options begin after '-', long ones after
         * '--'. getopt_long_only also accepts a single '-' before a long
         * name. */
        nextchar = argv[optind] + 1;
        if (longopts != NULL && argv[optind][1] == '-') nextchar++;
    }

    if (longopts != NULL &&
        (argv[optind][1] == '-' ||
         (long_only && (argv[optind][2] || strchr(optstring, argv[optind][1]) == NULL)))) {
        nameend = nextchar;
        while (*nameend && *nameend != '=') nameend++;

        pfound = NULL;
        exact = 0;
        ambig = 0;
        indfound = -1;
        for (p = longopts, option_index = 0; p->name; p++, option_index++) {
            if (strncmp(p->name, nextchar, (size_t)(nameend - nextchar)) != 0) continue;
            if ((size_t)(nameend - nextchar) == strlen(p->name)) {
                /* Exact match: an abbreviation cannot beat it. */
                pfound = p;
                indfound = option_index;
                exact = 1;
                break;
            }
            if (pfound == NULL) {
                pfound = p;
                indfound = option_index;
            } else if (long_only || pfound->has_arg != p->has_arg ||
                       pfound->flag != p->flag || pfound->val != p->val) {
                ambig = 1;
            }
        }

        if (ambig && !exact) {
            if (opterr)
                fprintf(stderr, "%s: option '--%s' is ambiguous\n", argv[0], nextchar);
            nextchar += strlen(nextchar);
            optind++;
            optopt = 0;
            return '?';
        }

        if (pfound != NULL) {
            option_index = indfound;
            optind++;
            if (*nameend) {
                if (pfound->has_arg) {
                    optarg = nameend + 1;
                } else {
                    if (opterr)
                        fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                                argv[0], pfound->name);
                    nextchar += strlen(nextchar);
                    optopt = pfound->val;
                    return '?';
                }
            } else if (pfound->has_arg == required_argument) {
                if (optind < argc) {
                    optarg = argv[optind++];
                } else {
                    if (opterr)
                        fprintf(stderr, "%s: option '--%s' requires an argument\n",
                                argv[0], pfound->name);
                    nextchar += strlen(nextchar);
                    optopt = pfound->val;
                    return optstring[0] == ':' ? ':' : '?';
                }
            }
            nextchar += strlen(nextchar);
            if (longindex != NULL) *longindex = option_index;
            if (pfound->flag) {
                *pfound->flag = pfound->val;
                return 0;
            }
            return pfound->val;
        }

        /* Not a known long option. getopt_long_only may still interpret it as
         * a short option when its first character is one. */
        if (!long_only || argv[optind][1] == '-' || strchr(optstring, *nextchar) == NULL) {
            if (opterr) {
                if (argv[optind][1] == '-')
                    fprintf(stderr, "%s: unrecognized option '--%s'\n", argv[0], nextchar);
                else
                    fprintf(stderr, "%s: unrecognized option '-%s'\n", argv[0], nextchar);
            }
            nextchar = (char *)"";
            optind++;
            optopt = 0;
            return '?';
        }
        /* fall through to the short-option path */
    }

    c = *nextchar++;
    temp = strchr(optstring, c);

    /* The last character of a cluster finishes the argv element. */
    if (*nextchar == '\0') optind++;

    if (temp == NULL || c == ':' || c == ';') {
        if (opterr && optstring[0] != ':')
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        optopt = c;
        return '?';
    }
    if (temp[1] == ':') {
        if (temp[2] == ':') {
            /* Optional argument: only an attached one counts. */
            if (*nextchar != '\0') {
                optarg = nextchar;
                optind++;
            }
            nextchar = NULL;
        } else {
            if (*nextchar != '\0') {
                optarg = nextchar;
                optind++;
            } else if (optind == argc) {
                if (opterr && optstring[0] != ':')
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
                optopt = c;
                return optstring[0] == ':' ? ':' : '?';
            } else {
                optarg = argv[optind++];
            }
            nextchar = NULL;
        }
    }
    return (int)(unsigned char)c;
}

int getopt(int argc, char *const argv[], const char *optstring) {
    return getopt_internal(argc, (char **)argv, optstring, NULL, NULL, 0);
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    return getopt_internal(argc, (char **)argv, optstring, longopts, longindex, 0);
}

int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex) {
    return getopt_internal(argc, (char **)argv, optstring, longopts, longindex, 1);
}
