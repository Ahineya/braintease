# Crypto coprocessor

Ripple exposes an optional, stateless crypto coprocessor through the existing
`DEVICE_*` mailbox. RVM and native Fuse-X share its host implementation. Guest
wolfSSL retains TLS state, sockets, certificate parsing, trusted roots, hostname
and date validation, record processing, and random key generation. The device
performs public-key calculations using RustCrypto curve libraries and bounded
public RSA modular arithmetic.

This is a virtual hardware accelerator. No host TLS connection, certificate
store, certificate-policy decision, or persistent host key handle is involved.
AES-GCM also has a bounded stateless record interface. Streaming hashes,
HMAC and HKDF remain available as guest software.
Bounded SHA-256/512, password derivation, AES-IGE and secret-exponent modular
arithmetic are also available as stateless device operations.

## Mailbox protocol

Submit command **13**, `CRYPTO`, with a six-cell descriptor:

```text
[1, 6, request_bank, request_offset, 2056, 0]
```

The request occupies 2,056 C16 cells. The first eight cells are:

```text
[version=1, operation, curve, a_length, b_length, c_length, output_capacity, reserved=0]
```

Four 512-cell arrays follow, in order: `a`, `b`, `c`, `output`. Each used array
cell holds one octet, 0–255. All integers in these arrays use big-endian octet
encoding. Lengths count octets/cells, not host bytes. Unused input cells are
ignored. The result replaces `output_capacity` with its actual length and
writes only those output cells.

| Operation | ID | Curve | Input a | Input b | Input c | Output |
| --- | --- | --- | --- | --- | --- | --- |
| Query | 0 | 0 | empty | empty | empty | `[1, 7, 30]` |
| EC public key | 1 | 256, 384, 521 | fixed-width private scalar | empty | empty | SEC1 public point |
| ECDH | 2 | 256, 384, 521 | fixed-width private scalar | SEC1 peer point | empty | fixed-width shared secret |
| ECDSA verify | 3 | 256, 384, 521 | SEC1 public point | precomputed hash, 20–64 octets | fixed-width `r || s` | one octet, 0 or 1 |
| RSA public arithmetic | 4 | 0 | unsigned operand | modulus | public exponent | operand raised to exponent modulo modulus |

The exact legacy query reply remains `[1, 7, 30]`, including when the caller
supplies only three output cells. An **extended query**, operation 0 with the
`curve`/parameter field set to 1, returns `[1, 254, 127]`: version followed by the
low and high octets of the operation mask (operations 1 through 14). Old hosts
reject this selector; callers may retain the successful legacy discovery result.
The ABI 1 arithmetic operations require parameter 0. Operations 13/14 use the
separate ABI 2 record layout below.

| Operation | ID | Input a | Input b | Input c | Output |
| --- | --- | --- | --- | --- | --- |
| Modular exponentiation | 5 | base, 1–256 octets | odd 2048-bit modulus, exactly 256 octets | exponent, 1–512 octets | 256 octets |
| PBKDF2-HMAC-SHA512 | 6 | password, 0–512 octets | salt, 0–512 octets | 4-octet big-endian iteration count, 1–100000 | 64 octets |
| AES-256-IGE encrypt | 7 | aligned data, 0–512 octets | 32-octet key | 32-octet IV | same size as input |
| AES-256-IGE decrypt | 8 | aligned data, 0–512 octets | 32-octet key | 32-octet IV | same size as input |
| SHA-256 | 9 | data, 0–512 octets | empty | empty | 32 octets |
| SHA-512 | 10 | data, 0–512 octets | empty | empty | 64 octets |
| Modular multiplication | 11 | operand, exactly 256 octets | odd 2048-bit modulus, exactly 256 octets | operand, exactly 256 octets | 256 octets |
| Public semiprime factorization | 12 | odd unsigned integer below 2^63, 1–8 octets | empty | empty | sorted p and q, 4 big-endian octets each |

Operation 11 requires both operands smaller than the modulus and uses
RustCrypto Montgomery multiplication. Operation 12 only accepts a product of
two distinct odd primes that each fit in 32 bits. Deterministic 64-bit
Miller–Rabin checks validate factors; Pollard rho has a total one-million-step
budget across at most sixteen polynomials. Invalid inputs and exhausted work
budgets fail without changing guest memory. This public proof-of-work helper
never handles secret values. Both operations are stateless; SRP and MTProto
protocol validation remain guest work.

