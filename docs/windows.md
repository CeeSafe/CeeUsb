# Windows

## How it works

- **Enumeration** lists device interfaces of `GUID_DEVINTERFACE_USB_DEVICE` and `GUID_DEVINTERFACE_USB_HUB`
  (cfgmgr32), resolves each to its device node, and asks the parent hub's driver about the port
  (`IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`, and `…_EX_V2` for high-speed devices, to tell SuperSpeed
  apart): device descriptor, speed, address, active configuration. The serial string and the active configuration
  descriptor come from the hub driver's cache (`IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION`), so enumeration does
  no device I/O. Each hub is opened once per listing. Product names are `DEVPKEY_Device_BusReportedDeviceDesc`; a
  composite device's interface names come from its child functions. `DeviceId` is a 64-bit hash of the device's
  instance ID (case-insensitive): stable while the device is connected, and the same in its removal event. (The C#
  version used the device node handle, which Windows reassigns when a removed device is looked up, so removals never
  matched arrivals.)
- **Opening** re-reads the port and the configurations through the hub (the info may be stale). Nothing needs
  WinUSB until an interface is claimed; `device.getDescriptor` and the string calls go through the hub too.
- **Claiming** opens the WinUSB file that covers the interface: the device's own path when WinUSB is bound to the
  whole device, or the composite function's path (from its registry `DeviceInterfaceGUIDs`) when `usbccgp` splits
  it. Interfaces of one function share the file; an interface that is not the function's first is reached with
  `WinUsb_GetAssociatedInterface`. The file is associated with the library's completion port.
- **Transfers** are `WinUsb_ReadPipe` / `WinUsb_WritePipe` with the slot's OVERLAPPED; the completion thread turns
  each dequeued OVERLAPPED back into its slot. IN pipes get `RAW_IO` (requests go to the controller as they are;
  whole packets, which submit already requires). `cancelAll` is `WinUsb_AbortPipe`; `clearHalt` is
  `WinUsb_ResetPipe`.
- **Control transfers** are `WinUsb_ControlTransfer` on the claimed interface, synchronous, with the control
  pipe's `PIPE_TRANSFER_TIMEOUT` set to the call's timeout when it changes (0: none; WinUSB's default of 5 s is
  cleared at claim).
- **Hotplug** is `CM_Register_Notification` for USB device interfaces; the callback runs on a thread-pool thread.

## Binding WinUSB

A device (or composite function) must be bound to WinUSB to be claimed. Options, simplest first:

1. Firmware with a Microsoft OS descriptor (WCID) that asks for WinUSB: no setup on the host.
2. Zadig (GUI): pick the device, pick WinUSB, install.
3. An INF that points the hardware ID at the in-box WinUSB, installed with `pnputil`. Windows refuses an unsigned
   package; on a machine without driver-signing tools, a self-signed catalog does it:

```powershell
# cxprinter.inf: Class=USBDevice, ClassGuid={88BAE032-5A81-49f0-BC3D-A4FF138216D6},
#   [Std.NTamd64] %DESC% = USB_Install, USB\VID_0483&PID_5840
#   [USB_Install] Include=winusb.inf, Needs=WINUSB.NT; [USB_Install.Services] Include=winusb.inf, Needs=WINUSB.NT.Services
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=WinUSB binding' -CertStoreLocation Cert:\LocalMachine\My `
  -KeyUsage DigitalSignature -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
Export-Certificate -Cert $cert -FilePath C:\winusb\binding.cer
certutil -addstore -f Root C:\winusb\binding.cer; certutil -addstore -f TrustedPublisher C:\winusb\binding.cer
New-FileCatalog -Path C:\winusb -CatalogFilePath C:\winusb\cxprinter.cat -CatalogVersion 2.0
Set-AuthenticodeSignature -FilePath C:\winusb\cxprinter.cat -HashAlgorithm SHA256 -Certificate $cert
pnputil /add-driver C:\winusb\cxprinter.inf /install
foreach ($s in 'My','Root','TrustedPublisher') { certutil -delstore -f $s $cert.Thumbprint }   # the binding stays
```

To undo: `pnputil /delete-driver oemNN.inf /uninstall /force` (the published name `pnputil` printed); the
device returns to its original driver.

## Limits (WinUSB's)

- No `setConfiguration`, no `reset`, no kernel-driver detach (`Unsupported`, or a no-op for detach).
- Control transfers only through a claimed interface; an interface-recipient request must carry that interface's
  number in `index` (`InvalidArgument` otherwise).
- The manufacturer string is not cached by the OS, so `DeviceInfo.manufacturer` is null.
- Hub descriptor requests are capped at 4095 bytes.
