#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline canonical FS request/reply adapter; no device or transport access.

Input is the COMPLETE canonical packet emitted by ccci_fs_assembly_feed(), not
an individual MORE fragment. Output is a complete canonical reply, not a TX
fragment; the kernel transport must fragment it and assign HIF sequence/assert.
Opaque header metadata is preserved here except the TX channel and total size.

Unknown semantics raise UnsupportedOperation rather than a guessed reply.
Supported Find-family and recursive deletion policies are documented in
MODEM-FS-SEMANTICS.md. Vendor-uninitialized failure bytes are deliberately zeroed. This
adapter does not provide request leases, duplicate suppression,
concurrency, restart recovery, a daemon, or service-readiness certification.
"""

from dataclasses import dataclass, field
import struct

from modem_fs_backend import MAX_IO, MAX_PATH, UnsupportedOperation

MESSAGE_MAX = 0x40c8
ARGS_MAX = 6
RX_CHANNEL = 14
TX_CHANNEL = 15
MORE = 0x80000000


class MalformedRequest(ValueError):
    """Invalid canonical envelope or argument schema; never a vendor errno."""


class StaleGeneration(Exception):
    """Revoked lifecycle; caller must discard the transaction without a reply."""


@dataclass(frozen=True)
class Request:
    data0: int
    channel_sequence: int
    slot: int
    operation: int
    args: tuple = field(repr=False)


def decode_request(packet):
    """Validate all argument slices before returning immutable argument bytes."""
    if not isinstance(packet, bytes) or not 24 <= len(packet) <= MESSAGE_MAX:
        raise MalformedRequest('invalid canonical packet size')
    data0, size, channel_sequence, slot, operation, argc = struct.unpack_from(
        '<6I', packet)
    op16 = operation & 0xffff
    if (size != len(packet) or channel_sequence & 0xffff != RX_CHANNEL or
            data0 & MORE or slot >= 5 or
            operation not in (op16, 0xffff0000 | op16) or
            not 0x1001 <= op16 <= 0x1025 or argc > ARGS_MAX):
        raise MalformedRequest('invalid canonical header')
    offset = 24
    args = []
    for unused in range(argc):
        if len(packet) - offset < 4:
            raise MalformedRequest('truncated argument length')
        length, = struct.unpack_from('<I', packet, offset)
        offset += 4
        padded = (length + 3) & ~3
        if padded > len(packet) - offset:
            raise MalformedRequest('truncated argument or padding')
        args.append(packet[offset:offset + length])
        offset += padded
    if offset != len(packet):
        raise MalformedRequest('trailing canonical bytes')
    return Request(data0, channel_sequence, slot, operation, tuple(args))


def _u32(value):
    if len(value) != 4:
        raise MalformedRequest('scalar argument must contain one u32')
    return struct.unpack('<I', value)[0]


def _path(value):
    # Do not normalize, convert, truncate, or log paths at the wire boundary.
    # Full UTF-16 decoding, terminator and traversal checks belong to backend.
    if not value or len(value) > MAX_PATH or len(value) % 2:
        raise MalformedRequest('invalid wide path length')
    return value


def _count(args, count):
    if len(args) != count:
        raise MalformedRequest('invalid operation argument count')


def _length(value):
    result = _u32(value)
    if result > MAX_IO:
        raise MalformedRequest('operation exceeds bounded I/O size')
    return result


def _operation(request):
    """Decode a known operation fully, before allowing any backend call."""
    op = request.operation & 0xffff
    args = request.args
    if op == 0x1001:
        _count(args, 2)
        return op, (_path(args[0]), _u32(args[1]))
    if op == 0x1002:
        _count(args, 3)
        handle, offset, whence = map(_u32, args)
        if whence > 2:
            raise MalformedRequest('invalid seek origin')
        if offset & 0x80000000:
            offset -= 1 << 32
        return op, (handle, offset, whence)
    if op == 0x1003:
        _count(args, 2)
        return op, (_u32(args[0]), _length(args[1]))
    if op == 0x1004:
        _count(args, 3)
        handle, length = _u32(args[0]), _length(args[2])
        if len(args[1]) != length:
            raise MalformedRequest('write payload length mismatch')
        return op, (handle, args[1], length)
    if op in (0x1005, 0x1009, 0x1014):
        _count(args, 1)
        return op, (_u32(args[0]),)
    if op == 0x1007:
        _count(args, 1)
        return op, (_path(args[0]),)
    if op == 0x100c:
        _count(args, 3)
        return op, (_path(args[0]), _path(args[1]), _u32(args[2]))
    if op == 0x1021:
        _count(args, 3)
        return op, (_path(args[0]), _u32(args[1]), _u32(args[2]))
    if op == 0x1010:
        _count(args, 1)
        return op, (_path(args[0]),)
    if op == 0x100e:
        # GetDiskInfo: wide drive path plus one bounded scalar argument whose
        # exact width is not yet established; the backend ignores it.
        _count(args, 2)
        if len(args[1]) > 16:
            raise MalformedRequest('GetDiskInfo scalar too long')
        return op, (_path(args[0]), args[1])
    if op == 0x1018:
        _count(args, 2)
        return op, (_path(args[0]), _u32(args[1]))
    if op == 0x1013:
        _count(args, 2)
        capacity = _u32(args[1])
        if not capacity:
            raise MalformedRequest('Find capacity must be positive')
        return op, (_u32(args[0]), capacity)
    if op == 0x1012:
        _count(args, 4)
        if len(args[1]) != 1 or len(args[2]) != 1:
            raise MalformedRequest('FindFirst attribute/mask must contain one byte')
        capacity = _u32(args[3])
        if not capacity:
            raise MalformedRequest('Find capacity must be positive')
        return op, (_path(args[0]), args[1][0], args[2][0], capacity)
    if op == 0x1025:
        _count(args, 1)
        return op, (_path(args[0]),)
    if op == 0x101a:
        _count(args, 3)
        query = tuple(map(_u32, args))
        if query not in ((4, 2, 12), (8, 2, 12)):
            raise UnsupportedOperation('GetDrive query tuple unresolved')
        return op, query
    if op == 0x101c:
        _count(args, 0)
        return op, ()
    if op == 0x1022:
        _count(args, 2)
        name = _path(args[0])
        if len(args[1]) != 40:
            raise MalformedRequest('CMPT_Read requires ten u32 words')
        words = struct.unpack('<10I', args[1])
        # The MD has been observed sending 0x1f and 0x13; every stage bit is
        # independent, so admit the known subset and reject unknown bits.
        if words[0] & ~0x1f:
            raise UnsupportedOperation('CMPT_Read mask outside the known stages')
        if words[0] & 4 and words[6] > 2:
            raise MalformedRequest('invalid CMPT_Read seek origin')
        if words[0] & 8 and words[8] > MAX_IO:
            raise MalformedRequest('invalid CMPT_Read size')
        return op, (name, args[1])
    raise UnsupportedOperation('operation reply schema unresolved')


def _word(value):
    if type(value) is not int or not -(1 << 31) <= value <= 0xffffffff:
        raise ValueError('backend scalar outside wire range')
    return struct.pack('<I', value & 0xffffffff)


# Argument positions that carry a path, used only by the opt-in trace so a
# write payload is never mistaken for text.
_PATH_ARG = {0x1001: (0,), 0x1007: (0,), 0x100c: (0, 1), 0x1010: (0,),
             0x100e: (0,), 0x1018: (0,), 0x1012: (0,), 0x1025: (0,),
             0x1022: (0,)}


def request_summary(op, args):
    """Content-free description of one decoded FS request for the opt-in trace.

    Path arguments are shown; every other byte string is reduced to its length.
    """
    paths = _PATH_ARG.get(op, ())
    parts = []
    for index, arg in enumerate(args):
        if index in paths and isinstance(arg, bytes):
            parts.append(''.join(chr(byte) if 0x20 <= byte < 0x7f else '.'
                                 for byte in arg))
        elif op == 0x1022 and index == 1 and isinstance(arg, bytes) and len(arg) == 40:
            # The ten request words are numeric metadata, never file content.
            parts.append('words=' + ','.join('%#x' % value
                                             for value in struct.unpack('<10I', arg)))
        elif type(arg) is bytes:
            parts.append('<%d bytes>' % len(arg))
        else:
            parts.append(str(arg))
    return 'op=%#06x %s' % (op, ' '.join(parts))


def _reply_args(op, result):
    if not isinstance(result, tuple):
        raise ValueError('backend response must be a tuple')
    if op in (0x1001, 0x1002, 0x1005, 0x1007, 0x100c, 0x1010, 0x1014, 0x1018,
              0x101a, 0x101c, 0x1021):
        if len(result) != 1:
            raise ValueError('invalid backend scalar response')
        return (_word(result[0]),)
    if op in (0x1004, 0x1009):
        if len(result) != 2:
            raise ValueError('invalid backend pair response')
        return tuple(map(_word, result))
    if op == 0x1003:
        if (len(result) != 3 or not isinstance(result[2], bytes) or
                result[1] != len(result[2]) or len(result[2]) > MAX_IO):
            raise ValueError('invalid backend read response')
        return _word(result[0]), _word(result[1]), result[2]
    if op in (0x1012, 0x1013):
        if (len(result) != 3 or type(result[0]) is not int or
                not isinstance(result[1], bytes) or len(result[1]) != 52 or
                not isinstance(result[2], bytes) or len(result[2]) > MAX_PATH or
                len(result[2]) % 2):
            raise ValueError('invalid backend Find response')
        if result[0] < 0:
            if result[1] != bytes(52) or result[2] != b'':
                raise ValueError('invalid backend Find failure storage')
        elif result[2] and (len(result[2]) < 4 or result[2][-2:] != b'\0\0'):
            raise ValueError('invalid backend Find name terminator')
        return _word(result[0]), result[1], result[2]
    if op == 0x1021:
        _count(args, 3)
        return op, (_path(args[0]), _u32(args[1]), _u32(args[2]))
    if op == 0x1010:
        _count(args, 1)
        return op, (_path(args[0]),)
    if op == 0x100e:
        if len(result) != 2 or not isinstance(result[1], bytes) or len(result[1]) != 84:
            raise ValueError('invalid backend disk info response')
        return _word(result[0]), result[1]
    if op == 0x1025:
        if len(result) != 2 or not isinstance(result[1], bytes) or len(result[1]) != 24:
            raise ValueError('invalid backend file detail response')
        return _word(result[0]), result[1]
    if op == 0x1022:
        if (len(result) != 4 or not isinstance(result[0], bytes) or len(result[0]) != 8 or
                not isinstance(result[3], bytes) or result[2] != len(result[3]) or
                len(result[3]) > MAX_IO):
            raise ValueError('invalid backend CMPT_Read response')
        completed, error = struct.unpack('<Ii', result[0])
        if ((error == 0 and completed != 0x1f) or error > 0 or
                (error < 0 and completed not in (0, 1, 3, 7, 15))):
            raise ValueError('invalid backend CMPT_Read stage/error')
        return result[0], _word(result[1]), _word(result[2]), result[3]
    raise UnsupportedOperation('operation reply schema unresolved')


def respond(backend, generation, packet, trace=None):
    """Execute one complete request serially against an already-open work copy.

    The caller owns generation leases, admission, duplicate suppression, and
    stop synchronization. Lifecycle rejection never fabricates a wire response.
    ``trace`` receives the content-free request summary and the backend status.
    """
    request = decode_request(packet)
    op, args = _operation(request)
    if not backend.active or generation != backend.generation:
        raise StaleGeneration('working-copy generation revoked')
    result = backend.operate(generation, op, *args)
    if trace is not None:
        status = result[0] if isinstance(result, tuple) else result
        trace(request_summary(op, args), status)
    values = _reply_args(op, result)
    # The MD strips the response prefix on TX but requires it on every reply.
    body = bytearray(struct.pack('<II', request.operation | 0xffff0000, len(values)))
    for value in values:
        body.extend(struct.pack('<I', len(value)))
        body.extend(value)
        body.extend(b'\0' * (-len(value) % 4))
    total = 16 + len(body)
    if total > MESSAGE_MAX:
        raise ValueError('backend response exceeds canonical packet limit')
    header = struct.pack('<4I', request.data0, total,
                         request.channel_sequence & 0xffff0000 | TX_CHANNEL,
                         request.slot)
    return header + body
