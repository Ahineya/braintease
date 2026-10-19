//! Global layout and sparse, relocated data segments (no zero-filled image).
use crate::memory::{BANK, CELLS};
use rcc_ir::{ir::*, target::Target, BankTag};
use std::collections::BTreeMap;
use wasm_encoder::{ConstExpr, DataSection};

pub(crate) struct Layout {
    pub addresses: BTreeMap<String, u64>,
    pub bank: u64,
    pub heap: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn union_cell_storage_keeps_both_words_of_relocated_callbacks() {
        let layout = Layout { addresses: BTreeMap::new(), bank: 1, heap: 5 * BANK };
        let codes = BTreeMap::from([("callback".into(), 0x00ff1234)]);
        let value = Value::ConstantAggregate(vec![Value::Constant(2), Value::Constant(3),
                                                  Value::Function("callback".into())]);
        let ty = IrType::Array { size: 4, element_type: Box::new(IrType::I16) };
        let mut cells = BTreeMap::new();
        layout.flatten(123, &ty, &value, &codes, &mut cells).unwrap();
        assert_eq!(cells, BTreeMap::from([(123, 2), (124, 3), (125, 0x1234), (126, 0xff)]));
        let too_small = IrType::Array { size: 3, element_type: Box::new(IrType::I16) };
        assert!(layout.flatten(123, &too_small, &value, &codes, &mut BTreeMap::new()).is_err());
    }
}
pub(crate) fn pointer(linear: u64) -> u64 {
    (linear / BANK) << 16 | linear % BANK
}
pub(crate) fn linear(ptr: u64) -> Result<u64, String> {
    if ptr >> 16 >= 256 || ptr & 65535 >= BANK {
        return Err(format!("invalid Wasm logical pointer {ptr:#x}"));
    }
    Ok((ptr >> 16) * BANK + (ptr & 65535))
}
impl Layout {
    pub fn new(module: &Module) -> Result<Self, String> {
        let mut cells = 1u64;
        for g in &module.globals {
            if g.linkage == Linkage::Declaration {
                return Err(format!(
                    "Wasm external global {} needs a definition",
                    g.name
                ));
            }
            cells = cells
                .checked_add(Target::Wasm.layout().size_in_cells(&g.var_type)?)
                .ok_or("Wasm global size overflow")?;
        }
        if cells > 16 * BANK {
            return Err("Wasm global storage exceeds 16 compatibility banks".into());
        }
        let bank = if cells > BANK { 32 } else { 1 };
        let mut cursor = bank * BANK + 1;
        let mut addresses = BTreeMap::new();
        for g in &module.globals {
            addresses.insert(g.name.clone(), pointer(cursor));
            cursor += Target::Wasm.layout().size_in_cells(&g.var_type)?;
        }
        // A single contiguous heap above globals keeps allocator and pointer
        // bounds explicit. Small programs retain the native bank-five origin.
        Ok(Self {
            addresses,
            bank,
            heap: cursor.max(5 * BANK),
        })
    }
    pub fn constant(&self, value: &Value, codes: &BTreeMap<String, u64>) -> Result<u64, String> {
        match value {
            Value::Constant(n) => Ok(*n as u64),
            Value::Undef => Ok(0),
            Value::Global(n) | Value::Function(n) => self
                .addresses
                .get(n)
                .or_else(|| codes.get(n))
                .copied()
                .ok_or_else(|| format!("unknown Wasm initializer symbol {n}")),
            Value::FatPtr(p) => {
                let v = self.constant(&p.addr, codes)?;
                if matches!(p.addr.as_ref(), Value::Constant(_)) {
                    let bank = match p.bank {
                        BankTag::Global => self.bank,
                        BankTag::Stack => 2,
                        BankTag::Heap(b) => u64::from(b),
                        BankTag::Null => 0,
                        _ => return Err("constant Wasm pointer has no bank".into()),
                    };
                    Ok((bank << 16) | (v & 65535))
                } else {
                    Ok(v)
                }
            }
            Value::AddressOffset { base, cells } => {
                let base = linear(self.constant(base, codes)?)?;
                let result = i128::from(base) + i128::from(*cells);
                if !(0..i128::from(CELLS)).contains(&result) {
                    return Err("Wasm initializer pointer offset out of bounds".into());
                }
                Ok(pointer(result as u64))
            }
            _ => Err(format!("nonconstant Wasm initializer {value:?}")),
        }
    }
    pub fn data(
        &self,
        module: &Module,
        codes: &BTreeMap<String, u64>,
    ) -> Result<DataSection, String> {
        let mut cells = BTreeMap::<u64, u16>::new();
        for g in &module.globals {
            if let Some(v) = &g.initializer {
                self.flatten(
                    linear(self.addresses[&g.name])?,
                    &g.var_type,
                    v,
                    codes,
                    &mut cells,
                )?;
            }
        }
        let mut data = DataSection::new();
        let mut bytes = Vec::new();
        let mut start = 0;
        let mut next = 0;
        for (cell, value) in cells {
            let offset = cell * 2;
            if offset.saturating_sub(next) > 32 && !bytes.is_empty() {
                data.active(0, &ConstExpr::i32_const(start as i32), bytes.drain(..));
            }
            if bytes.is_empty() {
                start = offset;
            }
            // Small zero gaps belong in one segment. Otherwise alternating
            // initialized/zero cells could exceed engines' segment limits.
            if !bytes.is_empty() {
                bytes.resize(bytes.len() + (offset - next) as usize, 0);
            }
            bytes.extend(value.to_le_bytes());
            next = offset + 2;
        }
        if !bytes.is_empty() {
            data.active(0, &ConstExpr::i32_const(start as i32), bytes);
        }
        Ok(data)
    }
    fn flatten(
        &self,
        at: u64,
        ty: &IrType,
        value: &Value,
        codes: &BTreeMap<String, u64>,
        out: &mut BTreeMap<u64, u16>,
    ) -> Result<(), String> {
        let layout = Target::Wasm.layout();
        match (ty, value) {
            (IrType::Array { size, element_type }, Value::ConstantArray(values)) => {
                if values.len() as u64 > *size {
                    return Err("too many Wasm array initializer values".into());
                }
                let stride = layout.size_in_cells(element_type)?;
                for (i, v) in values.iter().enumerate() {
                    self.flatten(
                        at + i as u64 * stride,
                        element_type,
                        &Value::Constant(*v),
                        codes,
                        out,
                    )?;
                }
            }
            (IrType::Array { size, element_type }, Value::ConstantAggregate(values)) => {
                if **element_type == IrType::I16 && values.iter().all(|v|
                    matches!(v, Value::Constant(_) | Value::FatPtr(_) | Value::Function(_) | Value::Global(_))) {
                    let mut cell = 0;
                    for v in values {
                        let pointer_value = matches!(v, Value::FatPtr(_) | Value::Function(_) | Value::Global(_));
                        let width = if pointer_value { 2 } else { 1 };
                        if cell + width > *size { return Err("cell initializer exceeds object storage".into()); }
                        let value_type = if pointer_value { IrType::FatPtr(Box::new(IrType::I16)) } else { IrType::I16 };
                        self.flatten(at + cell, &value_type, v, codes, out)?;
                        cell += width;
                    }
                    return Ok(());
                }
                if values.len() as u64 > *size {
                    return Err("too many Wasm aggregate initializer values".into());
                }
                let stride = layout.size_in_cells(element_type)?;
                for (i, v) in values.iter().enumerate() {
                    self.flatten(at + i as u64 * stride, element_type, v, codes, out)?;
                }
            }
            (IrType::Struct { fields, .. }, Value::ConstantAggregate(values)) => {
                if values.len() > fields.len() {
                    return Err("too many Wasm struct initializer values".into());
                }
                let mut at = at;
                for (ty, v) in fields.iter().zip(values) {
                    self.flatten(at, ty, v, codes, out)?;
                    at += layout.size_in_cells(ty)?;
                }
            }
            _ => {
                let count = layout.size_in_cells(ty)?;
                if count > 4 {
                    return Err(format!("unsupported Wasm initializer for {ty}"));
                }
                let value = self.constant(value, codes)?;
                for i in 0..count {
                    let word = (value >> (16 * i)) as u16;
                    if word != 0 {
                        out.insert(at + i, word);
                    }
                }
            }
        }
        Ok(())
    }
}
