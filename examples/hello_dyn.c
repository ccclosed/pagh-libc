#include <stdio.h>
extern int extra_answer(void);
int main(int argc, char **argv, char **envp) {
    (void)envp;
    printf("hello from a dynamically linked C program\n");
    printf("argc=%d argv0=%s extra=%d\n", argc, argc ? argv[0] : "", extra_answer());
    return extra_answer() == 42 ? 0 : 1;
}
