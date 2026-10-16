//! RAM access with moving address counters and local whole-word transfers.
//! PTR marks words 1..address; its first zero is one word *after* the RAM
//! target. This keeps the origin sentinel intact even for address zero.
use super::*;

fn step() -> &'static str {
    // Shift the three START counters from right to left to avoid overlap,
    // then extend PTR by one word. No bank*size product uses a 16-bit cell.
    ">>>>>>>>>>>>>>>>[->>>>>>>>+<<<<<<<<]<<<<<<<<[->>>>>>>>+<<<<<<<<]<<<<<<<<[->>>>>>>>+<<<<<<<<]>>+>>>>>>"
}
fn seek(b: &mut Bf, size: u16) {
    b.copy(BANK, 8);
    b.copy(OFF, 16);
    b.at(8);
    b.raw("[->>>>>>>>>>>>>>>>[-]");
    b.raw(&"+".repeat(usize::from(size)));
    b.raw("[-<<<<<<<<<<<<<<<<");
    b.raw(step());
    b.raw(">>>>>>>>>>>>>>>>]<<<<<<<<<<<<<<<<]");
    b.raw(">>>>>>>>[-<<<<<<<<");
    b.raw(step());
    b.raw(">>>>>>>>]<<<<<<<<");
    // At target+1 START. The previous word is either marked or the origin.
    b.raw("<<<<<<<<>>[<<<<<<<<]<<");
    b.at = 0;
}
fn target() -> &'static str {
    ">>>>>>>>>>[>>>>>>>>]"
}
fn cleanup(b: &mut Bf) {
    b.at(0);
    b.raw(target());
    b.raw("<<<<<<<<[-<<<<<<<<]<<");
    b.at = 0;
}
pub(super) fn access(size: u16, host: bool, store: bool) -> Bf {
    let mut b = Bf::default();
    seek(&mut b, size);
    if store {
        b.copy(VALUE, 5);
        b.at(5);
        b.raw("[->>>>>>>>+<<<<<<<<]>>>>>[>>>[->>>>>>>>+<<<<<<<<]>>>>>]>>>");
        // Carry back one word from the sentinel to the actual RAM cell.
        b.raw("[-<<<<<<<<+>>>>>>>>]<<<<<<<<<[-]>[-<+>]<");
        if host {
            b.raw("!");
        }
        // From RAM -> preceding PTR -> origin; address zero needs no special
        // backwards move and therefore never visits a negative tape index.
        b.raw("<<[<<<<<<<<]<<");
        b.at = 0;
        // Plain BF's byte TTY and ready flag retain the existing MMIO ABI.
        b.zero(BANK, |b| {
            b.zero(OFF, |b| {
                if !host {
                    b.at(4);
                    b.raw(".");
                }
                b.set(12, 1);
            })
        });
    } else {
        b.at(0);
        b.raw(target());
        b.raw("<<<<<<<<>>");
        if host {
            b.raw("?");
        }
        b.raw("[->+>+<<]>>[-<<+>>]<<>");
        // Carry SA left on PTR, preserving the original RAM value.
        b.raw("<<<[>>>[-<<<<<<<<+>>>>>>>>]<<<<<<<<<<<]>>>");
        b.at = 5;
    }
    cleanup(&mut b);
    if !store {
        b.take(5, 8);
    }
    b
}
