#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pure same-board user RPC codec/providers; no device, Android or file access.

Callers explicitly supply immutable audited property facts or a complete
audited SAR policy. This is not an endpoint runner, lease owner or readiness assertion.
Unknown facts fail closed; proven missing properties must be explicitly None.
See MODEM-USER-RPC.md for ELF provenance and bounded hardening differences.
"""
from dataclasses import dataclass, field
import struct

PACKET_MAX = 2048
ARGS_MAX = 6
PROPERTY_VALUE_MAX = 91
PROPERTY = 0x400f
SAR = 0x4010


class MalformedRequest(ValueError):
    """Invalid packet/schema; no reply is generated."""


class Unavailable(Exception):
    """Audited provider/policy is unavailable; never a serialized MD errno."""


@dataclass(frozen=True)
class Request:
    data0: int
    channel_sequence: int
    slot: int
    operation: int
    args: tuple = field(repr=False)


def decode_request(packet):
    if type(packet) is not bytes or not 24 <= len(packet) <= PACKET_MAX:
        raise MalformedRequest('invalid RPC packet size')
    data0, size, channel, slot, operation, argc = struct.unpack_from('<6I', packet)
    if (size != len(packet) or channel & 0xffff != 32 or slot > 2 or
            operation > 0xffff or argc > ARGS_MAX):
        raise MalformedRequest('invalid RPC header')
    offset = 24
    args = []
    for unused in range(argc):
        if len(packet) - offset < 4:
            raise MalformedRequest('missing argument length')
        length, = struct.unpack_from('<I', packet, offset)
        offset += 4
        padded = (length + 3) & ~3
        if padded > len(packet) - offset:
            raise MalformedRequest('truncated argument or padding')
        args.append(packet[offset:offset + length])
        offset += padded
    if offset != len(packet):
        raise MalformedRequest('trailing RPC bytes')
    return Request(data0, channel, slot, operation, tuple(args))


def _name(value, *, empty=False, limit=PACKET_MAX):
    if (type(value) is not bytes or len(value) > limit or b'\0' in value or
            (not empty and not value)):
        raise ValueError('invalid immutable fact string')


@dataclass(frozen=True, repr=False)
class PropertySnapshot:
    # Tuple of (exact key bytes, exact value bytes or proven absent None).
    facts: tuple = field(repr=False)

    def __post_init__(self):
        if type(self.facts) is not tuple or len(self.facts) > 256:
            raise ValueError('invalid property snapshot')
        seen = set()
        for fact in self.facts:
            if type(fact) is not tuple or len(fact) != 2:
                raise ValueError('invalid property fact')
            key, value = fact
            _name(key)
            if key in seen:
                raise ValueError('duplicate property fact')
            seen.add(key)
            if value is not None:
                _name(value, empty=True, limit=PROPERTY_VALUE_MAX)

    def query(self, key):
        for name, value in self.facts:
            if name == key:
                return b'' if value is None else value
        raise Unavailable('property fact unavailable')


@dataclass(frozen=True, repr=False)
class SarSnapshot:
    product_hw: bytes = field(repr=False)
    regionmark: bytes = field(repr=False)
    # Complete audited policy: ((project, ((region, u32 index), ...)), ...).
    projects: tuple = field(repr=False)

    def __post_init__(self):
        _name(self.product_hw, empty=True, limit=PROPERTY_VALUE_MAX)
        _name(self.regionmark, empty=True, limit=PROPERTY_VALUE_MAX)
        if type(self.projects) is not tuple or not 1 <= len(self.projects) <= 128:
            raise ValueError('complete SAR policy required')
        seen = set()
        for project in self.projects:
            if type(project) is not tuple or len(project) != 2:
                raise ValueError('invalid SAR project')
            hardware, regions = project
            _name(hardware)
            if hardware in seen:
                raise ValueError('duplicate SAR project')
            seen.add(hardware)
            if type(regions) is not tuple or not 1 <= len(regions) <= 128:
                raise ValueError('invalid SAR region policy')
            region_seen = set()
            for entry in regions:
                if type(entry) is not tuple or len(entry) != 2:
                    raise ValueError('invalid SAR region entry')
                region, index = entry
                _name(region, empty=True, limit=PROPERTY_VALUE_MAX)
                if region in region_seen or type(index) is not int or not 0 <= index <= 0xffffffff:
                    raise ValueError('invalid SAR region/index')
                region_seen.add(region)

    def query(self):
        for hardware, regions in self.projects:
            if hardware == self.product_hw:
                for region, index in regions:
                    if region == self.regionmark:
                        return index
                break
        return 0xffff


def _encode_reply(request, args):
    payload = b''.join(struct.pack('<I', len(arg)) + arg +
                       b'\0' * (-len(arg) % 4) for arg in args)
    length = 24 + len(payload)
    if length > PACKET_MAX:
        raise MalformedRequest('reply exceeds RPC capacity')
    return struct.pack('<6I', request.data0, length,
                       request.channel_sequence & 0xffff0000 | 33,
                       request.slot, request.operation | 0xffff0000,
                       len(args)) + payload


def respond(packet, *, properties=None, sar=None):
    """Return one complete reply; HIF owns final TX sequence/assert assignment.

    Exceptions produce no bytes. No state, output mutation or transport action
    is performed. An absent provider means unavailable, never a default
    reply. Snapshot construction is the caller's explicit audit assertion.
    """
    request = decode_request(packet)
    if request.operation == PROPERTY:
        if not request.args:
            raise MalformedRequest('property key argument required')
        key = request.args[0]
        if key.endswith(b'\0'):
            key = key[:-1]
        if not key or b'\0' in key:
            raise MalformedRequest('invalid property key')
        if type(properties) is not PropertySnapshot:
            raise Unavailable('property provider unavailable')
        value = properties.query(key)
        return _encode_reply(request, (struct.pack('<i', len(value)), value + b'\0'))
    if request.operation == SAR:
        if type(sar) is not SarSnapshot:
            raise Unavailable('SAR provider unavailable')
        return _encode_reply(request, (struct.pack('<i', 0), struct.pack('<I', sar.query())))
    raise Unavailable('operation is not an implemented user RPC')
