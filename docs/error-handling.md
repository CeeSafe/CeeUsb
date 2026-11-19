# Errors and diagnostics

## Control-plane errors: `UsbError`

Opening, claiming, configuring, control transfers and the blocking `read`/`write` helpers return
`Result<T, UsbError>`. A `UsbError` is a Copy value of three fields: nothing allocates to report one.

| `kind` | Means | Typical cause |
|---|---|---|
| `Disconnected` | the device is gone | unplugged; ENODEV/ESHUTDOWN, kIOReturnNoDevice, ERROR_DEVICE_NOT_CONNECTED |
| `Busy` | someone else has it | a kernel driver owns the interface (EBUSY), another program has exclusive access, a second claim/open in this process, an alternate setting changed with endpoints open |
| `PermissionDenied` | not allowed | Linux device node permissions (see linux.md) |
| `NotFound` | no such thing | configuration, interface, alternate setting or endpoint |
| `Unsupported` | not on this OS, or not with this driver | `reset`/`setConfiguration` on Windows; a Windows device not bound to WinUSB |
| `TimedOut` | a blocking call ran out of time | the transfer was cancelled and is back |
| `Cancelled` | the task was cancelled | async helpers |
| `InvalidArgument` | the request cannot be made | control data over 65535 bytes; a Windows interface request whose index is not the claimed interface; an endpoint of the wrong type or direction in `openEndpointChecked` |
| `NoMemory` | an allocation failed | |
| `Other` | anything else | the `code` says what; a control **stall** is `Other` with the OS's stall code (EPIPE 32, kIOUSBPipeStalled, ERROR_GEN_FAILURE 31) |

`operation` says what was being done (`Open`, `ClaimInterface`, `ControlTransfer`, …), and `code` is the OS's
number: `errno` on Linux, an `IOReturn` on macOS, a Win32 error on Windows, 0 when there is none.
`e.write(mut textWriter)` renders "claiming the interface: in use by a kernel driver or another program (OS error
0x00000010)"; `e.message()` and `e.during()` give the two parts.

## Data-plane errors: `TransferStatus`

Each `Completion` carries a status, the bytes moved (`actualLength`, which can be nonzero on failure) and the OS
`code`:

| Status | Linux | macOS | Windows |
|---|---|---|---|
| `Success` | 0 | kIOReturnSuccess, kIOReturnUnderrun | 0 |
| `Cancelled` | ENOENT, ECONNRESET, ETIMEDOUT | kIOReturnAborted, timeouts | ERROR_OPERATION_ABORTED, ERROR_REQUEST_ABORTED, timeouts |
| `Stall` | EPIPE | kIOUSBPipeStalled | ERROR_GEN_FAILURE |
| `Disconnected` | ENODEV, ESHUTDOWN | kIOReturnNoDevice | ERROR_DEVICE_NOT_CONNECTED, ERROR_FILE_NOT_FOUND, ERROR_BAD_COMMAND |
| `Fault` | EPROTO, EILSEQ, EOVERFLOW, ECOMM, ETIME, EREMOTEIO | | |
| `InvalidArgument` | EINVAL; or an IN length that is not whole packets | kIOReturnBadArgument | |
| `QueueFull` | (not produced by a completion; a full ring gives the buffer back at submit) | | |
| `Unknown` | anything else | | |

After a `Stall`, call `endpoint.clearHalt()`. A halt stays until cleared, even across programs: a test that
starts with `clearHalt()` on its endpoints does not inherit one from an earlier run.

`completion.intoBuffer()` gives the buffer or the error; `transferError(status, code, operation)` turns a status
into a `UsbError`.

## Diagnostics

Off by default. `CEEUSB_TRACE=error|warn|debug` in the environment, or `Trace.setLevel(TraceLevel.Debug)`, writes
one line per event to stderr:

```
ceeusb debug: opened /sys/bus/usb/devices/3-1
ceeusb warn: IN transfer refused: requested length is not a nonzero multiple of the packet size: 100
ceeusb debug: enumeration skipped USB\VID_8087&PID_07DC\6&19F9463&0&3
```

- error: an event loop that stopped (nothing else would report it);
- warn: refused submits, IN lengths that are not whole packets, a WinUSB pipe policy that was refused;
- debug: devices skipped while enumerating, opens and claims and their failures, the heap fallback when usbfs
  mapping fails.

Lines are built on the stack and never carry transfer data.
