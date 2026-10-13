//! Verify port-local cell memory routines against host snapshot semantics.
//! This is a Rust-VM test harness, not a host implementation of either kernel.
use rvm::{VM, vm::VMState};
use std::{fs, path::PathBuf};
const BANK: usize = 64000;
const SENTINEL: u16 = 0xdead;

fn symbol(vm: &VM, name: &str) -> usize {
    *vm.debug_symbols.iter().find(|(_, n)| n.as_str() == name).unwrap_or_else(|| panic!("missing symbol {name}")).0
}
fn invoke(vm: &mut VM, name: &str, dst: (u16, u16), src: (u16, u16), args: &[u16]) -> u64 {
    let entry = symbol(vm, name);
    let halt = vm.instructions.iter().position(|i| i.opcode == 0).unwrap();
    // Exercise the leaf's preservation contract independently of a compiler.
    vm.registers[1] = (entry % BANK) as u16;
    vm.registers[2] = (entry / BANK) as u16;
    vm.registers[3] = (halt % BANK) as u16;
    vm.registers[4] = (halt / BANK) as u16;
    vm.registers[7] = dst.0; vm.registers[8] = dst.1;
    vm.registers[9] = src.0; vm.registers[10] = src.1;
    vm.registers[23..27].copy_from_slice(&[0xace1, 0x9876, 0xcdef, 0xfedc]);
    vm.registers[28] = 2; vm.registers[29] = 1000;
    vm.registers[30] = 777; vm.registers[31] = 1;
    for (i, arg) in args.iter().enumerate() { vm.memory[2 * BANK + 999 - i] = *arg; }
    let saved: Vec<_> = [3,4,23,24,25,26,28,29,30,31].into_iter().map(|r| (r,vm.registers[r])).collect();
    let before = vm.executed_instructions;
    while matches!(vm.state, VMState::Running) {
        let pc = vm.registers[2] as usize * BANK + vm.registers[1] as usize;
        assert!(!matches!(vm.instructions[pc].opcode, 3..=5 | 11..=13),
            "memory kernel performs an avoidable bitwise operation at {pc}");
        vm.step().unwrap();
        assert!(vm.executed_instructions - before < 5_000_000, "kernel instruction budget");
    }
    for (r, value) in saved { assert_eq!(vm.registers[r], value, "callee-saved register {r}"); }
    vm.executed_instructions - before
}
fn physical(addr: u16, tag: u16) -> usize {
    let bank = match tag { 65535 => 1, 65534 => 2, _ => tag as usize };
    bank * BANK + addr as usize
}
fn machine(binary: &[u8], disk: &PathBuf) -> VM {
    let mut vm = VM::with_options(BANK as u16, BANK * 16, Some(disk.clone()));
    vm.host_input_enabled = false;
    vm.load_binary(binary).unwrap();
    vm
}
fn main() {
    let root = PathBuf::from(std::env::args().nth(1).unwrap());
    let binary = fs::read(root.join("memory_kernels.bin")).unwrap();
    let disk = root.join("kernel-test.disk");
    let mut reports = Vec::new();
    let mut vm = machine(&binary,&disk);
    while matches!(vm.state, VMState::Running) {
        vm.step().unwrap();
        assert!(vm.executed_instructions < 10_000_000,"C ABI budget");
    }
    assert_eq!(vm.registers[5],0,"compiled C memory ABI fixture");
    reports.push(format!("{{\"case\":\"compiled_c_abi\",\"instructions\":{}}}",vm.executed_instructions));
    let lengths = [0u32,1,2,3,4,5,7,320,999,64000,65535,65536,65537,128001,196613];
    for (layout,(dst,src)) in [((0,8),(0,3)),((63999,8),(63990,3)),
                              ((10000,65535),(5000,65534))].into_iter().enumerate() {
        for count in lengths {
            if layout == 2 && count > 999 { continue; }
            for name in ["Doom_Memcpy","Doom_Memset"] {
                let mut vm = machine(&binary,&disk);
                let d = physical(dst.0,dst.1); let s = physical(src.0,src.1);
                vm.memory[d-1..d+count as usize+1].fill(SENTINEL);
                for i in 0..count as usize { vm.memory[s+i] = (i.wrapping_mul(3571)^0xfe19) as u16; }
                let expected: Vec<_> = if name == "Doom_Memcpy" { vm.memory[s..s+count as usize].to_vec() }
                                      else { vec![0xf17a;count as usize] };
                let input = if name == "Doom_Memcpy" {src} else {(0xf17a,0)};
                let steps = invoke(&mut vm,name,dst,input,&[count as u16,(count>>16)as u16]);
                assert_eq!(&vm.memory[d..d+count as usize],expected.as_slice(),"{name} layout {layout} count {count}");
                assert_eq!(vm.memory[d-1],SENTINEL); assert_eq!(vm.memory[d+count as usize],SENTINEL);
                assert_eq!((vm.registers[5],vm.registers[6]),dst,"original return pointer");
                if count == 64000 {
                    assert!(steps < if name == "Doom_Memcpy" { 300_000 } else { 170_000 },"memory hot path regressed");
                }
                reports.push(format!("{{\"case\":\"{name}_{layout}_{count}\",\"cells\":{count},\"instructions\":{steps}}}"));
            }
        }
    }
    // Compare complete touched region, including untouched guards, with memmove
    // on a host snapshot. Offsets cover both overlap directions, disjoint copy,
    // same-address identity, adjacent banks, high count words and pointer tags.
    for delta in [-64002i32,-19,-1,0,1,19,64002] {
        for count in lengths {
            let mut vm = machine(&binary,&disk);
            let s = 5*BANK+63990;
            let d = (s as i32+delta) as usize;
            let begin = d.min(s)-1; let end = (d.max(s)+count as usize+1).max(begin+1);
            for i in begin..end { vm.memory[i] = (i.wrapping_mul(3571)^0xfe19) as u16; }
            let mut expected = vm.memory[begin..end].to_vec();
            expected.copy_within(s-begin..s-begin+count as usize,d-begin);
            let dst=((d%BANK)as u16,(d/BANK)as u16); let src=((s%BANK)as u16,(s/BANK)as u16);
            let steps = invoke(&mut vm,"Doom_Memmove",dst,src,&[count as u16,(count>>16)as u16]);
            assert_eq!(&vm.memory[begin..end],expected.as_slice(),"memmove delta {delta} count {count}");
            assert_eq!((vm.registers[5],vm.registers[6]),dst,"original return pointer");
            reports.push(format!("{{\"case\":\"move_{delta}_{count}\",\"cells\":{count},\"instructions\":{steps}}}"));
        }
    }
    for (dst,src) in [((5000,65535),(4981,1)),((4981,1),(5000,65535)),
                      ((5000,65534),(4981,2)),((4981,2),(5000,65534)),
                      ((5000,65535),(5000,1))] {
        let mut vm=machine(&binary,&disk);
        let d=physical(dst.0,dst.1);let s=physical(src.0,src.1);
        let begin=d.min(s)-1;let end=d.max(s)+401;
        for i in begin..end { vm.memory[i]=i as u16; }
        let mut expected=vm.memory[begin..end].to_vec();
        expected.copy_within(s-begin..s-begin+400,d-begin);
        let steps=invoke(&mut vm,"Doom_Memmove",dst,src,&[400,0]);
        assert_eq!(&vm.memory[begin..end],expected.as_slice(),"tagged overlapping copy");
        assert_eq!((vm.registers[5],vm.registers[6]),dst,"original return tag");
        reports.push(format!("{{\"case\":\"tagged_move_{}_{}\",\"instructions\":{steps}}}",dst.1,src.1));
    }
    let report=format!("{{\"success\":true,\"bank_size\":64000,\"cases\":[{}]}}\n",reports.join(","));
    fs::write(root.join("kernel-results.json"),report).unwrap();
    println!("Memory kernels: {} cases passed (full 32-bit counts, cells, guards, overlap, bank tags, C ABI, saved registers and budgets)",reports.len());
}
