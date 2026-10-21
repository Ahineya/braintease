"""PUBLIC GTK rotation oracle: independent Python HMAC and OpenSSL AES wrap.
No device, network credentials or production crypto implementation is used.
"""
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent
helpers = runpy.run_path(str(HERE / 'wifi-wpa-vectors.py'))
mac, wrap, eapol, sign = [helpers[n] for n in ['mac', 'wrap', 'eapol', 'sign']]


def generate():
    pmk, sn, an = bytes(range(32)), bytes(range(32, 64)), bytes(range(64, 96))
    own, ap = bytes(range(2, 8)), bytes(range(4, 10))
    context = min(own, ap) + max(own, ap) + min(sn, an) + max(sn, an)
    ptk = b''.join(mac(pmk, b'Pairwise key expansion\0' + context + bytes([i])) for i in range(3))[:48]
    result = {}

    def g1(data, replay=9, rsc=17, info=0x1382):
        data = bytes(data)
        if len(data) % 8 or len(data) < 16:
            target = max(16, (len(data) + 8) // 8 * 8)
            data += b'\xdd' + bytes(target - len(data) - 1)
        p = eapol(info, replay, data=wrap(ptk[16:32], data))
        p[65:73] = rsc.to_bytes(8, 'little')
        return sign(p, ptk)

    for group in [2, 4]:
        n = 32 if group == 2 else 16
        key = bytes(range(160, 160 + n))
        kde = bytes([221, 6 + n, 0, 15, 172, 1, 2, 0]) + key
        good = g1(kde)
        result[f'key_{group}'] = key
        result[f'g1_{group}'] = good
        padded = bytearray(good) + bytes(4)
        padded[2:4] = (len(padded) - 4).to_bytes(2, 'big')
        padded[81:97] = bytes(16)
        result[f'g1_padded_{group}'] = sign(padded, ptk)
        result[f'g2_{group}'] = sign(eapol(0x302, 9), ptk)
        result[f'same_key_new_replay_{group}'] = g1(kde, 10, 0)
        result[f'g2_new_replay_{group}'] = sign(eapol(0x302, 10), ptk)
        result[f'changed_same_replay_{group}'] = g1(kde[:-1] + bytes([key[-1] ^ 1]), 9)
        result[f'missing_{group}'] = g1(bytes([221, 4, 0, 15, 172, 9]))
        result[f'duplicate_{group}'] = g1(kde + kde)
        result[f'bad_length_{group}'] = g1(bytes([221, 5 + n]) + kde[2:])
        bad = bytearray(good)
        bad[-1] ^= 1
        bad[81:97] = bytes(16)
        result[f'bad_wrap_{group}'] = sign(bad, ptk)
        # >256 authenticated unique keys exercise persistent queue reuse.
        for i in range(300):
            key = bytes((i // 256, i % 256)) + bytes((j * 23 + i * 7) % 256 for j in range(n - 2))
            kde = bytes([221, 6 + n, 0, 15, 172, 1, 1 + i % 2, 0]) + key
            result[f'cycle_{group}_{i}'] = g1(kde, 100 + i, 3)
            result[f'cycle_g2_{group}_{i}'] = sign(eapol(0x302, 100 + i), ptk)
    text = '/* PUBLIC vectors: Python hashlib/HMAC + OpenSSL RFC3394, no credentials. */\n'
    for name, data in result.items():
        text += f'static const unsigned char rekey_{name}[]={{' + ','.join(str(v) for v in data) + '};\n'
    for group in [2, 4]:
        text += f'static const unsigned char *const rekey_cycles_{group}[]={{' + ','.join(f'rekey_cycle_{group}_{i}' for i in range(300)) + '};\n'
        text += f'static const unsigned char *const rekey_replies_{group}[]={{' + ','.join(f'rekey_cycle_g2_{group}_{i}' for i in range(300)) + '};\n'
    return text


if __name__ == '__main__':
    (HERE / 'fixtures/wifi-rekey-vectors.h').write_text(generate())
