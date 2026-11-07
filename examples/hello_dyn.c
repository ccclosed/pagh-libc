#include <stdio.h>
int main(int argc, char **argv, char **envp) {
    (void)envp;
    printf("hello from a dynamically linked C program\n");
    printf("argc=%d argv0=%s\n", argc, argc ? argv[0] : "");
    return 0;
}
