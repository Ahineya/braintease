//! Execute GEP sequences with loads, stores, pointer fields, and matrix access.
//! Spill instructions are allowed; assertions concern observable memory values.
use rcc_ir::ir::{Value, IrType, FatPointer};
use crate::instr::{lower_gep, lower_store, lower_load};
use crate::regmgmt::{RegisterPressureManager, BankInfo};
use crate::naming::{new_function_naming, NameGenerator};
use rcc_codegen::{AsmInst, Reg};
use rcc_ir::BankTag;
use super::machine::Machine;
use super::gep_tests::{seed, seed_index, capture_pointer};

struct Program {
    mgr: RegisterPressureManager,
    naming: NameGenerator,
    insts: Vec<AsmInst>,
    bank_size: u16,
    offset: u16,
    next: u32,
}

impl Program {
    fn new(bank_size: u16) -> Self {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        mgr.set_memory_bank_size(bank_size);
        let naming = new_function_naming();
        let mut insts = vec![];
        let offset = bank_size - 4;
        seed(&mut mgr, naming.temp_name(1), offset, &mut insts);
        mgr.set_pointer_bank(naming.temp_name(1), BankInfo::Heap(10));
        Self { mgr, naming, insts, bank_size, offset, next: 10 }
    }

    fn id(&mut self) -> u32 { let id = self.next; self.next += 1; id }
    fn index(&mut self, value: i32) -> Value {
        let id = self.id();
        seed_index(&mut self.mgr, &self.naming, id, value, &mut self.insts)
    }
    fn gep(&mut self, base: &Value, index: Value, size: u32) -> Value {
        let id = self.id();
        self.insts.extend(lower_gep(&mut self.mgr, &mut self.naming, base, &[index], size, id, self.bank_size));
        Value::Temp(id)
    }
    fn store(&mut self, value: &Value, at: &Value, ty: &IrType) {
        self.insts.extend(lower_store(&mut self.mgr, &mut self.naming, value, at, ty));
    }
    fn load(&mut self, at: &Value, ty: &IrType) -> Value {
        let id = self.id();
        self.insts.extend(lower_load(&mut self.mgr, &mut self.naming, at, ty, id));
        Value::Temp(id)
    }
    fn output(&mut self, value: &Value, slot: u16) {
        let dest = Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(slot as i64)), bank: BankTag::Heap(40) });
        self.store(value, &dest, &IrType::I16);
    }
    fn key(&self, displacement: u32) -> (u16, u16) {
        let linear = self.offset as u32 + displacement;
        (10 + (linear / self.bank_size as u32) as u16, (linear % self.bank_size as u32) as u16)
    }
    fn run(&self) -> Machine { let mut m = Machine::new(self.bank_size); m.run(&self.insts); m }
}

