#ifndef PAGH_GETOPT_H
#define PAGH_GETOPT_H
/* Command-line option parsing: POSIX `getopt` plus the long-option extension
 * (`getopt_long`, `getopt_long_only`). The behaviour is the one programs built
 * on a Unix libc expect, so an applet can share its option table with the host
 * build.
 *
 * `optind` starts at 1 (argv[0] is the program name) and may be set to 0 to
 * restart parsing; `optarg` is the argument of the option just returned,
 * `optopt` the option character that failed, and `opterr` enables the built-in
 * messages (a leading ':' in optstring silences them and makes a missing
 * argument return ':'). */

struct option {
    const char *name;   /* long name, without the leading "--" */
    int has_arg;        /* no_argument, required_argument or optional_argument */
    int *flag;          /* NULL: getopt_long returns val; else *flag = val, return 0 */
    int val;            /* value to return / store */
};

#define no_argument 0
#define required_argument 1
#define optional_argument 2

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);
int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex);
#endif