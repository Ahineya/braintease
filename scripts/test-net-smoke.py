#!/usr/bin/env python3
"""Independent packet oracle: no external network, vmnet, TAP or administrator access."""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
GUEST_MAC = bytes.fromhex('020000000001')
HOST_MAC = bytes.fromhex('020000000002')
GUEST_IP = bytes([192, 168, 64, 250])
HOST_IP = bytes([192, 168, 64, 1])
PAYLOAD = b'host\x00\xffodd-bytes'


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return total ^ 65535


def arp(operation, destination, target_mac, target_ip):
    return destination + HOST_MAC + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x800, 6, 4, operation) + HOST_MAC + HOST_IP + target_mac + target_ip


def echo(kind, identifier, sequence, payload):
    data = struct.pack('!BBHHH', kind, 0, 0, identifier, sequence) + payload
    return data[:2] + struct.pack('!H', checksum(data)) + data[4:]


def ip_frame(icmp):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(icmp), 17, 0, 64, 1, 0, HOST_IP, GUEST_IP)
    header = header[:10] + struct.pack('!H', checksum(header)) + header[12:]
    return GUEST_MAC + HOST_MAC + b'\x08\x00' + header + icmp


def run_case(mode, jit, timeout, monitor=False):
    build = ROOT / 'build/network' / ('monitor' if monitor else '') / mode
    program = 'monitor' if monitor else 'ping'
    label = mode + ('-jit' if jit else '')
    log = build / (label + '-fixture.log')
    packets = []
    flags = {'arp_request': False, 'arp_reply': False, 'echo_request': False, 'echo_reply': False}
    if monitor:
        flags = {'announcement': False, 'arp_reply': False, 'echo_reply': False}
    replies = set()
    payloads = {3: PAYLOAD, 4: b'', 5: bytes(range(56))} if monitor else {3: PAYLOAD}
    result = {'success': False, 'mode': mode, 'jit': jit, 'monitor': monitor}
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='rnet-') as temporary, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as peer, log.open('w') as output:
        path = str(Path(temporary) / 'peer.sock')
        peer.bind(path)
        peer.settimeout(0.1)
        env = os.environ.copy()
        if mode == 'native':
            command = [str(build / program)]
            env.update(RCC_NATIVE_NET=path, RCC_NATIVE_TIMEOUT=str(timeout), RCC_NATIVE_HEADLESS='1')
        else:
            bank = int((build / 'bank-size').read_text())
            command = [str(ROOT / 'target/release' / mode), str(build / (program + ('.bf' if mode == 'rbf' else '.bin'))),
                       '--net', path, '--headless', '--exit-code', '--timeout', str(timeout),
                       '--report', str(build / (label + '-fixture-guest.json'))]
            if mode == 'rvm':
                command += ['--bank-size', str(bank), '--memory', str(bank * 8), '--disk', str(build / 'fixture.disk.img')]
            if jit:
                command += ['--jit']
        guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT)
        client = None

        def record(direction, frame):
            if len(packets) >= 128:
                raise RuntimeError('unexpected packet flood')
            packets.append({'direction': direction, 'frame_hex': frame.hex(), 'wall_time': time.time()})

        def send(frame):
            record('host_to_guest', frame)
            assert peer.sendto(frame, client) == len(frame)

        try:
            while guest.poll() is None:
                if time.monotonic() - started > timeout:
                    raise RuntimeError('packet fixture timed out')
                if monitor and all(flags.values()) and replies == set(payloads):
                    content = log.read_text()
                    expected = [f'PING request from=192.168.64.1 id=43981 seq={seq} octets={len(data)}'
                                for seq, data in payloads.items()]
                    if all(line in content for line in expected) and content.count('PING reply queued') == 3:
                        assert content.count('PING request from=') == 3, content
                        assert 'PING monitor local=192.168.64.250 mask=255.255.255.0' in content, content
                        assert 'MAC=02:00:00:00:00:01' in content, content
                        assert guest.poll() is None, 'monitor exited after replying'
                        break
                try:
                    frame, source = peer.recvfrom(65536)
                except socket.timeout:
                    continue
                if frame == b'RNET\x01':
                    client = source
                    peer.sendto(b'RNET\x01' + GUEST_MAC + struct.pack('!H', 1500), client)
                    send(arp(1, b'\xff' * 6, b'\0' * 6, GUEST_IP))
                    request = ip_frame(echo(8, 0xabcd, 3, PAYLOAD))
                    corrupt_ip = bytearray(request)
                    corrupt_ip[24] ^= 1
                    send(bytes(corrupt_ip))
                    corrupt_icmp = bytearray(request)
                    corrupt_icmp[36] ^= 1
                    send(bytes(corrupt_icmp))
                    for seq, payload in payloads.items():
                        send(ip_frame(echo(8, 0xabcd, seq, payload)))
                    continue
                assert source == client, 'unexpected peer'
                if frame == b'RNET\x02':
                    peer.sendto(frame, client)
                    continue
                if frame == b'RNET\x04':
                    continue
                record('guest_to_host', frame)
                assert len(frame) >= 14 and frame[6:12] == GUEST_MAC
                if frame[12:14] == b'\x08\x06':
                    assert len(frame) >= 42 and frame[14:20] == struct.pack('!HHBB', 1, 0x800, 6, 4)
                    assert frame[22:28] == GUEST_MAC and frame[28:32] == GUEST_IP
                    operation = struct.unpack('!H', frame[20:22])[0]
                    if monitor and operation == 1:
                        assert frame[:6] == b'\xff' * 6 and frame[32:38] == b'\0' * 6
                        assert frame[38:42] == GUEST_IP, 'monitor probed an unrelated address'
                        assert not flags['announcement'], 'repeated startup announcement'
                        flags['announcement'] = True
                        continue
                    assert frame[38:42] == HOST_IP
                    if operation == 1:
                        assert not monitor, 'passive monitor sent an ARP probe'
                        flags['arp_request'] = True
                        send(arp(2, GUEST_MAC, GUEST_MAC, GUEST_IP))
                    else:
                        assert operation == 2 and frame[32:38] == HOST_MAC
                        flags['arp_reply'] = True
                else:
                    assert frame[:6] == HOST_MAC and frame[12:15] == b'\x08\x00\x45'
                    total = struct.unpack('!H', frame[16:18])[0]
                    assert 28 <= total <= len(frame) - 14 and checksum(frame[14:34]) == 0
                    assert frame[23] == 1 and frame[26:30] == GUEST_IP and frame[30:34] == HOST_IP
                    icmp = frame[34:14 + total]
                    assert checksum(icmp) == 0 and icmp[1] == 0
                    kind, _, _, identifier, sequence = struct.unpack('!BBHHH', icmp[:8])
                    if kind == 8:
                        assert not monitor, 'passive monitor sent an echo probe'
                        assert identifier == 0x5246 and sequence == 1
                        assert icmp[8:] == bytes(255 if i == 1 else i for i in range(17))
                        flags['echo_request'] = True
                        send(ip_frame(echo(0, identifier, sequence, icmp[8:])))
                    else:
                        assert kind == 0 and identifier == 0xabcd and sequence in payloads
                        assert icmp[8:] == payloads[sequence] and sequence not in replies
                        replies.add(sequence)
                        flags['echo_reply'] = True
            if monitor:
                assert guest.poll() is None, log.read_text()
                assert replies == set(payloads), replies
            else:
                assert guest.returncode == 0, log.read_text()
            assert all(flags.values()), flags
            if not monitor:
                assert 'PING OK received=1 responded=1 rejected=2 drops=0' in log.read_text(), log.read_text()
            result['success'] = True
            print(f'{label}: ' + ('persistent monitor, three logged replies, empty/odd/56-octet payloads'
                                 if monitor else 'ARP and ICMP both directions, odd/zero/255 octets, malformed checksums rejected'))
        except Exception as error:
            result['error'] = str(error)
            raise
        finally:
            if guest.poll() is None:
                guest.terminate()
                try:
                    guest.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()
            result.update(elapsed_seconds=time.monotonic() - started, checks=flags, packets=packets)
            (build / (label + '-fixture-results.json')).write_text(json.dumps(result, indent=2) + '\n')
            with (build / (label + '-fixture.pcap')).open('wb') as capture:
                capture.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
                for packet in packets:
                    frame = bytes.fromhex(packet['frame_hex'])
                    seconds = packet['wall_time']
                    capture.write(struct.pack('<IIII', int(seconds), int((seconds % 1) * 1000000), len(frame), len(frame)))
                    capture.write(frame)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['rvm', 'rbf', 'native'], action='append',
                        help='runtime to test (repeatable; defaults to RVM and native; RBF is opt-in)')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--monitor', action='store_true', help='test persistent monitor builds instead of one-shot ping')
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    for mode in args.mode or ['rvm', 'native']:
        run_case(mode, args.jit and mode != 'native', args.timeout, args.monitor)


if __name__ == '__main__':
    main()
