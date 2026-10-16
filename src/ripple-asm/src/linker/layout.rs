//! Bank-aware placement and monotone branch relaxation. The ISA stays unchanged.
use super::{CodeSegment, Linker};
use crate::types::{Instruction, ObjectFile, Opcode};
use std::collections::HashMap;

pub(super) struct Placement {
    pub instructions: Vec<Instruction>,
    pub maps: Vec<Vec<usize>>,
    /// 0 = local branch, 1 = non-linking JAL, 2 = inverse branch + JAL.
    pub far: Vec<Vec<u8>>,
}

fn branch(op: u8) -> bool {
    matches!(
        Opcode::from_u8(op),
        Some(Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge)
    )
}
fn always(i: Instruction) -> bool {
    i.word1 == i.word2 && matches!(Opcode::from_u8(i.opcode), Some(Opcode::Beq | Opcode::Bge))
}
fn never(i: Instruction) -> bool {
    i.word1 == i.word2 && matches!(Opcode::from_u8(i.opcode), Some(Opcode::Bne | Opcode::Blt))
}
fn inverse(op: u8) -> Opcode {
    match Opcode::from_u8(op) {
        Some(Opcode::Beq) => Opcode::Bne,
        Some(Opcode::Bne) => Opcode::Beq,
        Some(Opcode::Blt) => Opcode::Bge,
        Some(Opcode::Bge) => Opcode::Blt,
        _ => unreachable!("branch relocation was validated"),
    }
}

pub(super) fn place(
    objects: &[ObjectFile],
    order: &[(usize, CodeSegment)],
    bank: usize,
) -> Result<Placement, Vec<String>> {
    let mut symbols = HashMap::new();
    for (file, object) in objects.iter().enumerate() {
        for (name, label) in &object.labels {
            let index = label.absolute_address as usize / 4;
            if index > object.instructions.len() {
                return Err(vec![format!("code label '{name}' is outside its object")]);
            }
            if symbols.insert(name.as_str(), (file, index)).is_some() {
                return Err(vec![format!("Duplicate label '{name}' in file {file}")]);
            }
        }
    }
    let mut branches = Vec::new();
    for (file, object) in objects.iter().enumerate() {
        for (&index, reference) in &object.unresolved_references {
            if index >= object.instructions.len() {
                return Err(vec![format!(
                    "File {file}: unresolved reference index out of range: {index}"
                )]);
            }
            if branch(object.instructions[index].opcode) && reference.ref_type != "branch" {
                return Err(vec![format!(
                    "File {file}, instruction {index}: branch relocation must name a code label"
                )]);
            }
            if reference.ref_type == "branch" {
                if !branch(object.instructions[index].opcode) {
                    return Err(vec![format!("File {file}, instruction {index}: branch relocation on a non-branch instruction")]);
                }
                let &(target_file, target) = symbols
                    .get(reference.label.as_str())
                    .ok_or_else(|| vec![format!("Undefined label: {}", reference.label)])?;
                branches.push((file, index, target_file, target));
            }
        }
    }
    let mut far: Vec<Vec<u8>> = objects
        .iter()
        .map(|o| vec![0; o.instructions.len()])
        .collect();
    // Every nonterminal pass promotes at least one branch permanently. Bank
    // shifts can create further crossings; never shrinking avoids oscillation.
    let (maps, length) = loop {
        let (maps, length) = positions(objects, order, &far, bank)?;
        let mut changed = false;
        for &(file, index, target_file, target) in &branches {
            let instruction = objects[file].instructions[index];
            if far[file][index] == 0
                && !never(instruction)
                && maps[file][index] / bank != maps[target_file][target] / bank
            {
                far[file][index] = if always(instruction) { 1 } else { 2 };
                changed = true;
            }
        }
        if !changed {
            break (maps, length);
        }
    };
    validate_numeric_branches(objects, &maps)?;
    let mut instructions = Vec::with_capacity(length);
    for &(file, ref segment) in order {
        for index in segment.start..segment.end {
            instructions.resize(maps[file][index], Linker::nop_pad_instruction());
            let instruction = objects[file].instructions[index];
            match far[file][index] {
                1 => instructions.push(Instruction::new(Opcode::Jal, 0, 0, 0)),
                2 => {
                    instructions.push(Instruction::new(
                        inverse(instruction.opcode),
                        instruction.word1,
                        instruction.word2,
                        2,
                    ));
                    instructions.push(Instruction::new(Opcode::Jal, 0, 0, 0));
                }
                _ => instructions.push(instruction),
            }
        }
    }
    Ok(Placement {
        instructions,
        maps,
        far,
    })
}

