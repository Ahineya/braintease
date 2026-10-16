//! Contract methods share one rename identity across every implementation.
use super::*;

pub(super) struct Implementation {
    pub(super) ty: Ty,
    pub(super) contract: Key,
    pub(super) path: PathBuf,
    /// The syntax index erases type arguments; those impls need full inference.
    pub(super) exact: bool,
}

impl Workspace {
    pub(super) fn index_trait_methods(&mut self, modules: &[(PathBuf, Module)]) {
        for (path, ast) in modules {
            let env = self.globals[path].clone();
            for tr in &ast.traits {
                self.index_associated_declarations(path, tr);
                let owner = Self::key(path, tr.name_span);
                for method in &tr.methods {
                    let key = self.declare(path, &method.name, method.name_span, true);
                    let ty = method.ret.as_ref().and_then(|ty| type_of(ty, &env));
                    if let Some(ty) = &ty {
                        self.returns.insert(key.clone(), ty.clone());
                    }
                    if tr.exported {
                        self.exports.insert(key.clone());
                    }
                    if self
                        .trait_methods
                        .insert((owner.clone(), method.name.clone()), Binding { key, ty })
                        .is_some()
                    {
                        self.duplicates += 1;
                    }
                }
            }
        }
    }

    pub(super) fn index_trait_impl(&mut self, path: &Path, item: &Impl, env: &Env) {
        let Some((name, _)) = &item.trait_name else {
            return;
        };
        let Some(Ty::Named(owner)) = env.get(&type_key(name)).and_then(|b| b.ty.clone()) else {
            return;
        };
        if let Some(ty) = type_of(&item.ty, env) {
            let exact = item.type_params.is_empty()
                && matches!(&item.ty, ParsedTy::Named { args, .. } if args.is_empty());
            self.trait_impls.push(Implementation {
                ty,
                contract: owner.clone(),
                path: path.to_path_buf(),
                exact,
            });
        }
        for associated in &item.associated_types {
            let key = self
                .associated_types
                .get(&(owner.clone(), associated.name.clone()))
                .map(|b| b.key.clone());
            self.occurrence(path, associated.name_span, key, Spelling::Name);
        }
        for method in &item.items {
            let key = self
                .trait_methods
                .get(&(owner.clone(), method.name.clone()))
                .map(|b| b.key.clone());
            self.occurrence(path, method.name_span, key, Spelling::Name);
        }
    }

