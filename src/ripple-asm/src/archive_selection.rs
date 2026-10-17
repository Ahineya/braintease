//! Archive members are candidates, not unconditional executable inputs.
use crate::{Archive, LinkedProgram, Linker, ObjectFile};
use std::collections::HashSet;

impl Linker {
    /// Resolve archive dependencies to a fixed point, independent of argument
    /// order. Explicit objects take precedence; archive members retain order.
    pub fn link_archives(
        &self,
        objects: Vec<ObjectFile>,
        archives: Vec<Archive>,
    ) -> Result<LinkedProgram, Vec<String>> {
        self.link(select(objects, archives))
    }
}
fn select(mut objects: Vec<ObjectFile>, archives: Vec<Archive>) -> Vec<ObjectFile> {
    let mut candidates: Vec<_> = archives
        .into_iter()
        .flat_map(|a| a.objects)
        .map(|entry| Some(entry.object))
        .collect();
    if !objects.iter().any(|object| object.entry_point.is_some()) {
        objects.extend(candidates.into_iter().flatten());
        return objects;
    }
    loop {
        let defined: HashSet<_> = objects
            .iter()
            .flat_map(|o| o.labels.keys().chain(o.data_labels.keys()))
            .cloned()
            .collect();
        let mut needed: HashSet<_> = objects
            .iter()
            .flat_map(|o| o.unresolved_references.values().map(|r| r.symbol().to_string()))
            .collect();
        for meta in objects.iter().filter_map(|o| o.rcc.as_ref()) {
            needed.extend(meta.symbols.values().flatten().cloned());
        }
        needed.retain(|name| !defined.contains(name));
        let next = candidates.iter().position(|candidate| {
            candidate.as_ref().is_some_and(|object| {
                object
                    .labels
                    .keys()
                    .chain(object.data_labels.keys())
                    .any(|name| needed.contains(name))
            })
        });
        match next {
            Some(index) => objects.push(candidates[index].take().unwrap()),
            None => return objects,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{AssemblerOptions, RippleAssembler};
    fn object(source: &str) -> ObjectFile {
        RippleAssembler::new(AssemblerOptions::default())
            .assemble(source)
            .unwrap()
    }
    #[test]
    fn unused_members_do_not_reserve_data_or_introduce_undefined_dependencies() {
        let entry = object("start:\nCALL used\nHALT\n");
        let archive = Linker::create_archive(vec![
            (
                "unused".into(),
                object(".data\nbuffer:\n.space 200\n.code\nunused:\nCALL missing\nRET\n"),
            ),
            ("used".into(), object("used:\nCALL helper\nRET\n")),
            ("helper".into(), object("helper:\nRET\n")),
        ]);
        let linked = Linker::new(64)
            .link_archives(vec![entry], vec![archive])
            .unwrap();
        assert!(linked.data.is_empty());
        assert!(linked.labels.contains_key("helper"));
        assert!(!linked.labels.contains_key("unused"));
    }
    #[test]
    fn resolves_dependencies_across_archives_regardless_of_argument_order() {
        let entry = object("start:\nCALL used\nHALT\n");
        let dependencies =
            Linker::create_archive(vec![("helper".into(), object("helper:\nRET\n"))]);
        let callers =
            Linker::create_archive(vec![("used".into(), object("used:\nCALL helper\nRET\n"))]);
        let linked = Linker::new(64)
            .link_archives(vec![entry], vec![dependencies, callers])
            .unwrap();
        assert!(linked.labels.contains_key("helper"));
    }
    #[test]
    fn links_without_an_entry_preserve_archive_exports() {
        let archive = Linker::create_archive(vec![("unused".into(), object("unused:\nRET\n"))]);
        let linked = Linker::new(64)
            .link_archives(vec![], vec![archive])
            .unwrap();
        assert!(linked.labels.contains_key("unused"));
    }

    #[test]
    fn explicit_definitions_override_unused_archive_duplicates() {
        let entry = object("start:\nCALL used\nHALT\nused:\nRET\n");
        let archive = Linker::create_archive(vec![("other".into(), object("used:\nRET\n"))]);
        assert!(Linker::new(64)
            .link_archives(vec![entry], vec![archive])
            .is_ok());
    }
}
