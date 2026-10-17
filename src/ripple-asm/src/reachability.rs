//! Collect compiler-described functions and global initializers before packing.
//! Legacy objects remain intact, including their data and fallthrough behavior.
use crate::types::{ObjectFile, Opcode};
use std::collections::{HashMap, HashSet};

pub(crate) fn collect(objects: &mut [ObjectFile]) -> Result<(), Vec<String>> {
    // A library-only link has no closed-world entry point. Preserve its exports.
    if !objects.iter().any(|o| o.entry_point.is_some()) {
        return Ok(());
    }
    let mut definitions = HashSet::new();
    for o in objects.iter() {
        for name in o.labels.keys().chain(o.data_labels.keys()) {
            if !definitions.insert(name) {
                return Ok(());
            } // ordinary linker reports duplicates
        }
    }
    let mut graph = HashMap::<String, Vec<String>>::new();
    let mut roots = Vec::new();
    let mut ranges = Vec::new();
    for o in objects.iter() {
        if let Some(entry) = &o.entry_point {
            roots.push(entry.clone());
        }
        let Some(meta) = &o.rcc else {
            roots.extend(o.unresolved_references.values().map(|r| r.symbol().to_string()));
            ranges.push(None);
            continue;
        };
        roots.extend(meta.roots.iter().cloned());
        for (name, refs) in &meta.symbols {
            graph
                .entry(name.clone())
                .or_default()
                .extend(refs.iter().cloned());
        }
        let mut starts = Vec::new();
        for name in &meta.code {
            let label = o
                .labels
                .get(name)
                .ok_or_else(|| vec![format!("RCC metadata names missing code label '{name}'")])?;
            let start = label.absolute_address as usize / 4;
            if start >= o.instructions.len() {
                return Err(vec![format!("invalid RCC code boundary '{name}'")]);
            }
            starts.push((start, name.clone()));
        }
        starts.sort();
        if !o.instructions.is_empty() && starts.first().map(|s| s.0) != Some(0) {
            return Err(vec![
                "RCC metadata does not cover the beginning of its code".into(),
            ]);
        }
        if starts.windows(2).any(|w| w[0].0 == w[1].0) {
            return Err(vec!["overlapping RCC code boundaries".into()]);
        }
        let segments: Vec<_> = starts
            .iter()
            .enumerate()
            .map(|(i, (start, name))| {
                (
                    *start,
                    starts
                        .get(i + 1)
                        .map(|s| s.0)
                        .unwrap_or(o.instructions.len()),
                    name.clone(),
                )
            })
            .collect();
        // A legacy object may reference a block label inside a compiled function.
        // Keep its owning function, not just externally visible entry labels.
        for (name, label) in &o.labels {
            let target = label.absolute_address as usize / 4;
            if let Some((_, _, owner)) = segments
                .iter()
                .find(|(s, e, _)| *s <= target && target < *e)
            {
                if name != owner {
                    graph.entry(name.clone()).or_default().push(owner.clone());
                }
            }
        }
        let mut conservative = false;
        for (start, end, name) in &segments {
            let refs = graph.entry(name.clone()).or_default();
            for index in *start..*end {
                if let Some(r) = o.unresolved_references.get(&index) {
                    refs.push(r.symbol().to_string());
                }
                let inst = o.instructions[index];
                // Local symbolic calls are sometimes already resolved by rasm.
                if inst.opcode == Opcode::Jal as u8 && !o.unresolved_references.contains_key(&index)
                {
                    let target = inst.word3 as usize;
                    if let Some((_, _, callee)) = segments
                        .iter()
                        .find(|(s, e, _)| *s <= target && target < *e)
                    {
                        refs.push(callee.clone());
                    } else {
                        conservative = true;
                    }
                }
                if matches!(
                    Opcode::from_u8(inst.opcode),
                    Some(Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge)
                ) && !o.unresolved_references.contains_key(&index)
                {
                    let target = index as i64 + i64::from(inst.word3 as i16);
                    if target < *start as i64 || target >= *end as i64 {
                        conservative = true;
                    }
                }
            }
        }
        // Unexpected hand-written cross-function branches/fallthrough must not
        // authorize deleting instructions. Compiler functions end with RET/HALT
        // or an explicit branch (multiple returns can leave a trailing label).
        for (_, end, _) in &segments {
            if !matches!(
                Opcode::from_u8(o.instructions[end - 1].opcode),
                Some(
                    Opcode::Jalr
                        | Opcode::Halt
                        | Opcode::Iret
                        | Opcode::Beq
                        | Opcode::Bne
                        | Opcode::Blt
                        | Opcode::Bge
                )
            ) {
                conservative = true;
            }
        }
        if conservative {
            roots.extend(meta.symbols.keys().cloned());
            roots.extend(meta.code.iter().cloned());
            // Retain not just the instructions but their original addresses.
            // Opaque numeric jumps and fallthrough cannot be relocated using
            // compiler symbol metadata. Dependencies still participate above.
            ranges.push(None);
        } else {
            ranges.push(Some(segments));
        }
    }
    let mut live = HashSet::new();
    while let Some(name) = roots.pop() {
        if live.insert(name.clone()) {
            if let Some(refs) = graph.get(&name) {
                roots.extend(refs.iter().cloned());
            }
        }
    }
    let movable: Vec<_> = ranges.iter().map(Option::is_some).collect();
    for (o, segments) in objects.iter_mut().zip(ranges) {
        let Some(segments) = segments else {
            continue;
        };
        let mut map = vec![None; o.instructions.len()];
        let mut instructions = Vec::new();
        for (start, end, name) in segments {
            if live.contains(&name) {
                for i in start..end {
                    map[i] = Some(instructions.len());
                    instructions.push(o.instructions[i]);
                }
            }
        }
        for (old, inst) in o.instructions.iter().enumerate() {
            if let Some(new) = map[old] {
                if inst.opcode == Opcode::Jal as u8 && !o.unresolved_references.contains_key(&old) {
                    let target = inst.word3 as usize;
                    if let Some(Some(target)) = map.get(target) {
                        instructions[new].word3 = *target as u16;
                    } else {
                        return Err(vec!["live RCC call points to discarded code".into()]);
                    }
                }
            }
        }
        o.labels.retain(|_, label| {
            let old = label.absolute_address as usize / 4;
            if let Some(Some(new)) = map.get(old) {
                label.absolute_address = (*new * 4) as u32;
                true
            } else {
                false
            }
        });
        o.unresolved_references = std::mem::take(&mut o.unresolved_references)
            .into_iter()
            .filter_map(|(old, r)| map.get(old).copied().flatten().map(|new| (new, r)))
            .collect();
        o.instructions = instructions;
        if let Some(meta) = &mut o.rcc {
            meta.code.retain(|n| live.contains(n));
        }
    }
    crate::data_collection::collect(objects, &live, &movable)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::CompilerMetadata;
    use crate::{AssemblerOptions, Linker, RippleAssembler};
    fn object(source: &str, symbols: &[(&str, &[&str])], code: &[&str]) -> ObjectFile {
        let mut o = RippleAssembler::new(AssemblerOptions::default())
            .assemble(source)
            .unwrap();
        o.rcc = Some(CompilerMetadata {
            banked_data: false,
            symbols: symbols
                .iter()
                .map(|(k, v)| (k.to_string(), v.iter().map(|s| s.to_string()).collect()))
                .collect(),
            code: code.iter().map(|s| s.to_string()).collect(),
            roots: vec![],
            data: None,
        });
        o
    }
    #[test]
    fn preserves_callbacks_and_referenced_initializers_but_removes_dead_code() {
        let asm = RippleAssembler::new(AssemblerOptions::default());
        let entry = asm.assemble("start:\nCALL main\nHALT\n").unwrap();
        let body = object("main:\nCALL helper\nRET\ndead:\nCALL missing\nRET\nhelper:\nRET\ncallback:\nRET\n__rcc_init_live:\nRET\n__rcc_init_dead:\nRET\n",
            &[("main", &["live"]),("live", &["callback","__rcc_init_live"]),("dead_global", &["__rcc_init_dead"])],
            &["main","dead","helper","callback","__rcc_init_live","__rcc_init_dead"]);
        let linked = Linker::new(64).link(vec![entry, body]).unwrap();
        for name in ["main", "helper", "callback", "__rcc_init_live"] {
            assert!(linked.labels.contains_key(name), "{name}");
        }
        for name in ["dead", "__rcc_init_dead"] {
            assert!(!linked.labels.contains_key(name), "{name}");
        }
        let main = linked.labels["main"].absolute_address as usize / 4;
        assert_eq!(
            linked.instructions[main].word3,
            (linked.labels["helper"].absolute_address / 4) as u16
        );
    }
    #[test]
    fn library_only_and_legacy_objects_keep_all_code() {
        let mut objs = vec![object("one:\nRET\ntwo:\nRET\n", &[], &["one", "two"])];
        collect(&mut objs).unwrap();
        assert_eq!(objs[0].instructions.len(), 2);
        let mut legacy = RippleAssembler::new(AssemblerOptions::default())
            .assemble("start:\nHALT\nunused:\nRET\n")
            .unwrap();
        collect(std::slice::from_mut(&mut legacy)).unwrap();
        assert_eq!(legacy.instructions.len(), 2);
    }

    #[test]
    fn legacy_reference_to_a_block_keeps_its_function() {
        let asm = RippleAssembler::new(AssemblerOptions::default());
        let start = asm.assemble("start:\nCALL L_owner_inside\nHALT\n").unwrap();
        let body = object(
            "dead:\nRET\nowner:\nNOP\nL_owner_inside:\nRET\n",
            &[],
            &["dead", "owner"],
        );
        let linked = Linker::new(64).link(vec![start, body]).unwrap();
        assert!(linked.labels.contains_key("owner"));
        assert!(linked.labels.contains_key("L_owner_inside"));
        assert!(!linked.labels.contains_key("dead"));
    }

    #[test]
    fn opaque_numeric_jump_keeps_its_object_and_symbolic_dependencies() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let entry = assembler.assemble("start:\nCALL main\nHALT\n").unwrap();
        let opaque = object(
            "main:\nJAL RA, 3, 100\nRET\nunused:\nCALL dependency\nRET\n",
            &[],
            &["main", "unused"],
        );
        let dependency = object(
            "dead:\nRET\ndependency:\nRET\n",
            &[],
            &["dead", "dependency"],
        );
        let original = serde_json::to_value(&opaque).unwrap();
        let mut objects = vec![entry, opaque, dependency];
        collect(&mut objects).unwrap();
        assert_eq!(serde_json::to_value(&objects[1]).unwrap(), original);
        assert!(objects[2].labels.contains_key("dependency"));
        assert!(!objects[2].labels.contains_key("dead"));
    }
}
