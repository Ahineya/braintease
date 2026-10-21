#ifdef BOOT_TEST
/* Test images use a dedicated disposable disk. Restore the native bytes before
 * allowing the filesystem to mount; normal builds never perform test writes. */
static void block_selfcheck(void) {
    static uint8_t saved[8192], verify[8192], patch[5001];
    if (capacity < 16384) invalid("test disk too small", capacity);
    uint64_t at = capacity - sizeof(saved);
    if (block_transfer(at, saved, sizeof(saved), 0)) invalid("block test initial read", at);
    for (unsigned i = 0; i < sizeof(patch); ++i) patch[i] = (i * 171U + 39) % 251;
    if (block_transfer(at + 31, patch, sizeof(patch), 1) ||
        block_transfer(at, verify, sizeof(verify), 0)) invalid("block test partial I/O", at);
    unsigned mismatch = 0;
    for (unsigned i = 0; i < sizeof(saved); ++i)
        mismatch |= verify[i] != (i >= 31 && i < 31 + sizeof(patch) ? patch[i - 31] : saved[i]);
    if (block_transfer(at, saved, sizeof(saved), 1) || block_flush()) invalid("block test restore/flush", at);
    if (mismatch) invalid("block test RMW contents", at);
    if (!block_transfer(capacity, verify, 1, 0) ||
        !block_transfer(UINT64_MAX, verify, 1, 0) ||
        !block_transfer(capacity - 1, verify, 2, 1) ||
        !block_transfer(0, verify, 1, 2)) invalid("block test bounds", capacity);
    readonly = 1;
    int status = block_transfer(at, verify, 1, 1);
    readonly = 0;
    if (!status) invalid("block test readonly", at);
    serial("BLOCK_DRIVER_CHECKS_OK rmw_bounds_readonly_flush\n");
}
#endif
