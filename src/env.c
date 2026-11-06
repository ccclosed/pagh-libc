#include <stdlib.h>
#include <string.h>

char **environ;

char *getenv(const char *name) {
    if (!environ) return 0;
    size_t n = strlen(name);
    for (char **p = environ; *p; p++) {
        if (!strncmp(*p, name, n) && (*p)[n] == '=') return *p + n + 1;
    }
    return 0;
}
void __pagh_set_environ(char **envp) { environ = envp; }
