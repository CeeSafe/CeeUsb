# CosmoUsb (C#) → CeeUsb (CeeSafe)

The same capabilities, in CeeSafe's idiom: exceptions become `Result<T, UsbError>`, `Dispose` becomes dropping
the value, `CancellationToken` becomes task cancellation, sync + `*Async` pairs stay pairs.

## Enumeration

| C# | CeeSafe |
|---|---|
| `Usb.ListDevices()`, `ListDevicesAsync()` | `usb.Usb.listDevices()`, `listDevicesAsync()` → `Result<Vector<DeviceInfo>, UsbError>` |
| `Usb.ListBuses()`, `ListBusesAsync()` | `Usb.listBuses()`, `listBusesAsync()` |
| `Usb.FindDevice(selectors…)` | `Usb.findDevice(borrow selector)` → `Result<DeviceInfo?, UsbError>` (one selector) |
| `new UsbDeviceSelector { VendorId = …, … }` | `DeviceSelector.ids(vid, pid)`, `DeviceSelector.any()` then set fields |
| `info.Matches(selector)` | `info.matches(borrow selector)` |
| `UsbDeviceInfo` properties | `DeviceInfo` fields: `id`, `busId`, `address`, `portChain`, `vendorId`, …, `interfaces` |
| `SysfsPath` / `InstanceId` | `path` |
| `BusNumber`, `LocationId`, `RegistryEntryId`, `PortNumber`, `ParentInstanceId`, `Driver` | `busNumber`, `locationId`, `registryId`, `port`, `parentId`, `driver` |
| `Usb.WatchDevices()` / `await foreach` | `Usb.watch()` then `next()`, `tryNext()`, `await nextAsync()` → `HotplugEvent.Arrived(info)` / `Left(id)` |

## Devices and interfaces

| C# | CeeSafe |
|---|---|
| `info.Open()`, `OpenAsync()` | `Usb.open(borrow info)`, `Usb.openAsync(borrow info)` |
| `device.DeviceDescriptor` | `device.descriptor()` (`vendorId()`, `productId()`, …) |
| `device.Configurations` | `device.configurations()` (an iterator of zero-copy views) |
| `device.GetActiveConfiguration()` | `device.activeConfiguration()`; `activeConfigurationValue()` |
| `SetConfiguration`, `Reset` (+ `Async`) | `setConfiguration`, `reset` (+ `Async`) |
| `ClaimInterface`, `DetachAndClaimInterface` (+ `Async`) | `claimInterface`, `detachAndClaimInterface` (+ `Async`) |
| `DetachKernelDriver`, `AttachKernelDriver` | same, lower camel case |
| `GetDescriptor(type, index, lang, timeout)` → `byte[]` | `getDescriptor(type, index, lang, mut into[..], timeoutMs)` → bytes written |
| `GetStringDescriptor(index, lang, timeout)` | `getString(index, lang, timeoutMs)` → `String` |
| `GetStringDescriptorSupportedLanguages` | `stringLanguages(timeoutMs)` |
| `ControlIn(request, length, timeout)` → `byte[]` | `controlIn(request, mut into[..], timeoutMs)` → bytes received (no allocation) |
| `ControlOut(request, data, timeout)` | `controlOut(request, data, timeoutMs)` |
| `ControlInAsync`, `ControlOutAsync` | `controlInAsync`, `controlOutAsync` |
| `new UsbControlRequest(type, recipient, req, value, index)` | `ControlRequest.create(…)`, `ControlRequest.vendor(recipient, req, value, index)` |
| `iface.InterfaceNumber`, `GetAltSetting()` | `iface.number()`, `iface.altSetting()` |
| `SetAltSetting` (+ `Async`) | `setAltSetting` (+ `Async`) |
| `iface.Descriptors()`, `iface.Descriptor()` | `iface.altSettings()`, `iface.descriptor()` |
| `iface.OpenEndpoint(address)` | `iface.openEndpoint(address)`; `openEndpointWith(address, ring)` sets the ring size |
| `iface.OpenEndpoint(address, type, direction)` | `iface.openEndpointChecked(address, type, direction)` |

## Endpoints and transfers

| C# | CeeSafe |
|---|---|
| `ep.AllocateBuffer(n)` | `ep.allocate(n)` |
| `new UsbBuffer(n)`, `new UsbBuffer(span)` | `Buffer.create(n)`, `Buffer.from(slice)` |
| `buffer.Length`, `RequestedLength`, `Capacity` | `length()`, `requested()`, `size()` |
| `AsSpan()`, `Append`, `AppendFill`, `Clear`, `SetRequestedLength` | `view()`, `append`, `spare()` + `setLength`, `clear`, `setRequested` |
| `ep.Submit(buffer)` | `ep.submit(buffer)` → `Err(buffer)` only when the ring is full |
| `NextCompleteAsync(ct)` | `await ep.nextComplete()` |
| `WaitNextComplete(timeout)` | `ep.waitComplete(timeoutMs)` → `Completion?` |
| — | `ep.tryComplete()`: never waits |
| `completion.Status`, `ActualLength`, `NativeErrorCode`, `Buffer` | `status`, `actualLength`, `code`, `buffer` |
| `completion.ThrowIfFailed()`, `GetBufferOrThrow()` | `completion.isSuccess()`, `completion.intoBuffer()` |
| `ep.Pending` | `ep.pending()`; `ep.capacity()` is the ring size |
| `TransferBlocking(buffer, timeout)` | `ep.transfer(buffer, timeoutMs)` |
| `Read`, `Write`, `ReadAsync`, `WriteAsync` | `read`, `write`, `readAsync`, `writeAsync` |
| `CancelAll`, `ClearHalt` (+ `Async`) | `cancelAll`, `clearHalt` (+ `Async`) |
| `ep.ToReader(size)`, `reader.NumTransfers = n` | `Reader.create(ep, size, n)`, `reader.setTransfers(n)` |
| `reader.Read`, `ReadAsync`, `ReadMessageAsync` | `read(into, timeoutMs)`, `readAsync`, `readMessage` / `readMessageAsync` |
| `ep.ToWriter(size)` | `Writer.create(ep, size, n)` |
| `Write`, `WriteAsync`, `Submit`, `SubmitEnd`, `Flush`, `FlushEnd` (+ `Async`) | same, lower camel case; `flushAsync`, `flushEndAsync` |

## Errors

| C# | CeeSafe |
|---|---|
| `UsbException` (`Kind`, `NativeErrorCode`) | `UsbError` (`kind`, `code`, and `operation`) |
| `UsbDeviceException`, `UsbPermissionException` | `kind` `Disconnected`, `PermissionDenied` |
| `UsbDescriptorException` | `kind` `Other`, `operation` `ReadDescriptors` |
| `UsbTransferException` (`Status`) | a `Completion`'s `status`, or `transferError(status, code, operation)` |
| `TimeoutException` from `Read`/`Write` | `ErrorKind.TimedOut` |
| `OperationCanceledException` | `ErrorKind.Cancelled`, or `AsyncError.Cancelled` |

## Diagnostics

| C# | CeeSafe |
|---|---|
| `TraceSource("CosmoUsb")` with a listener | `CEEUSB_TRACE=error\|warn\|debug`, or `Trace.setLevel(TraceLevel.…)`: lines on stderr |

## Not carried over

- The `netstandard2.0` build (Windows-only): CeeSafe compiles per target.
- `IAsyncEnumerable` hotplug: CeeSafe has no async iterators; `await watcher.nextAsync()` in a loop is the same.
- Several selectors in one `FindDevice` call: call `findDevice` per selector.
