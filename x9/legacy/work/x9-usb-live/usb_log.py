#!/usr/bin/env python3
"""Read X9 logs over EP0/libusb. Read-only; never resets the device.

New receivers support4096-byte batches (request0x47). Old receivers keep
working through64-byte frames (request0x46). Filtering affects stdout only;
--output always preserves all received log payload bytes.
"""
import argparse
import ctypes as C
import struct
import sys
import time

LIB = '/opt/homebrew/lib/libusb-1.0.dylib'
MAGIC = 0x474f4c58
REQ, SIZE = 0x46, 64
BATCH_REQ, BATCH_SIZE = 0x47, 4096


class USB:
    def __init__(self, batch=True):
        self.u = C.CDLL(LIB)
        u = self.u
        self.ctx = C.c_void_p()
        u.libusb_init.argtypes = [C.POINTER(C.c_void_p)]
        u.libusb_init.restype = C.c_int
        if u.libusb_init(C.byref(self.ctx)):
            raise RuntimeError('libusb initialization failed')
        u.libusb_open_device_with_vid_pid.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16]
        u.libusb_open_device_with_vid_pid.restype = C.c_void_p
        u.libusb_close.argtypes = [C.c_void_p]
        u.libusb_exit.argtypes = [C.c_void_p]
        u.libusb_control_transfer.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8,
            C.c_uint16, C.c_uint16, C.POINTER(C.c_ubyte), C.c_uint16, C.c_uint]
        u.libusb_control_transfer.restype = C.c_int
        self.handle = None
        self.buffer = (C.c_ubyte * BATCH_SIZE)()
        self.prefer_batch = batch
        self.batch = None if batch else False
        self.frame_size = SIZE
        self.last_ms = 0.0

    def open(self):
        if not self.handle:
            self.handle = self.u.libusb_open_device_with_vid_pid(self.ctx, 0x1209, 1)
        return bool(self.handle)

    def close_handle(self):
        if self.handle:
            self.u.libusb_close(self.handle)
            self.handle = None
        self.batch = None if self.prefer_batch else False
        self.frame_size = SIZE

    def transfer(self, cursor, request, size):
        return self.u.libusb_control_transfer(self.handle, 0xc0, request,
            cursor & 65535, cursor >> 16, self.buffer, size, 1500)

    def read(self, cursor):
        begin = time.monotonic()
        if self.prefer_batch and self.batch is not False:
            n = self.transfer(cursor, BATCH_REQ, BATCH_SIZE)
            if n == -9:  # LIBUSB_ERROR_PIPE: receiver rejected the new request.
                self.batch = False
                self.frame_size = SIZE
                print('[USB legacy mode:40 payload bytes/request; '
                    'batch needs a newer boot image]', file=sys.stderr, flush=True)
            elif n < 0:
                raise IOError('USB batch transfer: libusb status ' + str(n))
            else:
                if self.batch is None:
                    print('[USB batch mode:up to4072 payload bytes/request]',
                        file=sys.stderr, flush=True)
                self.batch = True
                self.frame_size = BATCH_SIZE
                self.last_ms = (time.monotonic() - begin) * 1000
                return bytes(self.buffer[:n])
        n = self.transfer(cursor, REQ, SIZE)
        if n < 0:
            raise IOError('USB control transfer: libusb status ' + str(n))
        self.last_ms = (time.monotonic() - begin) * 1000
        return bytes(self.buffer[:n])

    def close(self):
        self.close_handle()
        self.u.libusb_exit(self.ctx)


def parse(frame, limit=BATCH_SIZE):
    if not 24 <= len(frame) <= limit:
        raise ValueError('invalid frame length')
    magic, epoch, start, end, total, flags = struct.unpack_from('<6I', frame)
    if (magic != MAGIC or not epoch or end < start or end - start != len(frame) - 24
            or end > total or flags & ~3):
        raise ValueError('invalid log frame header')
    return epoch, start, end, total, flags, frame[24:]


class DisplayFilter:
    def __init__(self):
        self.pending = b''

    def feed(self, data):
        self.pending += data
        lines = self.pending.split(b'\n')
        self.pending = lines.pop()
        out = b''.join(line + b'\n' for line in lines if not line.startswith(b'lk50:'))
        if len(self.pending) > 65536:
            out += self.pending
            self.pending = b''
        return out

    def finish(self):
        out, self.pending = self.pending, b''
        return b'' if out.startswith(b'lk50:') else out


class Output:
    def __init__(self, raw, hide_lk, quiet):
        self.raw = raw
        self.display = DisplayFilter() if hide_lk and not quiet else None
        self.quiet = quiet
        self.pending = bytearray()
        self.raw_pending = bytearray()
        self.last_flush = time.monotonic()

    def feed(self, data):
        if self.raw:
            self.raw_pending.extend(data)
        if not self.quiet:
            self.pending.extend(self.display.feed(data) if self.display else data)

    def finish_stage(self):
        if self.display:
            self.pending.extend(self.display.finish())
        self.flush(force=True)

    def flush(self, force=False):
        now = time.monotonic()
        if not force and now - self.last_flush < .05 and max(len(self.pending), len(self.raw_pending)) < 65536:
            return
        if self.raw_pending:
            self.raw.write(self.raw_pending)
            self.raw.flush()
            self.raw_pending.clear()
        if self.pending:
            sys.stdout.buffer.write(self.pending)
            sys.stdout.buffer.flush()
            self.pending.clear()
        self.last_flush = now

    def close(self):
        self.finish_stage()
        if self.raw:
            self.raw.close()


