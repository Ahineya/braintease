//! Read-only native capabilities, confined to straight-line IR regions. A
//! denied/missed grant is not reacquired for every subsequent scalar access.
use super::emit::Cg;
use cranelift_codegen::ir::{condcodes::IntCC, types, InstBuilder, MemFlagsData, Value};
use rcc_ir::ir::Instruction as I;

#[derive(Clone, Copy)]
pub(super) struct ReadView {
    pub valid: Value,
    start: Value,
    count: Value,
    base: Value,
}

impl ReadView {
    pub(super) fn values(self) -> [Value; 4] {
        [self.valid, self.start, self.count, self.base]
    }
    pub(super) fn from_values(values: &[Value]) -> Self {
        Self {
            valid: values[0],
            start: values[1],
            count: values[2],
            base: values[3],
        }
    }
}

/// Require at least two loads between barriers; isolated scalar loads cannot
/// amortize acquisition. Never carry native addresses across an IR edge or a
/// resumable checkpoint. GEP's inline branch preserves the view, its helper
/// branch explicitly invalidates it. This does not hoist across loops.
pub(super) fn read_regions(instructions: &[I]) -> Vec<bool> {
    let mut result = vec![false; instructions.len()];
    let mut start = 0;
    let mut loads = 0;
    for (i, instruction) in instructions.iter().enumerate() {
        let safe = matches!(
            instruction,
            I::Load {
                volatile: false,
                ..
            } | I::Binary { .. }
                | I::Unary { .. }
                | I::GetElementPtr { .. }
                | I::Alloca { .. }
                | I::Select { .. }
                | I::Cast { .. }
                | I::Phi { .. }
                | I::Comment(_)
                | I::DebugLoc { .. }
        );
        if !safe {
            if loads >= 2 {
                result[start..i].fill(true);
            }
            start = i + 1;
            loads = 0;
        } else if matches!(instruction, I::Load { .. }) {
            loads += 1;
        }
    }
    if loads >= 2 {
        result[start..].fill(true);
    }
    result
}

