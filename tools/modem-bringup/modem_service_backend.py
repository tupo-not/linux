#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Explicit experimental FS endpoint runner, not a modem launch/ready service.

The 32-byte record header is LE <IIQQII>: version, kind, generation, token,
slot, canonical packet length. Each read/write must transfer ONE WHOLE record.
There is no stream reassembly or retry after a short read/write. Any unsupported
request or failure closes the endpoint so the kernel can revoke the lease.

Only the caller's marked private WorkingCopy is used. User RPC requires an
explicit immutable audited policy file; no Android properties are read. No operation logs paths, packet bytes, contents or exception text.
The caller must explicitly provide --endpoint, --root and --generation; this
program does not start the modem, provision data, or claim services are ready.
--trace-rpc-names and --trace-fs are the only opt-in exceptions: they print the
request operation, a bounded printable name or path, and the backend status so
the operator can author an honest policy and follow the observed sequence.
Payload bytes are never printed and both traces are off by default.
"""

import argparse
from dataclasses import dataclass, field
from functools import partial
import json
import os
import stat
import struct
import sys

from modem_fs_backend import WorkingCopy, UnsupportedOperation
from modem_fs_wire import MESSAGE_MAX, StaleGeneration, respond
from modem_rpc_backend import PropertySnapshot, SarSnapshot, decode_request as rpc_decode_request
from modem_rpc_backend import respond as rpc_respond

VERSION = 1
FS = 0
RPC = 1
HEADER = struct.Struct('<IIQQII')
RPC_MAX = 2048
RECORD_MAX = HEADER.size + MESSAGE_MAX
TRACE_NAME_MAX = 64


class RecordError(ValueError):
    """Malformed endpoint record; messages contain no untrusted input."""


@dataclass(frozen=True)
class Record:
    kind: int
    generation: int
    token: int
    slot: int
    packet: bytes = field(repr=False)


def _generation(value):
    if type(value) is not int or not 0 < value <= 0xffffffffffffffff:
        raise RecordError('invalid expected generation')


def decode_record(data, expected_generation):
    """Reject stale/malformed metadata before any backend file operation."""
    _generation(expected_generation)
    if not isinstance(data, bytes) or not HEADER.size + 24 <= len(data) <= RECORD_MAX:
        raise RecordError('invalid record size')
    version, kind, generation, token, slot, length = HEADER.unpack_from(data)
    if version != VERSION or kind not in (FS, RPC) or token == 0:
        raise RecordError('invalid record metadata')
    if generation != expected_generation:
        raise StaleGeneration('record generation revoked')
    limit, slots, channel = ((MESSAGE_MAX, 5, 14) if kind == FS else (RPC_MAX, 3, 32))
    if length < 24 or length > limit or slot >= slots or len(data) != HEADER.size + length:
        raise RecordError('invalid record bounds')
    packet = data[HEADER.size:]
    data0, canonical_length, channel_sequence, canonical_slot = struct.unpack_from('<4I', packet)
    if ((kind == FS and data0 & 0x80000000) or canonical_length != length or
            channel_sequence & 0xffff != channel or canonical_slot != slot):
        raise RecordError('canonical header and record disagree')
    return Record(kind, generation, token, slot, packet)


def rpc_request_name(packet):
    """Operation plus the bounded printable first-argument name.

    This is the explicit opt-in diagnostic used to author an honest policy; it
    returns no reply, reads no value and hides every non-printable byte.
    """
    request = rpc_decode_request(packet)
    name = request.args[0] if request.args else b''
    if name.endswith(b'\0'):
        name = name[:-1]
    text = ''.join(chr(byte) if 0x20 <= byte < 0x7f else '.' for byte in name[:TRACE_NAME_MAX])
    return request.operation, text


def _reply(record, packet):
    limit, channel = (MESSAGE_MAX, 15) if record.kind == FS else (RPC_MAX, 33)
    if not isinstance(packet, bytes) or not 24 <= len(packet) <= limit:
        raise RecordError('invalid canonical response size')
    length, sequence, slot, operation = struct.unpack_from('<4I', packet, 4)
    request_operation, = struct.unpack_from('<I', record.packet, 16)
    expected_operation = request_operation | 0xffff0000
    if (length != len(packet) or sequence & 0xffff != channel or slot != record.slot or
            operation != expected_operation or packet[:4] != record.packet[:4]):
        raise RecordError('invalid canonical response metadata')
    return HEADER.pack(VERSION, record.kind, record.generation, record.token,
                       record.slot, len(packet)) + packet


def serve(backend, generation, fd, *, rpc=None, trace=None, fs_trace=None,
          reader=os.read, writer=os.write):
    """Own/close fd; serve serial atomic records until EOF or an exception.

    The caller owns backend shutdown. Generation mismatch, malformed records,
    unsupported semantics, and short writes propagate AFTER fd closure. There
    are no payload logs or fallback replies. Endpoint closure revokes in-flight
    requests; this function never retries a potentially published response.
    ``trace`` receives the packet of each RPC request before it is answered and
    ``fs_trace`` receives each FS request summary and backend status.
    """
    count = 0
    try:
        _generation(generation)
        while True:
            # One extra byte detects an oversized atomic record instead of
            # accidentally accepting its valid-looking truncated prefix.
            data = reader(fd, RECORD_MAX + 1)
            if data == b'':
                return count
            record = decode_record(data, generation)
            if record.kind == FS:
                packet = respond(backend, generation, record.packet, trace=fs_trace)
            else:
                # The opt-in name trace runs even without a provider so the
                # operator can author a policy from real requests.
                if trace is not None:
                    trace(record.packet)
                if rpc is None:
                    raise UnsupportedOperation('RPC policy is not configured')
                packet = rpc(record.packet)
            reply = _reply(record, packet)
            if writer(fd, reply) != len(reply):
                raise RecordError('short endpoint write')
            count += 1
    finally:
        os.close(fd)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise RecordError('duplicate policy field')
        result[key] = value
    return result


def _policy_bytes(value):
    if type(value) is not str:
        raise RecordError('policy string required')
    return value.encode('utf-8')


def load_rpc_policy(path):
    """Load one immutable policy, before opening the kernel endpoint.

    The caller asserts provenance/completeness; permissions alone do not prove
    facts. No path, property, SAR value or parser exception is logged here.
    """
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or
                not 0 < info.st_size <= 65536):
            raise RecordError('invalid private policy file')
        chunks = []
        total = 0
        while True:
            chunk = os.read(fd, 65537 - total)
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > 65536:
                raise RecordError('policy exceeds size limit')
        if total != info.st_size:
            raise RecordError('policy size changed')
    finally:
        os.close(fd)
    config = json.loads(b''.join(chunks), object_pairs_hook=_unique_object)
    if (type(config) is not dict or type(config.get('version')) is not int or
            config['version'] != 1 or set(config) - {'version', 'properties', 'sar'}):
        raise RecordError('invalid policy schema')
    properties = None
    if 'properties' in config:
        values = config['properties']
        if type(values) is not dict:
            raise RecordError('invalid property namespace')
        properties = PropertySnapshot(tuple((_policy_bytes(k), None if v is None else
                                             _policy_bytes(v)) for k, v in values.items()))
    sar = None
    if 'sar' in config:
        values = config['sar']
        if (type(values) is not dict or
                set(values) != {'product_hw', 'regionmark', 'projects'} or
                type(values['projects']) is not dict):
            raise RecordError('invalid SAR policy')
        projects = []
        for hardware, regions in values['projects'].items():
            if type(regions) is not dict:
                raise RecordError('invalid SAR region policy')
            projects.append((_policy_bytes(hardware), tuple((_policy_bytes(region), index)
                                                          for region, index in regions.items())))
        sar = SarSnapshot(_policy_bytes(values['product_hw']), _policy_bytes(values['regionmark']),
                          tuple(projects))
    return partial(rpc_respond, properties=properties, sar=sar)


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's normal errors may repeat an arbitrary private path/value.
        self.exit(2, 'invalid backend arguments\n')


def main(argv=None):
    parser = _ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint', required=True, help='explicit kernel endpoint node')
    parser.add_argument('--root', required=True, help='existing marked private working copy')
    parser.add_argument('--generation', required=True, help='explicit nonzero generation')
    parser.add_argument('--rpc-policy', help='explicit private audited user RPC policy JSON')
    parser.add_argument('--trace-rpc-names', action='store_true',
                        help='opt-in: print RPC operation and bounded request name')
    parser.add_argument('--trace-fs', action='store_true',
                        help='opt-in: print FS operation, path and backend status')
    args = parser.parse_args(argv)
    backend = None
    fd = -1
    status = 0
    trace = None
    fs_trace = None
    if args.trace_rpc_names:
        def trace(packet):
            operation, name = rpc_request_name(packet)
            sys.stderr.write('rpc request op=%#06x name=%s\n' % (operation, name))
            sys.stderr.flush()
    if args.trace_fs:
        def fs_trace(summary, status):
            # Only a scalar status is printed; anything else is reduced to its
            # type name so no backend payload can leak into the trace.
            text = status if isinstance(status, str) else (
                '%d' % status if type(status) is int else '<%s>' % type(status).__name__)
            sys.stderr.write('fs %s status=%s\n' % (summary, text))
            sys.stderr.flush()
    try:
        generation = int(args.generation, 0)
        _generation(generation)
        rpc = load_rpc_policy(args.rpc_policy) if args.rpc_policy else None
        backend = WorkingCopy(args.root, generation)
        fd = os.open(args.endpoint, os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
        if not stat.S_ISCHR(os.fstat(fd).st_mode):
            raise RecordError('endpoint must be a character device')
        owned_fd, fd = fd, -1
        serve(backend, generation, owned_fd, rpc=rpc, trace=trace, fs_trace=fs_trace)
    except KeyboardInterrupt:
        sys.stderr.write('modem backend interrupted\n')
        status = 130
    except Exception as error:
        # Class name only: it is bounded and carries no path or payload text.
        sys.stderr.write('modem backend stopped after an error (%s)\n'
                         % type(error).__name__)
        status = 1
    finally:
        cleanup_error = False
        if fd >= 0:
            try:
                os.close(fd)
            except Exception:
                cleanup_error = True
        if backend is not None:
            try:
                cleanup_error |= bool(backend.shutdown())
            except Exception:
                cleanup_error = True
        if cleanup_error and not status:
            sys.stderr.write('modem backend cleanup failed\n')
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
