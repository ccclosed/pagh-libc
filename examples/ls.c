#include <stdio.h>
#include <dirent.h>
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) { fdprintf(2, "ls: cannot open %s\n", path); return 1; }
    struct pagh_dirent *e;
    while ((e = readdir(d))) {
        printf("%.*s%s\n", (int)e->d_name_len, e->d_name, e->d_type == DT_DIR ? "/" : "");
    }
    closedir(d);
    return 0;
}
