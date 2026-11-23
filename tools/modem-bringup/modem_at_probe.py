#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Minimal AT client for the MD AT/MUX byte pipe.

The MD is the modem: this tool establishes the 0710 control channel, sends AT
commands and prints the MD's own answers. It never answers AT itself. Output is
bounded and printable-only, so a response line cannot leak binary payload.

--self-test runs the same state machine against an in-memory modem model, which
is the only path that is exercised without hardware.
"""
from pathlib import Path
import argparse
import os
import select
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modem_at_mux as mux  # noqa: E402

LINE_MAX = 256
READ_MAX = 1024


class AtError(RuntimeError):
    """Transport or protocol failure; messages carry no payload text."""


def _pump(buffer, frames_out, text_out):
    """Split a mixed stream: 0710 frames start with a flag, the rest is AT text.

    The same-board MD frames its echo but sends the AT answers as raw text, so
    both shapes must be handled from one stream. Returns the unconsumed tail.
    """
    while buffer:
        if buffer[0] == mux.FLAG:
            end = buffer.find(bytes((mux.FLAG,)), 1)
            if end < 0:
                return buffer
            try:
                frames_out.append(mux.decode(buffer[:end + 1]))
            except mux.MuxError:
                text_out += buffer[:end + 1]
            buffer = buffer[end + 1:]
        else:
            nxt = buffer.find(bytes((mux.FLAG,)))
            if nxt < 0:
                text_out += buffer
                return b''
            text_out += buffer[:nxt]
            buffer = buffer[nxt:]
    return b''


def _write_retry(write, data, deadline=None):
    """Write with bounded retries: EAGAIN here is queue backpressure, not a fault."""
    if deadline is None:
        deadline = time.monotonic() + 2.0
    while True:
        try:
            return write(data)
        except BlockingIOError:
            if time.monotonic() >= deadline:
                raise AtError('transmit queue stayed busy')
            time.sleep(0.05)


class FdReader:
    """select/read wrapper so the same state machine runs on a real fd or a model."""

    def __init__(self, fd, reader=os.read):
        self.fd = fd
        self.reader = reader

    def fileno(self):
        return self.fd

    def read(self, size):
        return self.reader(self.fd, size)


def _drain(reader, buffer, deadline):
    """Read whatever is available until the deadline; returns (frames, buffer)."""
    out = []
    while time.monotonic() < deadline:
        ready, _, _ = select.select([reader], [], [],
                                    max(0.0, deadline - time.monotonic()))
        if not ready:
            break
        chunk = reader.read(READ_MAX)
        if not chunk:
            raise AtError('endpoint closed')
        buffer += chunk
        if len(buffer) >= 4:
            break
    return out, buffer


def _discard(reader, quiet):
    """Drop already-queued bytes so a late answer cannot answer the next command."""
    deadline = time.monotonic() + quiet
    dropped = 0
    while time.monotonic() < deadline:
        ready, _, _ = select.select([reader], [], [],
                                    max(0.0, deadline - time.monotonic()))
        if not ready:
            break
        chunk = reader.read(READ_MAX)
        if not chunk:
            break
        dropped += len(chunk)
        deadline = time.monotonic() + quiet
    return dropped


def _split(buffer, frames):
    """Pump the raw buffer into frames plus raw text: (text, unconsumed tail)."""
    text = bytearray()
    tail = _pump(buffer, frames, text)
    return bytes(text), tail


def _take_lines(text, lines):
    """Extract complete printable lines; keep the unterminated tail."""
    while b'\n' in text:
        raw, text = text.split(b'\n', 1)
        line = mux.printable(raw.replace(b'\r', b''), LINE_MAX).strip('.')
        if line:
            lines.append(line)
    return text


def establish(reader, write, timeout):
    """0710 establishment is symmetric: send SABM, answer the peer's SABM, wait.

    The same-board MD sends its own SABM rather than a UA, so both directions
    are tracked separately and reported; DM is a refusal, never a retry.
    """
    _write_retry(write, mux.encode(0, mux.CONTROL_SABM))
    deadline = time.monotonic() + timeout
    buffer = b''
    seen_sabm = seen_ua = False
    while time.monotonic() < deadline:
        got, buffer = _drain(reader, buffer, deadline)
        _text, buffer = _split(buffer, got)
        for frame in got:
            if frame.dlci != 0:
                continue
            if frame.control == mux.CONTROL_SABM:
                seen_sabm = True
                _write_retry(write, mux.encode(0, mux.CONTROL_UA))
            elif frame.control == mux.CONTROL_UA:
                seen_ua = True
            elif frame.control == mux.CONTROL_DM:
                raise AtError('modem refused the control channel')
        if seen_sabm and seen_ua:
            return {'sabm': True, 'ua': True}
    # Silence is reported, not invented: the caller decides what to do with it,
    # because an already-established MD ignores a duplicate SABM.
    return {'sabm': seen_sabm, 'ua': seen_ua}


def command(reader, write, text, timeout):
    """Send one AT command and return the printable response lines."""
    if not text or len(text) > mux.PAYLOAD_MAX - 2:
        raise AtError('command length out of range')
    if any(not 0x20 <= ord(char) < 0x7f for char in text):
        raise AtError('command must be printable ASCII')
    _discard(reader, 0.2)
    _write_retry(write, mux.encode(0, mux.CONTROL_UIH, (text + '\r').encode('ascii')),
                 time.monotonic() + 5.0)
    deadline = time.monotonic() + timeout
    buffer = b''
    text = b''
    lines = []
    while time.monotonic() < deadline:
        got, buffer = _drain(reader, buffer, deadline)
        chunk, buffer = _split(buffer, got)
        text += chunk
        for frame in got:
            if frame.dlci == 0 and frame.control == mux.CONTROL_UIH:
                text += frame.payload
        text = _take_lines(text, lines)
        if any(line in ('OK', 'ERROR', '+CME ERROR', 'NO CARRIER') for line in lines):
            return lines
    return lines


class ModelModem:
    """In-memory stand-in for --self-test only; it is not the MD."""

    def __init__(self, send_sabm=False):
        self.send_sabm = send_sabm
        self.inbox = bytearray()
        self.outbox = bytearray()
        self.established = False
        self.commands = []

    def write(self, data):
        self.inbox += data
        got, rest = mux.frames(bytes(self.inbox))
        self.inbox = bytearray(rest)
        for frame in got:
            if frame.control == mux.CONTROL_SABM:
                self.established = True
                self.outbox += mux.encode(0, mux.CONTROL_UA)
                if self.send_sabm:
                    self.outbox += mux.encode(0, mux.CONTROL_SABM)
            elif frame.control == mux.CONTROL_UIH and self.established:
                text = frame.payload.decode('ascii', 'replace').strip()
                self.commands.append(text)
                if text == 'AT':
                    reply = b'\r\nOK\r\n'
                elif text == 'AT+CSQ':
                    reply = b'\r\n+CSQ: 20,99\r\n\r\nOK\r\n'
                elif text == 'AT+CREG?':
                    reply = b'\r\n+CREG: 0,2\r\n\r\nOK\r\n'
                else:
                    reply = b'\r\nERROR\r\n'
                self.outbox += mux.encode(0, mux.CONTROL_UIH, reply)

    def read(self, size):
        chunk = bytes(self.outbox[:size])
        del self.outbox[:len(chunk)]
        return chunk


def self_test():
    def run(model):
        rfd, wfd = os.pipe()
        reader = FdReader(rfd)

        def write(data):
            model.write(data)
            reply = model.read(READ_MAX)
            if reply:
                os.write(wfd, reply)

        summary = establish(reader, write, 1.0)
        return summary, reader, write, rfd, wfd

    # The MD answers with UA: the plain case.
    model = ModelModem()
    summary, reader, write, rfd, wfd = run(model)
    assert summary == {'sabm': False, 'ua': True} and model.established
    assert command(reader, write, 'AT', 1.0) == ['OK']
    assert command(reader, write, 'AT+CSQ', 1.0) == ['+CSQ: 20,99', 'OK']
    assert command(reader, write, 'AT+CREG?', 1.0) == ['+CREG: 0,2', 'OK']
    assert command(reader, write, 'AT+NOPE', 1.0) == ['ERROR']
    assert model.commands == ['AT', 'AT+CSQ', 'AT+CREG?', 'AT+NOPE']
    for bad in ('', 'AT\x01', 'A' * 300):
        try:
            command(reader, write, bad, 1.0)
        except AtError:
            pass
        else:
            raise AssertionError('accepted an invalid command')

    # A silent peer is reported honestly instead of being treated as ready.
    class Silent(ModelModem):
        def write(self, data):
            pass

    rfd, wfd = os.pipe()
    try:
        summary = establish(FdReader(rfd), lambda data: None, 0.2)
        assert summary == {'sabm': False, 'ua': False}
    finally:
        os.close(rfd)
        os.close(wfd)

    # The MD sends its own SABM, as this board does: both directions settle.
    model = ModelModem(send_sabm=True)
    summary, reader, write, rfd, wfd = run(model)
    assert summary == {'sabm': True, 'ua': True}
    assert command(reader, write, 'AT', 1.0) == ['OK']

    os.close(rfd)
    os.close(wfd)
    print('PASS: AT client state machine: symmetric SABM/UA, UIH round-trip,')
    print('      response lines, ERROR termination and command validation')


def dump(device, seconds, hex_bytes=0, sabm=False, send=None):
    """Read-only view of what the MD sends: dlci, control and a bounded text."""
    fd = os.open(device, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
    reader = FdReader(fd)
    buffer = b''
    seen = bytearray()
    deadline = time.monotonic() + seconds
    try:
        if sabm:
            os.write(fd, mux.encode(0, mux.CONTROL_SABM))
            sys.stdout.write('sent SABM\n')
            time.sleep(0.3)
        if send is not None:
            os.write(fd, mux.encode(0, mux.CONTROL_UIH, (send + '\r').encode('ascii')))
            sys.stdout.write('sent UIH %r\n' % send)
        while time.monotonic() < deadline:
            got, buffer = _drain(reader, buffer, min(deadline, time.monotonic() + 0.5))
            seen += buffer[len(buffer) - len(buffer):]
            for frame in got:
                sys.stdout.write('dlci=%d control=0x%02x len=%d text=%r\n'
                                 % (frame.dlci, frame.control, len(frame.payload),
                                    mux.printable(frame.payload)))
    finally:
        os.close(fd)
    if buffer:
        sys.stdout.write('tail=%d bytes unparsed\n' % len(buffer))
        if hex_bytes:
            head = buffer[:hex_bytes]
            sys.stdout.write('hex=%s\n' % ' '.join('%02x' % byte for byte in head))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', help='AT byte pipe, e.g. /dev/ccci-at-<devname>')
    parser.add_argument('--command', action='append', default=[],
                        help='AT command to send; repeatable')
    parser.add_argument('--timeout', type=float, default=3.0)
    parser.add_argument('--self-test', action='store_true',
                        help='run the client against an in-memory model')
    parser.add_argument('--dump', type=float, default=0.0, metavar='SECONDS',
                        help='only read and describe frames for this long')
    parser.add_argument('--hex', type=int, default=0, metavar='BYTES',
                        help='with --dump: also show at most this many raw bytes')
    parser.add_argument('--sabm', action='store_true',
                        help='with --dump: send SABM first to request 0710 mode')
    parser.add_argument('--send', default=None,
                        help='with --dump: send this AT text as a UIH frame first')
    args = parser.parse_args(argv)

    if args.self_test:
        self_test()
        return 0
    if args.device and args.dump:
        return dump(args.device, args.dump, args.hex, args.sabm, args.send)
    if not args.device or not args.command:
        parser.error('--device and at least one --command are required')

    fd = os.open(args.device, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
    reader = FdReader(fd)

    def write(data):
        return os.write(fd, data)

    try:
        summary = establish(reader, write, args.timeout)
        sys.stdout.write('established sabm=%d ua=%d\n' % (summary['sabm'], summary['ua']))
        for text in args.command:
            lines = command(reader, write, text, args.timeout)
            sys.stdout.write('%s -> %s\n' % (text, ' | '.join(lines) if lines else '<no reply>'))
    finally:
        os.close(fd)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
