#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Probe IPv6 after HS2, PDP activation and DPMAIF ring setup.

Run as root on the device with ccmnidp0 down. The probe reports no carrier
addresses and removes its temporary address, route and interface settings.
"""
import argparse
import ipaddress
import json
from pathlib import Path
import re
import secrets
import socket
import struct
import subprocess
import time

IFACE = 'ccmnidp0'
RESOLVER = '2001:4860:4860::8888'
UL_STATUS = Path('/sys/devices/platform/10014000.dpmaif/ul')


def pdp_iid(output):
    match = re.search(r'\+CGPADDR:\s*1,([^\n]+)', output)
    if match:
        for field in re.findall(r'"([0-9.]+)"', match[1]):
            octets = field.split('.')
            if len(octets) == 16:
                iid = int.from_bytes(bytes(map(int, octets)), 'big')
                if 0 < iid < 2**64:
                    return iid
    raise ValueError('expected the 64-bit PDP interface identifier in AT+CGPADDR=1')


def ip(*args, check=True):
    return subprocess.run(['ip', *args], check=check, capture_output=True, text=True)


def bind_interface(sock):
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, IFACE.encode() + b'\0')


def ra_prefix(message):
    pos = 16
    while pos + 2 <= len(message):
        size = message[pos + 1] * 8
        if not size or pos + size > len(message):
            break
        opt = message[pos:pos + size]
        if (opt[0] == 3 and size == 32 and opt[2] == 64 and opt[3] & 64
                and int.from_bytes(opt[4:8], 'big')):
            prefix = ipaddress.IPv6Address(opt[16:32])
            if prefix.is_global and not prefix.is_multicast:
                return int(prefix) >> 64
        pos += size
    return None


def solicit(link_local, report, attempts=3):
    idx = socket.if_nametoindex(IFACE)
    with socket.socket(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_ICMPV6) as sock:
        bind_interface(sock)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, idx)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 255)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_RECVHOPLIMIT, 1)
        sock.bind((str(link_local), 0, 0, idx))
        for _ in range(attempts):
            sock.sendto(b'\x85' + b'\0' * 7, ('ff02::2', 0, 0, idx))
            deadline = time.monotonic() + 3
            while (remaining := deadline - time.monotonic()) > 0:
                sock.settimeout(remaining)
                try:
                    msg, ancillary, _, peer = sock.recvmsg(2048, 256)
                except TimeoutError:
                    break
                if len(msg) < 16 or msg[:2] != b'\x86\0':
                    continue
                if not ipaddress.IPv6Address(peer[0]).is_link_local:
                    continue
                if not any(level == socket.IPPROTO_IPV6 and kind == socket.IPV6_HOPLIMIT
                           and struct.unpack('i', data)[0] == 255
                           for level, kind, data in ancillary):
                    continue
                report['ra_received'] = True
                prefix = ra_prefix(msg)
                if prefix is not None:
                    return prefix
    return None


def dns_reply(resolver=RESOLVER):
    token = secrets.randbits(16)
    question = b'\x03www\x07example\x03com\x00\x00\x01\x00\x01'
    family = socket.AF_INET6 if ipaddress.ip_address(resolver).version == 6 else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as sock:
        bind_interface(sock)
        sock.settimeout(5)
        sock.connect((resolver, 53))
        sock.send(struct.pack('!6H', token, 0x100, 1, 0, 0, 0) + question)
        try:
            data = sock.recv(4096)
        except TimeoutError:
            return False
    if len(data) < 12:
        return False
    reply_id, flags, qd, an, _, _ = struct.unpack_from('!6H', data)
    return (reply_id == token and flags & 0x800f == 0x8000
            and qd == 1 and an > 0 and data[12:].startswith(question))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pdp_log', type=Path, help='local AT+CGPADDR=1 output')
    args = parser.parse_args()
    try:
        iid = pdp_iid(args.pdp_log.read_text())
    except ValueError as error:
        parser.error(str(error))

    link = json.loads(ip('-j', 'link', 'show', 'dev', IFACE).stdout)[0]
    addresses = json.loads(ip('-j', '-6', 'addr', 'show', 'dev', IFACE).stdout)
    if 'UP' in link['flags'] or any(dev.get('addr_info') for dev in addresses):
        parser.error('ccmnidp0 must start down without configured IPv6 addresses')

    link_local = str(ipaddress.IPv6Address(int(ipaddress.IPv6Address('fe80::')) | iid))
    conf = Path('/proc/sys/net/ipv6/conf', IFACE)
    saved = {conf / name: (conf / name).read_text()
             for name in ('accept_ra', 'autoconf', 'router_solicitations')}
    report = {'ra_received': False, 'autonomous_prefix': False, 'dns_matched': False,
              'ul_before': UL_STATUS.read_text().strip()}
    route_added = False
    link_up = False
    global_addr = None
    try:
        for path in saved:
            path.write_text('0\n')
        ip('link', 'set', IFACE, 'up')
        link_up = True
        ip('-6', 'addr', 'add', link_local + '/64', 'dev', IFACE, 'nodad')
        prefix = solicit(link_local, report)
        if prefix is not None:
            report['autonomous_prefix'] = True
            addr = str(ipaddress.IPv6Address((prefix << 64) | iid))
            ip('-6', 'addr', 'add', addr + '/64', 'dev', IFACE, 'nodad')
            global_addr = addr
            ip('-6', 'route', 'add', RESOLVER + '/128', 'dev', IFACE, 'src', addr)
            route_added = True
            report['dns_matched'] = dns_reply()
            ping = subprocess.run(['ping', '-6', '-I', IFACE, '-c', '5', '-W', '2',
                                   RESOLVER], capture_output=True, text=True)
            counts = re.search(r'(\d+) packets transmitted, (\d+) (?:packets )?received',
                               ping.stdout)
            report['ping'] = list(map(int, counts.groups())) if counts else None
    finally:
        if route_added:
            ip('-6', 'route', 'del', RESOLVER + '/128', 'dev', IFACE, check=False)
        if global_addr:
            ip('-6', 'addr', 'del', global_addr + '/64', 'dev', IFACE, check=False)
        if link_up:
            ip('-6', 'addr', 'del', link_local + '/64', 'dev', IFACE, check=False)
            ip('link', 'set', IFACE, 'down', check=False)
        for path, value in saved.items():
            path.write_text(value)
        report['ul_after'] = UL_STATUS.read_text().strip()
        print(json.dumps(report))
    return 0 if report['dns_matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
