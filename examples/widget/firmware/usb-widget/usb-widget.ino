// A custom USB device on an ATmega32U4, built to be driven from host-side USB
// code rather than from a serial terminal.
//
// The board already presents CDC serial (that is how it is programmed). This
// adds a THIRD interface alongside it, using Arduino's PluggableUSB hooks:
//
//   interface 0,1  CDC        the usual serial port, kept for debug output
//   interface 2    vendor     class 0xFF, two 64-byte bulk endpoints
//
// Class 0xFF means no operating system driver claims it, which is exactly what
// makes it available to user-space USB libraries.
//
// ---------------------------------------------------------------------------
// Control requests
//
// bmRequestType is vendor + interface:
//   device-to-host  0xC1
//   host-to-device  0x41
// wIndex MUST be the vendor interface number. Requests with wIndex 0 are
// routed to the CDC interface by the core and never reach this code.
//
//   0x01 GET_INFO     IN    ASCII identification string
//   0x02 SET_LED      OUT   no data; wValue 0 = off, non-zero = on
//   0x03 GET_LED      IN    1 byte, current LED state
//   0x04 GET_UPTIME   IN    4 bytes, millis(), little endian
//   0x05 GET_ECHOES   IN    4 bytes, bulk packets echoed so far
//   0x10 SET_FAULT    OUT   no data; wValue selects a fault mode
//   0x11 GET_FAULT    IN    1 byte, current fault mode
//
// Fault injection
//
// Real peripherals cannot be asked to misbehave, which makes host-side error
// paths hard to test. SET_FAULT makes this device misbehave deliberately:
//
//   0  none       behave correctly
//   1  stall      STALL every vendor control request except SET/GET_FAULT
//   2  short      answer control IN transfers with half the bytes asked for
//   3  silent     accept bulk writes and never reply, to exercise read timeouts
//   4  truncate   echo back only half of each bulk packet
//   5  detach     drop off the bus for ~1.5s, then re-enumerate
//
// SET_FAULT and GET_FAULT are always honoured, so stall mode stays recoverable
// without unplugging the board.
//
// Bulk endpoints
//
//   Write bytes to the OUT endpoint and the device returns them unchanged on
//   the IN endpoint. GET_ECHOES proves the device really handled them rather
//   than the host looping back to itself.
// ---------------------------------------------------------------------------

#include <PluggableUSB.h>
#include <USBCore.h>

static const int LED = LED_BUILTIN;  // pin 13
static const char INFO_STRING[] = "usb-widget/1.0 atmega32u4";

// Vendor request codes.
static const uint8_t REQ_GET_INFO = 0x01;
static const uint8_t REQ_SET_LED = 0x02;
static const uint8_t REQ_GET_LED = 0x03;
static const uint8_t REQ_GET_UPTIME = 0x04;
static const uint8_t REQ_GET_ECHOES = 0x05;
static const uint8_t REQ_SET_FAULT = 0x10;
static const uint8_t REQ_GET_FAULT = 0x11;

static const uint8_t FAULT_NONE = 0;
static const uint8_t FAULT_STALL = 1;
static const uint8_t FAULT_SHORT = 2;
static const uint8_t FAULT_SILENT = 3;
static const uint8_t FAULT_TRUNCATE = 4;
static const uint8_t FAULT_DETACH = 5;

static volatile uint8_t faultMode = FAULT_NONE;
static volatile bool detachRequested = false;

// Touched from the USB interrupt, so anything loop() also reads is volatile.
static volatile bool ledOn = false;
static volatile uint32_t echoCount = 0;
static volatile bool ledChanged = false;

// Sends a control IN payload, deliberately truncated in fault mode 2. A host
// must cope with a device returning fewer bytes than wLength asked for.
static int sendControl(const void *data, int length) {
  if (faultMode == FAULT_SHORT) {
    length /= 2;
  }
  return USB_SendControl(0, data, length);
}

// The descriptor block this interface contributes to the configuration.
typedef struct {
  InterfaceDescriptor iface;
  EndpointDescriptor in;
  EndpointDescriptor out;
} VendorDescriptor;

class VendorInterface : public PluggableUSBModule {
 public:
  VendorInterface() : PluggableUSBModule(2, 1, epTypes) {
    epTypes[0] = EP_TYPE_BULK_IN;
    epTypes[1] = EP_TYPE_BULK_OUT;
    PluggableUSB().plug(this);
  }

  uint8_t interfaceNumber() const { return pluggedInterface; }
  uint8_t inEndpoint() const { return pluggedEndpoint; }
  uint8_t outEndpoint() const { return pluggedEndpoint + 1; }

 protected:
  int getInterface(uint8_t *interfaceCount) {
    *interfaceCount += 1;
    VendorDescriptor desc = {
        // class 0xFF, subclass 0, protocol 0: vendor specific, no OS driver.
        D_INTERFACE(pluggedInterface, 2, 0xFF, 0x00, 0x00),
        D_ENDPOINT(USB_ENDPOINT_IN(pluggedEndpoint), USB_ENDPOINT_TYPE_BULK, 64, 0),
        D_ENDPOINT(USB_ENDPOINT_OUT(pluggedEndpoint + 1), USB_ENDPOINT_TYPE_BULK, 64, 0),
    };
    return USB_SendControl(0, &desc, sizeof(desc));
  }

