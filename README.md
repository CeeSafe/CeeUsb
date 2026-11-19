# CeeUsb

Cross-platform, low-level USB device access in CeeSafe: a port of
CosmoUsb (C#, itself a reimplementation of Rust's nusb) that talks to each operating system's
USB stack directly. Not a line-by-line port: the transfer path, event loops and enumeration were redesigned around
CeeSafe's ownership model to remove the C# version's per-transfer costs.

| Platform | I/O | Enumeration | Completions | Hotplug |
|---|---|---|---|---|
| Linux | usbfs ioctls | sysfs (`descriptors` file) | one epoll thread | udev netlink |
| macOS | IOKit IOUSBLib (COM vtables, called from CeeSafe) | IORegistry | one CFRunLoop thread | IOKit notifications |
| Windows | WinUSB | cfgmgr32 + hub IOCTLs | one I/O completion port thread | CM_Register_Notification |

No libusb, no C library beyond the OS's own. The C in the package is `src/platform/native.c`: `errno`, macOS
framework autolinking, one data symbol, and three one-call trampolines (see `CEESAFE_FINDINGS.md` F2, F6, F7).

## Quick start

```ceesafe
// Enumerate (no device I/O: the OS's cache)
for (d in usb.Usb.listDevices()!.view()) {
    Console.writeLine(d.vendorId);
}

// Find, open, claim
let info = usb.Usb.findDevice(borrow usb.DeviceSelector.ids(0x1234, 0x5678))!!;
let device = usb.Usb.open(borrow info)!;
let iface = device.detachAndClaimInterface(0)!;      // Linux detaches a kernel driver atomically

// Control transfers: the caller's memory is the data stage, no allocation
var raw = [0u8; 18];
let n = device.getDescriptor(usb.DESCRIPTOR_DEVICE, 0, 0, mut raw[..], 1000)!;

// One-shot bulk transfers (the endpoint reuses one buffer across calls)
var out = iface.openEndpoint(0x01)!;
var input = iface.openEndpoint(0x81)!;
out.write(payload, 1000)!;
let got = input.read(mut buffer[..], 1000)!;
```

### Streaming: the transfer ring

Keep several transfers in flight so the host controller never waits; completions come back in submission order,
and buffers go round and round without allocating:

```ceesafe
var ep = iface.openEndpointWith(0x81, 8)!;            // room for 8 transfers in flight
for (i in 0..4) {
    ep.submit(ep.allocate(16384)!)!;                  // Linux: usbfs-mapped, zero-copy memory
}
loop {
    let c = ep.waitComplete(0)!;                      // or: await ep.nextComplete()
    process(c.buffer.view());
    var b = c.buffer;
    b.clear();
    ep.submit(b)!;                                    // reuse: no allocation, no mmap
}
```

Or as a byte stream: `usb.Reader.create(ep, 16384, 4)` / `usb.Writer.create(ep, 16384, 4)` with `read`, `readAsync`,
`readMessage` (until a short packet), `write`, `writeAsync`, `flush`, `flushEnd` (ends with a zero-length packet when
needed).

### Hotplug

```ceesafe
let watcher = usb.Usb.watch()!;      // first...
let present = usb.Usb.listDevices()!; // ...then list, so nothing is missed
match (watcher.next()) {             // or tryNext(), or await nextAsync()
    Some(usb.HotplugEvent.Arrived(info)) => { … }
    Some(usb.HotplugEvent.Left(id)) => { … }
    null => {}
}
```

### Sync and async

Every transfer can be waited for from a thread (`waitComplete`, `read`, `write`, `transfer`) or from a task
(`nextComplete`, `readAsync`, `writeAsync`; cancelling the task cancels the transfer and gets its buffer back
first). Control transfers and the setup calls are blocking syscalls on every OS; their `*Async` forms
(`Usb.openAsync`, `listDevicesAsync`, `claimInterfaceAsync`, `setConfigurationAsync`, `resetAsync`,
`setAltSettingAsync`, `clearHaltAsync`, `controlInAsync`, …) run them on a worker thread (`Async.blocking`), as the C#
version ran them on the thread pool. `docs/api-mapping.md` maps every C# call to its CeeSafe form.

## Error model

Control-plane calls return `Result<T, UsbError>`: a coarse `ErrorKind` (`Disconnected`, `Busy`,
`PermissionDenied`, `NotFound`, `Unsupported`, `TimedOut`, `Cancelled`, `InvalidArgument`, `NoMemory`, `Other`),
the `Operation` it happened in, and the OS code (errno, IOReturn, Win32). `UsbError` is a Copy value; nothing
allocates to report an error. Diagnostics are off by default: `CEEUSB_TRACE=error|warn|debug` (or
`Trace.setLevel`) traces to stderr. See `docs/error-handling.md`. Data transfers report a `TransferStatus` per `Completion` (with partial lengths), so
one failure never stops a stream; after a `Stall`, call `endpoint.clearHalt()`.

## What changed against the C# version, and why

| C# CosmoUsb | CeeUsb | Effect |
|---|---|---|
| Per transfer: `TransferEntry` + platform context object + `GCHandle` + (Linux) a `NativeMemory` URB, a lock on submit and on complete, a `TaskCompletionSource` per wait | A fixed ring of slots per endpoint; the OS's request record (URB, OVERLAPPED) is the slot's first field, so the pointer the OS returns *is* the slot. One atomic store publishes a completion; an `AsyncSignal` wakes the waiter | zero allocation and no lock per transfer: **165 ns** per submit→complete→wake→take round trip across threads |
| Linux event loop reaps **one** URB per `epoll_wait`, dispatches through a dictionary under a global lock | Reaps until EAGAIN per wakeup; the epoll data is the device's record | one wakeup per batch of completions |
| Linux control transfer = URB + userspace `timerfd` + sorted timeout set under a lock + heap buffer + queue | `USBDEVFS_CONTROL`: one ioctl; the kernel enforces the timeout | **≈70 µs less per control transfer** (table below) |
| macOS control transfer = async request + run-loop hop + heap request + GCHandle; device async event source | `DeviceRequestTO`, synchronous; no device event source | no run-loop round trip |
| Windows control transfer = thread-pool OVERLAPPED + `System.Threading.Timer` → `CancelIoEx` | `WinUsb_ControlTransfer` synchronous with WinUSB's pipe timeout | no timer, no allocation |
| `ReadAsync`/`WriteAsync` allocate (Linux: `mmap`/`munmap`) a buffer per call | The endpoint keeps one scratch buffer | read loops allocate once |
| Writer disposes a completed buffer whenever a write buffer is active and allocates (mmaps) a new one: per transfer at ≥2 in flight | Reader and Writer keep a free list | streaming allocates nothing after warm-up |
| Linux enumeration: ~12 text attribute files per device | One read of sysfs's binary `descriptors` (device + configurations); interface names only when the interface has a string | **≈7× faster** |
| Windows enumeration: parent hub opened 3× per device; V2 IOCTL for every device | Each hub opened once per listing; V2 only for high/super speed | ≈25–30% faster (cfgmgr32 dominates) |
| macOS enumeration: ~15 `IORegistryEntrySearchCFProperty` per device; open iterates every USB device to find one ID | One `IORegistryEntryCreateCFProperties` per entry; open uses `IORegistryEntryIDMatching` | fewer registry walks |
| Descriptor walking allocates enumerators and a `SortedDictionary` of lists | Iterators over the OS blob; nothing allocated | zero-copy |
| Hand-kept reference counts; teardown deferred to the thread pool when the last ref drops on the run-loop thread | `Arc` of cores; event-loop teardown via an epoch barrier (Linux epoll, macOS run-loop source): after removal, wait until the loop has passed once | no use-after-free window, no thread-pool hop |
| macOS run loop sleeps 10 ms and retries when it has no sources | A permanent barrier source keeps the loop alive | no polling |
| Windows `cancelAll` = `CancelIoEx` per transfer | One `WinUsb_AbortPipe` | |
| Windows `DeviceId` = device node handle, reassigned when a removed device is looked up | A hash of the instance ID | removal events match arrivals (a bug in the C# version, found by a live replug) |

### Measured (same machines, same devices; `examples/bench` vs `benchmarks/csharp`)

| | CeeUsb | CosmoUsb (C#, .NET 10, self-contained) |
|---|---:|---:|
| Linux: `listDevices` (3 devices), mean of 2000 | **213 µs** | 1,490 µs |
| Linux: GET_DESCRIPTOR(device) round trip, high-speed hub, mean of 3000 | **228 µs** | 298 µs |
| Linux: same from a task (`controlInAsync`) | 274 µs | — |
| Windows 11: `listDevices` (6 devices), mean of 200 | **2.4–3.0 ms** | 3.3–3.7 ms |
| Windows 11: GET_DESCRIPTOR through a claimed interface (WinUSB, full-speed), mean of 2000, 3 runs | 257 µs (±1) | 254–289 µs |
| Windows 11: one-transfer 2-byte bulk OUT `write` (WinUSB, full-speed), mean of 2000, 3 runs | 223–229 µs | 227–256 µs |
| macOS (M1): `listDevices` (3 devices behind a USB-C adapter), mean of 500, 2 runs | **340–350 µs** | 760–860 µs |
| macOS: GET_DESCRIPTOR(device), mean of 2000 (answered from IOKit's cache: sync call vs async + run-loop hop) | **2–3 µs** | 177–224 µs |
| macOS: one-transfer 2-byte bulk OUT `write` (full-speed printer), mean of 2000, 2 runs | **223 µs** | 403–624 µs |
| Ring overhead per transfer (no OS call, cross-thread) | 165 ns | — |

Streaming on Linux against Gadget Zero (`g_zero`) on the `dummy_hcd` virtual controller, 5 s per run. Both sides
reach the controller's own ceiling, so the measure is the CPU each spends to get there (`/usr/bin/time`, user + sys;
the C# figures include ~0.1-0.2 s of runtime start-up):

| Stream (`bench stream`) | Throughput, both | CeeUsb CPU | CosmoUsb CPU |
|---|---:|---:|---:|
| bulk IN, 16 KiB × 8 in flight | 26 MB/s | **0.36 s (7%)** | 0.72 s (14%) |
| bulk OUT, 16 KiB × 8 in flight | 26 MB/s | **0.79 s (16%)** | 1.43 s (28%) |
| bulk IN, 512 B × 32 in flight (~16,000 transfers/s) | 8 MB/s | **1.04 s (20%)** | 2.31 s (45%) |
| bulk OUT, 512 B × 32 in flight | 8 MB/s | **1.09 s (21%)** | 1.99 s (38%) |

The round trips are bus-bound (a control transfer spans several (micro)frames); the difference is the software
path. On Linux that path was the C# version's URB + timerfd + queue machinery, and removing it shows. On Windows a
single round trip is dominated by WinUSB and the bus on both sides: the two are level, the port's times steadier
from run to run. The ring's gains (no allocation, no lock per transfer) show under load, with several transfers in
flight, which these devices cannot sustain. On macOS the transfer path itself is ~2× faster than the C# version's
(run-loop completion → ring slot + AsyncSignal, against GCHandle + TransferQueue lock + TaskCompletionSource). Linux: Ubuntu 24.04, kernel 6.8, x86-64. Windows: Windows 11 x64, MinGW gcc.

## Platform requirements

- **Linux:** write access to `/dev/bus/usb/BBB/DDD`, e.g. a udev rule
  `SUBSYSTEM=="usb", ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", MODE="0660", TAG+="uaccess"`.
  LP64 only (x86-64, arm64), as the C# version.
- **macOS:** vendor-class devices may need `device.setConfiguration(1)` before their interfaces appear. Devices
  claimed by a kernel driver are not accessible.
- **Windows:** the device (or composite function) must be bound to **WinUSB** (WCID descriptor, INF, or Zadig).
  Other devices enumerate and read descriptors (through the hub) but cannot be claimed. No `setConfiguration`,
  no `reset`; device-level control transfers go through a claimed `Interface`; the manufacturer string is not
  cached by the OS.

## Verification status

| | Linux | macOS | Windows |
|---|---|---|---|
| Unit tests (`ceesafec test`, 28: descriptors, descriptor fuzzing, selectors, errors, ring ordering/full/validation, timeouts that cancel, cross-thread and async waits, Reader/Writer, zero-length packets) | pass | pass | pass |
| Bus and device enumeration | real devices, matches `lsusb` | real devices, matches `ioreg` | real devices |
| Open, GET_DESCRIPTOR, string descriptors | real devices | real devices | real devices (through the hub) |
| Claim errors (kernel driver owns it) | EBUSY → `Busy` | — | non-WinUSB → `Unsupported` |
| Hotplug | removal and arrival through the kernel (gadget unloaded and reloaded), teardown | real replug: arrival and removal, IDs paired, teardown | real replug: arrival and removal, IDs paired, teardown |
| Detach a bound kernel driver and claim; driver reattached on release | Gadget Zero (`usbtest` detached and reattached) | — | — |
| Claim, control through the interface | Gadget Zero | real device (IOKit) | real device (WinUSB) |
| Interrupt IN and OUT, `usbhid` detached and reattached, Ctrl+C cancelling a read | virtual HID device (`tools/linux-hid-gadget.sh`) | — | — |
| Bulk OUT and IN, timeouts and cancel, several in flight, `cancelAll`, async cancel, stall + `clearHalt` | Gadget Zero, every byte checked, zero-copy usbfs buffers | real device (IOKit) | real device (WinUSB) |

Windows and macOS were verified against the same STM32 USB printer (0483:5840, bulk 0x03/0x81; bound to WinUSB on
Windows, no setup on macOS), with `examples/printer`, which sends nothing that prints. Linux was verified against
Linux's Gadget Zero on the `dummy_hcd` virtual host controller (`examples/gzero`, 20 checks: the gadget fills and
checks every packet with a pattern, so every byte streamed in either direction is verified, and a corrupted write
is answered with a stall that `clearHalt` recovers). Ubuntu does not ship `dummy_hcd`; it builds out of tree from
the kernel's `drivers/usb/gadget/udc/dummy_hcd.c`, then `modprobe libcomposite; insmod dummy_hcd.ko;
modprobe g_zero pattern=1`. Interrupt transfers were verified the same way against a vendor-defined HID gadget
(`tools/linux-hid-gadget.sh`, `examples/interrupt`). `examples/widget` (the C# usb-widget fault-injection test, with
its Arduino firmware) and `examples/fingerprint` (the ZKTeco scanner capture) are ported but not run: no such
hardware was available.

## Layout

```
src/            errors, types, descriptors (zero-copy), info, buffer, transfer (the ring), device (the API),
                stream (Reader/Writer), hotplug (Watcher), sys (POSIX/Win32 basics)
src/platform/   linux.cee, macos.cee, windows.cee, native.c
include/        usb_os.h: the OS macros cfg() names (CEESAFE_FINDINGS F3)
tests/          descriptor, ring and stream tests
docs/           architecture, transfers, error handling and diagnostics, linux, macos, windows, API mapping from C#
examples/       list (lsusb), info (open a device and read it), watch (hotplug), interrupt (interrupt reports),
                printer and gzero (transfer tests on hardware), widget and fingerprint (C# hardware samples, unverified),
                bench
tools/          linux-hid-gadget.sh: a virtual HID device for interrupt tests
benchmarks/     csharp: the same benchmark against the C# original
```

## License

MIT OR Apache-2.0, as CosmoUsb and nusb: [LICENSE-MIT](LICENSE-MIT), [LICENSE-APACHE](LICENSE-APACHE).
