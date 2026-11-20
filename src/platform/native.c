/* The little that CeeSafe cannot say itself: errno is a C macro (a thread-local lvalue behind a libc function), so it
 * is read here; and the tracing helpers below. Everything else, the ioctls, IOKit's COM vtables and WinUSB, is called
 * from CeeSafe directly. */
#include <errno.h>

int usb_errno(void) { return errno; }

/* Tracing (src/trace.cee): the level from CEEUSB_TRACE, and a write of one finished line to stderr. Declared
 * @effects() on the CeeSafe side so that a trace call may sit on a path async code reaches (a submit): a short
 * write to stderr is treated as prompt, and tracing is off unless asked for. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int usb_trace_env_level(void) {
    const char *v = getenv("CEEUSB_TRACE");
    if (v == NULL || *v == 0) return 0;
    if (strcmp(v, "error") == 0 || strcmp(v, "1") == 0) return 1;
    if (strcmp(v, "warn") == 0 || strcmp(v, "2") == 0) return 2;
    if (strcmp(v, "debug") == 0 || strcmp(v, "3") == 0 || strcmp(v, "all") == 0) return 3;
    return 0;
}

void usb_trace_write(const unsigned char *line, size_t length) {
    fwrite(line, 1, length, stderr);
    fflush(stderr);
}
