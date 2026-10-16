use super::{LANES, ORIGIN};

// Code operands occupy lane 3, in reverse order, starting at word 37.
// CPU registers/extent metadata occupy lane 1. Never touch START, pointer,
// scratch or the fast lane: a command can execute halfway through BF STORE.
fn cpu_cell(word: usize) -> usize {
    ORIGIN + word * LANES + 1
}
fn code_cell(index: usize, word: usize) -> usize {
    ORIGIN + (37 + index * 4 + word) * LANES + 3
}
pub(super) fn code_capacity(size: u16, tape_len: usize, read: impl Fn(usize) -> u16) -> usize {
    use ripple_asm::bf_cpu::*;
    if size == 0 || tape_len <= cpu_cell(CAPACITY_FULL_WORD) || read(cpu_cell(MAGIC_WORD)) != MAGIC
    {
        return 0;
    }
    let full = read(cpu_cell(CAPACITY_FULL_WORD));
    if full > 1 {
        return 0;
    }
    let bank = read(cpu_cell(CAPACITY_BANK_WORD));
    let off = read(cpu_cell(CAPACITY_OFF_WORD));
    if off >= size || full != 0 && (bank != 0 || off != 0) {
        return 0;
    }
    let cap = if full != 0 {
        usize::from(size) * 65536
    } else {
        usize::from(bank) * usize::from(size) + usize::from(off)
    };
    // Retain the same ALU scratch and guards reserved by the linker.
    if cap == 0 || code_cell(cap - 1, 3).saturating_add(36 * LANES) >= tape_len {
        return 0;
    }
    cap
}
pub(super) fn code_extent(size: u16, read: impl Fn(usize) -> u16) -> usize {
    use ripple_asm::bf_cpu::*;
    if read(cpu_cell(EXTENT_FULL_WORD)) != 0 {
        usize::from(size) * 65536
    } else {
        usize::from(read(cpu_cell(EXTENT_BANK_WORD))) * usize::from(size)
            + usize::from(read(cpu_cell(EXTENT_OFF_WORD)))
    }
}
pub(super) fn code_growth(
    size: u16,
    cap: usize,
    extent: usize,
    start: usize,
    count: usize,
) -> Result<usize, u16> {
    use rvm::devices::{STATUS_BAD_STATE, STATUS_BOUNDS};
    if size == 0 || cap == 0 {
        return Err(STATUS_BOUNDS);
    }
    let end = start
        .checked_add(count)
        .filter(|&end| end <= cap)
        .ok_or(STATUS_BOUNDS)?;
    if extent > cap {
        return Err(STATUS_BAD_STATE);
    }
    if count == 0 {
        return Ok(extent);
    }
    Ok(if end > extent {
        end.div_ceil(usize::from(size))
            .saturating_mul(usize::from(size))
            .min(cap)
    } else {
        extent
    })
}
pub(super) fn code_write(
    size: u16,
    extent: usize,
    grown: usize,
    start: usize,
    code: &[[u16; 4]],
    mut write: impl FnMut(usize, u16),
) {
    use ripple_asm::bf_cpu::*;
    // Every fallible check precedes these writes. No tape-sized staging vector
    // is needed even when STORC-style growth initializes many reserved banks.
    for i in extent..grown {
        for word in 0..4 {
            write(code_cell(i, word), if word == 3 { 0x42 } else { 0 });
        }
    }
    for (i, w) in (start..start + code.len()).zip(code) {
        for word in 0..4 {
            write(
                code_cell(i, word),
                if word == 3 { w[0] & 255 } else { w[3 - word] },
            );
        }
    }
    if grown > extent {
        write(
            cpu_cell(EXTENT_BANK_WORD),
            (grown / usize::from(size)) as u16,
        );
        write(
            cpu_cell(EXTENT_OFF_WORD),
            (grown % usize::from(size)) as u16,
        );
        write(
            cpu_cell(EXTENT_FULL_WORD),
            u16::from(grown / usize::from(size) == 65536),
        );
    }
}
