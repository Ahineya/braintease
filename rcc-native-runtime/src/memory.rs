use super::*;

pub(super) fn linear(pointer: u64) -> usize {
    let bank = pointer >> 16;
    let offset = pointer & 65535;
    if bank >= 256 || offset >= BANK as u64 {
        fatal(&format!(
            "pointer outside native compatibility memory: 0x{pointer:x}"
        ));
    }
    bank as usize * BANK + offset as usize
}
pub(super) fn pointer(index: usize) -> u64 {
    (((index / BANK) as u64) << 16) | (index % BANK) as u64
}
impl Runtime {
    pub(super) fn exclude_globals(&mut self, range: std::ops::Range<usize>) {
        if self
            .allocated
            .iter()
            .any(|(&start, &size)| start < range.end && range.start < start + size)
        {
            fatal("native globals overlap an existing allocation");
        }
        self.free = self
            .free
            .iter()
            .flat_map(|(&start, &size)| {
                let end = start + size;
                if start >= range.end || end <= range.start {
                    return vec![(start, size)];
                }
                let mut pieces = Vec::new();
                if start < range.start {
                    pieces.push((start, range.start - start));
                }
                if end > range.end {
                    pieces.push((range.end, end - range.end));
                }
                pieces
            })
            .collect();
    }
    pub(super) fn allocate(&mut self, count: u64) -> u64 {
        allocate_from(&mut self.free, &mut self.allocated, count)
    }
    pub(super) fn release(&mut self, ptr: u64) {
        release_from(&mut self.free, &mut self.allocated, ptr)
    }
    pub(super) fn enable_process_memory(&mut self) {
        let end = address_space::KERNEL_START;
        if self
            .allocated
            .iter()
            .any(|(&start, &size)| start + size > end)
        {
            fatal("private allocation overlaps native kernel arena");
        }
        self.free.retain(|start, size| {
            if *start >= end {
                false
            } else {
                *size = (*size).min(end - *start);
                true
            }
        });
        self.memory.enable_processes();
    }
}

#[no_mangle]
unsafe extern "C" fn rcc_native_global_memory(ctx: *mut Runtime, bank: u64, cells: u64) -> u64 {
    use rcc_ir::native_image::{GLOBAL_BANK, GLOBAL_BANKS};
    if bank != u64::from(GLOBAL_BANK) || cells == 0 || cells > u64::from(GLOBAL_BANKS) * BANK as u64
    {
        fatal("invalid native global reservation");
    }
    let rt = unsafe { &mut *ctx };
    let range = bank as usize * BANK..bank as usize * BANK + cells as usize;
    rt.exclude_globals(range.clone());
    rt.extended_globals = Some(range);
    0
}

pub(super) fn allocate_from(
    free: &mut BTreeMap<usize, usize>,
    allocated: &mut BTreeMap<usize, usize>,
    count: u64,
) -> u64 {
    let Ok(count) = usize::try_from(count) else {
        return 0;
    };
    if count == 0 {
        return 0;
    }
    // Some existing RFL null checks inspect the address cell. Keep
    // allocation starts away from a bank's zero offset in this ABI.
    let Some((free_start, start, available)) = free.iter().find_map(|(&start, &available)| {
        let skip = usize::from(start % BANK == 0);
        (available >= count.saturating_add(skip)).then_some((start, start + skip, available - skip))
    }) else {
        return 0;
    };
    free.remove(&free_start);
    if available > count {
        free.insert(start + count, available - count);
    }
    allocated.insert(start, count);
    pointer(start)
}

pub(super) fn release_from(
    free: &mut BTreeMap<usize, usize>,
    allocated: &mut BTreeMap<usize, usize>,
    ptr: u64,
) {
    if ptr == 0 {
        return;
    }
    let mut start = linear(ptr);
    let Some(mut size) = allocated.remove(&start) else {
        fatal("invalid free or double free")
    };
    if let Some((&before, &length)) = free.range(..start).next_back() {
        if before + length == start {
            free.remove(&before);
            start = before;
            size += length;
        }
    }
    if let Some((&after, &length)) = free.range(start..).next() {
        if start + size == after {
            free.remove(&after);
            size += length;
        }
    }
    free.insert(start, size);
}

#[cfg(test)]
mod global_tests {
    use super::*;
    #[test]
    fn extended_globals_are_excluded_from_allocations_and_fork_privately() {
        let mut rt = Runtime::new();
        let cells = 140003;
        unsafe { rcc_native_global_memory(&mut rt, 32, cells) };
        let start = 32 * BANK;
        rt.memory.write(start + 64000, 0xbeef);
        let before = (32 - 5) * BANK - 1;
        assert_eq!(rt.allocate(before as u64), pointer(5 * BANK + 1));
        let after = rt.allocate(100);
        assert_eq!(after, pointer(start + cells as usize));
        rt.enable_process_memory();
        let mut child = rt.memory.clone();
        child.write(start + 64000, 0x1234);
        assert_eq!(rt.memory.read(start + 64000), 0xbeef);
        rt.release(after);
        assert_eq!(rt.allocate(200), after);
    }
}

