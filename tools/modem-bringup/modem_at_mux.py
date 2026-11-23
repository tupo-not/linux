#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""GSM 07.10 basic-mode framing for the MD AT/MUX channel.

The kernel side is a byte pipe: it owns the CCCI header and nothing else, so
this module owns the 0710 stream. Only basic mode is implemented: one control
channel (DLCI 0) carrying AT commands, SABM/UA/DM establishment and UIH data.

Every decoder bound is explicit; a malformed frame is rejected, never guessed.
No AT parsing happens here and no reply is ever synthesized.
"""
from dataclasses import dataclass

FLAG = 0xF9
ESCAPE = 0x7D
ESC_FLAG = 0x5E
ESC_ESCAPE = 0x5D

CONTROL_SABM = 0x2F
CONTROL_UA = 0x63
CONTROL_DM = 0x0F
CONTROL_DISC = 0x43
CONTROL_UIH = 0xEF

FRAME_MAX = 512
PAYLOAD_MAX = 256


class MuxError(ValueError):
    """Malformed or unsupported 0710 frame; messages carry no payload text."""


def _fcs_table():
    table = []
    for byte in range(256):
        crc = byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0xE0) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
        table.append(crc)
    return tuple(table)


FCS_TABLE = _fcs_table()


def fcs(data):
    """07.10 frame check sequence: complemented CRC-8 over the address byte."""
    crc = 0xFF
    for byte in data:
        crc = FCS_TABLE[crc ^ byte]
    return (0xFF - crc) & 0xFF


def _stuff(body):
    out = bytearray()
    for byte in body:
        if byte == FLAG:
            out += bytes((ESCAPE, ESC_FLAG))
        elif byte == ESCAPE:
            out += bytes((ESCAPE, ESC_ESCAPE))
        else:
            out.append(byte)
    return bytes(out)


def encode(dlci, control, payload=b''):
    """One framed 0710 basic-mode frame including both flags."""
    if not 0 <= dlci <= 63:
        raise MuxError('dlci out of range')
    if len(payload) > PAYLOAD_MAX:
        raise MuxError('payload too large')
    if control == CONTROL_UIH:
        body = bytes((0x03 | (dlci << 2), control, len(payload) << 1)) + payload
    else:
        body = bytes((0x03 | (dlci << 2), control))
    return bytes((FLAG,)) + _stuff(body + bytes((fcs(body),))) + bytes((FLAG,))


@dataclass(frozen=True)
class Frame:
    dlci: int
    control: int
    payload: bytes = b''


def _destuff(stream):
    out = bytearray()
    escaped = False
    for byte in stream:
        if escaped:
            if byte == ESC_FLAG:
                out.append(FLAG)
            elif byte == ESC_ESCAPE:
                out.append(ESCAPE)
            else:
                raise MuxError('invalid escape sequence')
            escaped = False
        elif byte == ESCAPE:
            escaped = True
        elif byte == FLAG:
            raise MuxError('unexpected flag inside a frame')
        else:
            out.append(byte)
    if escaped:
        raise MuxError('truncated escape sequence')
    return bytes(out)


def decode(frame):
    """Decode exactly one frame; reject anything that is not one whole frame."""
    if not isinstance(frame, (bytes, bytearray)):
        raise MuxError('frame must be bytes')
    if len(frame) < 5 or len(frame) > FRAME_MAX:
        raise MuxError('frame size out of range')
    if frame[0] != FLAG or frame[-1] != FLAG:
        raise MuxError('frame must start and end with a flag')
    body = _destuff(frame[1:-1])
    if len(body) < 3:
        raise MuxError('frame body too short')
    address, control = body[0], body[1]
    if address & 0x03 != 0x03:
        raise MuxError('unsupported address field')
    if fcs(body[:-1]) != body[-1]:
        raise MuxError('frame check sequence mismatch')
    dlci = address >> 2
    if control == CONTROL_UIH:
        if len(body) < 4:
            raise MuxError('uih frame without length')
        length = body[2] >> 1
        if body[2] & 0x01:
            raise MuxError('extended uih length is unsupported')
        if length > PAYLOAD_MAX or len(body) != 4 + length:
            raise MuxError('uih length disagrees with the frame')
        return Frame(dlci, control, body[3:3 + length])
    if len(body) != 3:
        raise MuxError('control frame with a payload')
    if control not in (CONTROL_SABM, CONTROL_UA, CONTROL_DM, CONTROL_DISC):
        raise MuxError('unsupported control field')
    return Frame(dlci, control)


def frames(stream):
    """Split a byte stream into whole frames; the tail is returned unparsed."""
    out = []
    index = 0
    size = len(stream)
    while index < size:
        start = stream.find(bytes((FLAG,)), index)
        if start < 0:
            break
        end = stream.find(bytes((FLAG,)), start + 1)
        if end < 0:
            return out, stream[start:]
        out.append(decode(stream[start:end + 1]))
        index = end + 1
    return out, stream[index:]


def printable(payload, limit=96):
    """Bounded printable view of an AT payload; nothing else is exposed."""
    text = ''.join(chr(byte) if 0x20 <= byte < 0x7f else '.' for byte in payload[:limit])
    return text
