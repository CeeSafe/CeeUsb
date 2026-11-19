/* Which operating system this build is for, as macros CeeSafe's cfg() may name (the compiler's own, such as
 * __APPLE__, are reserved). Included by every extern block of the package through @header. */
#ifndef USB_OS_H
#define USB_OS_H
#if defined(__APPLE__)
#define USB_OS_MACOS 1
#elif defined(__linux__)
#define USB_OS_LINUX 1
#elif defined(_WIN32)
#define USB_OS_WINDOWS 1
#endif
#if defined(__linux__) && defined(__x86_64__)
#define USB_EPOLL_PACKED 1
#endif
#if defined(__APPLE__) || defined(__linux__)
#define USB_OS_POSIX 1
#endif
#endif
