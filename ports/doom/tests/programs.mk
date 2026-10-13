# Small regressions compile the same engine sources as the complete port.
NATIVE_FIXTURES := map_decode world_widths clock_widths
FIXTURE_PROGRAMS := numeric libc wad_lookup wad_directory wad_read wad_read_small wad_read_perf asset_words graphics_cache demo_playback stat_storage info_storage projection_widths sky_widths event_widths texture_columns texture_metadata keyboard_controls config_bindings assets render_kernels raster_kernels memory_kernels alloc_fast $(NATIVE_FIXTURES)

SOURCES_numeric = tests/numeric.c engine/m_fixed.c engine/tables.c $(TABLE_SOURCES)
SOURCES_libc := tests/compat_libc.c
SOURCES_wad_lookup := tests/wad_lookup.c engine/w_wad.c engine/w_file.c engine/z_zone.c src/wad_io.c src/w_file_port.c src/asset_metadata.c
SOURCES_wad_directory := tests/wad_directory.c engine/w_wad.c engine/z_zone.c src/wad_io.c
SOURCES_wad_read := tests/wad_read.c src/wad_io.c
SOURCES_wad_read_small := tests/wad_read_small.c src/wad_io.c
SOURCES_wad_read_perf := tests/wad_read_perf.c src/wad_io.c
SOURCES_asset_words := tests/asset_words.c src/wad_io.c
# The short fixture includes the full fixture with its bulk reads disabled.
$(BUILD)/ripple/wad_read_small/test_wad_read_small.asm $(BUILD)/native/wad_read_small/wad_read_small: tests/wad_read.c
SOURCES_graphics_cache := tests/graphics_cache.c src/patch_cache.c src/wad_io.c
SOURCES_demo_playback := tests/demo_playback.c src/wad_io.c
SOURCES_stat_storage := tests/stat_storage.c
SOURCES_info_storage = tests/info_storage.c $(INFO_SOURCES)
SOURCES_projection_widths := tests/projection_widths.c engine/m_fixed.c
SOURCES_sky_widths := tests/sky_widths.c engine/r_sky.c
SOURCES_event_widths := tests/event_widths.c
SOURCES_texture_columns := tests/texture_columns.c
SOURCES_texture_metadata := tests/texture_metadata.c engine/w_wad.c engine/w_file.c engine/z_zone.c src/wad_io.c src/w_file_port.c src/asset_metadata.c src/patch_cache.c src/alloc_fast.c src/memory_fast.asm
SOURCES_keyboard_controls := tests/input_commands.c src/config_port.c
SOURCES_config_bindings := tests/config_bindings.c engine/m_controls.c engine/i_joystick.c
SOURCES_assets := tests/assets.c src/wad_io.c
SOURCES_render_kernels := tests/render_kernels.c src/render_fast.asm
SOURCES_raster_kernels := tests/raster_kernels.c src/raster_fast.asm
SOURCES_memory_kernels := tests/memory_kernels.c src/memory_fast.asm
SOURCES_alloc_fast := tests/alloc_fast.c src/alloc_fast.c src/memory_fast.asm
SOURCES_map_decode := tests/map_decode.c engine/m_fixed.c
SOURCES_world_widths := tests/world_widths.c engine/p_maputl.c engine/m_bbox.c engine/m_fixed.c
SOURCES_clock_widths := tests/clock_widths.c

GENERATE_projection_widths = node tools/fixtures.mjs projection_widths $(GENERATED)
GENERATE_texture_columns = node tools/fixtures.mjs texture_columns $(GENERATED)
GENERATE_texture_metadata = node tools/fixtures.mjs texture_metadata $(GENERATED)
GENERATE_keyboard_controls = node tools/fixtures.mjs keyboard_controls $(GENERATED)
GENERATE_config_bindings = node tools/fixtures.mjs config_bindings $(GENERATED)
