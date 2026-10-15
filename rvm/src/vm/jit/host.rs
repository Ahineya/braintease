//! Native helper ABI, using the reference device and fault implementation.
use super::{HostIds, VM};
use crate::constants::TEXT40_BASE_WORD;
use cranelift_codegen::ir::{types, AbiParam};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{FuncId, Linkage, Module};

pub(super) fn build_module() -> Result<(JITModule, HostIds), String> {
    let mut builder = JITBuilder::with_flags(
        // Region size and warmup bound the cost of optimizing hot code.
        &[("opt_level", "speed")],
        cranelift_module::default_libcall_names(),
    )
    .map_err(|err| format!("JIT: {err}"))?;
    builder.symbol("rvm_jit_load", rvm_jit_load as *const u8);
    builder.symbol("rvm_jit_store", rvm_jit_store as *const u8);
    builder.symbol("rvm_jit_loadc", rvm_jit_loadc as *const u8);
    builder.symbol("rvm_jit_storc", rvm_jit_storc as *const u8);
    builder.symbol("rvm_jit_halt", rvm_jit_halt as *const u8);
    builder.symbol("rvm_jit_brk", rvm_jit_brk as *const u8);
    builder.symbol("rvm_jit_unknown", rvm_jit_unknown as *const u8);
    let mut module = JITModule::new(builder);
    let ids = HostIds {
        load: import(&mut module, "rvm_jit_load", &[types::I32, types::I32], true)?,
        store: import(
            &mut module,
            "rvm_jit_store",
            &[types::I32, types::I32, types::I32],
            true,
        )?,
        loadc: import(
            &mut module,
            "rvm_jit_loadc",
            &[types::I32, types::I32],
            true,
        )?,
        storc: import(
            &mut module,
            "rvm_jit_storc",
            &[types::I32, types::I32],
            true,
        )?,
        halt: import(&mut module, "rvm_jit_halt", &[], false)?,
        brk: import(&mut module, "rvm_jit_brk", &[], false)?,
        unknown: import(&mut module, "rvm_jit_unknown", &[types::I32], false)?,
    };
    Ok((module, ids))
}

fn import(
    module: &mut JITModule,
    name: &str,
    extra: &[cranelift_codegen::ir::Type],
    ret: bool,
) -> Result<FuncId, String> {
    let mut sig = module.make_signature();
    sig.params
        .push(AbiParam::new(module.target_config().pointer_type()));
    for ty in extra {
        sig.params.push(AbiParam::new(*ty));
    }
    if ret {
        sig.returns.push(AbiParam::new(types::I32));
    }
    module
        .declare_function(name, Linkage::Import, &sig)
        .map_err(|err| format!("JIT import {name} failed: {err}"))
}

/// The pointer is the VM that entered the block. The block holds no Rust
/// reference across the call.
extern "C" fn rvm_jit_load(vm: *mut VM, bank: i32, addr: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    match vm.read_cell(bank as u16, addr as u16) {
        Ok(value) => i32::from(value),
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

extern "C" fn rvm_jit_store(vm: *mut VM, bank: i32, addr: i32, value: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    let bank = bank as u16;
    let addr = addr as u16;
    let boundary = bank == 0 && usize::from(addr) < TEXT40_BASE_WORD;
    match vm.write_cell(bank, addr, value as u16) {
        Ok(()) => {
            // The interpreter marks output ready at instruction completion.
            vm.output_ready = true;
            if boundary {
                1
            } else {
                0
            }
        }
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

extern "C" fn rvm_jit_loadc(vm: *mut VM, bank: i32, addr: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    match vm.guest_loadc(bank as u16, addr as u16) {
        Ok(()) => 0,
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

extern "C" fn rvm_jit_storc(vm: *mut VM, bank: i32, addr: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    match vm.guest_storc(bank as u16, addr as u16) {
        Ok(()) => 0,
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

extern "C" fn rvm_jit_halt(vm: *mut VM) {
    unsafe { &mut *vm }.guest_halt();
}

extern "C" fn rvm_jit_brk(vm: *mut VM) {
    unsafe { &mut *vm }.guest_brk();
}

extern "C" fn rvm_jit_unknown(vm: *mut VM, opcode: i32) {
    let vm = unsafe { &mut *vm };
    vm.jit_fault = Some(format!("Unknown opcode: 0x{opcode:02X}"));
}
