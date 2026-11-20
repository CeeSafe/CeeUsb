/* kCFRunLoopDefaultMode, declared alone: CoreFoundation's own headers declare the CF functions with their CF types,
 * which conflict with this package's `*void` declarations of them (CEESAFE_FINDINGS F11). Only Apple builds include
 * this (src/platform/macos.cee's configured block). */
#ifndef USB_CF_H
#define USB_CF_H
extern const void *const kCFRunLoopDefaultMode;
#endif
