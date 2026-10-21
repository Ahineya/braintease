/* Compile-time QEMU regression only, after the scheduler's resource snapshot.
 * Reproduce HP AX211 BAR page-table allocation without touching that MMIO. */
    U free_before_map = page_available(), owned_before_map = page_mapping_owned();
    device_map(0x48192c4000ULL, 16384);
    if (free_before_map - page_available() != 2 || page_mapping_owned() - owned_before_map != 2)
        fail("late device map ownership", page_mapping_owned() - owned_before_map);
    device_map(0x48192c4000ULL, 16384);
    if (free_before_map - page_available() != 2 || page_mapping_owned() - owned_before_map != 2)
        fail("repeated device map ownership", page_mapping_owned() - owned_before_map);
    U temporary_page = page_alloc();
    if (!temporary_page) fail("ownership test allocation", 0);
#if BOOT_PAGE_OWNERSHIP_TEST == 1
    page_free(temporary_page);
#elif BOOT_PAGE_OWNERSHIP_TEST != 2
#error Unsupported page ownership test
#endif
    serial("LATE_DEVICE_MAP_TEST_OK retained=2 repeated=0 context_leak=");
    hex(BOOT_PAGE_OWNERSHIP_TEST == 2); serial("\n");
