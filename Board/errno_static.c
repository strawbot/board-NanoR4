// Board/errno_static.c — a single, static errno.
//
// The float-math wrappers in libm (sqrtf, powf, logf, expf, asinf, acosf, ...)
// call __errno(). The C library's version reaches the per-thread reent
// structure (_impure_data) and, through it, the stdio streams: about 600 bytes
// of RAM for state nothing here uses. There are no threads, so one plain int is
// enough, and defining __errno here means the library's copy is never linked.

int *__errno(void) {
    static int err;
    return &err;
}
