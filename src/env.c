#include <stdlib.h>
#include <string.h>

char **environ;

void __pagh_set_environ(char **envp) { environ = envp; }

/* A small mutable environment layered on the kernel's read-only frame. */
#define ENV_MAX 32
static char *env_store[ENV_MAX];
static int env_count;

static int env_index(const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < env_count; i++)
        if (!strncmp(env_store[i], name, n) && env_store[i][n] == '=') return i;
    return -1;
}
char *getenv(const char *name) {
    int i = env_index(name);
    if (i >= 0) return strchr(env_store[i], '=') + 1;
    if (!environ) return 0;
    size_t n = strlen(name);
    for (char **p = environ; *p; p++)
        if (!strncmp(*p, name, n) && (*p)[n] == '=') return *p + n + 1;
    return 0;
}
int setenv(const char *name, const char *value, int overwrite) {
    int i = env_index(name);
    if (i >= 0 && !overwrite) return 0;
    size_t n = strlen(name), v = strlen(value);
    char *entry = malloc(n + v + 2);
    if (!entry) return -1;
    memcpy(entry, name, n); entry[n] = '='; memcpy(entry + n + 1, value, v + 1);
    if (i >= 0) env_store[i] = entry;
    else if (env_count < ENV_MAX) env_store[env_count++] = entry;
    else { free(entry); return -1; }
    return 0;
}
int unsetenv(const char *name) {
    int i = env_index(name);
    if (i < 0) return 0;
    for (int j = i; j + 1 < env_count; j++) env_store[j] = env_store[j + 1];
    env_count--;
    return 0;
}
