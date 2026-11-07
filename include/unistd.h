#ifndef PAGH_UNISTD_H
#define PAGH_UNISTD_H
#include <stddef.h>
#include <stdint.h>
#include <pagh.h>
long read(int fd, void *buf, size_t n);
long write(int fd, const void *buf, size_t n);
int open(const char *path, int flags, ...);
int close(int fd);
long lseek(int fd, long off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int mkdir(const char *path, int mode);
int rename(const char *old, const char *new_);
int symlink(const char *target, const char *path);
int link(const char *old, const char *new_);
int truncate(const char *path, long len);
int ftruncate(int fd, long len);
int chmod(const char *path, int mode);
int getpid(void);
void _exit(int code) __attribute__((noreturn));
void exit(int code) __attribute__((noreturn));
long sysconf(int name);
typedef int pid_t;
pid_t fork(void);
int execv(const char *path, char *const argv[]);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t getppid(void);
#define WNOHANG 1
int chdir(const char *path);
char *getcwd(char *buf, unsigned long size);
#define ERANGE 34
#define _SC_PAGESIZE 30
#endif
