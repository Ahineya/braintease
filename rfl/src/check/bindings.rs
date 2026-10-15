//! Bindings in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn bind(&mut self, name: String, ty: Ty, mutable: bool) -> u32 {
        self.bind_spanned(name, ty, mutable, Span::empty())
    }

    pub(super) fn bind_spanned(&mut self, name: String, ty: Ty, mutable: bool, def: Span) -> u32 {
        let id = self.bindings.len() as u32;
        self.bindings.push(Binding {
            ty,
            mutable,
            shared: false,
            def,
        });
        self.scopes.last_mut().unwrap().insert(name.clone(), id);
        if def.end > def.start && !name.starts_with("__") {
            let scope = self.scope_spans.last().copied().unwrap_or(def);
            let visible_from = self.next_visible.take().unwrap_or(def.end);
            let shown = self.show(ty);
            self.facts.push(BindingFact {
                name,
                def,
                ty: shown,
                scope,
                visible_from,
                variant: false,
            });
        }
        id
    }

    pub(super) fn lookup(&self, name: &str) -> Option<u32> {
        for scope in self.scopes.iter().rev() {
            if let Some(id) = scope.get(name) {
                return Some(*id);
            }
        }
        None
    }

    /// A name from an enclosing function becomes an immutable local.
    /// The value is copied when the closure is created.
    pub(super) fn resolve(&mut self, name: &str) -> Option<u32> {
        if let Some(id) = self.lookup(name) {
            return Some(id);
        }
        let parent = self
            .outer_scopes
            .iter()
            .rev()
            .find_map(|scope| scope.get(name).copied())?;
        if let Some(found) = self.captures.iter().find(|cap| cap.parent == parent) {
            self.scopes
                .last_mut()
                .unwrap()
                .insert(name.to_string(), found.local);
            return Some(found.local);
        }
        let parent_binding = self.outer_bindings[parent as usize].clone();
        let ty = parent_binding.ty;
        if parent_binding.shared && !shareable(ty) && self.capture_error.is_none() {
            self.capture_error = Some(CompileError::at(
                self.source,
                0,
                format!("`{}` cannot be captured mutably", self.show(ty)),
            ));
        }
        let local = self.bind(name.to_string(), ty, parent_binding.shared);
        if parent_binding.shared {
            self.bindings[local as usize].shared = true;
        }
        self.captures.push(crate::typed::Capture {
            parent,
            local,
            ty,
            shared: parent_binding.shared,
        });
        Some(local)
    }
}
