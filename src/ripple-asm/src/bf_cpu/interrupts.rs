use super::*;

pub(super) fn store() -> Bf {
    let mut b = Bf::default();
    b.zero(BANK, |b| {
        b.eq(OFF, 21, |b| {
            decode(b, VALUE, 96, true, false);
            encode_pending(b);
        });
        b.eq(OFF, 22, |b| {
            b.copy(VALUE, ENABLE);
            decode(b, VALUE, 112, false, false);
        });
        b.eq(OFF, 23, |b| {
            b.if_(BUSY, |b| b.if_(VALUE, |b| b.set(ACK, 1)))
        });
        b.eq(OFF, 24, |b| b.copy(VALUE, cell(48)));
        b.eq(OFF, 25, |b| b.copy(VALUE, cell(49)));
        // CAUSE is read-only. Restore all six canonical values after stores.
        sync_mmio(b);
    });
    b
}
pub(super) fn boundary(_bank: u16) -> Bf {
    let mut b = Bf::default();
    // Publish expirations at the next instruction boundary, matching RVM.
    // In particular HALT cannot raise an event after execution has stopped.
    b.if_(TIMER, |b| {
        b.zero(REM_LO, |b| {
            b.zero(REM_HI, |b| {
                b.set(pending(1), 1);
                encode_pending(b);
                b.copy(csr(3), REM_LO);
                b.copy(csr(4), REM_HI);
                sync_mmio(b);
            })
        })
    });
    b.clear(JUMP);
    b.zero(BUSY, |b| {
        b.if_(PENDING, |b| {
            b.if_(ENABLE, |b| {
                b.clear(TMP);
                for bit in 0..16 {
                    b.zero(TMP, |b| {
                        b.if_(pending(bit), |b| {
                            b.if_(enabled(bit), |b| {
                                b.set(TMP, 1);
                                b.clear(pending(bit));
                                b.set(CAUSE, (bit + 1) as u16);
                            })
                        })
                    });
                }
                b.if_(TMP, |b| {
                    for (r, c) in [(1, 0), (2, 1), (28, 7), (29, 8), (30, 9)] {
                        b.copy(reg(r), csr(c));
                    }
                    b.copy(ENABLE, csr(2));
                    b.set(BUSY, 1);
                    b.clear(ACK);
                    b.if_(csr(6), |b| {
                        b.copy(csr(5), reg(28));
                        b.copy(csr(6), reg(29));
                        b.copy(csr(6), reg(30));
                    });
                    b.copy(cell(48), reg(2));
                    b.copy(cell(49), reg(1));
                    b.set(JUMP, 1);
                    encode_pending(b);
                    sync_mmio(b);
                });
            })
        })
    });
    b.clear(TMP);
    b
}
pub(super) fn tick() -> Bf {
    let mut b = Bf::default();
    b.if_(TIMER, |b| {
        b.zero(RESET, |b| {
            b.zero(REM_LO, |b| b.add(REM_HI, u16::MAX));
            b.add(REM_LO, u16::MAX);
        });
    });
    b.clear(RESET);
    b
}
pub(super) fn read_csr() -> Bf {
    let mut b = Bf::default();
    b.clear(VALID);
    b.clear(RESULT);
    for c in 0..12 {
        b.eq(W2, c, |b| {
            b.set(VALID, 1);
            match c {
                0..=2 | 7..=9 => b.if_(BUSY, |b| b.copy(csr(c as usize), RESULT)),
                10 => b.copy(PENDING, RESULT),
                11 => b.set(RESULT, 2),
                _ => b.copy(csr(c as usize), RESULT),
            }
        });
    }
    b.zero(VALID, |b| fault(b, 2));
    for r in [1, 2] {
        b.eq(W1, r, |b| fault(b, 1));
    }
    b.zero(ERROR, |b| write_register(b, W1, RESULT));
    b
}
pub(super) fn write_csr(bank: u16, banks: Option<u16>) -> Bf {
    let mut b = Bf::default();
    read_register(&mut b, W1, VALUE);
    b.zero(ERROR, |b| {
        b.clear(VALID);
        for c in 0..11 {
            b.eq(W2, c, |b| {
                b.set(VALID, 1);
                match c {
                    0..=2 | 7..=9 => {
                        b.zero(BUSY, |b| fault(b, 3));
                        b.if_(BUSY, |b| b.copy(VALUE, csr(c as usize)));
                    }
                    3 | 4 => {
                        b.copy(VALUE, csr(c as usize));
                        b.copy(csr(3), REM_LO);
                        b.copy(csr(4), REM_HI);
                        b.clear(TIMER);
                        b.if_(REM_LO, |b| b.set(TIMER, 1));
                        b.if_(REM_HI, |b| b.set(TIMER, 1));
                        b.set(RESET, 1);
                    }
                    5 => {
                        b.zero(VALUE, |b| fault(b, 4));
                        if let Some(n) = banks {
                            b.set(TMP, n);
                            b.lt(VALUE, TMP, CMP);
                            b.zero(CMP, |b| fault(b, 4));
                        }
                        b.zero(ERROR, |b| b.copy(VALUE, csr(5)));
                    }
                    6 => {
                        b.if_(VALUE, |b| {
                            b.set(TMP, bank);
                            b.lt(VALUE, TMP, CMP);
                            b.zero(CMP, |b| fault(b, 4));
                            b.zero(csr(5), |b| fault(b, 4));
                        });
                        b.zero(ERROR, |b| b.copy(VALUE, csr(6)));
                    }
                    10 => {
                        decode(b, VALUE, 96, false, true);
                        encode_pending(b);
                        sync_mmio(b);
                    }
                    _ => unreachable!(),
                }
            });
        }
        b.zero(VALID, |b| fault(b, 2));
    });
    b
}
pub(super) fn iret() -> Bf {
    let mut b = Bf::default();
    b.zero(BUSY, |b| fault(b, 5));
    b.if_(BUSY, |b| {
        b.zero(ACK, |b| {
            for bit in 0..16 {
                b.eq(CAUSE, (bit + 1) as u16, |b| b.set(pending(bit), 1));
            }
        });
        for (r, c) in [(1, 0), (2, 1), (28, 7), (29, 8), (30, 9)] {
            b.copy(csr(c), reg(r));
        }
        b.copy(csr(2), ENABLE);
        decode(b, ENABLE, 112, false, false);
        b.clear(BUSY);
        b.clear(CAUSE);
        b.clear(ACK);
        for c in [0, 1, 2, 7, 8, 9] {
            b.clear(csr(c));
        }
        b.set(JUMP, 1);
        encode_pending(b);
        sync_mmio(b);
    });
    b
}