impl Cg<'_, '_> {
    fn acquire_read_view(&mut self, pointer: Value, words: Value) -> Result<ReadView, String> {
        let read = self.k(1);
        let record = self.runtime("rcc_native_memview_v1", &[pointer, words, read])?;
        let granted = self.b.create_block();
        let denied = self.b.create_block();
        let done = self.b.create_block();
        for _ in 0..4 {
            self.b.append_block_param(done, types::I64);
        }
        let exists = self.b.ins().icmp_imm_u(IntCC::NotEqual, record, 0);
        self.b.ins().brif(exists, granted, &[], denied, &[]);
        self.b.switch_to_block(denied);
        let zero = self.k(0);
        self.b
            .ins()
            .jump(done, &[zero.into(), zero.into(), zero.into(), zero.into()]);
        self.b.switch_to_block(granted);
        // A nonzero helper result promises a readable aligned 40-byte record.
        // Copy immediately; no runtime calls before all five loads complete.
        let fields: Vec<_> = (0..5)
            .map(|i| {
                self.b
                    .ins()
                    .load(types::I64, MemFlagsData::trusted(), record, i * 8)
            })
            .collect();
        let start = fields[2];
        let count = fields[3];
        let base = fields[4];
        let version = self.b.ins().icmp_imm_u(IntCC::Equal, fields[0], 1);
        let flags = self.b.ins().icmp_imm_u(IntCC::Equal, fields[1], 1);
        let nonzero = self.b.ins().icmp_imm_u(IntCC::NotEqual, count, 0);
        let bounded = self
            .b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThanOrEqual, count, 64000);
        let start_ok = self
            .b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThan, start, 65536 * 64000);
        let bank_size = self.k(64000);
        let offset = self.b.ins().urem(start, bank_size);
        let remaining = self.b.ins().isub(bank_size, offset);
        let bank_ok = self
            .b
            .ins()
            .icmp(IntCC::UnsignedLessThanOrEqual, count, remaining);
        let base_ok = self.b.ins().icmp_imm_u(IntCC::NotEqual, base, 0);
        let base_low = self.b.ins().band_imm_u(base, 1);
        let aligned = self.b.ins().icmp_imm_u(IntCC::Equal, base_low, 0);
        let bytes = self.b.ins().ishl_imm_u(count, 1);
        let last_byte = self.b.ins().iadd(base, bytes);
        let host_ok = self
            .b
            .ins()
            .icmp(IntCC::UnsignedGreaterThanOrEqual, last_byte, base);
        let mut valid = version;
        for condition in [
            flags, nonzero, bounded, start_ok, bank_ok, base_ok, aligned, host_ok,
        ] {
            valid = self.b.ins().band(valid, condition);
        }
        let valid = self.b.ins().uextend(types::I64, valid);
        self.b.ins().jump(
            done,
            &[valid.into(), start.into(), count.into(), base.into()],
        );
        self.b.switch_to_block(done);
        let p = self.b.block_params(done);
        Ok(ReadView {
            valid: p[0],
            start: p[1],
            count: p[2],
            base: p[3],
        })
    }

    pub(super) fn scalar_load(
        &mut self,
        pointer: Value,
        cells: u64,
        volatile: bool,
    ) -> Result<Value, String> {
        let words = self.k(cells as i64);
        if !self.compiler.read_views || !self.read_region || volatile || !(1..=4).contains(&cells) {
            return self.runtime("rcc_native_load", &[pointer, words]);
        }
        let view = match self.read_view {
            Some(view) => view,
            None => self.acquire_read_view(pointer, words)?,
        };
        let bank = self.b.ins().ushr_imm_u(pointer, 16);
        let offset = self.b.ins().band_imm_u(pointer, 65535);
        let bank_ok = self
            .b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThan, bank, 65536);
        let ram = self.b.ins().icmp_imm_u(IntCC::NotEqual, bank, 0);
        let offset_ok =
            self.b
                .ins()
                .icmp_imm_u(IntCC::UnsignedLessThanOrEqual, offset, 64000 - cells as i64);
        let dense = self.b.ins().imul_imm_s(bank, 64000);
        let dense = self.b.ins().iadd(dense, offset);
        let after_start = self
            .b
            .ins()
            .icmp(IntCC::UnsignedGreaterThanOrEqual, dense, view.start);
        let delta = self.b.ins().isub(dense, view.start);
        let enough =
            self.b
                .ins()
                .icmp_imm_u(IntCC::UnsignedGreaterThanOrEqual, view.count, cells as i64);
        let available = self.b.ins().iadd_imm_s(view.count, -(cells as i64));
        let covered = self
            .b
            .ins()
            .icmp(IntCC::UnsignedLessThanOrEqual, delta, available);
        let mut valid = self.b.ins().icmp_imm_u(IntCC::NotEqual, view.valid, 0);
        for condition in [bank_ok, ram, offset_ok, after_start, enough, covered] {
            valid = self.b.ins().band(valid, condition);
        }
        let fast = self.b.create_block();
        let slow = self.b.create_block();
        let done = self.b.create_block();
        for _ in 0..5 {
            self.b.append_block_param(done, types::I64);
        }
        self.b.ins().brif(valid, fast, &[], slow, &[]);
        self.b.switch_to_block(fast);
        self.runtime("rcc_native_poll", &[])?;
        let bytes = self.b.ins().ishl_imm_u(delta, 1);
        let address = self.b.ins().iadd(view.base, bytes);
        let mut value = self.k(0);
        for cell in 0..cells {
            let word = self.b.ins().load(
                types::I16,
                MemFlagsData::trusted(),
                address,
                (cell * 2) as i32,
            );
            let word = self.b.ins().uextend(types::I64, word);
            let shifted = self.b.ins().ishl_imm_u(word, (cell * 16) as i64);
            value = self.b.ins().bor(value, shifted);
        }
        let mut result = vec![value.into()];
        result.extend(view.values().map(cranelift_codegen::ir::BlockArg::from));
        self.b.ins().jump(done, &result);
        self.b.switch_to_block(slow);
        let value = self.runtime("rcc_native_load", &[pointer, words])?;
        let zero = self.k(0);
        // No old backing address remains live on the helper-return edge.
        self.b.ins().jump(
            done,
            &[
                value.into(),
                zero.into(),
                zero.into(),
                zero.into(),
                zero.into(),
            ],
        );
        self.b.switch_to_block(done);
        let value = self.b.block_params(done)[0];
        self.read_view = Some(ReadView::from_values(&self.b.block_params(done)[1..]));
        Ok(value)
    }
}
