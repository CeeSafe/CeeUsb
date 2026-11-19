#!/bin/sh
# A virtual HID device with interrupt IN and OUT endpoints, for testing interrupt transfers without hardware:
# a configfs HID gadget on dummy_hcd's virtual controller (see README, "Verification status").
#   up:   stop g_zero (one gadget per virtual controller), create the HID gadget, bind it
#   down: unbind and remove it, start g_zero again (pattern=1)
# The report descriptor is vendor-defined (usage page 0xFF00, 64-byte input and output reports), so the host's
# kernel exposes it as a raw HID node only, never as a keyboard or mouse. The device side is /dev/hidgN: write a
# 64-byte report to send it on interrupt IN; read one to get what the host sent on interrupt OUT.
set -e
G=/sys/kernel/config/usb_gadget/ceeusb_hid
case "$1" in
up)
    modprobe libcomposite
    modprobe usb_f_hid
    rmmod g_zero 2>/dev/null || true
    mkdir -p $G
    echo 0x1d6b > $G/idVendor          # Linux Foundation
    echo 0x0104 > $G/idProduct         # Multifunction Composite Gadget
    mkdir -p $G/strings/0x409
    echo "CeeUsb" > $G/strings/0x409/manufacturer
    echo "CeeUsb interrupt test" > $G/strings/0x409/product
    mkdir -p $G/functions/hid.usb0
    echo 0 > $G/functions/hid.usb0/protocol
    echo 0 > $G/functions/hid.usb0/subclass
    echo 64 > $G/functions/hid.usb0/report_length
    # Usage Page (Vendor 0xFF00), Usage 1, Collection (Application), Logical 0..255, 8 bits x 64,
    # Usage 1 Input (Data,Var,Abs), Usage 1 Output (Data,Var,Abs), End Collection.
    printf '\x06\x00\xff\x09\x01\xa1\x01\x15\x00\x26\xff\x00\x75\x08\x95\x40\x09\x01\x81\x02\x09\x01\x91\x02\xc0' \
        > $G/functions/hid.usb0/report_desc
    mkdir -p $G/configs/c.1
    echo 100 > $G/configs/c.1/MaxPower
    ln -sf $G/functions/hid.usb0 $G/configs/c.1/
    ls /sys/class/udc | head -1 > $G/UDC
    ;;
down)
    if [ -d $G ]; then
        echo "" > $G/UDC || true
        rm -f $G/configs/c.1/hid.usb0
        rmdir $G/configs/c.1 $G/functions/hid.usb0 $G/strings/0x409 $G 2>/dev/null || true
    fi
    modprobe g_zero pattern=1
    ;;
*)
    echo "usage: $0 up|down" >&2
    exit 1
    ;;
esac
