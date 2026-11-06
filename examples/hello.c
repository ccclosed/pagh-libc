#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv, char **envp) {
    (void)envp;
    printf("hello from C on pagh-riscv\n");
    printf("argc=%d argv0=%s pid=%d\n", argc, argc ? argv[0] : "", getpid());
    return 0;
}
