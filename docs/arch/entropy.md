# Host entropy and guest random devices

`DEVICE_*` command `HOST_RANDOM` (12), advertised by capability
`VM_CAP_HOST_RANDOM` (0x8000), obtains octets from the host operating system's
cryptographic random source. It is separate from the seeded, reproducible
`RNG`/`RNG_SEED` registers. Those registers are unsuitable for keys, TLS seeds
or session secrets.

The six-cell descriptor is `[1, 6, bank, offset, count, 0]`. `count` must be
1–256. Addresses and lengths count 16-bit guest cells; each returned cell
contains one octet in 0–255. The output can cross a bank boundary. The host
validates the complete physical RAM range, excludes MMIO cells and descriptor
aliasing, then obtains entropy into temporary host storage before copying it
to guest RAM. Invalid requests and OS failures leave both the descriptor and
destination unchanged. OS failure returns `IO`; there is no clock, seeded PRNG
or deterministic fallback. An unavailable service must fail the caller.

`vm_host_random(output, count)` in `runtime/vm_devices.c` submits the request.
Kernel callers serialize the mailbox against interrupts. The shared Rust
`DeviceState` implements the command for RVM and the native runtime, and is
also used by the RBF host device adapter. RBF execution has not been covered
by the guest acceptance test. Requests advance no simulation ticks, raise no
IRQ, consume no input and publish no graphics frame. Entropy is deliberately
not replayable; restoring a checkpoint does not reproduce earlier samples.

## FUZIX and Fuse-X

Both kernels expose read-only `/dev/urandom`, character device major 1,
minor 4. Reads return at most 256 octets per call; callers fill larger buffers
with a read loop. A zero-length read returns zero. Bad user ranges fail with
`EFAULT` before entropy is requested. Unsupported hosts return `ENOSYS`, and
host entropy failures return `EIO`.

The kernel reads into supervisor-owned staging storage, then copies through
the normal user-memory path. This keeps physical device addresses distinct
from process virtual addresses and preserves MMU copy-on-write behavior.
Forked processes must seed their own cryptographic generators after a fork;
sharing a saved userspace generator state is not equivalent to reading fresh
entropy. The device is not a substitute for a reviewed cryptographic library.

## Validation

```sh
cargo test -p rvm --lib entropy --offline
cargo test -p rvm --lib host_random --offline
make -C ports/fuzix test-entropy
make -C fuse-x test-entropy
```

Device tests cover range checks, bank crossings, descriptor overlap, octet
values, unchanged memory on provider failure and independence from guest
simulation time. Guest tests run on private disks and cover short reads,
canaries, invalid pointers, repeated reads, fork/COW isolation and a clean
filesystem check afterward. Different successive samples are a smoke check,
not a statistical assessment of entropy quality.
