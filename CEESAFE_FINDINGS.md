# CeeSafe findings

Bugs and gaps in CeeSafe met while porting CosmoUsb (C#) to CeeSafe as this package. Each has what was tried, what
the compiler said, and what this package does instead. Found with ceesafec 0.77.1 on macOS arm64 unless an entry says
otherwise; Linux checks on Ubuntu 24.04 x86-64 (gcc), Windows checks on Windows 11 (MinGW gcc).

| | What | Kind | State |
|---|---|---|---|
| F1 | a `Slice<r, T>` field passed to a call inside a returned struct literal is taken as a loan of `this` (N0245) | bug | fixed in ceesafec 0.79.0; workaround removed |
| F2 | a manifest cannot link a macOS framework | gap | fixed in 0.79.0 (`frameworks`, ADR 0184); workaround removed |
| F3 | `cfg()` cannot name the compiler's OS macros, and there is no target-OS condition of CeeSafe's own | gap | fixed in 0.79.0 (target conditions, ADR 0179); `usb_os.h` removed |
| F4 | no detached thread: a dropped `Thread<T>` is joined, so a process-lifetime event loop cannot be a CeeSafe thread | gap | fixed in 0.79.0 (`Thread.service`, ADR 0182); the three loops use it |
| F5 | async code cannot wait for readiness of a descriptor it owns (the runtime's `ceesafe_async_wait_readable` is private) | gap | fixed in 0.79.0 (`AsyncFd`, ADR 0183); not adopted: the blocking API shares the rings (docs/architecture.md) |
| F6 | `@cfg` is refused on a C static (`static kCFRunLoopDefaultMode: *void;`), so per-OS C data cannot be named | gap | fixed in 0.79.0 (configured statics, ADR 0179); used, with F11 |
| F7 | a call through a C function pointer always counts as `sleep` + `alloc`, with no way to declare it prompt | gap | fixed in 0.79.0 (ADR 0181); trampolines removed |
| F8 | constants of struct type are not supported (N0216), even for `@repr(C)` structs of integers | gap | fixed in 0.79.0 (ADR 0180); GUIDs and keys are constants |
| F9 | the generated C draws compiler warnings: unused labels, `-Wreturn-type`, `-Wmaybe-uninitialized` | build noise | fixed in 0.79.0 |
| F10 | `ceesafec test` shows a passing test's output only with `--format json` | papercut | fixed in 0.79.0 (`--show-output`, ADR 0185); the README says to use it |
| F11 | an imported C static needs a header that declares it, and a system header also declares the functions this package declares with other pointer types, which C then rejects | gap | open; worked around |
| F12 | ceesafec 0.78.1's Windows runtime warns in every build (`-Wsign-conversion`, `ceesafe_rt_io.c:375`) | build noise | fixed in 0.79.1 |
| F13 | `ceesafec build` in a `kind = "library"` package fails with N0300, "an executable needs a 'main' function" | papercut | open |

## F1. A slice field passed to a call inside a returned struct literal is taken as a loan of `this`

**Kind:** bug (borrow checker, false positive). A `Slice<r, u8>` is Copy, and the field's region is `r`, the struct's
own: returning a value built from it as `Filter<r>` is sound, and is accepted when the field is first copied into a
local.

```ceesafe
struct Walk<region r> {
    bytes: Slice<r, u8>;

    fn create(bytes: Slice<r, u8>) -> Walk<r> => Walk { bytes: bytes };
}

struct Filter<region r> {
    inner: Walk<r>;
    n: u8;
}

struct Group<region r> {
    body: Slice<r, u8>;
    n: u8;

    // N0245: the returned reference may point into what parameter 'this' refers to
    fn filter(borrow this) -> Filter<r> => Filter { inner: Walk.create(this.body), n: 2 };
}
```

The same field read directly into the literal (`Inner { bytes: this.body }`) is accepted; only the call argument
position trips it.

**Here:** `let body = this.body;` first, then `Walk.create(body)` (`src/descriptors.cee`, `InterfaceGroup.altSettings`).

## F2. A manifest cannot link a macOS framework

**Kind:** gap (packages). `links` takes library names for `-l`; IOKit and CoreFoundation are frameworks
(`-framework IOKit`), and nothing in `ceesafe.toml` passes one. `--cflag -framework --cflag IOKit` works for a
program built by hand, but a library cannot ask its users to remember it. CeeSafe's own runtime and the Clay hosts
`dlopen` the frameworks instead.

**Here:** the package's C file (`src/platform/native.c`) carries Mach-O autolink options, which ld64 honours:

```c
__asm__(".linker_option \"-framework\", \"IOKit\"\n.linker_option \"-framework\", \"CoreFoundation\"\n");
```

**Wanted:** `frameworks = ["IOKit", "CoreFoundation"]` under `[target.macos]`.

## F3. `cfg()` cannot name `__APPLE__`, `__linux__` or `_WIN32`

**Kind:** gap. `if (cfg("__APPLE__"))` and `@cfg("__linux__")` are N0292 ("reserved by the C standard"), which is
right about the spelling but leaves no way to write per-OS code without a header of one's own. The manifest already
knows the OS (`[target.linux]`, `[target.macos]`, `[target.windows]`).

**Here:** `include/usb_os.h` defines `USB_OS_LINUX`, `USB_OS_MACOS`, `USB_OS_WINDOWS` and `USB_OS_POSIX` from the
compiler's macros, and every extern block names it with `@header("usb_os.h")`.

**Wanted:** predefined conditions, e.g. `cfg("os.linux")`, or the reserved OS macros allowed by name.

A related papercut: a function declared `@cfg("USB_OS_POSIX")` cannot be used inside `if (cfg("USB_OS_LINUX"))`
alone, even though Linux implies POSIX here; the call must be wrapped in both conditions.

## F4. No detached thread

**Kind:** gap (lib/std threads). A `Thread<T>` that is dropped is joined, and every thread ends before `main`
returns. A USB event loop (epoll on Linux, a CFRunLoop on macOS, a completion port on Windows) lives for the
process and is started lazily from library code, which has nowhere to keep the handle and must not make `main`
wait for it at exit.

**Here:** `pthread_create` + `pthread_detach` (`CreateThread` on Windows) with an `@callback` entry point
(`src/sys.cee`, `startThread`).

**Wanted:** `Thread.spawnDetached(move () => …)`, or a daemon flag.

## F5. Async code cannot wait for readiness of its own descriptor

**Kind:** gap (lib/std async). usbfs signals finished transfers by making its descriptor writable (`POLLOUT`).
The runtime's executor already waits on descriptors (`ceesafe_async_wait_readable` / `_writable` in lib/std's
`async.cee`), but those are private declarations, and declaring them again in a package is N0201 (one C name, one
declaration). So a task cannot park on a usbfs descriptor, and a library must run its own epoll thread and wake
tasks through `AsyncSignal`, an extra thread hop per completion batch.

**Here:** one process-wide epoll thread (`src/platform/linux.cee`) reaps every ready URB per wakeup and raises the
owning endpoint's `AsyncSignal`.

**Wanted:** a public `AsyncFd.readable(fd)` / `writable(fd)`.

## F6. `@cfg` is refused on a C static

**Kind:** gap. `kCFRunLoopDefaultMode` is a CoreFoundation data symbol that exists only on macOS. Declared in an
`extern "C"` block under `@cfg("USB_OS_MACOS")`, it is N0290 ("'@cfg' cannot be used on a C static"); without the
`@cfg`, Linux and Windows builds declare a symbol they do not have (and every build must type-check the block).

**Here:** a one-line C accessor in `src/platform/native.c` (`usb_cf_default_mode()`), declared `@cfg` like any
function.

**Wanted:** `@cfg` on `static` declarations in extern blocks, with the same N0350 rule as functions.

## F7. A call through a C function pointer cannot be declared prompt

**Kind:** gap (effects, type-system.md §32.1). IOKit's user-client API is COM: every call goes through a vtable of C
function pointers, and each such call counts as `sleep` and `alloc`. `extern "C" fn` types take no effect list, so
`ReadPipeAsync`, `WritePipeAsync` and `AbortPipe`, which return at once, make every async caller N0295. The same
holds one level down for `ioctl`, which is one C function for blocking requests (USBDEVFS_CONTROL) and non-blocking
ones (SUBMITURB, DISCARDURB, REAPURBNDELAY): its declaration can carry only one effect list, and two declarations
of one C name are N0201.

**Here:** C trampolines in `src/platform/native.c` (`usb_iokit_pipe_async`, `usb_iokit_abort_pipe`,
`usb_ioctl_prompt`), declared `@effects()`. Each is one call.

**Wanted:** effects on C function pointer types (`@effects() extern "C" fn(…)`), and a way to give one C function two
CeeSafe declarations (`@cname` aliases with their own effects).

## F8. Constants of struct type

**Kind:** gap. `const GUID_USB_DEVICE: Guid = Guid { data1: …, data2: …, data3: …, data4: [...] };` is N0216
("constants of struct type are not supported yet"), for a `@repr(C)` struct of integers and an array. Windows code
is full of such constants (GUIDs, DEVPROPKEYs).

**Here:** functions that return the value (`fn guidUsbDevice() -> Guid => Guid { … };`), in
`src/platform/windows.cee`.

## F9. Warnings in the generated C

**Kind:** build noise (no runtime fault seen). Every build of this package prints C compiler warnings for code the
user did not write:

- `-Wunused-label` (`bb43:` and similar) from clang and gcc, several per build.
- `-Wreturn-type` ("no return statement in function returning non-void", gcc/MinGW) for a function whose body is
  an infinite `loop` inside `if (cfg("X")) { … }` followed by `return 0;`: in the builds where `X` holds, the
  emitted function ends without a return after the loop (the trailing return was dropped as unreachable).
  `winLoopMain` in `src/platform/windows.cee` is one.
- `-Wmaybe-uninitialized` on temporaries of lib/std's `StringCopying.from` when gcc inlines it into a caller
  (MinGW gcc, `-O2`).

## F10. A passing test's output

**Kind:** papercut (`ceesafec test`). A test that measures something (`streamThroughTheRing` prints the ring's cost
per transfer) shows its output only through `--format json`; the text report shows output for failures only. A
`--show-output` (as `cargo test -- --nocapture`) would do.

## F11. An imported static's header conflicts with the package's own declarations

**Kind:** gap (C interop, type-system.md §39.4). An imported static is "data its headers define": CeeSafe emits no
declaration of it, so its block needs a `@header` that declares `kCFRunLoopDefaultMode`. The header that does,
`<CoreFoundation/CoreFoundation.h>`, also declares `CFRunLoopAddSource` and the other CF functions with CF types
(`CFRunLoopRef`, `CFStringRef`), and the package declares them with `*void`: the generated C's prototypes conflict
with the header's (`conflicting types for 'CFRunLoopAddSource'`, a dozen of them). Found with ceesafec 0.79.0.

**Here:** `include/usb_cf.h`, one line, `extern const void *const kCFRunLoopDefaultMode;`, as the configured block's
header.

**Wanted:** a C static whose declaration CeeSafe emits itself when no header declares it (as it does for functions),
or a way to name CF's pointer typedefs so the package's declarations can match the header's.

## F12. A warning from the runtime in every Windows build

**Kind:** build noise (ceesafec 0.78.1 and 0.79.0, MinGW gcc 16.2). `ceesafe_rt_io.c:375`, added in 0.78.1 for
read-only files: `attributes & ~FILE_ATTRIBUTE_READONLY` complements an `int` and converts -2 to `DWORD`, which
`-Wsign-conversion` reports. `~(DWORD)FILE_ATTRIBUTE_READONLY` is the fix.

**Fixed** in ceesafec 0.79.1 (the complement is of a `DWORD`); the manifest needs 0.79.1, so Windows builds are warning-free.

## F13. Building a library package asks for a `main`

**Kind:** papercut (tooling.md §4, diagnostics.md N0300; ceesafec 0.79.1 on macOS, Linux and Windows, and 0.79.0).
`ceesafec build` in this package, whose manifest says `kind = "library"`, builds an executable by default and stops at
the first file: `src/buffer.cee:1:1: error[N0300]: an executable needs a 'main' function: fn main() -> i32 { ... }`.
The manifest already says the package has no `main`, and the error points at a file that has nothing to do with it.
`ceesafec build --emit lib` works.

**Here:** nothing to change; `ceesafec check` and `ceesafec test` are what a library runs, and the examples build
executables.

**Wanted:** in a library package, `build` with no `--emit` makes what a library makes (`--emit lib`, or a check that
says so), or N0300 names the manifest's `kind` and suggests `--emit lib` instead of pointing at the first source file.
