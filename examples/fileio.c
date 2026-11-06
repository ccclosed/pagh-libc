#include <stdio.h>
#include <string.h>
int main(void) {
    FILE *f = fopen("/mnt/cfile.txt", "w");
    if (!f) { printf("fopen w failed\n"); return 1; }
    fprintf(f, "line one\nline two\n");
    fclose(f);
    f = fopen("/mnt/cfile.txt", "r");
    if (!f) { printf("fopen r failed\n"); return 1; }
    char buf[64];
    while (fgets(buf, sizeof buf, f)) printf("%s", buf);
    fclose(f);
    return 0;
}
