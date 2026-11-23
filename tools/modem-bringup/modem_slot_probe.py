#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Query one or both SIM contexts via the Android-style basic MUX (MT6893).

Use --enter-basic once after HS2 to switch from raw AT to basic MUX.
Without --radio-on, query only. Slot results always require framed replies.
"""

import argparse
import os
import re
import select
import time

from modem_at_mux import FLAG
from modem_at_probe import AtError, FdReader, _write_retry

SLOTS = {1: 3, 2: 8}
SABM, UA, DM, DISC, UIH = 0x2f, 0x63, 0x0f, 0x43, 0xef
QUERIES = ('AT+CFUN?', 'AT+CPIN?', 'AT+CEREG?', 'AT+COPS?', 'AT+ERAT?',
           'AT+E5GOPT?', 'AT+C5GREG?')
MAX_PAYLOAD = 512
MAX_REPLY = 4096


def basic_fcs(header):
    """Reflected CRC-8 used by the vendor's basic-mode header."""
    crc = 0xff
    for byte in header:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xe0 if crc & 1 else 0)
    return crc ^ 0xff


def frame(dlci, control, payload=b''):
    length = len(payload)
    if length > MAX_PAYLOAD:
        raise ValueError('oversize MUX payload')
    if length >= 128:
        size = bytes(((length << 1) & 0xff, length >> 7))
    else:
        size = bytes((2 * length + 1,))
    header = bytes((dlci * 4 + 3, control)) + size
    return bytes((FLAG,)) + header + payload + bytes((basic_fcs(header), FLAG))


def enter_basic(reader, write):
    # Mirror the no-PIN Android startup chat. The modem can return only
    # echoes early after HS2, so SABM/UA remains the mode-switch verdict.
    for command in (b'AT', b'ATZ', b'ATE0', b'AT+CMUX=0,0,5,512'):
        data = command + b'\r\n'
        if _write_retry(write, data) != len(data):
            raise AtError('short MUX startup write')
        deadline = time.monotonic() + 0.65
        reply = b''
        while (remaining := deadline - time.monotonic()) > 0:
            ready, _, _ = select.select([reader], [], [], remaining)
            if not ready:
                break
            chunk = reader.read(1024)
            if not chunk:
                raise AtError('MUX endpoint closed during startup')
            reply = (reply + chunk)[-MAX_REPLY:]
            if b'\r\nERROR\r\n' in reply:
                raise AtError('modem rejected MUX startup chat')