#[test]
fn test_gep_then_store() {
    let mut p = Program::new(4096);
    let ptr = p.gep(&Value::Temp(1), Value::Constant(5), 1);
    p.store(&Value::Constant(42), &ptr, &IrType::I16);
    assert_eq!(p.run().memory.get(&p.key(5)), Some(&42));
}
#[test]
fn test_gep_then_load() {
    let mut p = Program::new(4096);
    let ptr = p.gep(&Value::Temp(1), Value::Constant(3), 1);
    let value = p.load(&ptr, &IrType::I16);
    p.output(&value, 0);
    let mut machine = Machine::new(p.bank_size);
    machine.memory.insert(p.key(3), 123);
    machine.run(&p.insts);
    assert_eq!(machine.memory.get(&(40, 0)), Some(&123));
}
#[test]
fn test_gep_with_dynamic_index_then_store() {
    let mut p = Program::new(4096);
    let index = p.index(51);
    let ptr = p.gep(&Value::Temp(1), index, 2);
    p.store(&Value::Constant(52), &ptr, &IrType::I16);
    assert_eq!(p.run().memory.get(&p.key(102)), Some(&52));
}
#[test]
fn test_gep_chain() {
    let mut p = Program::new(4096);
    let row = p.gep(&Value::Temp(1), Value::Constant(3), 10);
    let element = p.gep(&row, Value::Constant(7), 1);
    capture_pointer(&mut p.mgr, &mut p.naming, &element, Reg::Rv0, Reg::Rv1, &mut p.insts);
    let machine = p.run();
    let (bank, offset) = p.key(37);
    assert_eq!((machine.regs[Reg::Rv0 as usize], machine.regs[Reg::Rv1 as usize]), (offset, bank));
}
#[test]
fn test_gep_in_loop_pattern() {
    let mut p = Program::new(4096);
    for i in 0..12 {
        let ptr = p.gep(&Value::Temp(1), Value::Constant(i), 1);
        p.store(&Value::Constant(i), &ptr, &IrType::I16);
    }
    let machine = p.run();
    for i in 0..12 { assert_eq!(machine.memory.get(&p.key(i)), Some(&(i as u16))); }
}
#[test]
fn test_gep_with_large_array_crossing_banks() {
    let mut p = Program::new(4096);
    let ptr = p.gep(&Value::Temp(1), Value::Constant(5000), 1);
    let value = p.load(&ptr, &IrType::I16);
    p.output(&value, 0);
    let mut machine = Machine::new(p.bank_size);
    machine.memory.insert(p.key(5000), 777);
    machine.run(&p.insts);
    assert_eq!(machine.memory.get(&(40, 0)), Some(&777));
}
#[test]
fn test_gep_load_store_pointer() {
    for bank_size in [128, 4096, 64000, 65535] {
        let mut p = Program::new(bank_size);
        let ptr = p.gep(&Value::Temp(1), Value::Constant(3), 1);
        let value = Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(17)), bank: BankTag::Heap(31) });
        let ty = IrType::FatPtr(Box::new(IrType::I16));
        p.store(&value, &ptr, &ty);
        let loaded = p.load(&ptr, &ty);
        capture_pointer(&mut p.mgr, &mut p.naming, &loaded, Reg::Rv0, Reg::Rv1, &mut p.insts);
        let machine = p.run();
        assert_eq!((machine.regs[Reg::Rv0 as usize], machine.regs[Reg::Rv1 as usize]), (17, 31));
        assert_eq!(machine.memory.get(&p.key(3)), Some(&17));
        assert_eq!(machine.memory.get(&p.key(4)), Some(&31));
    }
}
#[test]
fn test_gep_chain_with_stores_and_loads() {
    let mut p = Program::new(4096);
    for j in 3..5 {
        let row = p.gep(&Value::Temp(1), Value::Constant(2), 10);
        let element = p.gep(&row, Value::Constant(j), 1);
        p.store(&Value::Constant(20 + j), &element, &IrType::I16);
    }
    for j in 3..5 {
        let row = p.gep(&Value::Temp(1), Value::Constant(2), 10);
        let element = p.gep(&row, Value::Constant(j), 1);
        let loaded = p.load(&element, &IrType::I16);
        p.output(&loaded, j as u16);
    }
    let machine = p.run();
    for j in 3..5 {
        assert_eq!(machine.memory.get(&p.key(20 + j)), Some(&((20 + j) as u16)));
        assert_eq!(machine.memory.get(&(40, j as u16)), Some(&((20 + j) as u16)));
    }
}
#[test]
fn test_gep_with_dynamic_2d_access() {
    let mut p = Program::new(4096);
    let i = p.index(300);
    let j = p.index(72);
    let row = p.gep(&Value::Temp(1), i, 100);
    let element = p.gep(&row, j, 1);
    p.store(&Value::Constant(999), &element, &IrType::I16);
    assert_eq!(p.run().memory.get(&p.key(30072)), Some(&999));
}
#[test]
fn test_gep_struct_field_simulation() {
    let mut p = Program::new(4096);
    let y = p.gep(&Value::Temp(1), Value::Constant(1), 1);
    p.store(&Value::Constant(10), &y, &IrType::I16);
    let next = p.gep(&Value::Temp(1), Value::Constant(2), 1);
    let value = Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(17)), bank: BankTag::Global });
    let ty = IrType::FatPtr(Box::new(IrType::I16));
    p.store(&value, &next, &ty);
    let loaded = p.load(&next, &ty);
    capture_pointer(&mut p.mgr, &mut p.naming, &loaded, Reg::Rv0, Reg::Rv1, &mut p.insts);
    let machine = p.run();
    assert_eq!(machine.memory.get(&p.key(1)), Some(&10));
    assert_eq!((machine.regs[Reg::Rv0 as usize], machine.regs[Reg::Rv1 as usize]), (17, 1));
}