// SAFETY CONTRACT for all generated-code entry points: ctx points to the one
// exclusively borrowed live Runtime. Other operands are logical values, never
// raw host addresses; every span is checked before accessing the address space.
#[no_mangle]
unsafe extern "C" fn rcc_native_load(ctx: *mut Runtime, ptr: u64, words: u64) -> u64 {
    let runtime = unsafe { &mut *ctx };
    runtime.poll();
    if !(1..=4).contains(&words) {
        fatal("invalid native load width");
    }
    let start = linear(ptr);
    if start
        .checked_add(words as usize)
        .is_none_or(|end| end > CELLS)
    {
        fatal("native load out of bounds");
    }
    let mut value = 0u64;
    for i in 0..words as usize {
        value |= u64::from(runtime.read(start + i)) << (16 * i);
    }
    value
}
#[no_mangle]
unsafe extern "C" fn rcc_native_store(ctx: *mut Runtime, ptr: u64, value: u64, words: u64) -> u64 {
    let runtime = unsafe { &mut *ctx };
    runtime.poll();
    if !(1..=4).contains(&words) {
        fatal("invalid native store width");
    }
    let start = linear(ptr);
    if start
        .checked_add(words as usize)
        .is_none_or(|end| end > CELLS)
    {
        fatal("native store out of bounds");
    }
    for i in 0..words as usize {
        runtime.write(start + i, (value >> (16 * i)) as u16);
    }
    0
}
#[no_mangle]
unsafe extern "C" fn rcc_native_gep(ctx: *mut Runtime, ptr: u64, offset: u64) -> u64 {
    let start = linear(ptr);
    let result = (start as i128) + (offset as i64 as i128);
    if !(0..CELLS as i128).contains(&result) {
        eprintln!(
            "native pointer: base={ptr:#x}, offset={}{}",
            offset as i64,
            unsafe { &*ctx }.processes.diagnostic()
        );
        fatal("native pointer arithmetic out of bounds");
    }
    pointer(result as usize)
}
#[no_mangle]
unsafe extern "C" fn rcc_native_stack_mark(ctx: *mut Runtime) -> u64 {
    unsafe { (*ctx).stack as u64 }
}
#[no_mangle]
unsafe extern "C" fn rcc_native_stack_alloc(ctx: *mut Runtime, cells: u64) -> u64 {
    let runtime = unsafe { &mut *ctx };
    runtime.poll();
    let start = runtime.stack;
    runtime.stack = start
        .checked_add(usize::try_from(cells).unwrap_or_else(|_| fatal("native frame size overflow")))
        .filter(|end| *end < 5 * BANK)
        .unwrap_or_else(|| fatal("native v1 stack region exhausted"));
    pointer(start)
}
#[no_mangle]
unsafe extern "C" fn rcc_native_stack_restore(ctx: *mut Runtime, mark: u64) -> u64 {
    let runtime = unsafe { &mut *ctx };
    if mark < 2 * BANK as u64 + 1 || mark > runtime.stack as u64 {
        fatal("invalid native stack restore");
    }
    runtime.stack = mark as usize;
    0
}
#[no_mangle]
unsafe extern "C" fn rcc_native_register(ctx: *mut Runtime, token: u64, address: u64) -> u64 {
    let runtime = unsafe { &mut *ctx };
    runtime
        .functions
        .insert(token as u32, (address as usize, (token >> 32) as u32));
    0
}
#[no_mangle]
unsafe extern "C" fn rcc_native_function(ctx: *mut Runtime, token: u64, arguments: u64) -> u64 {
    let runtime = unsafe { &mut *ctx };
    let (address, arity) = runtime
        .functions
        .get(&(token as u32))
        .copied()
        .unwrap_or_else(|| fatal("invalid native function pointer"));
    if arguments != u64::from(arity) {
        fatal("native indirect call signature mismatch");
    }
    address as u64
}

#[no_mangle]
unsafe extern "C" fn rcc_native_poll(ctx: *mut Runtime) -> u64 {
    unsafe { &mut *ctx }.poll();
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn allocator_reuses_and_coalesces_without_zero_offset_allocations() {
        let mut runtime = Runtime::new();
        let a = runtime.allocate((BANK - 1) as u64);
        let b = runtime.allocate(32);
        assert_eq!(a & 65535, 1);
        assert_eq!(b & 65535, 1);
        runtime.release(b);
        let c = runtime.allocate(16);
        assert_eq!(b, c);
        runtime.release(c);
        runtime.release(a);
        assert_eq!(runtime.allocate(0), 0);
        assert_eq!(runtime.allocate(u64::MAX), 0);
        let large = runtime.allocate((BANK * 3) as u64);
        assert_ne!(large, 0);
        runtime.release(large);
    }
    #[test]
    fn wide_memory_and_gep_normalize_bank_crossings() {
        let mut runtime = Runtime::new();
        let base = pointer(6 * BANK - 2);
        let value = 0xfedc_ba98_7654_3210;
        // All ctx pointers here borrow this live runtime exclusively per call.
        unsafe {
            rcc_native_store(&mut runtime, base, value, 4);
            assert_eq!(rcc_native_load(&mut runtime, base, 4), value);
            let next = rcc_native_gep(&mut runtime, base, 3);
            assert_eq!(next, pointer(6 * BANK + 1));
            assert_eq!(rcc_native_gep(&mut runtime, next, (-3i64) as u64), base);
            let mark = rcc_native_stack_mark(&mut runtime);
            rcc_native_stack_alloc(&mut runtime, 100);
            rcc_native_stack_restore(&mut runtime, mark);
            assert_eq!(rcc_native_stack_mark(&mut runtime), mark);
        }
    }
    #[test]
    fn rgb565_front_is_read_only_and_ram_outside_the_buffers_survives() {
        let mut runtime = Runtime::new();
        runtime.write(rvm::constants::HDR_DISP_RESOLUTION, 0x0202);
        runtime.write(rvm::constants::HDR_DISP_MODE, rvm::constants::DISP_RGB565);
        runtime.write(32, 123);
        runtime.write(36, 0xf800);
        runtime.write(100, 42);
        assert_eq!(runtime.read(32), 0);
        runtime.write(rvm::constants::HDR_DISP_FLUSH, 1);
        assert_eq!(
            runtime.display.snapshot().unwrap().pixels,
            [0xf800, 0, 0, 0]
        );
        assert_eq!(runtime.read(100), 42);
    }
}