fn positions(
    objects: &[ObjectFile],
    order: &[(usize, CodeSegment)],
    far: &[Vec<u8>],
    bank: usize,
) -> Result<(Vec<Vec<usize>>, usize), Vec<String>> {
    let mut maps: Vec<Vec<usize>> = objects
        .iter()
        .map(|o| vec![0; o.instructions.len() + 1])
        .collect();
    let mut position = 0usize;
    for &(file, ref segment) in order {
        let width = segment.end - segment.start
            + far[file][segment.start..segment.end]
                .iter()
                .filter(|&&f| f == 2)
                .count();
        let remaining = bank - position % bank;
        if position != 0 && width > remaining && remaining != bank {
            position += remaining;
        }
        let protect_calls = width > bank || far[file][segment.start..segment.end].contains(&2);
        for index in segment.start..segment.end {
            let instruction = objects[file].instructions[index];
            if far[file][index] == 2 {
                if bank < 3 {
                    return Err(vec![
                        "cross-bank conditional branches need a bank size of at least 3".into(),
                    ]);
                }
                // The inverse branch skips to PC+2 without changing PCB.
                // Its false continuation must remain inside the same bank.
                if position % bank > bank - 3 {
                    position += bank - position % bank;
                }
            }
            if protect_calls
                && instruction.word1 != 0
                && matches!(
                    Opcode::from_u8(instruction.opcode),
                    Some(Opcode::Jal | Opcode::Jalr)
                )
                && position % bank == bank - 1
            {
                if bank < 2 {
                    return Err(vec![
                        "bank-spanning calls need a bank size of at least 2".into()
                    ]);
                }
                // A raw return PC equal to bank_size aliases the first
                // instruction of the next bank and can execute it twice.
                // Keep compiler continuations canonical without an ABI change.
                position += 1;
            }
            maps[file][index] = position;
            position += if far[file][index] == 2 { 2 } else { 1 };
        }
        // Adjacent source functions can be emitted in another order. Their
        // shared boundary belongs to the next function, not this one's end.
        if segment.end == objects[file].instructions.len() {
            maps[file][segment.end] = position;
        }
        if position > bank * 65536 || position > u32::MAX as usize / 4 {
            return Err(vec![
                "linked code exceeds the bank/address space representable by the executable format"
                    .into(),
            ]);
        }
    }
    Ok((maps, position))
}

fn validate_numeric_branches(
    objects: &[ObjectFile],
    maps: &[Vec<usize>],
) -> Result<(), Vec<String>> {
    for (file, object) in objects.iter().enumerate() {
        let nonlinear = maps[file]
            .iter()
            .enumerate()
            .any(|(i, &p)| p != maps[file][0] + i);
        if !nonlinear {
            continue;
        }
        for (index, instruction) in object.instructions.iter().enumerate() {
            if !branch(instruction.opcode)
                || object.unresolved_references.contains_key(&index)
                || never(*instruction)
            {
                continue;
            }
            let delta = i64::from(instruction.word3 as i16);
            let target = index as i64 + delta;
            if target < 0
                || target > object.instructions.len() as i64
                || maps[file][target as usize] as i64 - maps[file][index] as i64 != delta
            {
                return Err(vec![format!("File {file}, instruction {index}: numeric branch crosses relocated code; use a symbolic label and reassemble the object")]);
            }
        }
    }
    Ok(())
}