    pub(super) fn resolve_method(
        &mut self,
        path: &Path,
        ty: Ty,
        name: &str,
        span: Span,
        env: &Env,
        extension: bool,
    ) -> Option<Binding> {
        let inherent: Vec<_> = self
            .methods
            .get(&(ty.clone(), name.into()))
            .into_iter()
            .flatten()
            .filter(|b| self.reachable[path].contains(&b.key.path))
            .cloned()
            .collect();
        if !inherent.is_empty() {
            if inherent.len() == 1 {
                return inherent.into_iter().next();
            }
            self.uncertain.push((path.to_path_buf(), span));
            return None;
        }
        let visible: HashSet<_> = env
            .iter()
            .filter(|(name, _)| name.starts_with('@'))
            .filter_map(|(_, b)| match &b.ty {
                Some(Ty::Named(k)) if self.traits.contains(k) => Some(k.clone()),
                _ => None,
            })
            .collect();
        let mut owners = HashSet::new();
        if let Ty::Named(key) = &ty {
            if self.traits.contains(key) {
                owners.insert(key.clone());
            }
            if let Some(bounds) = self.bounds.get(key) {
                owners.extend(bounds.iter().filter(|k| visible.contains(*k)).cloned());
            }
        }
        for implementation in &self.trait_impls {
            let owner = &implementation.contract;
            if implementation.ty == ty
                && visible.contains(owner)
                && self.reachable[path].contains(&implementation.path)
                && self
                    .trait_methods
                    .contains_key(&(owner.clone(), name.into()))
            {
                if !implementation.exact {
                    // Never mistake an inapplicable conditional trait method
                    // for a receiver-first free function with the same name.
                    self.uncertain.push((path.to_path_buf(), span));
                    return None;
                }
                owners.insert(owner.clone());
            }
        }
        let mut candidates: Vec<_> = owners
            .into_iter()
            .filter_map(|owner| self.trait_methods.get(&(owner, name.into())).cloned())
            .collect();
        if candidates.len() == 1 {
            return candidates.pop();
        }
        if candidates.len() > 1 {
            self.uncertain.push((path.to_path_buf(), span));
            return None;
        }
        extension.then(|| env.get(name).cloned()).flatten()
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::workspace;
    use super::*;
    fn single(source: &str) -> (Workspace, PathBuf) {
        (
            workspace(&[("main.rfl", source)]),
            PathBuf::from("/rename-test/main.rfl"),
        )
    }
    fn apply(w: &Workspace, edits: &[Edit]) -> BTreeMap<PathBuf, String> {
        let mut sources: BTreeMap<_, _> = w
            .files
            .iter()
            .filter(|(_, f)| f.editable)
            .map(|(p, f)| (p.clone(), f.source.clone()))
            .collect();
        for edit in edits.iter().rev() {
            sources
                .get_mut(&edit.path)
                .unwrap()
                .replace_range(edit.span.start..edit.span.end, &edit.text);
        }
        sources
    }

    #[test]
    fn rename_contract_updates_implementations_bound_calls_and_qualified_calls() {
        let source = "trait Read { fn read(&self) -> i16; } impl Read for i16 { fn read(&self) -> i16 { 1 } } impl Read for bool { fn read(&self) -> i16 { 2 } } fn generic<T: Read>(v: T) -> i16 { v.read() } fn main() { let x = 1; println(x.read()); println(Read::read(&x)); }";
        let (w, path) = single(source);
        for at in [
            source.find("read").unwrap(),
            source.find("fn read(&self) -> i16 { 1").unwrap() + 3,
            source.find("v.read").unwrap() + 2,
            source.find("x.read").unwrap() + 2,
        ] {
            let edits = w.rename(&path, at, "fetch").unwrap();
            assert_eq!(edits.len(), 6);
            let updated = apply(&w, &edits);
            assert_eq!(updated[&path], source.replace("read", "fetch"));
            crate::check_source(&updated[&path]).unwrap();
        }
    }

    #[test]
    fn trait_names_and_bounds_rename_through_import_aliases() {
        let library = "export trait Read { fn read(&self) -> i16; } export struct Value { n: i16 } impl Read for Value { fn read(&self) -> i16 { self.n } }";
        let main = "import { Read as Reader, Value } from \"./lib\"; fn generic<T: Reader>(v: T) -> i16 { v.read() } fn main() { let v = Value { n: 2 }; println(v.read()); println(Reader::read(&v)); }";
        let w = workspace(&[("lib.rfl", library), ("main.rfl", main)]);
        let lib = PathBuf::from("/rename-test/lib.rfl");
        let main_path = PathBuf::from("/rename-test/main.rfl");
        let updated = apply(
            &w,
            &w.rename(&lib, library.find("Read").unwrap(), "Readable")
                .unwrap(),
        );
        assert_eq!(updated[&lib], library.replace("Read", "Readable"));
        assert_eq!(updated[&main_path], main.replace("Read as", "Readable as"));
        let updated = apply(
            &w,
            &w.rename(&main_path, main.find("v.read").unwrap() + 2, "fetch")
                .unwrap(),
        );
        assert_eq!(updated[&lib], library.replace("read", "fetch"));
        assert_eq!(updated[&main_path], main.replace("read", "fetch"));
    }

    #[test]
    fn same_spelling_keeps_inherent_and_other_contracts_separate() {
        let source = "trait A { fn read(&self) -> i16; } trait B { fn read(&self) -> i16; } impl A for i16 { fn read(&self) -> i16 { 1 } } impl B for bool { fn read(&self) -> i16 { 2 } } struct V { n: i16 } impl V { fn read(&self) -> i16 { self.n } } fn main() { let a = 1; let b = true; let v = V { n: 3 }; println(a.read()); println(b.read()); println(v.read()); }";
        let (w, path) = single(source);
        let edits = w
            .rename(&path, source.find("a.read").unwrap() + 2, "fetch")
            .unwrap();
        assert_eq!(edits.len(), 3);
        let updated = apply(&w, &edits);
        assert!(updated[&path].contains("b.read()"));
        assert!(updated[&path].contains("v.read()"));
        crate::check_source(&updated[&path]).unwrap();
        assert!(w
            .rename(&path, source.find("a.read").unwrap() + 2, "print")
            .is_err());
    }

    #[test]
    fn implicit_printable_methods_are_library_symbols() {
        let source =
            "fn bounded<T: Printable>(v: T) { v.print(); } fn main() { let x = 1; x.print(); }";
        let (w, path) = single(source);
        assert!(w
            .prepare(&path, source.find("v.print").unwrap() + 2)
            .is_none());
        assert!(w
            .prepare(&path, source.find("x.print").unwrap() + 2)
            .is_none());
        assert!(w
            .rename(&path, source.find("v.print").unwrap() + 2, "display")
            .is_err());
        let library = canonical(&PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("std/io.rfl"));
        assert!(w
            .occurrences
            .iter()
            .any(|o| o.path == path && o.key.as_ref().is_some_and(|k| k.path == library)));
    }

    #[test]
    fn erased_type_arguments_cannot_misidentify_a_free_extension_as_a_trait_method() {
        let source = "trait Read { fn read(&self) -> i16; } impl Read for Vec<i16> { fn read(&self) -> i16 { 1 } } fn read(v: Vec<bool>) -> i16 { 2 } fn main() { let v: Vec<bool> = []; println(v.read()); }";
        crate::check_source(source).unwrap();
        let (w, path) = single(source);
        assert!(w
            .rename(&path, source.find("read").unwrap(), "fetch")
            .is_err());
        assert!(w
            .prepare(&path, source.find("v.read").unwrap() + 2)
            .is_none());
    }

    #[test]
    fn ambiguous_calls_and_contract_collisions_refuse_partial_rename() {
        let source = "trait A { fn read(&self) -> i16; fn fetch(&self) -> i16; } impl A for i16 { fn read(&self) -> i16 { 1 } fn fetch(&self) -> i16 { 2 } } fn main() {}";
        let (w, path) = single(source);
        assert!(w
            .rename(&path, source.find("read").unwrap(), "fetch")
            .is_err());
        let source = "trait A { fn read(&self) -> i16; } trait B { fn read(&self) -> i16; } impl A for i16 { fn read(&self) -> i16 { 1 } } impl B for i16 { fn read(&self) -> i16 { 2 } } fn main() { let a = 1; a.read(); }";
        let (w, path) = single(source);
        assert!(w
            .rename(&path, source.find("read").unwrap(), "fetch")
            .is_err());
    }
}
