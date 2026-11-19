# Architecture

```
Public API      Usb · Device · Interface · Endpoint · Buffer · Completion · Reader · Writer · Watcher
                descriptors (zero-copy views) · DeviceInfo / BusInfo · UsbError / TransferStatus · Trace
                     │
Cores           DeviceCore ◄─Arc── InterfaceCore ◄─Arc── Endpoint (owns its Ring)
                     │                                       │
                     │ compile-time dispatch: if (cfg("USB_OS_…"))
     ┌───────────────┼──────────────────────┐                │ completions
 platform/linux   platform/macos     platform/windows        ▼
 usbfs ioctls     IOUSBLib vtables   WinUSB + cfgmgr32   one event-loop thread per process:
 sysfs, netlink   IORegistry          hub IOCTLs          epoll · CFRunLoop · I/O completion port
```

There is no backend interface and no virtual call: `include/usb_os.h` defines `USB_OS_LINUX`, `USB_OS_MACOS`,
`USB_OS_WINDOWS` and `USB_OS_POSIX`, and each operation branches on them with `if (cfg(…))`, which compiles to C's
`#if`. Every branch is type-checked on every build (CeeSafe requires it), so a change that breaks another
platform's code fails the check on the machine where it is made.

## Ownership

| Value | Owns | Shared with |
|---|---|---|
| `Device` | an `Arc<DeviceCore>`: the usbfs fd / IOUSBLib device object / WinUSB file list, the descriptor blob | the interfaces claimed from it |
| `Interface` | an `Arc<InterfaceCore>`: the claim (Linux), the IOUSBLib interface and its run-loop source (macOS), the WinUSB handle (Windows) | the endpoints opened from it |
| `Endpoint` | a `Box<Ring>` of transfer slots, the buffers in flight, a scratch buffer | the event-loop thread, through raw slot pointers only while a transfer is in flight |
| `Buffer` | heap memory, or a usbfs mapping (Linux) | nobody: it moves into the endpoint on submit and back out in the Completion |

Dropping in any order is safe: an endpoint keeps its interface claimed and its device open (it holds an Arc);
the last holder releases the OS resource. This is what the C# version did with hand-kept reference counts.

`DeviceCore`, `InterfaceCore`, `Ring`, `Buffer` and the Windows handle types are `@threadSafe`: they hold native
pointers, and each says in its doc comment why sharing them is sound. That is why the package's manifest says
`trusted = true`.

## The transfer ring

See [transfers.md](transfers.md). In short: a fixed array of slots per endpoint; the OS's request record (URB,
OVERLAPPED) is a slot's first field, so the pointer the OS returns on completion is the slot; one release store
publishes a result; an `AsyncSignal` wakes the owner. No lock and no allocation per transfer.

## Event loops

One thread per process per platform, started on first use with `pthread_create` / `CreateThread` and detached
(a CeeSafe `Thread` is joined at exit; see CEESAFE_FINDINGS F4). Nothing the user owns is ever dropped on it.

| | Wakes on | Per wakeup | Deregistration |
|---|---|---|---|
| Linux | epoll: each device's usbfs fd (`EPOLLOUT` = URBs to reap), the netlink socket, an eventfd | reaps every finished URB (`REAPURBNDELAY` until `EAGAIN`) | `EPOLL_CTL_DEL`, then wait until the loop's epoch counter moves past the current pass (the eventfd wakes it) |
| macOS | CFRunLoop: each claimed interface's async event source, each watcher's notification port, a permanent barrier source | IOKit calls the completion callout with the slot as `refcon` | `CFRunLoopRemoveSource`, then signal the barrier source and wait for its callout |
| Windows | `GetQueuedCompletionStatus` on one completion port that every WinUSB file is associated with | one OVERLAPPED (= slot) per dequeue | none needed: an endpoint drains its transfers before its file can close |

The epoch barrier is what lets an owner close a descriptor (or release an IOKit object) right after removing it:
once the loop has started a new pass, no event for the removed source can still be in hand. The C# version used a
lock per registration on Linux and deferred macOS teardown to the thread pool.

## Control transfers

Synchronous everywhere: `USBDEVFS_CONTROL` (the kernel enforces the timeout), IOKit's `DeviceRequestTO`, WinUSB's
`WinUsb_ControlTransfer` with the pipe timeout policy. The caller's memory is the data stage. Async code uses
`controlInAsync` / `controlOutAsync`, which run the call on a worker thread (`Async.blocking`).

## What is C

`src/platform/native.c`: `errno`; the Mach-O autolink options for IOKit and CoreFoundation; the address of
`kCFRunLoopDefaultMode`; three one-call trampolines that let async code submit and cancel (`ioctl` for the
non-blocking usbfs requests, IOKit's `ReadPipeAsync`/`WritePipeAsync`/`AbortPipe` through the vtable); the trace
level from the environment and the stderr write. Each exists because of a gap logged in `CEESAFE_FINDINGS.md`.