  // No class-specific descriptors (an HID report descriptor would go here).
  int getDescriptor(USBSetup &setup) {
    (void)setup;
    return 0;
  }

  // Runs in USB interrupt context. Keep it short, and never call Serial from
  // here -- printing inside an ISR is a good way to deadlock the USB stack.
  bool setup(USBSetup &setup) {
    if ((setup.bmRequestType & REQUEST_TYPE) != REQUEST_VENDOR) {
      return false;
    }
    if (setup.wIndex != pluggedInterface) {
      return false;
    }

    bool toHost = (setup.bmRequestType & REQUEST_DIRECTION) == REQUEST_DEVICETOHOST;

    // Handled before the stall check so a stalling device can still be told to
    // stop stalling. Otherwise mode 1 would need a physical reset to undo.
    if (setup.bRequest == REQ_SET_FAULT) {
      if (toHost) return false;
      faultMode = (uint8_t)setup.wValueL;
      if (faultMode == FAULT_DETACH) {
        detachRequested = true;
        faultMode = FAULT_NONE;
      }
      return true;
    }
    if (setup.bRequest == REQ_GET_FAULT) {
      if (!toHost) return false;
      uint8_t mode = faultMode;
      USB_SendControl(0, &mode, sizeof(mode));
      return true;
    }

    // Returning false makes the core STALL the control endpoint, which is what
    // a real device does when it cannot satisfy a request.
    if (faultMode == FAULT_STALL) {
      return false;
    }

    switch (setup.bRequest) {
      case REQ_GET_INFO:
        if (!toHost) return false;
        sendControl(INFO_STRING, sizeof(INFO_STRING) - 1);
        return true;

      case REQ_SET_LED:
        if (toHost) return false;
        ledOn = setup.wValueL != 0;
        ledChanged = true;
        return true;  // no data stage; the core sends the status packet

      case REQ_GET_LED: {
        if (!toHost) return false;
        uint8_t value = ledOn ? 1 : 0;
        sendControl(&value, sizeof(value));
        return true;
      }

      case REQ_GET_UPTIME: {
        if (!toHost) return false;
        uint32_t ms = millis();
        sendControl(&ms, sizeof(ms));
        return true;
      }

      case REQ_GET_ECHOES: {
        if (!toHost) return false;
        uint32_t n = echoCount;
        sendControl(&n, sizeof(n));
        return true;
      }
    }
    return false;
  }

 private:
  uint8_t epTypes[2];
};

static VendorInterface vendor;

// Moves whatever arrived on the OUT endpoint straight back out the IN one.
static void pollBulk() {
  int available = USB_Available(vendor.outEndpoint());
  if (available <= 0) {
    return;
  }

  uint8_t buffer[64];
  int n = USB_Recv(vendor.outEndpoint(), buffer, min(available, (int)sizeof(buffer)));
  if (n <= 0) {
    return;
  }

  echoCount++;

  // Consume the packet but never answer it, so the host's read times out while
  // the counter still proves the data arrived.
  if (faultMode == FAULT_SILENT) {
    return;
  }

  int reply = (faultMode == FAULT_TRUNCATE) ? n / 2 : n;
  if (reply > 0) {
    USB_Send(vendor.inEndpoint() | TRANSFER_RELEASE, buffer, reply);
  }
  // Deliberately no Serial output here. A CDC write blocks when the host is
  // not draining the port, which would stall the echo path for whoever is
  // driving the vendor interface.
}

void setup() {
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);

  Serial.begin(115200);
  unsigned long started = millis();
  while (!Serial && millis() - started < 3000) {
  }

  Serial.println();
  Serial.println(F("usb-widget ready"));
  Serial.print(F("vendor interface: "));
  Serial.println(vendor.interfaceNumber());
  Serial.print(F("bulk IN endpoint: "));
  Serial.println(vendor.inEndpoint());
  Serial.print(F("bulk OUT endpoint: "));
  Serial.println(vendor.outEndpoint());
}

void loop() {
  // Done here rather than in the interrupt so the triggering request can finish
  // its status stage before the device disappears from the bus.
  if (detachRequested) {
    detachRequested = false;
    delay(50);
    USBDevice.detach();
    delay(1500);
    USBDevice.attach();
  }

  pollBulk();

  if (ledChanged) {
    ledChanged = false;
    digitalWrite(LED, ledOn ? HIGH : LOW);
  }
  // Nothing in loop() writes to Serial. A CDC write stalls when the host is
  // not draining the port, and stalling here would stop servicing the vendor
  // interface -- control requests would keep working, because those run in the
  // USB interrupt, while bulk transfers silently died.
}
