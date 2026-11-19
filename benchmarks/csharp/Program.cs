// The C# original's side of examples/bench (CosmoUsb): same operations, same output.
using System.Diagnostics;
using CosmoUsb;
using CosmoUsb.Descriptors;

static void Report(string what, long n, double ns) =>
    Console.WriteLine($"{what}: {ns / n / 1000:F2} us/op");

string mode = args.Length > 0 ? args[0] : "enum";
if (mode == "enum")
{
    long n = args.Length > 1 ? long.Parse(args[1]) : 1000;
    int devices = 0;
    for (int i = 0; i < 10; i++) devices = Usb.ListDevices().Count; // JIT warm-up
    var sw = Stopwatch.StartNew();
    for (long i = 0; i < n; i++) devices = Usb.ListDevices().Count;
    Report("enumerate", n, sw.Elapsed.TotalNanoseconds);
    Console.WriteLine($"devices: {devices}");
    return;
}
ushort vid = Convert.ToUInt16(args[1], 16), pid = Convert.ToUInt16(args[2], 16);
long count = args.Length > 3 ? long.Parse(args[3]) : 1000;
var info = Usb.FindDevice(new UsbDeviceSelector { VendorId = vid, ProductId = pid })
    ?? throw new Exception("device not found");
if (mode == "open")
{
    for (int i = 0; i < 10; i++) info.Open().Dispose();
    var sw = Stopwatch.StartNew();
    for (long i = 0; i < count; i++) info.Open().Dispose();
    Report("open+close", count, sw.Elapsed.TotalNanoseconds);
    return;
}
using var device = info.Open();
if (mode == "stream")
{
    byte address = Convert.ToByte(args[3], 16);
    int size = int.Parse(args[4]), depth = int.Parse(args[5]);
    long seconds = long.Parse(args[6]);
    using var siface = device.DetachAndClaimInterface(0);
    using var ep = siface.OpenEndpoint(address);
    ep.ClearHalt();
    bool isIn = (address & 0x80) != 0;
    for (int i = 0; i < depth; i++)
    {
        var b = ep.AllocateBuffer(size);
        if (!isIn)
        {
            b.Clear();
            var fill = b.AppendFill(size, 0);
            for (int k = 0; k < size; k++) fill[k] = (byte)(k % 512 % 63);
        }
        ep.Submit(b);
    }
    long bytes = 0;
    var st = Stopwatch.StartNew();
    while (st.Elapsed.TotalSeconds < seconds)
    {
        var c = ep.WaitNextComplete(TimeSpan.FromSeconds(2)) ?? throw new Exception("timeout");
        bytes += c.ActualLength;
        if (isIn) c.Buffer.SetRequestedLength(c.Buffer.Capacity);
        ep.Submit(c.Buffer);
    }
    Console.WriteLine($"stream: {bytes / st.Elapsed.TotalSeconds / 1e6:F0} MB/s");
    ep.CancelAll();
    while (ep.Pending > 0)
        ep.WaitNextComplete(TimeSpan.FromSeconds(2))?.Buffer.Dispose();
    return;
}
if (mode is "icontrol" or "bulkout")
{
    using var iface = device.ClaimInterface(0);
    var get = new UsbControlRequest(UsbControlType.Standard, UsbRecipient.Device, 0x06, 0x0100, 0);
    if (mode == "icontrol")
    {
        for (int i = 0; i < 10; i++) iface.ControlIn(get, 18, TimeSpan.FromSeconds(1));
        var si = Stopwatch.StartNew();
        for (long i = 0; i < count; i++) iface.ControlIn(get, 18, TimeSpan.FromSeconds(1));
        Report("interface control GET_DESCRIPTOR", count, si.Elapsed.TotalNanoseconds);
        return;
    }
    using var outEp = iface.OpenEndpoint(0x03);
    byte[] init = [0x1B, 0x40];
    for (int i = 0; i < 10; i++) outEp.Write(init, TimeSpan.FromSeconds(1));
    var sb = Stopwatch.StartNew();
    for (long i = 0; i < count; i++) outEp.Write(init, TimeSpan.FromSeconds(1));
    Report("bulk OUT write (2 bytes)", count, sb.Elapsed.TotalNanoseconds);
    return;
}
for (int i = 0; i < 10; i++) device.GetDescriptor(UsbDescriptorType.Device, 0, 0, TimeSpan.FromSeconds(1));
var s = Stopwatch.StartNew();
for (long i = 0; i < count; i++) device.GetDescriptor(UsbDescriptorType.Device, 0, 0, TimeSpan.FromSeconds(1));
Report("control GET_DESCRIPTOR", count, s.Elapsed.TotalNanoseconds);
