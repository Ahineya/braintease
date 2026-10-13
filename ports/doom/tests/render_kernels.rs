//! Execute the actual linked guest leaf routines with hostile bank boundaries.
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
        vm.step().unwrap();
        assert!(vm.executed_instructions - before < 2_000_000, "kernel instruction budget");
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
    let binary = fs::read(root.join("render_kernels.bin")).unwrap();
    let disk = root.join("kernel-test.disk");
    let mut reports = Vec::new();
    // C ABI test covers real calls, with source/palette globals and local outputs.
    let mut vm = machine(&binary, &disk);
    while matches!(vm.state, VMState::Running) {
        vm.step().unwrap();
        assert!(vm.executed_instructions < 10_000_000, "C ABI fixture instruction budget");
    }
    assert_eq!(vm.registers[5], 0, "compiled C ABI fixture");
    reports.push(format!("{{\"case\":\"compiled_c_abi\",\"instructions\":{}}}",vm.executed_instructions));
    let lengths = [0,1,2,3,4,5,7,19,255,256,999,1000,1001,63999,64000,65535];
    for kernel in ["Doom_ConvertRGB565", "Doom_ConvertRGB565Octets"] {
    for (layout, (dst,src,palette)) in [
        ((0,7),(0,5),(80,3)),
        ((63000,7),(63999,5),(63744,3)),
        ((63999,7),(63000,5),(63880,3)),
        ((10000,65535),(5000,65534),(63800,3)),
        ((10000,7),(5000,5),(63800,65535)),
    ].into_iter().enumerate() {
        for count in lengths {
            // Tagged pointers intentionally stay within their live allocations;
            // large cases use separate heap banks so buffers cannot overlap.
            if layout == 3 && count > 1001 { continue; }
            let mut vm = machine(&binary, &disk);
            let d = physical(dst.0,dst.1); let s = physical(src.0,src.1);
            let p = physical(palette.0,palette.1);
            let colors: Vec<_> = (0u16..256).map(|v| v.wrapping_mul(257)^0xa5a5).collect();
            vm.memory[p..p+256].copy_from_slice(&colors);
            for i in 0..count as usize {
                let value = (i.wrapping_mul(3571)^0xfe19) as u16;
                vm.memory[s+i] = if kernel.ends_with("Octets") { value & 255 } else { value };
            }
            vm.memory[d-1..d+count as usize+1].fill(SENTINEL);
            let expected: Vec<_> = (0..count as usize).map(|i| colors[(vm.memory[s+i]&255) as usize]).collect();
            let steps = invoke(&mut vm,kernel,dst,src,&[palette.0,palette.1,count]);
            assert_eq!(&vm.memory[d..d+count as usize],expected.as_slice(),"RGB layout {layout} count {count}");
            assert_eq!(vm.memory[d-1],SENTINEL); assert_eq!(vm.memory[d+count as usize],SENTINEL);
            if count == 64000 && layout <= 1 {
                let limit = if kernel.ends_with("Octets") { 450_000 } else { 500_000 };
                assert!(steps < limit, "RGB hot path regressed");
            }
            reports.push(format!("{{\"case\":\"{kernel}_{layout}_{count}\",\"pixels\":{count},\"instructions\":{steps}}}"));
        }
    }
    }
    for (layout,(dst,src)) in [((100,7),(120,5)),((63900,7),(63999,5)),
                              ((63681,7),(63990,5)),((10000,65535),(5000,65534))].into_iter().enumerate() {
        for count in [0,1,2,3,19,127,200,255] {
            if layout == 3 && count > 127 { continue; }
            let mut vm = machine(&binary,&disk);
            let d = physical(dst.0,dst.1); let s = physical(src.0,src.1);
            let end = d + count as usize * 320 + 1;
            vm.memory[d-1..end].fill(SENTINEL);
            for i in 0..count as usize { vm.memory[s+i]=(i*73+19) as u16; }
            let steps = invoke(&mut vm,"Doom_CopyPatchColumn",dst,src,&[count]);
            for i in d-1..end {
                let expected = if i >= d && (i-d)%320 == 0 && (i-d)/320 < count as usize {
                    (((i-d)/320)*73+19) as u16
                } else { SENTINEL };
                assert_eq!(vm.memory[i],expected,"patch layout {layout} count {count} cell {i}");
            }
            assert!(steps < 3000,"patch hot path regressed");
            reports.push(format!("{{\"case\":\"patch_{layout}_{count}\",\"pixels\":{count},\"instructions\":{steps}}}"));
        }
    }
    for scale in [1u16, 2, 4] {
        for (layout, dst) in [(0,7),(63999,7),(63841,7),(10000,65535),(5000,65534)].into_iter().enumerate() {
            for count in [0u16,1,2,3,4,5,7,19,80,159,160,320] {
                for value in [0u16,243,0xa5a5] {
                    let mut vm = machine(&binary,&disk);
                    let d = physical(dst.0,dst.1);
                    let end = d + count as usize * scale as usize + 1;
                    vm.memory[d-1..end].fill(SENTINEL);
                    let steps = invoke(&mut vm,"Doom_FillSamples",dst,(value,count),&[scale]);
                    for i in d-1..end {
                        let expected = if i >= d && (i-d)%scale as usize == 0 && (i-d)/ (scale as usize) < count as usize {
                            value
                        } else {SENTINEL};
                        assert_eq!(vm.memory[i],expected,"fill scale {scale} layout {layout} count {count} cell {i}");
                    }
                    assert!(steps < 2000,"sampled fill instruction budget");
                    if count == 320 && layout != 1 && layout != 2 {
                        assert!(steps < 850,"sampled fill contiguous-bank hot path regressed");
                    }
                    if count == 0 {assert!(steps <= 3,"empty fill did more than return");}
                    reports.push(format!("{{\"case\":\"fill_sample_{scale}_{layout}_{count}_{value}\",\"instructions\":{steps}}}"));
                }
            }
        }
        for (layout, (dst,src,palette)) in [
            ((100,7),(123,5),(80,3)),
            ((63999,7),(63999,5),(63999,3)),
            ((63841,7),(63681,5),(63744,3)),
            ((10000,65535),(5000,65534),(63880,3)),
            ((10000,7),(5000,5),(63800,65535)),
        ].into_iter().enumerate() {
            for count in [0u16,1,2,3,4,5,7,19,80,159,160,320] {
                let mut vm = machine(&binary,&disk);
                let d = physical(dst.0,dst.1); let s = physical(src.0,src.1);
                let p = physical(palette.0,palette.1);
                let colors: Vec<_> = (0u16..256).map(|v| v.wrapping_mul(197)^0xc79e).collect();
                vm.memory[p..p+256].copy_from_slice(&colors);
                // Poison discarded cells with invalid indices: reading them
                // cannot accidentally look like a correct converted sample.
                vm.memory[s..s+count as usize*scale as usize+1].fill(SENTINEL);
                for i in 0..count as usize { vm.memory[s+i*scale as usize] = ((i*73+19)&255) as u16; }
                vm.memory[d-1..d+count as usize+1].fill(SENTINEL);
                let source_before = vm.memory[s..s+count as usize*scale as usize+1].to_vec();
                let steps = invoke(&mut vm,"Doom_ConvertRGB565SampledRow",dst,src,&[palette.0,palette.1,count,scale]);
                for i in 0..count as usize { assert_eq!(vm.memory[d+i],colors[(i*73+19)&255]); }
                assert_eq!(vm.memory[d-1],SENTINEL); assert_eq!(vm.memory[d+count as usize],SENTINEL);
                assert_eq!(&vm.memory[s..s+source_before.len()],source_before.as_slice());
                assert!(steps < 6000,"sampled row conversion instruction budget");
                reports.push(format!("{{\"case\":\"rgb_sample_{scale}_{layout}_{count}\",\"instructions\":{steps}}}"));
            }
        }
        for kernel in ["Doom_CopySamples", "Doom_CopyPatchColumnSampled"] {
            let stride = scale as usize * if kernel.contains("Patch") {320} else {1};
            for (layout,(dst,src)) in [((100,7),(120,5)),((63900,7),(63999,5)),
                                      ((63681,7),(63990,5)),((10000,65535),(5000,65534))].into_iter().enumerate() {
                for count in [0u16,1,2,3,19,127,200,255] {
                    if layout == 3 && count > 19 {continue;}
                    let mut vm = machine(&binary,&disk);
                    let d = physical(dst.0,dst.1); let s = physical(src.0,src.1);
                    let end = d + count as usize * stride + 1;
                    vm.memory[d-1..end].fill(SENTINEL);
                    vm.memory[s..s+count as usize*scale as usize+1].fill(0xbeef);
                    for i in 0..count as usize {vm.memory[s+i*scale as usize]=(i*73+19) as u16;}
                    let source_before = vm.memory[s..s+count as usize*scale as usize+1].to_vec();
                    let steps = invoke(&mut vm,kernel,dst,src,&[count,scale]);
                    for i in d-1..end {
                        let expected = if i >= d && (i-d)%stride == 0 && (i-d)/stride < count as usize {
                            (((i-d)/stride)*73+19) as u16
                        } else {SENTINEL};
                        assert_eq!(vm.memory[i],expected,"{kernel} scale {scale} layout {layout} count {count} cell {i}");
                    }
                    assert_eq!(&vm.memory[s..s+source_before.len()],source_before.as_slice());
                    assert!(steps < 4000,"sampled copy instruction budget");
                    reports.push(format!("{{\"case\":\"{kernel}_{scale}_{layout}_{count}\",\"instructions\":{steps}}}"));
                }
            }
        }
    }
    // A zero count must not even normalize or dereference a bogus pointer.
    let mut vm = machine(&binary,&disk);
    assert!(invoke(&mut vm,"Doom_FillSamples",(65535,65000),(0,0),&[]) <= 3);
    let report = format!("{{\"success\":true,\"bank_size\":64000,\"cases\":[{}]}}\n",reports.join(","));
    fs::write(root.join("kernel-results.json"),&report).unwrap();
    println!("Render kernels: {} cases passed (pixels, guards, bank tags, C ABI, saved registers and instruction budgets)",reports.len());
}
