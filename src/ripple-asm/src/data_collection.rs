//! Remove unreachable compiler data objects while preserving legacy layouts.
use crate::types::ObjectFile;
use std::collections::{HashMap, HashSet};

pub(crate) fn collect(
    objects: &mut [ObjectFile],
    live: &HashSet<String>,
    movable: &[bool],
) -> Result<(), Vec<String>> {
    for (object, movable) in objects.iter_mut().zip(movable) {
        let Some(data) = object.rcc.as_ref().and_then(|meta| meta.data.as_ref()) else {
            continue;
        };
        if !movable {
            continue;
        }
        // Numeric GP-relative addresses explicitly retain the old TU layout.
        // This includes references from opaque assembly and older objects.
        if object
            .data_labels
            .keys()
            .any(|name| name.starts_with("__gp_") && live.contains(name))
        {
            continue;
        }
        // rasm's legacy memory offset is part of label coordinates, not the
        // stored byte vector. Preserve it while moving per-global reservations.
        let bias = object
            .data_labels
            .iter()
            .find(|(name, _)| name.starts_with("__gp_"))
            .map_or(0, |(_, &offset)| offset as usize);
        let mut spans = Vec::new();
        for (name, &size) in data {
            let start =
                *object.data_labels.get(name).ok_or_else(|| {
                    vec![format!("RCC data metadata names missing label '{name}'")]
                })? as usize;
            let end = start
                .checked_add(size as usize)
                .ok_or_else(|| vec![format!("RCC data size overflow for '{name}'")])?;
            spans.push((start, end, name.clone()));
        }
        spans.sort();
        let mut end = bias;
        for (start, next, name) in &spans {
            if *start != end || *next > object.data.len() + bias {
                return Err(vec![format!(
                    "RCC data metadata has a gap, overlap or invalid range at '{name}'"
                )]);
            }
            end = *next;
        }
        if end != object.data.len() + bias {
            return Err(vec![
                "RCC data metadata does not cover its reservation".into()
            ]);
        }
        if object
            .data_labels
            .keys()
            .any(|name| !data.contains_key(name) && !name.starts_with("__gp_"))
        {
            return Err(vec!["RCC relocatable data has an undescribed alias".into()]);
        }
        let mut compact = Vec::new();
        let mut labels = HashMap::new();
        for (start, end, name) in spans {
            if live.contains(&name) {
                labels.insert(name, (compact.len() + bias) as u32);
                compact.extend_from_slice(&object.data[start - bias..end - bias]);
            }
        }
        for name in object
            .data_labels
            .keys()
            .filter(|name| name.starts_with("__gp_"))
        {
            labels.insert(name.clone(), bias as u32);
        }
        object.data = compact;
        object.data_labels = labels;
        if let Some(data) = object.rcc.as_mut().and_then(|meta| meta.data.as_mut()) {
            data.retain(|name, _| live.contains(name));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::CompilerMetadata;
    use crate::{AssemblerOptions, Linker, RippleAssembler};
    fn object(pin_base: bool) -> ObjectFile {
        let mut options = AssemblerOptions::default();
        options.memory_offset = 0;
        let mut object = RippleAssembler::new(options).assemble(
            ".data\n__gp_fixture:\nunused:\n.space 24\nlive:\n.space 2\n.code\nmain:\nLI T0, live\nRET\n").unwrap();
        object.rcc = Some(CompilerMetadata {
            banked_data: false,
            symbols: HashMap::from([(
                "main".into(),
                if pin_base {
                    vec!["__gp_fixture".into()]
                } else {
                    vec![]
                },
            )]),
            code: vec!["main".into()],
            roots: vec![],
            data: Some(HashMap::from([("unused".into(), 24), ("live".into(), 2)])),
        });
        object
    }
    #[test]
    fn removes_dead_storage_and_relocates_live_data() {
        let entry = RippleAssembler::new(AssemblerOptions::default())
            .assemble("start:\nCALL main\nHALT\n")
            .unwrap();
        let linked = Linker::new(16).link(vec![entry, object(false)]).unwrap();
        assert_eq!(linked.data.len(), 2);
        assert_eq!(linked.data_labels["live"], 0);
        assert!(!linked.data_labels.contains_key("unused"));
        let main = linked.labels["main"].absolute_address as usize / 4;
        assert_eq!(linked.instructions[main].word2, 0);
    }
    #[test]
    fn preserves_assembler_coordinate_bias_after_compaction_and_other_objects() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let entry = assembler.assemble("start:\nCALL main\nHALT\n").unwrap();
        let mut prefix_options = AssemblerOptions::default();
        prefix_options.memory_offset = 0;
        let prefix = RippleAssembler::new(prefix_options)
            .assemble(".data\nprefix:\n.space 3\n")
            .unwrap();
        let mut body = object(false);
        for offset in body.data_labels.values_mut() {
            *offset += 2;
        }
        let linked = Linker::new(16).link(vec![entry, prefix, body]).unwrap();
        // The last two GP cells must be covered by the loader reservation.
        assert_eq!(linked.data.len(), 7);
        assert_eq!(linked.data_labels["live"], 5);
        assert_eq!(linked.data_labels["__gp_fixture"], 5);
        let main = linked.labels["main"].absolute_address as usize / 4;
        assert_eq!(linked.instructions[main].word2, 5);
    }

    #[test]
    fn numeric_base_and_legacy_metadata_preserve_storage() {
        for legacy in [false, true] {
            let entry = RippleAssembler::new(AssemblerOptions::default())
                .assemble("start:\nCALL main\nHALT\n")
                .unwrap();
            let mut object = object(true);
            if legacy {
                object.rcc.as_mut().unwrap().data = None;
            }
            let linked = Linker::new(64).link(vec![entry, object]).unwrap();
            assert_eq!(linked.data.len(), 26);
            assert_eq!(linked.data_labels["live"], 24);
        }
    }
    #[test]
    fn malformed_data_metadata_is_rejected() {
        let entry = RippleAssembler::new(AssemblerOptions::default())
            .assemble("start:\nCALL main\nHALT\n")
            .unwrap();
        let mut object = object(false);
        object
            .rcc
            .as_mut()
            .unwrap()
            .data
            .as_mut()
            .unwrap()
            .insert("unused".into(), 25);
        assert!(Linker::new(64)
            .link(vec![entry, object])
            .unwrap_err()
            .iter()
            .any(|e| e.contains("overlap")));
    }
}
