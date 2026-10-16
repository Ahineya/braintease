//! Associated type names belong to the trait's type namespace.
use super::*;
impl Workspace {
    pub(super) fn index_associated_declarations(&mut self, path: &Path, tr: &Trait) {
        let owner = Self::key(path, tr.name_span);
        for associated in &tr.associated_types {
            let key = self.declare(path, &associated.name, associated.name_span, true);
            if tr.exported {
                self.exports.insert(key.clone());
            }
            if self
                .associated_types
                .insert(
                    (owner.clone(), associated.name.clone()),
                    Binding { key, ty: None },
                )
                .is_some()
            {
                self.duplicates += 1;
            }
        }
    }

    pub(super) fn projected_binding(
        &mut self,
        path: &Path,
        base: &ParsedTy,
        explicit: Option<&str>,
        member: &str,
        span: Span,
        env: &Env,
    ) -> Option<Binding> {
        let contract = explicit
            .and_then(|name| env.get(&type_key(name)))
            .or_else(|| {
                matches!(base, ParsedTy::Named { name, .. } if name == "Self")
                    .then(|| env.get("#SelfTrait"))
                    .flatten()
            })
            .and_then(|b| match &b.ty {
                Some(Ty::Named(owner)) => Some(owner.clone()),
                _ => None,
            });
        let mut candidates = HashMap::new();
        if let Some(owner) = contract {
            if let Some(binding) = self.associated_types.get(&(owner, member.into())) {
                candidates.insert(binding.key.clone(), binding.clone());
            }
        } else if let Some(ty) = type_of(base, env) {
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
                    owners.extend(bounds.iter().cloned());
                }
            }
            for implementation in &self.trait_impls {
                if implementation.ty == ty
                    && visible.contains(&implementation.contract)
                    && self.reachable[path].contains(&implementation.path)
                    && self
                        .associated_types
                        .contains_key(&(implementation.contract.clone(), member.into()))
                {
                    if !implementation.exact {
                        self.uncertain.push((path.to_path_buf(), span));
                        return None;
                    }
                    owners.insert(implementation.contract.clone());
                }
            }
            for owner in owners {
                if let Some(binding) = self.associated_types.get(&(owner, member.into())) {
                    candidates.insert(binding.key.clone(), binding.clone());
                }
            }
        }
        if candidates.len() == 1 {
            return candidates.into_values().next();
        }
        self.uncertain.push((path.to_path_buf(), span));
        None
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::{apply, workspace};
    use super::*;

    #[test]
    fn rename_associated_type_updates_contract_definitions_and_projections() {
        let source = "trait A { type Out; fn get(&self) -> Self::Out; } impl A for i16 { type Out=i16; fn get(&self) -> Self::Out { *self } } fn get<T:A>(x:T) -> T::Out { x.get() } fn main() { let n:i16::Out=get(42); }";
        let w = workspace(&[("main.rfl", source)]);
        let path = Path::new("/rename-test/main.rfl");
        for at in [
            source.find("Out").unwrap(),
            source.find("T::Out").unwrap() + 3,
            source.find("type Out=i16").unwrap() + 5,
        ] {
            let result = apply(&w, "main.rfl", at, "Output");
            assert_eq!(result[path], source.replace("Out", "Output"));
            crate::check_source(&result[path]).unwrap();
        }
        let result = apply(&w, "main.rfl", source.find("A {").unwrap(), "Source");
        assert!(result[path].contains("Self::Out"));
        assert!(result[path].contains("trait Source"));
    }

    #[test]
    fn associated_names_are_distinct_between_traits_and_from_global_types() {
        let source = "trait A { type Out; } trait B { type Out; } struct Out { n:i16 } impl A for i16 { type Out=i16; } impl B for i16 { type Out=Out; } fn main() { let a:<i16 as A>::Out=42; let b:<i16 as B>::Out=Out{n:42}; }";
        let w = workspace(&[("main.rfl", source)]);
        let result = apply(&w, "main.rfl", source.find("Out").unwrap(), "Output");
        let text = &result[Path::new("/rename-test/main.rfl")];
        assert!(text.contains("trait A { type Output; }"));
        assert!(text.contains("trait B { type Out; }"));
        assert!(text.contains("struct Out"));
        assert!(text.contains("as A>::Output"));
        assert!(text.contains("as B>::Out"));
        crate::check_source(text).unwrap();
    }

    #[test]
    fn aliases_and_generic_bounds_keep_associated_type_identity_across_files() {
        let library = "export trait A { type Out; fn get(&self) -> Self::Out; } export struct S { n:i16 } impl A for S { type Out=String; fn get(&self) -> Self::Out { \"hi\" } }";
        let main = "import { A as Source, S as Item } from \"./lib\"; fn get<T:Source>(x:T) -> <T as Source>::Out { x.get() } fn main() { let value:Item::Out=get(Item{n:1}); }";
        let w = workspace(&[("lib.rfl", library), ("main.rfl", main)]);
        let updated = apply(&w, "main.rfl", main.find("::Out").unwrap() + 2, "Output");
        assert_eq!(
            updated[Path::new("/rename-test/lib.rfl")],
            library.replace("Out", "Output")
        );
        assert_eq!(
            updated[Path::new("/rename-test/main.rfl")],
            main.replace("Out", "Output")
        );
    }

    #[test]
    fn ambiguous_projections_and_member_collisions_refuse_partial_edits() {
        let source =
            "trait A { type X; type Y; } impl A for i16 { type X=i16; type Y=bool; } fn main() {}";
        let w = workspace(&[("main.rfl", source)]);
        assert!(w
            .rename(
                Path::new("/rename-test/main.rfl"),
                source.find("X;").unwrap(),
                "Y"
            )
            .is_err());
        let source = "trait A { type Out; } trait B { type Out; } impl A for i16 { type Out=i16; } impl B for i16 { type Out=bool; } fn main() { let x:i16::Out=42; }";
        let w = workspace(&[("main.rfl", source)]);
        assert!(w
            .rename(
                Path::new("/rename-test/main.rfl"),
                source.find("Out").unwrap(),
                "Output"
            )
            .is_err());
    }
}
