#ifndef PAGH_SYS_TYPES_H
#define PAGH_SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>

/* POSIX-shaped scalar types. `pid_t` lives in <unistd.h> (as on Linux) and
 * `off_t` is not modelled: lseek takes a `long`. */
typedef long ssize_t;
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

#endif
