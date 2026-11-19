# Linux

## How it works

- **Enumeration** reads `/sys/bus/usb/devices`. For each device it reads `busnum`, `devnum`, `speed`, `devpath`
  and the binary `descriptors` file (the device descriptor and every configuration, as the kernel cached them)
  in one read; interface classes come from the active configuration in that blob, and an interface's name is read
  (`<dev>:<cfg>.<n>/interface`) only when the interface has a string. Manufacturer, product and serial come from
  sysfs only when the descriptor says the device has them.
- **Opening** is `open("/dev/bus/usb/BBB/DDD", O_RDWR | O_CLOEXEC)`, one read of the same file for descriptors,
  `USBDEVFS_GET_SPEED`, and registration with the epoll loop.
- **Transfers** are URBs (`USBDEVFS_SUBMITURB`); the URB is the first field of its ring slot. The epoll thread
  reaps with `USBDEVFS_REAPURBNDELAY` until `EAGAIN` on every wakeup. `cancelAll` is `USBDEVFS_DISCARDURB`, newest
  first.
- **Control transfers** are `USBDEVFS_CONTROL`: synchronous, the timeout enforced by the kernel.
- **Buffers** from `ep.allocate` are `mmap`ed from the usbfs fd: zero-copy DMA memory.
- **Claiming** is `USBDEVFS_CLAIMINTERFACE`; `detachAndClaimInterface` is `USBDEVFS_DISCONNECT_CLAIM` with
  "except usbfs", which detaches a kernel driver and claims in one step; the driver is reattached
  (`USBDEVFS_CONNECT`) when the interface is released.
- **Hotplug** listens on the udev netlink multicast group (`NETLINK_KOBJECT_UEVENT`, group 2) on the same epoll
  thread and accepts only udev's messages ("libudev" header) for `SUBSYSTEM=usb`, `DEVTYPE=usb_device`.
  Removal events carry `DeviceId` = bus << 8 | address.

## Permissions

The user needs read-write access to `/dev/bus/usb/BBB/DDD`. A udev rule, e.g.
`/etc/udev/rules.d/70-mydevice.rules`:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", MODE="0660", TAG+="uaccess"
```

Without it, `open` fails with `PermissionDenied`.

## Limits

- LP64 only (x86-64, arm64): the ioctl numbers and struct layouts are 64-bit ones.
- `epoll_event` is packed on x86-64 and natural elsewhere; both are handled (`USB_EPOLL_PACKED`).
- usbfs limits the memory of all transfers in flight (`/sys/module/usbcore/parameters/usbfs_memory_mb`, 16 MiB by
  default). A submit beyond it completes with an error (`ENOMEM`, `Unknown`).
- A device opened right after it was auto-suspended takes the kernel's resume time (tens of ms for a hub).

## Testing without hardware

`dummy_hcd` (a virtual host and device controller) with a gadget gives real devices the kernel treats as real:

- `g_zero pattern=1`: bulk source/sink with a byte pattern in every packet, checked in both directions
  (`examples/gzero`).
- `tools/linux-hid-gadget.sh up`: a vendor-defined HID device with interrupt IN and OUT endpoints
  (`examples/interrupt`); `down` restores g_zero.

Ubuntu does not ship `dummy_hcd`; build it from the running kernel's source:

```sh
apt install linux-headers-$(uname -r)
mkdir dummy_hcd && cd dummy_hcd
curl -fsSLO https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/usb/gadget/udc/dummy_hcd.c   # your version
echo 'obj-m += dummy_hcd.o' > Makefile
make -C /lib/modules/$(uname -r)/build M=$PWD modules
modprobe libcomposite && insmod dummy_hcd.ko && modprobe g_zero pattern=1
```

The gadget's controller is a separate bus: the machine's real USB devices are untouched, and `rmmod g_zero
dummy_hcd` removes everything.