class BasicMux:
    def __init__(self, reader, write):
        self.reader = reader
        self.write = write
        self.buffer = bytearray()

    def send(self, dlci, control, payload=b''):
        data = frame(dlci, control, payload)
        if _write_retry(self.write, data) != len(data):
            raise AtError('short MUX frame write')

    def receive(self, deadline):
        while time.monotonic() < deadline:
            start = self.buffer.find(FLAG)
            if start < 0:
                self.buffer.clear()
            elif start:
                del self.buffer[:start]
            if len(self.buffer) >= 4:
                address, control, size = self.buffer[1:4]
                if address == FLAG or not address & 1:
                    del self.buffer[0]
                    continue
                hlen = 4 if size & 1 else 5
                if len(self.buffer) >= hlen:
                    length = size >> 1
                    if hlen == 5:
                        length |= self.buffer[4] << 7
                    end = hlen + length
                    if length > MAX_PAYLOAD:
                        del self.buffer[0]
                        continue
                    if len(self.buffer) >= end + 2:
                        if (self.buffer[end + 1] != FLAG or
                                basic_fcs(self.buffer[1:hlen]) != self.buffer[end]):
                            del self.buffer[0]
                            continue
                        payload = bytes(self.buffer[hlen:end])
                        del self.buffer[:end + 1]  # allow a shared frame flag
                        return address >> 2, control, payload
            ready, _, _ = select.select([self.reader], [], [],
                                        max(0, deadline - time.monotonic()))
            if not ready:
                break
            chunk = self.reader.read(1024)
            if not chunk:
                raise AtError('MUX endpoint closed')
            self.buffer.extend(chunk)
        return None

    def open_channel(self, dlci, timeout):
        self.send(dlci, SABM | 0x10)
        deadline = time.monotonic() + timeout
        while received := self.receive(deadline):
            channel, control, _ = received
            kind = control & ~0x10
            if kind == SABM and channel in (0, dlci):
                self.send(channel, UA | (control & 0x10))
            if channel == dlci:
                if kind == UA:
                    return
                if kind == DM:
                    raise AtError(f'DLCI {dlci} refused')
        raise AtError(f'DLCI {dlci} did not acknowledge SABM')

    def query(self, dlci, command, timeout):
        return self.exchange(dlci, command, timeout, prefix=command[2:-1] + ':')

    def radio_on(self, dlci, timeout):
        def enabled():
            return '+CFUN: 1' in self.query(dlci, 'AT+CFUN?', timeout)

        if enabled():
            return
        try:
            self.exchange(dlci, 'AT+CFUN=1', timeout)
        except AtError:
            pass  # a lost terminal response does not mean RF stayed off
        if not enabled():
            raise AtError(f'DLCI {dlci} radio enable was not confirmed')

    def exchange(self, dlci, command, timeout, prefix=None):
        self.send(dlci, UIH, command.encode('ascii') + b'\r')
        deadline = time.monotonic() + timeout
        tail = b''
        total = 0
        lines = []
        while received := self.receive(deadline):
            channel, control, payload = received
            if channel != dlci:
                continue
            kind = control & ~0x10
            if kind in (DM, DISC):
                raise AtError(f'DLCI {dlci} closed during AT query')
            if kind != UIH:
                continue
            total += len(payload)
            if total > MAX_REPLY:
                raise AtError(f'DLCI {dlci} AT response too long')
            parts = (tail + payload).replace(b'\r', b'\n').split(b'\n')
            tail = parts.pop()
            for part in parts:
                line = part.decode('ascii', 'replace').strip()
                if line == command:
                    continue
                if line == 'OK' and prefix and not lines:
                    raise AtError(f'DLCI {dlci} returned OK without a {command} result')
                if line in ('OK', 'ERROR') or line.startswith(('+CME ERROR:', '+CMS ERROR:')):
                    return lines + [line]
                if prefix and line.startswith(prefix) and line.isascii() and line.isprintable():
                    lines.append(line)
        raise AtError(f'DLCI {dlci} timed out on {command}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--slot', type=int, choices=SLOTS, action='append')
    parser.add_argument('--enter-basic', action='store_true',
                        help='switch from raw AT on a fresh modem boot')
    parser.add_argument('--radio-on', action='store_true',
                        help='enable radio on each requested slot before querying it')
    parser.add_argument('--timeout', type=float, default=4.0)
    args = parser.parse_args()
    if not 0 < args.timeout <= 30:
        parser.error('timeout must be in (0, 30] seconds')
    fd = os.open(args.device, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
    mux = BasicMux(FdReader(fd), lambda data: os.write(fd, data))
    result = 0
    try:
        if args.enter_basic:
            enter_basic(mux.reader, mux.write)
        mux.open_channel(0, args.timeout)
        for slot in dict.fromkeys(args.slot or SLOTS):
            dlci = SLOTS[slot]
            try:
                mux.open_channel(dlci, args.timeout)
                if args.radio_on:
                    mux.radio_on(dlci, args.timeout)
                for command in QUERIES:
                    try:
                        lines = mux.query(dlci, command, args.timeout)
                    except AtError as error:
                        print(f'SIM{slot}: {error}', flush=True)
                        result = 1
                        break
                    # Omit cell and location identifiers from registration replies.
                    if command in ('AT+CEREG?', 'AT+C5GREG?'):
                        lines = [re.sub(r'^(\+C(?:5G|E)REG:\s*\d+(?:,\d+)?),.*',
                                        r'\1 [details omitted]', line) for line in lines]
                    print(f'SIM{slot} DLCI={dlci} {command}: {" | ".join(lines)}',
                          flush=True)
            except AtError as error:
                print(f'SIM{slot}: {error}', flush=True)
                result = 1
    finally:
        os.close(fd)
    return result


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (AtError, OSError) as error:
        raise SystemExit(str(error)) from None