Operation 5 requires the modulus's top bit set and base smaller than modulus.
It uses RustCrypto `crypto-bigint` Montgomery exponentiation across **all 4096
exponent bits**, independent of the exponent's numeric size. The existing
variable-time `BigUint` RSA-public operation is never used for secret exponents.
Protocol-specific prime/group, peer-value and proof checks remain in the guest.
This primitive does not perform SRP or authenticate a Telegram session itself.

IGE uses `IV = previous ciphertext block || previous plaintext block`, following
OpenSSL's convention. It applies no padding. For multiple requests, carry the
last ciphertext/plaintext blocks into the next IV, in that order. Empty input
returns empty output. There are no host-side cipher contexts or key handles.
All requests remain synchronous; even accelerated derivation temporarily holds
the mailbox and kernel call. Iteration and buffer caps bound each request.
The full 100000-round guest PBKDF2 fixture measured 55 ms on the development Mac
in an October 18 native run; this is an observation, not a latency guarantee.

Curve IDs identify NIST P-256, P-384 and P-521. Scalars and ECDH output use
32, 48 and 66 octets respectively. SEC1 points are uncompressed
`04 || X || Y`, exactly 65, 97 or 133 octets. The host validates scalar ranges
and public points. Signature verification uses the supplied hash without
hashing it again; invalid signature values return the ordinary result `0`.

RSA accepts 2,048-, 3,072- or 4,096-bit odd moduli, an odd exponent of at least
three encoded in at most eight octets, and an operand smaller than the modulus.
The result is padded to the modulus width. Padding, digest algorithms, PSS and
certificate verification remain guest responsibilities.

The query reply contains protocol version, curve mask (bits 0–2 in the order
above), and operation mask (bit `n` advertises operation `n`). Discovery uses this
query because the original 16-bit CAPS field is full. An older host returns
`UNKNOWN_COMMAND`; no existing capability bit is reassigned. The original query remains unchanged; the
extended selector described above discovers operations beyond the legacy set.

The host validates the complete request, including output capacity, before
writing a result. Bounds errors, malformed inputs and unsupported operations
preserve all guest memory. Request/descriptor overlap and MMIO access are
rejected. Requests and descriptors may cross physical banks. Calls are
synchronous, bounded and do not advance simulation ticks or require a new IRQ.

## AES-GCM record layout (ABI 2)

Command 13 retains its six-cell DEVICE descriptor, with request length
`130 + data_length`. The request is `vm_crypto_gcm_request`:

| Cell offset | Contents |
| --- | --- |
| 0..5 | `[2, operation, key_length, aad_length, data_length, 0]` |
| 6..37 | Raw key storage; key length must be 16 or 32 octets |
| 38..49 | 12-octet nonce |
| 50..65 | Detached 16-octet tag; output for encrypt, input for decrypt |
| 66..129 | AAD storage; at most 64 used octets |
| 130 onward | In-place record data; 0..18432 used octets |

Operations 13/14 encrypt/decrypt with RustCrypto AES-GCM. Only the used key,
AAD and data cells, the nonce and the decrypt tag must contain octets. Reserved
fields, exact submitted length, memory bounds, descriptor overlap and algorithm
shape are validated before output. Decryption authenticates the complete record
before any guest write; a rejected tag returns `BAD_STATE` with the complete
request unchanged. Success replaces only record data and, for encryption, tag.
Temporary host key/data buffers are zeroized. No host cipher context survives
a request. Nonce uniqueness, record sequencing and TLS policy remain guest work.
The exact ABI 1 query reply is unchanged; the extended mask advertises 13/14.

## Guest kernel interface

Both kernels provide `/dev/crypto`, character device major 1, minor 5, with
`RIPPLE_CRYPTO_IOCTL` (`0x4370`). The public request structure and operation
constants are in `runtime/include/vm_crypto.h`. ABI 2 records use
`RIPPLE_CRYPTO_GCM_IOCTL` (`0x4371`), with only `130 + data_length` cells copied.
An invalid authentication tag returns `EACCES`; invalid pointers/shapes keep
the existing `EFAULT`/`EINVAL` contract. The payload is never copied back on
failure.

The kernel copies the complete request into local staging memory, submits its
physical address with interrupts disabled around the shared mailbox, then
copies a successful response back through the normal user-memory/COW helpers.
It never forwards user virtual addresses to a physical MMIO device. Temporary
kernel and host secret buffers are cleared after use. When the device rejects a request, the kernel
does not copy a response to userspace. Invalid pointers fail with `EFAULT`, malformed
requests with `EINVAL`, and an unavailable command/operation with `ENOSYS`.

