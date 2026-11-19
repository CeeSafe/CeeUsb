/* The little that CeeSafe cannot say itself:
 *  - errno is a C macro (a thread-local lvalue behind a libc function), so it is read here;
 *  - on macOS the IOKit and CoreFoundation frameworks are linked by the object file itself (Mach-O autolinking),
 *    since a package manifest names libraries (-l) and not frameworks.
 * Everything else, the ioctls, IOKit's COM vtables and WinUSB, is called from CeeSafe directly. */
#include <errno.h>

#if defined(__APPLE__)
__asm__(".linker_option \"-framework\", \"IOKit\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"\n");
#endif

int usb_errno(void) { return errno; }

#if defined(__APPLE__)
/* kCFRunLoopDefaultMode is a C data symbol; CeeSafe cannot declare a C static under @cfg (CEESAFE_FINDINGS F6). */
extern const void *kCFRunLoopDefaultMode;
const void *usb_cf_default_mode(void) { return kCFRunLoopDefaultMode; }
#endif

/* Trampolines for calls that return at once but that CeeSafe would count as possibly sleeping: a call through a
 * C function pointer (IOKit's COM vtables) always is, and ioctl is declared once for blocking and non-blocking
 * requests alike. Declared @effects() on the CeeSafe side so that async code may submit and cancel transfers
 * (CEESAFE_FINDINGS F7). Each is one call, no logic. */
#if defined(__linux__)
#include <sys/ioctl.h>
/* USBDEVFS_SUBMITURB, _DISCARDURB and _REAPURBNDELAY: queue, cancel or reap; none waits. */
int usb_ioctl_prompt(int fd, unsigned long request, void *argument) { return ioctl(fd, request, argument); }
#endif

#if defined(__APPLE__)
typedef void (*usb_async_callback)(void *refcon, int result, void *arg0);
typedef int (*usb_pipe_async)(void *self, unsigned char pipe, void *buffer, unsigned int size,
                              usb_async_callback callback, void *refcon);
typedef int (*usb_pipe_op)(void *self, unsigned char pipe);

/* IOUSBInterfaceInterface700: AbortPipe is slot 28, ReadPipeAsync 33, WritePipeAsync 34 (IOUSBLib.h). */
static void **usb_vtbl(void *self) { return *(void ***)self; }

int usb_iokit_pipe_async(void *self, unsigned char pipe, int write, void *buffer, unsigned int size,
                         usb_async_callback callback, void *refcon) {
    usb_pipe_async f = (usb_pipe_async)usb_vtbl(self)[write ? 34 : 33];
    return f(self, pipe, buffer, size, callback, refcon);
}

int usb_iokit_abort_pipe(void *self, unsigned char pipe) {
    usb_pipe_op f = (usb_pipe_op)usb_vtbl(self)[28];
    return f(self, pipe);
}
#endif

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
