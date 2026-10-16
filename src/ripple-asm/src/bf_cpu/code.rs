//! Harvard code access. A temporary pointer-lane prefix locates instruction
//! cells; moving counters in lane zero avoid overflowing bank * bank_size.
use super::*;

fn normalize(b: &mut Bf, bank_size: u16) {
    b.set(TMP, bank_size);
    b.set(VALID, 1);
    b.loop_(VALID, |b| {
        b.lt(OFF, TMP, CMP);
        b.clear(VALID);
        b.zero(CMP, |b| {
            b.add(OFF, bank_size.wrapping_neg());
            b.add(BANK, 1);
            b.set(VALID, 1);
            b.zero(BANK, |b| {
                fault(b, 6);
                b.clear(VALID);
            });
        });
    });
}
fn less_pair(b: &mut Bf, ab: usize, ao: usize, cb: usize, co: usize, result: usize) {
    b.lt(ab, cb, result);
    // Equality without clobbering either input.
    b.copy(ab, cell(75));
    b.copy(cb, cell(76));
    b.loop_(cell(76), |b| {
        b.add(cell(76), u16::MAX);
        b.add(cell(75), u16::MAX);
    });
    b.zero(cell(75), |b| b.lt(ao, co, result));
    b.clear(cell(75));
}
/// Leave a prefix of ones in PTR, words 1..target-1. Target is operand 3,
/// word 37 + 4 * index. The three moving counters live in otherwise-zero START.
fn seek(b: &mut Bf, size: u16) {
    let p = 37 * 8;
    b.copy(BANK, p);
    b.copy(OFF, p + 8);
    b.at(8 + 2);
    // Fill the fixed prefix without touching RAM, code or the fast lane.
    b.raw(&format!("{}{}", "+>>>>>>>>".repeat(36), "<<"));
    // Now at START of word 37. Move the counters by one instruction, marking
    // every intervening pointer cell. All three counters move with the head.
    let step = counter_step();
    // step starts at START; ends at START four words later.
    b.raw("[");
    b.raw("-");
    b.raw(">>>>>>>>>>>>>>>>[-]");
    b.raw(&"+".repeat(usize::from(size)));
    b.raw("<<<<<<<<<<<<<<<<");
    b.raw(">>>>>>>>>>>>>>>>[ - <<<<<<<<<<<<<<<<");
    b.raw(step);
    b.raw(">>>>>>>>>>>>>>>>]<<<<<<<<<<<<<<<<]");
    b.raw(">>>>>>>>[-<<<<<<<<");
    b.raw(step);
    b.raw(">>>>>>>>]<<<<<<<<");
    // Return from target START using the prefix; leave target PTR zero.
    b.raw("<<<<<<<<>>[<<<<<<<<]<<");
    // Dynamic sequence is back at the origin.
    b.at = 0;
}
fn target() -> &'static str {
    ">>>>>>>>>>[>>>>>>>>]"
}
fn home_from_ptr() -> &'static str {
    "<<<<<<<<[<<<<<<<<]<<"
}
// Carry the whole word along the pointer prefix. Each hop is an affine BF
// transfer, rather than returning to the origin once per unit of its value.
// This also keeps large instruction operands practical in a literal BF CPU.
fn read_word(b: &mut Bf, word: usize, dest: usize) {
    b.clear(dest);
    b.at(0);
    b.raw(target());
    b.raw(&">".repeat(word * 8 + 1));
    b.raw("[->>+>+<<<]>>>[-<<<+>>>]<<<>>");
    for _ in 0..word {
        b.raw("[-<<<<<<<<+>>>>>>>>]<<<<<<<<");
    }
    b.raw("[-<<<<<<<<+>>>>>>>>]<<<<<<<<<<<");
    b.raw("[>>>[-<<<<<<<<+>>>>>>>>]<<<<<<<<<<<]<<");
    b.at = 0;
    b.take(5, dest);
}
fn write_word(b: &mut Bf, word: usize, source: usize) {
    b.copy(source, 5);
    b.at(5);
    b.raw("[->>>>>>>>+<<<<<<<<]>>>>>[>>>[->>>>>>>>+<<<<<<<<]>>>>>]>>>");
    for _ in 0..word {
        b.raw("[->>>>>>>>+<<<<<<<<]>>>>>>>>");
    }
    b.raw("<<[-]>>[-<<+>>]");
    b.raw(&"<".repeat(word * 8 + 3));
    b.raw(home_from_ptr());
    b.at = 0;
}
fn cleanup(b: &mut Bf) {
    b.at(0);
    b.raw(target());
    b.raw("<<<<<<<<[-<<<<<<<<]<<");
    b.at = 0;
}
fn counter_step() -> &'static str {
    "[->>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>+<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<]>>>>>>>>[->>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>+<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<]>>>>>>>>[->>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>+<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<]<<<<<<<<<<<<<<<<>>+>>>>>>>>+>>>>>>>>+>>>>>>>>+>>>>>>"
}
// Copy a fixed word into one of the moving START counters at the target.
fn target_counter(b: &mut Bf, source: usize, word: usize) {
    b.copy(source, 5);
    b.at(5);
    b.raw("[->>>>>>>>+<<<<<<<<]>>>>>[>>>[->>>>>>>>+<<<<<<<<]>>>>>]>>>");
    for _ in 0..word {
        b.raw("[->>>>>>>>+<<<<<<<<]>>>>>>>>");
    }
    b.raw("[-<<<<<+>>>>>]");
    b.raw(&"<".repeat(word * 8 + 3));
    b.raw(home_from_ptr());
    b.at = 0;
}
// Initialize a range in one physical pass. Re-seeking from the origin for
// every NOP would make loading an empty 64K code bank quadratic.
fn grow(b: &mut Bf, size: u16) {
    let db = cell(91);
    let doff = cell(92);
    b.copy(cell(73), db);
    b.copy(cell(EXTENT_BANK_WORD), cell(75));
    b.loop_(cell(75), |b| {
        b.add(cell(75), u16::MAX);
        b.add(db, u16::MAX);
    });
    b.copy(cell(74), doff);
    b.copy(cell(EXTENT_OFF_WORD), cell(75));
    b.loop_(cell(75), |b| {
        b.add(cell(75), u16::MAX);
        b.add(doff, u16::MAX);
    });
    b.lt(cell(74), cell(EXTENT_OFF_WORD), CMP);
    b.if_(CMP, |b| {
        b.add(db, u16::MAX);
        b.add(doff, size);
    });
    b.copy(cell(EXTENT_BANK_WORD), BANK);
    b.copy(cell(EXTENT_OFF_WORD), OFF);
    seek(b, size);
    target_counter(b, db, 0);
    target_counter(b, doff, 1);
    b.at(0);
    b.raw(target());
    b.raw("<<[");
    b.raw("-");
    b.raw(">>>>>>>>>>>>>>>>[-]");
    b.raw(&"+".repeat(usize::from(size)));
    b.raw("<<<<<<<<<<<<<<<<");
    let fill = format!(
        "{}{}",
        ">>>[-]>>>>>>>>[-]>>>>>>>>[-]>>>>>>>>[-]",
        "+".repeat(66)
    );
    let step = format!("{fill}{}{carry}", "<".repeat(27), carry = counter_step());
    b.raw(">>>>>>>>>>>>>>>>[-<<<<<<<<<<<<<<<<");
    b.raw(&step);
    b.raw(">>>>>>>>>>>>>>>>]<<<<<<<<<<<<<<<<]");
    b.raw(">>>>>>>>[-<<<<<<<<");
    b.raw(&step);
    b.raw(">>>>>>>>]<<<<<<<<");
    // The new end is the only zero pointer cell after the initialized prefix.
    b.raw("<<<<<<<<>>[-<<<<<<<<]<<");
    b.at = 0;
    b.copy(cell(73), cell(EXTENT_BANK_WORD));
    b.copy(cell(74), cell(EXTENT_OFF_WORD));
    b.copy(cell(88), cell(EXTENT_FULL_WORD));
}
pub(super) fn access(size: u16, cap: usize, store: bool) -> Bf {
    let mut b = Bf::default();
    read_register(&mut b, W2, BANK);
    read_register(&mut b, W3, OFF);
    b.zero(ERROR, |b| {
        normalize(b, size);
        // An exclusive capacity may lie exactly beyond PCB=65535.
        let cb = cap / usize::from(size);
        let co = cap % usize::from(size);
        if store && cb < 65536 {
            b.set(cell(73), cb as u16);
            b.set(cell(74), co as u16);
            less_pair(b, BANK, OFF, cell(73), cell(74), CMP);
            b.zero(CMP, |b| fault(b, 6));
        }
    });
    b.zero(ERROR, |b| {
        if store {
            b.copy(BANK, cell(83));
            b.copy(OFF, cell(84));
            // Grow to the end of the requested bank, bounded by reserved tape.
            less_pair(
                b,
                BANK,
                OFF,
                cell(EXTENT_BANK_WORD),
                cell(EXTENT_OFF_WORD),
                CMP,
            );
            b.if_(cell(87), |b| b.set(CMP, 1));
            b.zero(CMP, |b| {
                b.copy(BANK, cell(73));
                b.add(cell(73), 1);
                b.clear(cell(74));
                b.clear(cell(88));
                b.zero(cell(73), |b| b.set(cell(88), 1));
                let cb = cap / usize::from(size);
                let co = cap % usize::from(size);
                if cb < 65536 {
                    b.set(cell(85), cb as u16);
                    b.set(cell(86), co as u16);
                    less_pair(b, cell(85), cell(86), cell(73), cell(74), CMP);
                    b.if_(cell(88), |b| b.set(CMP, 1));
                    b.if_(CMP, |b| {
                        b.copy(cell(85), cell(73));
                        b.copy(cell(86), cell(74));
                        b.clear(cell(88));
                    });
                }
                grow(b, size);
            });
            b.copy(cell(83), BANK);
            b.copy(cell(84), OFF);
        } else {
            less_pair(
                b,
                BANK,
                OFF,
                cell(EXTENT_BANK_WORD),
                cell(EXTENT_OFF_WORD),
                CMP,
            );
            b.if_(cell(87), |b| b.set(CMP, 1));
            b.zero(CMP, |b| fault(b, 7));
        }
        b.zero(ERROR, |b| {
            seek(b, size);
            if store {
                // Snapshot the opcode's low octet; X registers remain unchanged.
                b.copy(reg(11), VALUE);
                b.set(TMP, 256);
                b.set(VALID, 1);
                b.loop_(VALID, |b| {
                    b.lt(VALUE, TMP, CMP);
                    b.clear(VALID);
                    b.zero(CMP, |b| {
                        b.add(VALUE, 65280);
                        b.set(VALID, 1);
                    });
                });
                for (word, src) in [(0, reg(14)), (1, reg(13)), (2, reg(12)), (3, VALUE)] {
                    write_word(b, word, src);
                }
            } else {
                // Address operands were snapshotted before overwriting X0..X3.
                for word in 0..4 {
                    read_word(b, word, reg(14 - word));
                }
            }
            cleanup(b);
        });
    });
    b
}
