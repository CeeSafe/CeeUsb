# Transfers

## Choosing an API

| You want | Use |
|---|---|
| one read or write, blocking | `ep.read(into, timeoutMs)`, `ep.write(data, timeoutMs)` (reuse one scratch buffer) |
| one read or write from a task | `await ep.readAsync(into)`, `await ep.writeAsync(data)` (cancel the task to cancel the transfer) |
| one transfer with your own buffer | `ep.transfer(buffer, timeoutMs)`: submit, wait, cancel on timeout, give the buffer back |
| throughput | the ring: `submit` several buffers, then `waitComplete` / `nextComplete` and resubmit |
| a byte stream | `Reader.create(ep, size, transfers)` / `Writer.create(ep, size, transfers)` |
| control | `device.controlIn/Out`, `iface.controlIn/Out` (on Windows only the interface's) |

Only bulk and interrupt endpoints open (`openEndpoint`); isochronous is not supported, as in nusb and the C#
version.

## The ring

```ceesafe
var ep = iface.openEndpointWith(0x81, 16)!;   // room for 16 transfers in flight (default 32)
for (i in 0..8) {
    var b = ep.allocate(16384)!;
    b.setRequested(16384);
    ep.submit(b)!;                            // Err(buffer) only when the ring is full
}
loop {
    let c = match (ep.waitComplete(1000)) {   // or `await ep.nextComplete()` in a task
        Some(c) => c,
        null => { break; }                    // timed out
    };
    if (!c.isSuccess()) { … c.status, c.code … }
    process(c.buffer.view());
    var b = c.buffer;
    b.clear();
    ep.submit(b)!;
}
```

- **Order.** Completions are taken in submission order, even when the OS finishes them out of order (the slot at
  the head must be done before anything is returned).
- **Submit never fails a transfer by throwing.** A refused submit (a bad length, a disconnected device) becomes
  that transfer's Completion, in order. The only refusal returned at once is a full ring, which gives the buffer
  back.
- **IN lengths** must be a nonzero multiple of the endpoint's packet size (`requested()`); anything else
  completes as `InvalidArgument` without reaching the OS. On completion `length()` is what arrived and
  `requested()` is kept, so a buffer can be resubmitted as it is.
- **Cancel** (`cancelAll`) asks the OS for every pending transfer back; they complete as `Cancelled` (with any
  partial data), in order. Linux discards newest first so that no later URB slips ahead of an earlier one.
- **Dropping an endpoint** cancels and waits for every transfer the OS still holds before freeing the ring and
  the buffers, so the OS never writes into freed memory.

## Buffers

`ep.allocate(n)` gives usbfs-mapped memory on Linux (`isZeroCopy()`): the controller reads and writes it
directly, with no copy in the kernel. If the mapping fails (old kernel, usbfs memory limit), a heap buffer is
used and a debug trace says so. `Buffer.create(n)` and `Buffer.from(data)` are heap buffers on every platform.

Fill an OUT buffer with `append(data)`, or in place through `spare()` followed by `setLength(n)`.

## Timeouts

Blocking calls take `timeoutMs` (0: no limit). A transfer that times out is cancelled and waited for, so its
buffer is home again before the call returns; the result is `ErrorKind.TimedOut`. Async calls have no timeout
argument: wrap them in `Async.timeout(ms, error, …)`, whose cancellation cancels the transfer the same way.

## Streams

- `Reader` keeps `transfers` IN transfers in flight, hands out their bytes across `read` calls of any size, and
  reports a transfer error only after the data before it has been read. `readMessage` collects transfers until
  one ends short (a short or zero-length packet). `setTransfers(0)` lets the stream run dry.
- `Writer` batches writes into transfers of `transferSize`, keeps up to `transfers` in flight, and `flushEnd`
  ends a message with a zero-length packet when its length is a whole number of packets.
- Both recycle their buffers: after warm-up a stream allocates nothing.

## Zero-length packets

A device that sends a report or message exactly filling whole packets usually follows it with a zero-length
packet. A read sized to whole packets then sees that packet as a 0-byte completion of its own: treat a 0-length
`Success` as "end of message", not as data (`examples/interrupt` does).
