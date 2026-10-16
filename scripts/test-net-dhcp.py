#!/usr/bin/env python3
"""Independent DHCP -> guest DNS -> ICMP peer; no host resolver or Internet.

Build dhcp-ping first. --clients 2 proves separate leases on the same peer.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('dns_oracle', ROOT / 'scripts/test-net-dns.py')
dns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dns)
wire = dns.wire
SERVER = bytes([192, 168, 64, 1])
TARGET = bytes([203, 0, 113, 9])
BROADCAST = b'\xff' * 4


def options(data):
    result = {}
    offset = 240
    assert data[236:240] == bytes([99, 130, 83, 99])
    while offset < len(data):
        code = data[offset]
        offset += 1
        if code == 255:
            return result
        if code == 0:
            continue
        size = data[offset]
        offset += 1
        assert offset + size <= len(data) and code not in result
        result[code] = data[offset:offset + size]
        offset += size
    raise AssertionError('unterminated DHCP options')


def option(code, value):
    return bytes([code, len(value)]) + value


def ethernet_ip(client, payload, protocol, source, destination):
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, len(payload) + 20, 13, 0, 64, protocol, 0, source, destination)
    ip = ip[:10] + struct.pack('!H', wire.checksum(ip)) + ip[12:]
    return client['mac'] + wire.HOST_MAC + b'\x08\x00' + ip + payload


def udp(client, data, source_port, destination_port, destination):
    header = struct.pack('!HHHH', source_port, destination_port, len(data) + 8, 0)
    pseudo = SERVER + destination + struct.pack('!BBH', 0, 17, len(header) + len(data))
    check = wire.checksum(pseudo + header + data) or 65535
    return ethernet_ip(client, header[:6] + struct.pack('!H', check) + data, 17, SERVER, destination)


def dhcp_reply(client, query, kind, **changes):
    data = bytearray(240)
    data[0:3] = b'\x02\x01\x06'
    data[4:8] = query[4:8]
    data[16:20] = client['ip']
    data[28:34] = client['mac']
    data[236:240] = bytes([99, 130, 83, 99])
    values = {53: bytes([kind]), 54: SERVER, 51: struct.pack('!I', 3600),
              1: bytes([255, 255, 255, 0]), 3: SERVER, 6: SERVER,
              58: struct.pack('!I', 1800), 59: struct.pack('!I', 3150)}
    if changes.get('wrong_server'):
        values[54] = bytes([192, 168, 64, 2])
    if changes.get('bad_timers'):
        values[58] = struct.pack('!I', 3500)
    if changes.get('wrong_id'):
        data[7] ^= 1
    if changes.get('wrong_mac'):
        data[33] ^= 1
    data += b''.join(option(k, v) for k, v in values.items())
    if changes.get('truncated'):
        data += bytes([99, 250])
    if changes.get('duplicate'):
        data += option(54, SERVER)
    data += b'\xff'
    return udp(client, data, 67, 68, client['ip'] if changes.get('unicast') else BROADCAST)


def arp_reply(client):
    return client['mac'] + wire.HOST_MAC + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x800, 6, 4, 2) + wire.HOST_MAC + SERVER + client['mac'] + client['ip']


def run_case(args, mode, case):
    build = ROOT / 'build/network/dhcp' / mode
    label = mode + ('-jit' if args.jit and mode == 'rvm' else '') + '-' + case
    started = time.monotonic()
    clients = {}
    processes = []
    logs = []
    result = {'success': False, 'mode': mode, 'case': case, 'clients': []}
    with tempfile.TemporaryDirectory(prefix='rdhcp-') as temporary, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as peer:
        path = str(Path(temporary) / 'nic.sock')
        peer.bind(path)
        peer.settimeout(0.1)
        try:
            for i in range(args.clients):
                log = build / (label + f'-{i}.log')
                output = log.open('w')
                logs.append((log, output))
                env = os.environ.copy()
                if mode == 'native':
                    command = [str(build / 'dhcp-ping')]
                    env.update(RCC_NATIVE_NET=path, RCC_NATIVE_PACED='1', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
                else:
                    bank = int((build / 'bank-size').read_text())
                    command = [str(ROOT / 'target/release/rvm'), str(build / 'dhcp-ping.bin'), '--net', path,
                               '--clock', 'paced', '--headless', '--exit-code', '--bank-size', str(bank),
                               '--memory', str(bank * 8), '--timeout', str(args.timeout), '--disk', str(build / f'dhcp-{i}.disk')]
                    if args.jit:
                        command.append('--jit')
                processes.append(subprocess.Popen(command, env=env, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT))
            while any(p.poll() is None for p in processes):
                if time.monotonic() - started > args.timeout:
                    raise RuntimeError('fixture timed out: ' + '\n'.join(p.read_text() for p, _ in logs))
                try:
                    frame, source = peer.recvfrom(65536)
                except socket.timeout:
                    continue
                if frame == b'RNET\x01':
                    if source not in clients:
                        i = len(clients)
                        clients[source] = dict(mac=bytes([2, 0, 0, 0, 0, i + 1]), ip=bytes([192, 168, 64, 42 + i]),
                                               discovers=0, requests=0, probes=[], announced=False, dns=False,
                                               replies=[], host_reply=False, declined=False)
                    peer.sendto(b'RNET\x01' + clients[source]['mac'] + struct.pack('!H', 1500), source)
                    continue
                assert source in clients
                client = clients[source]
                if frame == b'RNET\x02':
                    peer.sendto(frame, source)
                    continue
                if frame == b'RNET\x04':
                    continue
                assert frame[6:12] == client['mac']
                if frame[12:14] == b'\x08\x06':
                    assert frame[20:22] == b'\0\x01'
                    if frame[28:32] == b'\0' * 4:
                        assert frame[:6] == b'\xff' * 6 and frame[38:42] == client['ip']
                        client['probes'].append(time.monotonic())
                        if case == 'conflict' and not client['declined']:
                            conflict = bytearray(arp_reply(client))
                            conflict[28:32] = client['ip']
                            peer.sendto(conflict, source)
                    elif frame[38:42] == SERVER:
                        peer.sendto(arp_reply(client), source)
                    else:
                        assert frame[28:32] == client['ip'] and frame[38:42] == client['ip']
                        if not client['announced']:
                            assert len(client['probes']) == 3, client
                            assert time.monotonic() - client['probes'][-1] >= 1.9
                            client['announced'] = True
                            # A real incoming request must also be answered after applying the lease.
                            peer.sendto(ethernet_ip(client, wire.echo(8, 0xabcd, 1, b'host\0\xff'), 1, SERVER, client['ip']), source)
                    continue
                assert frame[12:15] == b'\x08\x00\x45' and wire.checksum(frame[14:34]) == 0
                total = struct.unpack('!H', frame[16:18])[0]
                assert total <= len(frame) - 14
                body = frame[34:14 + total]
                if frame[23] == 1:
                    assert client['announced'] and frame[26:30] == client['ip'] and wire.checksum(body) == 0
                    kind, _, _, identifier, seq = struct.unpack('!BBHHH', body[:8])
                    if kind == 0:
                        assert identifier == 0xabcd and body[8:] == b'host\0\xff'
                        client['host_reply'] = True
                    else:
                        assert client['dns'] and kind == 8 and frame[30:34] == TARGET and seq == len(client['replies']) + 1
                        assert body[8:] == bytes(255 if j == 1 else j for j in range(17))
                        client['replies'].append(seq)
                        peer.sendto(ethernet_ip(client, wire.echo(0, identifier, seq, body[8:]), 1, TARGET, client['ip']), source)
                    continue
                assert frame[23] == 17
                sport, dport, size, check = struct.unpack('!HHHH', body[:8])
                pseudo = frame[26:34] + struct.pack('!BBH', 0, 17, size)
                assert size == len(body) and check and wire.checksum(pseudo + body) == 0
                data = body[8:]
                if dport == 53:
                    assert client['announced'] and frame[26:30] == client['ip'] and frame[30:34] == SERVER
                    assert data[12:] == dns.name('syma-lang.org') + b'\0\x01\0\x01'
                    client['dns'] = True
                    answer = dns.response(data, dns.rr(b'\xc0\x0c', 1, TARGET, 45), 1)
                    peer.sendto(udp(client, answer, 53, sport, client['ip']), source)
                    continue
                assert sport == 68 and dport == 67 and frame[26:30] == b'\0' * 4 and frame[30:34] == BROADCAST
                opts = options(data)
                assert data[0:3] == b'\x01\x01\x06' and data[28:34] == client['mac']
                assert opts[61] == b'\x01' + client['mac']
                kind = opts[53][0]
                if kind == 1:
                    assert 50 not in opts and 54 not in opts and data[12:16] == b'\0' * 4
                    client['discovers'] += 1
                    if case == 'retry' and client['discovers'] == 1:
                        continue
                    for changes in ({'wrong_id': True}, {'wrong_mac': True}, {'truncated': True}, {'duplicate': True}):
                        peer.sendto(dhcp_reply(client, data, 2, **changes), source)
                    peer.sendto(dhcp_reply(client, data, 2), source)
                elif kind == 3:
                    client['requests'] += 1
                    assert opts[50] == client['ip'] and opts[54] == SERVER and data[12:16] == b'\0' * 4
                    peer.sendto(dhcp_reply(client, data, 5, wrong_server=True), source)
                    peer.sendto(dhcp_reply(client, data, 5, bad_timers=True), source)
                    peer.sendto(dhcp_reply(client, data, 5, unicast=True), source)
                else:
                    assert case == 'conflict' and kind == 4 and opts[50] == client['ip'] and not client['declined']
                    client['declined'] = True
                    client['ip'] = bytes([192, 168, 64, 80 + len(client['mac']) + list(clients).index(source)])
                    client['probes'] = []
            assert len(clients) == args.clients
            assert len({c['ip'] for c in clients.values()}) == args.clients
            for process, (log, _) in zip(processes, logs):
                assert process.returncode == 0, log.read_text()
                assert 'PING OK received=3 responded=1' in log.read_text(), log.read_text()
                assert 'dns=192.168.64.1' in log.read_text(), log.read_text()
            for client in clients.values():
                assert client['announced'] and client['dns'] and client['host_reply'] and client['replies'] == [1, 2, 3], client
                if case == 'conflict':
                    assert client['declined'] and client['discovers'] >= 2
                if case == 'retry':
                    assert client['discovers'] >= 2
                result['clients'].append({k: v.hex() if isinstance(v, bytes) else v for k, v in client.items()})
            result['success'] = True
            print(f'{label}: {args.clients} lease(s), ACD, guest DNS, three outgoing pings and host echo passed')
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            for _, output in logs:
                output.close()
            result['elapsed_seconds'] = time.monotonic() - started
            (build / (label + '.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['native', 'rvm'], action='append')
    parser.add_argument('--case', choices=['success', 'retry', 'conflict'], action='append')
    parser.add_argument('--clients', type=int, choices=[1, 2], default=1)
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--timeout', type=int, default=45)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    for mode in args.mode or ['rvm', 'native']:
        for case in args.case or ['success', 'retry', 'conflict']:
            run_case(args, mode, case)


if __name__ == '__main__':
    main()
