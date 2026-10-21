/* Existing FILE/cell helper ABI over native bytes. DMA never sees C16 cells.
 * The shared filesystem still owns block layout, caches and sync ordering. */
static void root_init(void) {
#ifdef PERSISTENT_ROOT
    ram_root_size = block_init();
#ifdef ROOT_NVME
    serial("ROOT_STORAGE nvme persistent\n");
#else
    serial("ROOT_STORAGE virtio persistent\n");
#endif
#else
    ram_root_size = (size_t)(root_payload_end - root_payload_start);
    if (ram_root_size > sizeof(ram_root)) fail("RAM root capacity", ram_root_size);
    memcpy(ram_root, root_payload_start, ram_root_size);
    serial("ROOT_STORAGE ram ephemeral\n");
#endif
}
static int root_transfer(size_t offset, void *data, size_t count, unsigned write) {
    if (offset > ram_root_size || count > ram_root_size - offset) return -1;
#ifdef PERSISTENT_ROOT
    return block_transfer(offset, data, count, write);
#else
    if (write) memcpy(ram_root + offset, data, count);
    else memcpy(data, ram_root + offset, count);
    return 0;
#endif
}
static int root_flush(void) {
#ifdef PERSISTENT_ROOT
    return block_flush();
#else
    return 0;
#endif
}
static void file(Context *c, U handle) {
    if (!c->file_open || handle != 1) fail("root stream handle", handle);
}
U rcc_native_ext_fopen(Context *c, U path, U mode) {
    const char expected[] = "root";
    size_t p = span(path, sizeof(expected)), m = span(mode, 4);
    for (unsigned i = 0; i < sizeof(expected); ++i) if (c->cells[p + i] != (uint8_t)expected[i]) return 0;
    if (c->cells[m] != 'r' || !((c->cells[m + 1] == 'b' && !c->cells[m + 2]) ||
        (c->cells[m + 1] == '+' && c->cells[m + 2] == 'b' && !c->cells[m + 3]))) return 0;
    if (c->file_open) fail("root opened twice", 0);
    c->file_open = 1; c->file_writable = c->cells[m + 1] == '+'; c->file_offset = 0; return 1;
}
U rcc_native_ext_fseek(Context *c, U handle, U delta, U whence) {
    file(c, handle);
    if (whence > 2) return 65535;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)c->file_offset : (int64_t)ram_root_size;
    int64_t offset = base + (int32_t)delta;
    if (offset < 0 || (U)offset > ram_root_size) return 65535;
    c->file_offset = (size_t)offset; return 0;
}
U rcc_native_ext_ftell(Context *c, U handle) { file(c, handle); return c->file_offset; }

/* mode: 0 octets->C16 chars, 1 C16 chars->octets, 2/3 full LE16 cells.
 * Completed chunks alone advance the stream after an I/O error. */
static U root_io(Context *c, U handle, U address, U count, unsigned mode) {
    file(c, handle);
    unsigned write = mode & 1, width = mode >= 2 ? 2 : 1;
    if (write && !c->file_writable) return 0;
    U remaining = (ram_root_size - c->file_offset) / width;
    if (count > remaining) count = remaining;
    size_t at = span(address, count);
    if (!write) dirty(c, at, count);
    uint8_t bytes[4096];
    U done = 0;
    while (done < count) {
        U n = count - done;
        if (n > sizeof(bytes) / width) n = sizeof(bytes) / width;
        if (write) for (U i = 0; i < n; ++i) {
            uint16_t value = c->cells[at + done + i];
            bytes[i * width] = value;
            if (width == 2) bytes[i * 2 + 1] = value >> 8;
        }
        if (root_transfer(c->file_offset, bytes, n * width, write)) break;
        if (!write) for (U i = 0; i < n; ++i)
            c->cells[at + done + i] = bytes[i * width] |
                (width == 2 ? (uint16_t)bytes[i * 2 + 1] << 8 : 0);
        c->file_offset += n * width; done += n;
    }
    return done;
}
U rcc_native_ext_fread(Context *c, U output, U size, U count, U handle) {
    if (!size || count > UINT64_MAX / size) return 0;
    return root_io(c, handle, output, size * count, 0) / size;
}
U rcc_native_ext_fwrite(Context *c, U input, U size, U count, U handle) {
    if (!size || count > UINT64_MAX / size) return 0;
    return root_io(c, handle, input, size * count, 1) / size;
}
U rcc_native_ext_rcc_hosted_read_cells(Context *c, U handle, U output, U count) {
    return root_io(c, handle, output, count, 2);
}
U rcc_native_ext_rcc_hosted_write_cells(Context *c, U handle, U input, U count) {
    return root_io(c, handle, input, count, 3);
}
U rcc_native_ext_fflush(Context *c, U handle) {
    file(c, handle); return root_flush() ? 65535 : 0;
}
U rcc_native_ext_fclose(Context *c, U handle) {
    file(c, handle);
    U status = root_flush() ? 65535 : 0;
    c->file_open = 0; return status;
}
U rcc_native_ext_rcc_hosted_lock_image(Context *c, U path, U write) {
    (void)c; (void)path; return write > 1 ? 65535 : 0;
}
U rcc_native_ext_rcc_hosted_sync_image(Context *c) {
    (void)c; return root_flush() ? 65535 : 0;
}
