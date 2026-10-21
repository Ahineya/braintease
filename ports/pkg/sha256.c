/* SHA-256 over explicit octets; no native structure serialization. */
#include "pkg.h"
#include <string.h>
static const uint32_t constants[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
    0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
    0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
    0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
    0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};
static uint32_t rotate(uint32_t n, unsigned bits) { return (n >> bits) | (n << (32 - bits)); }
static void block(PkgSha *s) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t, u, x, y;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)s->block[4 * i] << 24 | (uint32_t)s->block[4 * i + 1] << 16 |
               (uint32_t)s->block[4 * i + 2] << 8 | (uint32_t)s->block[4 * i + 3];
    for (i = 16; i < 64; i++) {
        x = w[i - 15];
        y = w[i - 2];
        w[i] = w[i - 16] + (rotate(x, 7) ^ rotate(x, 18) ^ (x >> 3)) + w[i - 7] +
               (rotate(y, 17) ^ rotate(y, 19) ^ (y >> 10));
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];
    for (i = 0; i < 64; i++) {
        t = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ ((~e) & g)) +
            constants[i] + w[i];
        u = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t;
        d = c;
        c = b;
        b = a;
        a = t + u;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}
void pkg_sha_init(PkgSha *s) {
    static const uint32_t initial[8] = {0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
                                        0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL};
    memcpy(s->h, initial, sizeof(initial));
    s->count = 0;
    s->used = 0;
}
void pkg_sha_update(PkgSha *s, const PkgByte *data, size_t n) {
    s->count += n;
    while (n--) {
        s->block[s->used++] = *data++ & 255U;
        if (s->used == 64) {
            block(s);
            s->used = 0;
        }
    }
}
void pkg_sha_end(PkgSha *s, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    uint64_t bits = s->count * 8;
    unsigned i;
    PkgByte byte;
    byte = 128;
    pkg_sha_update(s, &byte, 1);
    byte = 0;
    while (s->used != 56)
        pkg_sha_update(s, &byte, 1);
    for (i = 0; i < 8; i++) {
        byte = (PkgByte)((bits >> (56 - i * 8)) & 255);
        pkg_sha_update(s, &byte, 1);
    }
    for (i = 0; i < 32; i++) {
        unsigned value = (unsigned)((s->h[i / 4] >> (24 - (i % 4) * 8)) & 255);
        out[2 * i] = hex[value >> 4];
        out[2 * i + 1] = hex[value & 15];
    }
    out[64] = 0;
}
int pkg_hash_file(const char *path, char out[65], unsigned long *size) {
    PkgByte buffer[2048];
    size_t n, i;
    unsigned long total = 0;
    PkgSha sha;
    FILE *f = fopen(path, "rb");
    if (!f)
        return pkg_fail("cannot read", path);
    pkg_sha_init(&sha);
    while ((n = fread(buffer, 1, 2048, f)) != 0) {
        if (n > PKG_LIMIT - total) {
            fclose(f);
            return pkg_fail("file exceeds limit", path);
        }
        for (i = 0; i < n; i++)
            if (((unsigned long)buffer[i] & ~255UL) != 0) {
                fclose(f);
                return pkg_fail("non-octet file cell", path);
            }
        pkg_sha_update(&sha, buffer, n);
        total += (unsigned long)n;
    }
    if (ferror(f)) {
        fclose(f);
        return pkg_fail("read failed", path);
    }
    if (fclose(f))
        return pkg_fail("close failed", path);
    pkg_sha_end(&sha, out);
    if (size)
        *size = total;
    return 0;
}
