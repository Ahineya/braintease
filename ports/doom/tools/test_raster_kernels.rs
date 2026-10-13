//! Independent pixel/ABI oracle for the actual assembled column/span leaves.
//! Calls execute through VM::step; no host drawing routine replaces guest code.
use rvm::{vm::VMState, Instr, VM};
use std::{collections::BTreeMap, fs, path::PathBuf};

const BANK: usize = 64000;
const SENTINEL: u16 = 0xdead;
const SAVED: [usize; 10] = [3, 4, 23, 24, 25, 26, 28, 29, 30, 31];
const SEED: u32 = 0x16c0_ffee;

fn next(state: &mut u32) -> u32 {
    *state = state.wrapping_mul(1664525).wrapping_add(1013904223);
    *state
}

fn physical(pointer: (u16, u16)) -> usize {
    let bank = match pointer.1 { 65535 => 1, 65534 => 2, bank => bank as usize };
    bank * BANK + pointer.0 as usize
}

fn invoke(vm: &mut VM, entry: usize, dest: (u16, u16), source: (u16, u16),
          map: (u16, u16), position: u32, step: u32, count: u16, stride: Option<u16>) -> u64 {
    for r in 1..32 { vm.registers[r] = (r as u16).wrapping_mul(1783) ^ 0xa5a5; }
    vm.registers[1] = (entry % BANK) as u16;
    vm.registers[2] = (entry / BANK) as u16;
    // A caller in a different code bank catches an incorrect return-bank use.
    vm.registers[3] = 31;
    vm.registers[4] = 1;
    vm.registers[7] = dest.0; vm.registers[8] = dest.1;
    vm.registers[9] = source.0; vm.registers[10] = source.1;
    vm.registers[28] = 2; vm.registers[29] = 1000;
    vm.registers[30] = 777; vm.registers[31] = 1;
    let mut arguments = vec![map.0, map.1, position as u16, (position >> 16) as u16,
                     step as u16, (step >> 16) as u16, count];
    if let Some(stride) = stride { arguments.push(stride); }
    let stack = 2 * BANK + 1000;
    for (i, argument) in arguments.iter().enumerate() { vm.memory[stack - 1 - i] = *argument; }
    let saved: Vec<_> = SAVED.iter().map(|&r| (r, vm.registers[r])).collect();
    let stack_before = vm.memory[stack - 16..stack + 16].to_vec();
    let before = vm.executed_instructions;
    vm.state = VMState::Running;
    while matches!(vm.state, VMState::Running) {
        vm.step().expect("guest kernel step");
        assert!(vm.executed_instructions - before < 64 * u64::from(count) + 256, "raster instruction budget");
    }
    assert!(matches!(vm.state, VMState::Halted), "kernel must return to its caller");
    for (r, value) in saved { assert_eq!(vm.registers[r], value, "callee-saved register {r}"); }
    assert_eq!(vm.registers[0], 0);
    assert_eq!(&vm.memory[stack - 16..stack + 16], stack_before.as_slice(), "leaf changed caller stack");
    vm.executed_instructions - before
}

