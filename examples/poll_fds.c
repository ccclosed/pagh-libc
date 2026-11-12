/* poll(2)/ppoll(2), pipe2(2), dup/dup2(2), sigaltstack(2) and set_tid_address(2)
 * in one live use: the child writes a line through a pipe after dup2'ing the
 * write end onto stdout; the parent redirects the read end onto stdin (fd 0) and
 * waits for it with poll() instead of a usleep/sched_yield loop.
 *
 *   run /mnt/poll_fds
 *
 * What each line proves:
 *   set_tid_address  -> the kernel answered the calling thread's id;
 *   sigaltstack      -> a 24-byte stack_t round-trips (8 KiB, then SS_DISABLE);
 *   poll             -> fp 0 (a pipe after dup2) reported POLLIN and read got
 *                       the child's payload;
 *   ppoll            -> after the child exited the same fd reports POLLHUP and
 *                       read() returns 0 (EOF), with a 100 ms timespec;
 *   dup/dup2/pipe2   -> the descriptors were duplicated/redirected, not faked.
 *
 * tools/live-sockets.py runs this as a regression and requires "poll_fds: ok"
 * plus "exited with code 0". */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ALT_STACK_SIZE 8192
static unsigned char alt_stack[ALT_STACK_SIZE];

static int fail(const char *what) {
    printf("poll_fds: FAIL %s (errno=%d)\n", what, errno);
    return 1;
}

int main(void) {
    int tid_word = 0;
    int tid = set_tid_address(&tid_word);
    if (tid <= 0) return fail("set_tid_address");
    printf("poll_fds: set_tid_address -> tid %d\n", tid);

    stack_t ss, old;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    if (sigaltstack(&ss, 0) < 0) return fail("sigaltstack install");
    if (sigaltstack(0, &old) < 0) return fail("sigaltstack query");
    if (old.ss_sp != alt_stack || old.ss_size != sizeof alt_stack) {
        printf("poll_fds: altstack readback sp=%p size=%lu\n",
               (void *)old.ss_sp, (unsigned long)old.ss_size);
        return 1;
    }
    stack_t off;
    memset(&off, 0, sizeof off);
    off.ss_flags = SS_DISABLE;
    if (sigaltstack(&off, 0) < 0) return fail("sigaltstack disable");
    printf("poll_fds: sigaltstack -> %lu-byte alternate stack accepted\n",
           (unsigned long)old.ss_size);

    /* pipe() is the flags-0 spelling of pipe2(). */
    int spare[2];
    if (pipe(spare) < 0) return fail("pipe");
    close(spare[0]);
    close(spare[1]);

    int fds[2];
    if (pipe2(fds, 0) < 0) return fail("pipe2");
    pid_t child = fork();
    if (child < 0) return fail("fork");
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0) _exit(3); /* stdout -> pipe write end */
        close(fds[1]);
        printf("poll_fds: payload from the child\n");
        exit(0); /* POSIX exit: runs atexit handlers and ends the process */
    }
    close(fds[1]);
    if (dup2(fds[0], 0) < 0) return fail("dup2"); /* stdin <- pipe read end */
    close(fds[0]);

    int copy = dup(0);
    if (copy < 0) return fail("dup");
    printf("poll_fds: dup(0) -> fd %d\n", copy);
    close(copy);

    struct pollfd p;
    p.fd = 0;
    p.events = POLLIN;
    p.revents = 0;
    int ready = poll(&p, 1, 5000);
    if (ready != 1 || !(p.revents & POLLIN)) {
        printf("poll_fds: poll ready=%d revents=0x%x\n", ready, p.revents);
        return 1;
    }
    char line[64];
    long got = read(0, line, sizeof line - 1);
    if (got <= 0) return fail("read");
    line[got] = 0;
    if (strstr(line, "payload from the child") == 0) {
        printf("poll_fds: unexpected payload: %s", line);
        return 1;
    }
    printf("poll_fds: poll -> POLLIN, read %ld bytes\n", got);

    /* Wait for the child first, so "every write end is closed" is a fact and
     * not a race with the child's own exit: the next poll must see POLLHUP. */
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return fail("waitpid");
    if (status != 0) {
        printf("poll_fds: child status=%d\n", status);
        return 1;
    }
    struct pollfd e;
    e.fd = 0;
    e.events = POLLIN;
    e.revents = 0;
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 100000000; /* 100 ms */
    ready = ppoll(&e, 1, &timeout, 0);
    if (ready != 1 || !(e.revents & POLLHUP)) {
        printf("poll_fds: ppoll eof ready=%d revents=0x%x\n", ready, e.revents);
        return 1;
    }
    if (read(0, line, sizeof line) != 0) return fail("read at EOF");
    printf("poll_fds: ppoll -> POLLHUP, EOF\n");
    printf("poll_fds: ok\n");
    return 0;
}