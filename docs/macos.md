# macOS

## How it works

- **Enumeration** iterates `IOUSBHostDevice` services. Each entry's properties are copied once
  (`IORegistryEntryCreateCFProperties`) and read from the dictionary; interfaces are the entry's children in the
  IOService plane. `DeviceId` is the registry entry ID. Buses are `AppleUSBHostController` services.
- **Opening** finds the service with `IORegistryEntryIDMatching` and makes an `IOUSBDeviceInterface650` user
  client (`IOCreatePlugInInterfaceForService` + `QueryInterface`), whose COM vtable is called directly from
  CeeSafe (`DeviceVtbl` in `platform/macos.cee`). Configuration descriptors are copied from IOKit's cache. The
  device is opened for exclusive access if it can be (`USBDeviceOpen`); if not, descriptors still work and the
  calls that need exclusivity (`setConfiguration`, `reset`) ask again.
- **Control transfers** are `DeviceRequestTO`, synchronous; the device needs no async event source. Standard
  descriptor requests are often answered from IOKit's cache, in microseconds.
- **Claiming** finds the interface service (`CreateInterfaceIterator`), makes an `IOUSBInterfaceInterface700`,
  creates its async event source, adds it to the run loop and opens the interface.
- **Transfers** are `ReadPipeAsync` / `WritePipeAsync` on the pipe that matches the endpoint address
  (`GetPipeProperties`), with the slot as `refcon`; the completion callout runs on the run loop thread.
  `cancelAll` is `AbortPipe`; `clearHalt` is `ClearPipeStallBothEnds`.
- **Hotplug** registers `IOServiceFirstMatch` and `IOServiceTerminate` notifications for `IOUSBHostDevice` on a
  notification port whose source joins the run loop.

## Linking

IOKit and CoreFoundation are linked by the manifest (`frameworks` in `[target.macos]`), so programs that use the
package link them with no flags.

## Notes

- Vendor-class devices may come up unconfigured; `device.setConfiguration(1)` before claiming (the samples do it
  when `activeConfiguration()` fails).
- Devices or interfaces claimed by a kernel driver (HID keyboards, mass storage) cannot be opened from user space;
  there is no detach on macOS, and `detachAndClaimInterface` is the same as `claimInterface`.
- `reset` re-enumerates the device: it comes back with a new registry ID, so find it again.
- A device that drops off the bus and returns gets a new registry ID each time: a hotplug watcher sees each
  instance's arrival and removal separately.
