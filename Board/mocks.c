#include "sr595.h"
#include "muscle_sr.h"
#include "ttypes.h"
#include "tea.h"
#include "cli.h"
#include "byteq.h"
#include "timeout.h"
#include "printers.h"

#include "cli_transport.h"
#include "postmortem.h"
#include "hal_data.h"   // __get_IPSR()

#include <reent.h>
#include <sys/types.h>
#include <stddef.h>

// Every black hole ends here. Outputs first: the 595 keeps its last pattern, and
// a wire left latched on burns out in seconds.
void system_failure(Long reason) {
    muscle_sr_emergency_stop();   // stop the frame timer (it would keep latching patterns), then clear the chain
    postmortem_failure(reason, (uint32_t) __builtin_return_address(0), __get_IPSR());   // so show-postmortem can say why after the reset
    while (true);
}

// newlib's setjmp.o (used by cli.c stack-fault recovery) carries an EHABI
// unwind entry naming this personality routine. Without a local definition
// the linker pulls libgcc's C++ unwinder -> abort -> raise -> _kill/_getpid
// (~3.5KB dead code plus "not implemented" warnings). Pure C never unwinds.
void __aeabi_unwind_cpp_pr0(void) { system_failure(0); }

static Long dropped_chars = 0;

void show_cli() {
    print("\nDropped chars: ");    printDec(dropped_chars);           printCr();
    print("Active transport: ");   print(cli_transport_active_name()); printCr();
}

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