fn main() {
    let out = PathBuf::from(std::env::args().nth(1).expect("output directory"));
    let mut vm = VM::with_options(BANK as u16, BANK * 12, Some(out.join("test.disk")));
    vm.host_input_enabled = false;
    vm.load_binary(&fs::read(out.join("raster_test.bin")).unwrap()).unwrap();
    let entries: Vec<_> = ["Doom_DrawColumn", "Doom_DrawSpan", "Doom_DrawColumnLow", "Doom_DrawSpanLow"].iter().map(|name|
        *vm.debug_symbols.iter().find(|(_, symbol)| symbol.as_str() == *name)
            .unwrap_or_else(|| panic!("missing symbol {name}")).0).collect();
    assert!(vm.instructions.len() < BANK, "test return trap must be outside the kernel");
    vm.instructions.resize(BANK + 32, Instr::new(0, 0, 0, 0, 0));
    vm.memory.fill(SENTINEL);
    let mut reports = Vec::new();
    let mut state = SEED;
    let mut path_cases = BTreeMap::<&str, usize>::new();
    for (span,low) in [(false,false),(true,false),(false,true),(true,true)] {
        let entry = entries[span as usize + 2 * low as usize];
        let table_size = if span { 4096 } else { 128 };
        let stride = if span { if low {2} else {1} } else {320};
        let label = match (span,low) { (false,false)=>"column",(true,false)=>"span",
            (false,true)=>"column_low",(true,true)=>"span_low" };
        // If count is zero, even completely invalid pointers must be ignored.
        let before = vm.memory.clone();
        let instructions = invoke(&mut vm, entry, (65535, 65000), (65535, 65000),
                                  (65535, 65000), u32::MAX, u32::MAX, 0, None);
        assert!(instructions <= 5, "zero count performed more than a return");
        // Argument setup is the only allowed memory difference in this call.
        let stack = 2 * BANK + 1000;
        assert_eq!(&vm.memory[..stack - 7], &before[..stack - 7]);
        assert_eq!(&vm.memory[stack..], &before[stack..]);
        reports.push(format!("{{\"kernel\":\"{label}\",\"case\":\"zero_invalid_pointers\",\"instructions\":{instructions}}}"));
        let fast_last = if span { 59904 } else { 63872 };
        let layouts = [
            ((200, 7), (200, 5), (200, 3)),
            ((63680, 7), (fast_last, 5), (63744, 3)),
            ((63999, 7), (fast_last + 1, 5), (63745, 3)),
            ((63999, 7), (63999, 5), (63999, 3)),
            ((63000, 7), (10000, 65534), (50000, 65535)),
            ((32, 65535), (62000, 5), (63999, 3)),
            ((1200, 65534), (63900, 5), (1234, 4)),
            ((12345, 7), (59000, 65535), (12000, 65534)),
        ];
        for (layout, (dest, source, map)) in layouts.into_iter().enumerate() {
            let d = physical(dest); let s = physical(source); let m = physical(map);
            for sample in 0..(if span {65} else {64}) {
                // A maximal native count catches counter wrap and, in low
                // detail, a 131070-cell output crossing three bank edges.
                // Keep these long spans in separate heap buffers.
                if sample == 64 && layout != 0 && layout != 2 { continue; }
                let (position, step) = match sample {
                    0 => (0, 0),
                    1 => (u32::MAX, 1),
                    2 => (0, u32::MAX),
                    3 => (u32::MAX, 0),
                    4 => (0x0000ffff, 1),
                    5 => (0x7fffffff, 1),
                    6 => (0x80000000, 0x80000000),
                    7 => (0x0400fc00, 0x04010400),
                    _ => (next(&mut state), next(&mut state)),
                };
                let count = match sample { 0 => 0, 1 => 1, 2 => 2, 8 => 3, 9 => 127, 64 => u16::MAX, _ => 200 };
                let end = d + count as usize * stride + 1;
                let colors: Vec<_> = (0..256).map(|i| ((i * 73 + sample * 17 + 9) & 255) as u16).collect();
                let texels: Vec<_> = (0..table_size).map(|i| ((i * 17 + (i >> 2) + sample * 31) & 255) as u16).collect();
                vm.memory[s..s + table_size].copy_from_slice(&texels);
                vm.memory[m..m + 256].copy_from_slice(&colors);
                vm.memory[d - 1..end].fill(SENTINEL);
                let mut cursor = position;
                let expected: Vec<_> = (0..count).map(|_| {
                    let spot = if span { ((cursor >> 4) & 4032) | (cursor >> 26) }
                               else { (cursor >> 16) & 127 };
                    let pixel = colors[texels[spot as usize] as usize];
                    cursor = cursor.wrapping_add(step);
                    pixel
                }).collect();
                let instructions = invoke(&mut vm, entry, dest, source, map, position, step, count, None);
                // Every skipped column cell and both endpoint neighbors are guards.
                for cell in d - 1..end {
                    let expected_cell = if cell >= d && (cell - d) % stride < (if low {2} else {1})
                        && (cell - d) / stride < count as usize {
                        expected[(cell - d) / stride]
                    } else { SENTINEL };
                    assert_eq!(vm.memory[cell], expected_cell,
                        "{label} layout {layout} sample {sample} cell {} position {position:#x} step {step:#x}", cell - (d - 1));
                }
                assert_eq!(&vm.memory[s..s + table_size], texels.as_slice(), "source changed");
                assert_eq!(&vm.memory[m..m + 256], colors.as_slice(), "colormap changed");
                let fast = source.0 as usize + table_size <= BANK && map.0 as usize + 256 <= BANK;
                let path = match (span, low, fast) {
                    (false,false,true)=>"column_fast", (false,false,false)=>"column_split",
                    (true,false,true)=>"span_fast", (true,false,false)=>"span_split",
                    (false,true,true)=>"column_low_fast", (false,true,false)=>"column_low_split",
                    (true,true,true)=>"span_low_fast", (true,true,false)=>"span_low_split" };
                *path_cases.entry(path).or_default() += 1;
                // Columns deliberately replace per-pixel bitwise masks with
                // bounded add/subtract arithmetic. This costs <=3 extra
                // instructions per pixel but much less work on the BF CPU.
                if fast && count == 200 { assert!(instructions <= if low { if span {4500} else {4550} } else { if span {3700} else {3750} }, "hot raster path regressed"); }
                reports.push(format!("{{\"kernel\":\"{label}\",\"layout\":{layout},\"sample\":{sample},\"position\":{position},\"step\":{step},\"pixels\":{count},\"path\":\"{path}\",\"instructions\":{instructions}}}"));
            }
        }
    }
    // Strided leaves retain exactly one top-left sample from each output
    // block. Check both scales through the actual guest ABI, including source
    // and destination tags, split tables, and every discarded cell as a guard.
    for (span, name) in [(false, "Doom_DrawColumnStrided"), (true, "Doom_DrawSpanStrided")] {
        let entry = *vm.debug_symbols.iter().find(|(_, symbol)| symbol.as_str() == name)
            .unwrap_or_else(|| panic!("missing symbol {name}")).0;
        for scale in [2u16, 4] {
            let stride = if span { scale } else { 320 * scale };
            let zero_steps = invoke(&mut vm, entry, (65535, 65000), (65535, 65000),
                (65535, 65000), u32::MAX, u32::MAX, 0, Some(stride));
            assert!(zero_steps <= 5, "zero strided count must return before source or stride work");
            for (layout, (dest, source, map)) in [
                ((200, 7), (200, 5), (200, 3)),
                ((63999, 7), (63999, 5), (63999, 3)),
                ((63000, 7), (10000, 65534), (50000, 65535)),
                ((32, 65535), (62000, 5), (63999, 3)),
                ((1200, 65534), (63900, 5), (1234, 4)),
                ((12345, 7), (59000, 65535), (12000, 65534)),
            ].into_iter().enumerate() {
                let d = physical(dest); let s = physical(source); let m = physical(map);
                // Tagged destinations remain inside their allocations and do
                // not overlap the fixture's stack or other source tables.
                let count = if dest.1 >= 65534 { 3 } else { 50 };
                let table_size = if span { 4096 } else { 128 };
                for (position, step) in [(u32::MAX, 1), (0, u32::MAX),
                    (0x0400fc00, 0x04010400), (next(&mut state), next(&mut state))] {
                    let end = d + count as usize * stride as usize + 1;
                    let colors: Vec<_> = (0..256).map(|i| ((i * 73 + 9) & 255) as u16).collect();
                    let texels: Vec<_> = (0..table_size).map(|i| ((i * 17 + (i >> 2)) & 255) as u16).collect();
                    vm.memory[s..s + table_size].copy_from_slice(&texels);
                    vm.memory[m..m + 256].copy_from_slice(&colors);
                    vm.memory[d - 1..end].fill(SENTINEL);
                    let mut cursor = position;
                    let expected: Vec<_> = (0..count).map(|_| {
                        let spot = if span { ((cursor >> 4) & 4032) | (cursor >> 26) }
                                   else { (cursor >> 16) & 127 };
                        let pixel = colors[texels[spot as usize] as usize];
                        cursor = cursor.wrapping_add(step);
                        pixel
                    }).collect();
                    let instructions = invoke(&mut vm, entry, dest, source, map,
                        position, step, count, Some(stride));
                    for cell in d - 1..end {
                        let expected_cell = if cell >= d && (cell - d) % stride as usize == 0
                            && (cell - d) / (stride as usize) < count as usize {
                            expected[(cell - d) / stride as usize]
                        } else { SENTINEL };
                        assert_eq!(vm.memory[cell], expected_cell, "{name} scale {scale} layout {layout} cell {cell}");
                    }
                    assert_eq!(&vm.memory[s..s + table_size], texels.as_slice(), "strided source changed");
                    assert_eq!(&vm.memory[m..m + 256], colors.as_slice(), "strided map changed");
                    reports.push(format!("{{\"kernel\":\"{name}\",\"scale\":{scale},\"layout\":{layout},\"pixels\":{count},\"instructions\":{instructions}}}"));
                }
            }
        }
    }
    let paths = path_cases.iter().map(|(name, count)| format!("\"{name}\":{count}")).collect::<Vec<_>>().join(",");
    let result = format!("{{\"success\":true,\"bank_size\":{BANK},\"seed\":{SEED},\"call_count\":{},\"callee_saved_checked\":true,\"cross_bank_return_checked\":true,\"destination_guards_checked\":true,\"sources_unchanged\":true,\"caller_stack_unchanged\":true,\"path_cases\":{{{paths}}},\"cases\":[{}]}}\n", reports.len(), reports.join(","));
    fs::write(out.join("results.json"), result).unwrap();
    println!("Raster kernels: {} guest calls passed (pixels, bank carries, packed wraparound, both detail modes, guards, tags, ABI and instruction budgets)", reports.len());
}