Private scalars and ECDH results are visible to the host during computation.
The VM host already controls guest memory; this interface does not provide
hardware key isolation. It must not be described as an HSM.

## TLS integration and profiling

`ports/tls/ripple_crypto.c` registers wolfCrypt device callbacks, including the
find-device hook so certificate verification and keys initialized without an
explicit device ID use the accelerator. Guest software still generates fresh
private scalars from its host-seeded DRBG. Unhandled algorithms fall back to
wolfSSL. A device error is an error, not a verification success.

If `/dev/crypto` or the command is unavailable, the same executable uses software
crypto. Set `RIPPLE_TLS_SOFTWARE=1` in the guest to force this path. Set
`RIPPLE_TLS_PROFILE=1` to report trust loading, TLS handshake duration, and
accelerator callback counts and elapsed milliseconds, including locally rejected
requests. Reports contain no keys
or plaintext. Operation counters are cumulative within a process.

Both `htget` and Lynx use the shared TLS client, so they share this policy and
acceleration. Normal image updates install the device and preserve the guest's
existing trust bundle. RBF's Rust host adapter shares device dispatch; a
standalone Brainfuck runner may omit the command and use software crypto.

## Verification

Host tests cover all curves, invalid points/scalars/signatures, request bounds,
wide non-octet inputs and atomic failure. Guest vector tests compare curve
public keys and shared secrets with independently generated OpenSSL answers.
The ioctl test covers bank boundaries, guard cells, invalid user pointers,
malformed requests, and parent/child COW isolation.

```sh
cargo test -p rvm --lib crypto::
python3 ports/tls/tests/test_crypto.py --target native --executable build/tls-accel/cryptocheck.fxe
python3 ports/tls/tests/test_crypto.py --target ripple --executable build/tls-accel/cryptocheck.rxe
python3 ports/tls/tests/test_device.py --target native
python3 ports/tls/tests/test_device.py --target ripple
```

Build those acceptance programs with `ports/tls/app.py --test-crypto`, or
`--source ports/tls/tests/cryptodevcheck.c`, selecting the corresponding target
and output path. Add `--software` to the vector runner to compare the same guest
executable with acceleration disabled. RVM acceptance uses JIT.

## Observed performance

On the October 17 development run, the same fixed public-key/ECDH vector
batches produced these elapsed times. Builds and other guest tests were active,
so these are observations rather than isolated throughput benchmarks.

| Curve | Native software | Native accelerated | RVM JIT software | RVM JIT accelerated |
| --- | --- | --- | --- | --- |
| P-256 | 572 ms | 12 ms | 12,397 ms | 379 ms |
| P-384 | 1,173 ms | 14 ms | 43,032 ms | 972 ms |
| P-521 | 2,075 ms | 15 ms | 82,226 ms | 1,044 ms |

The accelerated RVM client also fetched example.com with the installed Mozilla
roots, where the software profile previously lost the connection during
certificate verification. That run reported four accelerated signature checks
in 24 ms. Total handshake and application startup remain much longer: trust
bundle loading/parsing, other software crypto, network waits and cold JIT work
are separate costs. The crypto operation counters do not measure those costs.

## RFL guest services

`std/crypto` uses `rfl/os/crypto.c` and the same guest ioctl on both OS targets.
It provides discovery, bounded device requests, SHA-256/512, PBKDF2, modular
exponentiation, AES-IGE, and entropy from `/dev/urandom`. The OS builder links
the adapter only when needed. It does not expose physical MMIO to user processes.
Device absence is an explicit error; this RFL facade does not silently implement
a software fallback. Existing wolfSSL callbacks retain their existing fallback.
Application passwords and copies in managed RFL buffers remain the application's
responsibility; erasing staging buffers does not guarantee erasure of every
compiler/library temporary.

Run the disposable-kernel acceptance suite with:

```sh
cargo test -p rvm --lib crypto::
cargo build --release -p rvm -p rcc-native-runtime
make -C fuse-x "$PWD/fuse-x/build/fuse-x"
python3 rfl/tools/test_guest_services.py --target both
```

Prepare both OS kernels and their shell applications before the final command.
The suite checks full independent PBKDF2/AES answers, the actual 100000-round
work factor, device discovery, random output, and guest socket/reactor behavior.
