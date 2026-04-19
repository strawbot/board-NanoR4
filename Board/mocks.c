#include "ttypes.h"
#include "tea.h"
#include "cli.h"
#include "byteq.h"
#include "timeout.h"
#include "printers.h"

#include <reent.h>
#include <sys/types.h>
#include <stddef.h>

void system_failure(Long reason) { while (true); }

static Long dropped_chars = 0;

void show_cli() { print("\nDropped chars: "), printDec(dropped_chars); }

void output() {
    Timeout box;
    setTimeout(secs(1), &box);
    while(fullbq(emitq))
        if (checkTimeout(&box))
            pullbq(emitq);
        else
            action_slice();
}

// ── newlib reentrant syscall stubs ─────────────────────────────────────────
// Providing strong defs for the _r variants keeps libc's closer.o / lseekr.o /
// readr.o / writer.o out of the link, which silences the "not implemented"
// warnings embedded in their .gnu.warning.* sections.
int _close_r(struct _reent *r, int fd) {
    (void)r; (void)fd;
    return -1;
}

_off_t _lseek_r(struct _reent *r, int fd, _off_t off, int whence) {
    (void)r; (void)fd; (void)off; (void)whence;
    return (_off_t)-1;
}

_ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t n) {
    (void)r; (void)fd; (void)buf; (void)n;
    return -1;
}

_ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t n) {
    (void)r; (void)fd; (void)buf;
    return (_ssize_t)n;   // pretend success so callers don't retry-loop
}
