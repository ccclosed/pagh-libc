#ifndef PAGH_DIRENT_H
#define PAGH_DIRENT_H
#include <stdint.h>
#include <stddef.h>
#define DT_UNKNOWN 0
#define DT_REG 0
#define DT_DIR 1
#define DT_LNK 2
#define DT_OTHER 3
struct pagh_dirent {
    uint64_t d_ino;
    unsigned char d_type;
    unsigned char d_name_len;
    unsigned char pad[2];
    char d_name[];
};
typedef struct PaghDir DIR;
DIR *opendir(const char *path);
struct pagh_dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif
