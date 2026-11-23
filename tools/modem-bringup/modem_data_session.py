#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a foreground MT6893 dual-stack session after HS2 and DL ring setup."""
import argparse
from contextlib import ExitStack
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

from modem_at_probe import AtError, FdReader, command, establish
from modem_ipv6_probe import IFACE, UL_STATUS, dns_reply, ip, pdp_iid, solicit
from modem_slot_probe import BasicMux, SLOTS

AT_DEVICE = '/dev/ccci-at-10209000.modem-platform'
MD_NET = Path('/sys/devices/platform/10209000.modem-platform/md_net')
RESOLVERS = ('8.8.8.8', '2001:4860:4860::8888')
POLICY_TABLE = '6893'
POLICY_PRIORITY = 11000
RAT_PROFILES = {'lte-only': ('3', '0'), 'nr-lte': ('19', '128')}
COPS_ACT = re.compile(r'\+COPS:\s*\d+,\s*\d+,\s*"[^"]*",\s*(\d+)')
PHY_COMMAND = 'AT+EPHYCONFIG=2'
NET_CHANNEL = UL_STATUS.with_name('net_channel')
LHIF_ROW = re.compile(r'^lhif\d+ net_if=(\d+) lhif_q=\d+ queue=([0-3])'
                      r'(?: requests=(\d+))?$', re.M)


def context_states(at):
    states = {}
    for line in at('AT+CGACT?'):
        state = re.fullmatch(r'\+CGACT:\s*(\d+),\s*([01])', line)
        if state:
            states[int(state[1])] = state[2] == '1'
    if 1 not in states:
        raise AtError('missing CID 1 status')
    return states


def context_active(at):
    return context_states(at)[1]


def rat_profile(at):
    for line in at('AT+ERAT?'):
        state = re.fullmatch(r'\+ERAT:\s*\d+,\s*\d+,\s*(\d{1,3}),\s*(\d{1,3}),\s*\d+', line)
        if state:
            return state.groups()
    raise AtError('cannot read configured RAT policy')


def set_rat_profile(at, profile):
    at('AT+ERAT=' + ','.join(profile), 15)
    if rat_profile(at) != profile:
        raise AtError('configured RAT policy did not match the request')


def enable_legacy_radio(exchange, stopping):
    def enabled(lines):
        modes = [match[1] for line in lines
                 if (match := re.fullmatch(r'\+CFUN:\s*(\d+)(?:,\s*\d+)?', line))]
        return modes == ['1'] and 'OK' in lines

    if stopping():
        return
    if enabled(exchange('AT+CFUN?')):
        print('RF mode 1 confirmed', flush=True)
        return
    for attempt in range(1, 5):
        if stopping():
            return
        exchange('AT+CFUN=1')
        if stopping():
            return
        # A missing or negative terminal response does not prove RF stayed off.
        if enabled(exchange('AT+CFUN?')):
            print(f'RF mode 1 confirmed after enable attempt {attempt}', flush=True)
            return
        if attempt < 4 and not stopping():
            time.sleep(2)
    if not stopping():
        raise AtError('RF mode 1 could not be confirmed')


def wait_for_rat(at, mode, stopping):
    allowed = {7} if mode == 'lte-only' else {7, 10, 11, 12, 13}
    deadline = time.monotonic() + 45
    while not stopping() and time.monotonic() < deadline:
        if '+CGATT: 1' in at('AT+CGATT?'):
            if mode is None:
                print('Packet attached', flush=True)
                return
            for line in at('AT+COPS?'):
                match = COPS_ACT.fullmatch(line)
                if match and int(match[1]) in allowed:
                    print(f'Packet attached, access technology {match[1]}', flush=True)
                    return
        time.sleep(2)
    if not stopping():
        raise AtError('timed out waiting for packet attach under the current RAT policy')


def report_radio(at):
    """Report the fields confirmed against the same-device RIL parser."""
    try:
        act = next((match[1] for line in at('AT+COPS?')
                    if (match := COPS_ACT.fullmatch(line))), 'unavailable')
    except AtError:
        act = 'unavailable'
    print(f'COPS AcT: {act}', flush=True)

    try:
        lines = at(PHY_COMMAND)
    except AtError:
        lines = []
    found = False
    for line in lines:
        if not line.startswith('+EPHYCONFIG:'):
            continue
        fields = [field.strip() for field in line.split(':', 1)[1].split(',')]
        if len(fields) != 8 or not all(re.fullmatch(r'[0-9]{1,9}', x) for x in fields):
            continue
        rat, dl, ul, dl_bw, ul_bw, band, _, _ = map(int, fields)
        if rat not in (4, 128) or not band:
            continue
        found = True
        name = 'LTE B' if rat == 4 else 'NR n'
        print(f'Physical channel: {name}{band}, DL/UL channel {dl}/{ul}, '
              f'DL/UL bandwidth fields {dl_bw}/{ul_bw}', flush=True)
    if not found:
        print('LTE/NR physical channel: unavailable', flush=True)


def tx_queue(status):
    for port, queue, _ in LHIF_ROW.findall(status):
        if port == '0':
            return int(queue)
    raise AtError('missing queue mapping for CCMNI channel 0')


def lhif_state():
    status = MD_NET.read_text()
    count = re.search(r'\blhif_requests=(\d+)\b', status)
    rows = LHIF_ROW.findall(status)
    if not count or any(not requests for _, _, requests in rows):
        raise AtError('per-channel LHIF counters are unavailable')
    return int(count[1]), {int(port): (int(queue), int(requests))
                           for port, queue, requests in rows}


def activation_mapping(before, after):
    old_count, old = before
    count, current = after
    changed = [(port, queue) for port, (queue, seq) in current.items()
               if seq != old.get(port, (None, 0))[1]]
    if (count != old_count + 1 or len(changed) != 1 or old.keys() - current.keys()):
        raise AtError('CID 1 activation has no unique LHIF mapping')
    port, queue = changed[0]
    if port > 255 or current[port][1] != old.get(port, (None, 0))[1] + 1:
        raise AtError('invalid LHIF channel or request counter')
    return port, queue


def check_interface():
    link = json.loads(ip('-j', 'link', 'show', 'dev', IFACE).stdout)[0]
    addresses = json.loads(ip('-j', 'addr', 'show', 'dev', IFACE).stdout)
    if 'UP' in link['flags'] or any(dev.get('addr_info') for dev in addresses):
        raise AtError(f'{IFACE} must start down and without configured addresses')


def check_policy_routes():
    for family in ('-4', '-6'):
        try:
            rules = json.loads(ip('-N', '-j', family, 'rule', 'show').stdout)
            routes = json.loads(ip('-N', '-j', family, 'route', 'show', 'table', 'all').stdout)
        except subprocess.CalledProcessError:
            raise AtError('policy routing requires IPv4 and IPv6 multiple tables') from None
        # Numeric output avoids overlooking a table with an alias in rt_tables.
        if any(rule.get('priority') == POLICY_PRIORITY for rule in rules):
            raise AtError(f'policy rule priority {POLICY_PRIORITY} is in use')
        if any(str(item.get('table')) == POLICY_TABLE for item in rules + routes):
            raise AtError(f'policy routing table {POLICY_TABLE} is in use')


def setup_routes(stack, src, args):
    family = f'-{src.version}'
    if args.policy_routing:
        route = ('default', 'dev', IFACE, 'src', str(src), 'table', POLICY_TABLE)
        rule = ('priority', str(POLICY_PRIORITY), 'oif', IFACE, 'table', POLICY_TABLE)
        ip(family, 'route', 'add', *route)
        stack.callback(ip, family, 'route', 'del', *route)
        ip(family, 'rule', 'add', *rule)
        stack.callback(ip, family, 'rule', 'del', *rule)
    for dst in args.targets:
        if dst.version == src.version:
            route = (f'{dst}/{dst.max_prefixlen}', 'dev', IFACE, 'src', str(src))
            ip(family, 'route', 'add', *route)
            stack.callback(ip, family, 'route', 'del', *route)


def address(stack, family, addr, *flags):
    ip(family, 'addr', 'add', addr, 'dev', IFACE, *flags)
    stack.callback(ip, family, 'addr', 'del', addr, 'dev', IFACE, *flags)


def setup_ipv4(stack, v4, args):
    conf = Path('/proc/sys/net/ipv6/conf', IFACE)
    for name in ('accept_ra', 'autoconf', 'router_solicitations'):
        path = conf / name
        previous = path.read_text()
        stack.callback(path.write_text, previous)
        path.write_text('0\n')
    ip('link', 'set', IFACE, 'up')
    stack.callback(ip, 'link', 'set', IFACE, 'down')
    address(stack, '-4', f'{v4}/32')
    setup_routes(stack, v4, args)


def try_ipv6(stack, at, state, args):
    if state['iid'] is None:
        try:
            state['iid'] = pdp_iid('\n'.join(at('AT+CGPADDR=1')))
        except ValueError:
            return 'PDP did not supply an IPv6 interface ID'
    iid = state['iid']
    if state['local'] is None:
        local = ipaddress.IPv6Address((0xfe80 << 112) | iid)
        address(stack, '-6', f'{local}/64', 'nodad')
        state['local'] = local
    ra = {}
    prefix = solicit(state['local'], ra, attempts=1)
    if prefix is None:
        if ra.get('ra_received'):
            return 'RA lacked a usable autonomous prefix'
        return 'no router advertisement'
    v6 = ipaddress.IPv6Address((prefix << 64) | iid)
    flags = ('nodad', 'noprefixroute') if args.policy_routing else ('nodad',)
    address(stack, '-6', f'{v6}/64', *flags)
    setup_routes(stack, v6, args)
    return None


def probe_dns(version, stopping):
    matched = False
    for _ in range(3):
        if stopping():
            return
        try:
            matched = dns_reply(RESOLVERS[0 if version == 4 else 1])
        except OSError:
            matched = False
        if matched:
            break
    print(f'IPv{version} DNS: {"matched" if matched else "no match"}', flush=True)


def deactivate(at):
    if context_active(at):
        at('AT+CGACT=0,1', 14)
    if context_active(at):
        raise AtError('CID 1 remains active after shutdown')


def run_session(at, args, stopping):
    check_interface()
    if args.policy_routing:
        check_policy_routes()
    if not args.activation_mapping and NET_CHANNEL.exists() and NET_CHANNEL.read_text().strip() != '0':
        raise AtError('legacy session requires CCMNI channel 0')
    if context_active(at):
        raise AtError('CID 1 already active; refusing to take over')
    with ExitStack() as stack:
        if args.apn:
            pdp = 'IP' if args.ipv4_only else 'IPV4V6'
            at(f'AT+CGDCONT=1,"{pdp}","{args.apn}"')
        if args.activation_mapping:
            previous_channel = NET_CHANNEL.read_text()
            before = lhif_state()
        # A timeout does not prove the command failed to reach the modem.
        stack.callback(deactivate, at)
        at('AT+CGACT=1,1', 14)
        if stopping():
            return True
        if args.activation_mapping:
            time.sleep(1)  # let the RPC complete after the AT confirmation
            channel, queue = activation_mapping(before, lhif_state())
            stack.callback(NET_CHANNEL.write_text, previous_channel)
            NET_CHANNEL.write_text(f'{channel}\n')
            print(f'CID 1 activation: CCMNI channel {channel}, UL queue {queue}', flush=True)
        else:
            queue = tx_queue(MD_NET.read_text())
        response = '\n'.join(at('AT+CGPADDR=1'))
        match = re.search(r'\+CGPADDR:\s*1,"([0-9.]+)"', response)
        if not match:
            raise AtError('missing CID 1 IPv4 address')
        try:
            v4 = ipaddress.IPv4Address(match[1])
        except ValueError:
            raise AtError('invalid CID 1 IPv4 address') from None
        if v4.is_unspecified:
            raise AtError('CID 1 has an unspecified IPv4 address')

        ul = dict(field.split('=') for field in UL_STATUS.read_text().split())
        if ul['started'] == '0':
            UL_STATUS.with_name('ul_start').write_text(f'{queue} 60 0\n')
        elif int(ul['queue']) != queue:
            raise AtError('armed UL ring does not match the modem queue mapping')

        setup_ipv4(stack, v4, args)
        print('CID 1 active, IPv4 configured', flush=True)
        deadline = None if args.seconds is None else time.monotonic() + args.seconds
        if args.radio_status and not stopping():
            report_radio(at)
        if args.probe:
            probe_dns(4, stopping)
        try:
            iid = None if args.ipv4_only else pdp_iid(response)
        except ValueError:
            iid = None
        ipv6 = {'iid': iid, 'local': None}
        ipv6_ready = args.ipv4_only
        next_ipv6 = next_check = time.monotonic()
        last_pending = None
        while not stopping() and (deadline is None or time.monotonic() < deadline):
            now = time.monotonic()
            if now >= next_check:
                if not context_active(at):
                    print('CID 1 disconnected', flush=True)
                    return False
                next_check = time.monotonic() + 5
            if not ipv6_ready and now >= next_ipv6:
                pending = try_ipv6(stack, at, ipv6, args)
                if pending is None:
                    ipv6_ready = True
                    print('IPv6 configured on the existing CID 1', flush=True)
                    if args.probe:
                        probe_dns(6, stopping)
                elif pending != last_pending:
                    print(f'IPv6 pending: {pending}; IPv4 remains configured', flush=True)
                last_pending = pending
                next_ipv6 = time.monotonic() + 5
            if not stopping():
                remaining = 1 if deadline is None else deadline - time.monotonic()
                if remaining > 0:
                    time.sleep(min(1, remaining))
        if args.radio_status and not stopping():
            report_radio(at)
    print('CID 1 deactivated and link cleaned up', flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apn', help='optionally set the CID 1 APN')
    parser.add_argument('--slot', type=int, choices=SLOTS,
                        help='select a data SIM on an already established basic MUX')
    parser.add_argument('--restore-slot', type=int, choices=SLOTS,
                        help='explicitly select this SIM at exit (the modem cannot query it)')
    parser.add_argument('--rat', choices=RAT_PROFILES,
                        help='temporarily set the selected SIM radio access policy')
    parser.add_argument('--radio-status', action='store_true',
                        help='report serving RAT and band at session start and completion')
    parser.add_argument('--enable-rf', action='store_true',
                        help='enable RF in the default AT context and wait for attach; leaves RF enabled')
    parser.add_argument('--activation-mapping', action='store_true',
                        help='select the channel reported by a unique LHIF request during activation')
    parser.add_argument('--ipv4-only', action='store_true',
                        help='request an IPv4-only PDP (requires --apn)')
    parser.add_argument('--target', action='append', type=ipaddress.ip_address, default=[],
                        help='install a host route through the data interface; repeatable')
    parser.add_argument('--probe', action='store_true', help='report UDP DNS reachability for each configured family')
    parser.add_argument('--policy-routing', action='store_true',
                        help='route interface-bound sockets through a dedicated dual-stack table')
    parser.add_argument('--seconds', type=int, help='stop after this many seconds online')
    parser.add_argument('--reconnects', type=int, default=2,
                        help='maximum retries on CID loss (default 2)')
    args = parser.parse_args()
    if args.apn and not re.fullmatch(r'[A-Za-z0-9.-]{1,100}', args.apn):
        parser.error('invalid APN')
    if (args.seconds is not None and args.seconds < 0) or args.reconnects < 0:
        parser.error('seconds and reconnects must be nonnegative')
    if args.policy_routing and args.target:
        parser.error('--policy-routing and --target are mutually exclusive')
    if args.rat and args.slot is None:
        parser.error('--rat requires --slot')
    if args.enable_rf and args.slot is not None:
        parser.error('--enable-rf requires the default AT context (omit --slot)')
    if args.restore_slot is not None and args.slot is None:
        parser.error('--restore-slot requires --slot')
    if args.ipv4_only and (not args.apn or any(x.version == 6 for x in args.target)):
        parser.error('--ipv4-only requires --apn and accepts only IPv4 targets')
    probes = RESOLVERS[:1] if args.ipv4_only else RESOLVERS
    targets = [ipaddress.ip_address(x) for x in probes] if args.probe and not args.policy_routing else []
    args.targets = list(dict.fromkeys(args.target + targets))

    stop_requested = False

    def stop(_signum, _frame):
        nonlocal stop_requested
        stop_requested = True

    def stopping():
        return stop_requested

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    lock = os.open('/run/mt6893-data-session.lock',
                   os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_RDWR, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(lock)
        raise AtError('a data session is already running') from None

    with ExitStack() as stack:
        stack.callback(os.close, lock)
        fd = os.open(AT_DEVICE, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
        stack.callback(os.close, fd)
        return run_modem(fd, args, stopping)


def select_data_slot(reader, write, args, stack):
    check_interface()
    if args.policy_routing:
        check_policy_routes()
    mux = BasicMux(reader, write)
    mux.open_channel(0, 8)
    for dlci in SLOTS.values():
        mux.open_channel(dlci, 8)

    def at_slot(dlci):
        def at(text, timeout=8):
            name = text[2:].split('=', 1)[0].rstrip('?')
            prefix = f'{name}:' if text.endswith('?') or text in ('AT+CGPADDR=1', PHY_COMMAND) else None
            lines = mux.exchange(dlci, text, timeout, prefix=prefix)
            if lines[-1] != 'OK':
                raise AtError(f'DLCI {dlci} rejected {name}')
            return lines
        return at

    clients = {number: at_slot(dlci) for number, dlci in SLOTS.items()}
    if any(any(context_states(client).values()) for client in clients.values()):
        raise AtError('PDP already active on a SIM; refusing to change data slot')

    def select(slot):
        clients[1](f'AT+EDATASIM={slot}')
        print(f'Data SIM {slot} selected', flush=True)

    if args.restore_slot is not None and args.restore_slot != args.slot:
        stack.callback(select, args.restore_slot)
    select(args.slot)
    return clients[args.slot]


def run_modem(fd, args, stopping):
    try:
        reader = FdReader(fd)

        def write(data):
            return os.write(fd, data)

        def exchange(text, timeout=8):
            return command(reader, write, text, timeout)

        def at(text, timeout=8):
            lines = exchange(text, timeout)
            if 'OK' not in lines:
                # Do not emit response payloads, which can contain carrier addresses.
                raise AtError(f'{text.split("=")[0]} failed')
            return lines

        if stopping():
            return 0
        with ExitStack() as stack:
            if args.slot is None:
                establish(reader, write, 8)
                if args.enable_rf:
                    enable_legacy_radio(exchange, stopping)
                    wait_for_rat(at, None, stopping)
            else:
                at = select_data_slot(reader, write, args, stack)
            if args.rat and not stopping():
                previous = rat_profile(at)
                requested = RAT_PROFILES[args.rat]
                if previous != requested:
                    stack.callback(set_rat_profile, at, previous)
                    set_rat_profile(at, requested)
                wait_for_rat(at, args.rat, stopping)
            for retry in range(args.reconnects + 1):
                if stopping() or run_session(at, args, stopping):
                    return 0
                if retry < args.reconnects:
                    print('waiting before the next CID 1 activation', flush=True)
                    for _ in range(10):
                        if stopping():
                            return 0
                        time.sleep(1)
                    wait_for_rat(at, args.rat, stopping)
        raise AtError('CID 1 reconnect budget exhausted')
    except subprocess.CalledProcessError as error:
        raise AtError(f'{error.cmd[0]} operation failed') from None


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except AtError as error:
        raise SystemExit(str(error)) from None
    except OSError as error:
        raise SystemExit(f'local I/O failed: {error.strerror}') from None
