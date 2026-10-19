//! Only explicitly supported compiler/runtime externals can reach host imports.
use crate::memory::{self, EXIT, FAULT};
use rcc_ir::ir::{Function as IrFunction, IrType};
use wasm_encoder::{Function, Instruction as W};
pub(crate) fn validate(f: &IrFunction) -> Result<(), String> {
    let arity = match f.name.as_str() {
        "__wasm_host_write" | "__wasm_dom_commit" | "__wasm_effect_commit" => 2,
        "__wasm_trap" | "__wasm_exit" | "__rcc_f64_floor" | "__rcc_f64_sqrt"
        | "__rcc_f64_from_u64" => 1,
        _ => {
            return Err(format!(
                "Wasm external function {} has no runtime binding",
                f.name
            ))
        }
    };
    if f.parameters.len() != arity || f.is_vararg {
        return Err(format!("invalid Wasm service signature {}", f.name));
    }
    let valid = match f.name.as_str() {
        "__wasm_host_write" | "__wasm_dom_commit" | "__wasm_effect_commit" => {
            f.parameters[0].1.is_pointer()
                && f.parameters[1].1 == IrType::I32
                && f.return_type == IrType::Void
        }
        "__wasm_trap" | "__wasm_exit" => {
            f.parameters[0].1 == IrType::I16 && f.return_type == IrType::Void
        }
        _ => f.parameters[0].1 == IrType::I64 && f.return_type == IrType::I64,
    };
    if !valid {
        return Err(format!("invalid Wasm service signature {}", f.name));
    }
    Ok(())
}
pub(crate) fn emit(
    f: &IrFunction,
    address: u32,
    dom_import: u32,
    effect_import: u32,
) -> Result<Function, String> {
    validate(f)?;
    let mut b = Function::new([]);
    match f.name.as_str() {
        "__wasm_host_write" | "__wasm_dom_commit" | "__wasm_effect_commit" => {
            for i in [
                W::LocalGet(0),
                W::LocalGet(1),
                W::Call(address),
                W::I32WrapI64,
                W::LocalGet(1),
                W::I32WrapI64,
                W::Call(if f.name == "__wasm_host_write" {
                    0
                } else if f.name == "__wasm_dom_commit" {
                    dom_import
                } else {
                    effect_import
                }),
                W::I64Const(0),
            ] {
                b.instruction(&i);
            }
        }
        "__wasm_trap" => {
            for i in [
                W::LocalGet(0),
                W::I32WrapI64,
                W::GlobalSet(FAULT),
                W::Unreachable,
            ] {
                b.instruction(&i);
            }
        }
        "__wasm_exit" => {
            for i in [W::LocalGet(0), W::I32WrapI64, W::GlobalSet(EXIT)] {
                b.instruction(&i);
            }
            memory::fault(&mut b, 7);
        }
        "__rcc_f64_from_u64" => {
            b.instruction(&W::LocalGet(0));
            b.instruction(&W::F64ConvertI64U);
            b.instruction(&W::I64ReinterpretF64);
        }
        "__rcc_f64_floor" | "__rcc_f64_sqrt" => {
            b.instruction(&W::LocalGet(0));
            b.instruction(&W::F64ReinterpretI64);
            b.instruction(&if f.name.ends_with("floor") {
                W::F64Floor
            } else {
                W::F64Sqrt
            });
            b.instruction(&W::I64ReinterpretF64);
        }
        _ => unreachable!(),
    }
    b.instruction(&W::End);
    Ok(b)
}