class Statistics:
    def __init__(self, interval):
        self.interval = interval
        self.begin = time.monotonic()
        self.frames = self.received = 0
        self.transfer_ms = 0.0
        self.epoch = self.cursor = self.total = None

    def record(self, epoch, end, total, count, ms):
        self.frames += 1
        self.received += count
        self.transfer_ms += ms
        self.epoch, self.cursor, self.total = epoch, end, total

    def report(self, mode, connected, force=False):
        now = time.monotonic()
        elapsed = now - self.begin
        if not self.interval or (not force and elapsed < self.interval) or elapsed <= 0:
            return
        average = self.transfer_ms / self.frames if self.frames else 0.0
        position = ('epoch=%s cursor=%s/%s backlog=%.1fKiB' %
            (self.epoch, self.cursor, self.total, (self.total - self.cursor) / 1024)) if self.epoch else 'no frames yet'
        print('[USB %.2fKiB/s %.1fframes/s avg-control=%.2fms %s mode=%s%s]' %
            (self.received / 1024 / elapsed, self.frames / elapsed, average, position,
             mode, '' if connected else ' disconnected'), file=sys.stderr, flush=True)
        self.frames = self.received = 0
        self.transfer_ms = 0.0
        self.begin = now


def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('--interval', type=float, default=.1,
        help='idle/disconnected polling interval; never delays backlog reads (default:0.1s)')
    a.add_argument('--output', type=argparse.FileType('xb'), default=None,
        help='save unfiltered log payload to a new file; refuses an existing file')
    a.add_argument('--tail', action='store_true',
        help='start at the current end; skips existing startup logs')
    a.add_argument('--hide-lk-records', action='store_true',
        help='filter lk50 records from stdout, including shell redirection; --output stays raw')
    a.add_argument('--quiet', action='store_true',
        help='disable stdout; requires --output; rate/status still go to stderr')
    a.add_argument('--stats', type=float, default=5,
        help='rate/backlog report interval on stderr,0 disables (default:5s)')
    a.add_argument('--no-batch', action='store_true',
        help='force the legacy64-byte protocol for compatibility')
    opt = a.parse_args()
    if not .02 <= opt.interval <= 10:
        a.error('--interval must be0.02..10 seconds')
    if opt.stats != 0 and not 1 <= opt.stats <= 3600:
        a.error('--stats must be0 or1..3600 seconds')
    if opt.quiet and not opt.output:
        a.error('--quiet requires --output')
    u = USB(batch=not opt.no_batch)
    cursor = 0xffffffff if opt.tail else 0
    epoch = None
    output = Output(opt.output, opt.hide_lk_records, opt.quiet)
    stats = Statistics(opt.stats)
    connected = False
    print('[waiting for USB device1209:0001]', file=sys.stderr, flush=True)
    try:
        while True:
            output.flush()
            stats.report('batch' if u.batch else 'legacy', connected)
            if not u.open():
                if connected:
                    print('[USB disconnected; waiting]', file=sys.stderr, flush=True)
                connected = False
                time.sleep(opt.interval)
                continue
            if not connected:
                print('[USB connected; requesting log]', file=sys.stderr, flush=True)
                connected = True
            try:
                frame = u.read(cursor)
                got, start, end, total, flags, data = parse(frame, u.frame_size)
            except (IOError, ValueError) as e:
                print('[log request failed: %s; waiting for receiver]' % e,
                    file=sys.stderr, flush=True)
                output.flush(force=True)
                u.close_handle()
                connected = False
                time.sleep(1)
                continue
            if epoch is not None and got != epoch:
                print('[USB log stage changed; restarting cursor]', file=sys.stderr, flush=True)
                output.finish_stage()
                u.batch = None if u.prefer_batch else False
                cursor = 0xffffffff if opt.tail else 0
                epoch = got
                continue
            epoch = got
            if flags & 1:
                print('[USB log lost %d bytes]' % (start - cursor), file=sys.stderr, flush=True)
            if flags & 2:
                print('[USB log cursor ahead; resynchronized]', file=sys.stderr, flush=True)
            if not flags and start != cursor:
                raise ValueError('unexpected cursor change')
            output.feed(data)
            stats.record(got, end, total, len(data), u.last_ms)
            cursor = end
            if cursor >= total:
                output.flush(force=True)
                time.sleep(opt.interval)
    except KeyboardInterrupt:
        pass
    finally:
        final_mode = 'batch' if u.batch else 'legacy'
        u.close()
        output.close()
        stats.report(final_mode, connected, force=True)


if __name__ == '__main__':
    main()
