# Architecture

```
Public API      Usb · Device · Interface · Endpoint · Buffer · Completion · Reader · Writer · Watcher
                descriptors (zero-copy views) · DeviceInfo / BusInfo · UsbError / TransferStatus · Trace
                     │
Cores           DeviceCore ◄─Arc── InterfaceCore ◄─Arc── Endpoint (owns its Ring)
                     │                                       │
                     │ compile-time dispatch: if (cfg("target.…"))
     ┌───────────────┼──────────────────────┐                │ completions
 platform/linux   platform/macos     platform/windows        ▼
 usbfs ioctls     IOUSBLib vtables   WinUSB + cfgmgr32   one event-loop thread per process:
 sysfs, netlink   IORegistry          hub IOCTLs          epoll · CFRunLoop · I/O completion port
```

There is no backend interface and no virtual call: each operation branches on CeeSafe's target conditions,
`if (cfg("target.linux"))`, `"target.macos"`, `"target.windows"` and `"target.unix"`, which compile to C's `#if`. Every branch is type-checked on every build (CeeSafe requires it), so a change that breaks another
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

One thread per process per platform, started on first use with `Thread.service` (named `usb-epoll`, `usb-runloop`
or `usb-iocp`), which runs for the process and is never joined. Nothing the user owns is ever dropped on it.

On Linux the loop stays a thread rather than tasks waiting with `AsyncFd`: the blocking API (`read`, `flush` with a
timeout) waits on the same rings, and a task that reaped through `AsyncFd` while the loop also reaps could park on
an edge the loop had already consumed.

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

`src/platform/native.c`: `errno`, which is a C macro; the trace level from the environment and the stderr write.
Everything else is CeeSafe: the non-blocking usbfs ioctls through `ioctlPrompt`, a second declaration of `ioctl`
with no effects; IOKit's `ReadPipeAsync`, `WritePipeAsync` and `AbortPipe` through vtable slots typed
`@effects() extern "C" fn`; `kCFRunLoopDefaultMode` as a configured C static (`include/usb_cf.h` declares it).
