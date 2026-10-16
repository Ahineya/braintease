//! Editor-only applicability queries share the checker's trait resolution rules.
use super::*;

pub(crate) fn trait_method_signature(method: &crate::ast::ImplItem) -> String {
    let params: Vec<_> = method
        .params
        .iter()
        .map(|p| {
            p.ty.as_ref()
                .map(parsed_brief)
                .unwrap_or_else(|| "_".into())
        })
        .collect();
    let ret = method
        .ret
        .as_ref()
        .map(parsed_brief)
        .unwrap_or_else(|| "unit".into());
    format!("fn({}) -> {ret}", params.join(", "))
}

impl Checker<'_> {
    pub(super) fn note_trait_completions(&mut self, span: Span, ty: Ty) {
        let scope = self
            .trait_scopes
            .iter()
            .position(|s| s.span.contains(span.start))
            .unwrap_or(usize::MAX);
        let key = (ty, scope);
        if !self.method_cache.contains_key(&key) {
            let names: HashSet<_> = self
                .traits
                .iter()
                .flat_map(|tr| tr.methods.iter().map(|m| m.name.clone()))
                .collect();
            let mut methods = Vec::new();
            for name in names {
                let Ok(candidates) = self.trait_candidates(span.start, ty, &name) else {
                    continue;
                };
                if candidates.len() != 1 {
                    continue;
                }
                let tr = self
                    .traits
                    .iter()
                    .find(|tr| tr.name == candidates[0])
                    .unwrap();
                let method = tr.methods.iter().find(|m| m.name == name).unwrap();
                if let Ty::Ref(id) = ty {
                    if method.receiver == Some(Recv::Value)
                        || (method.receiver == Some(Recv::Mut) && !self.refs[id as usize].mutable)
                    {
                        continue;
                    }
                }
                let contract = tr.name.clone();
                let Ok(signature) =
                    self.projected_method_signature(&contract, &name, self.referent(ty))
                else {
                    continue;
                };
                let params: Vec<_> = signature.params.iter().map(|ty| self.show(*ty)).collect();
                methods.push((
                    name,
                    format!("fn({}) -> {}", params.join(", "), self.show(signature.ret)),
                    contract,
                ));
            }
            methods.sort();
            self.method_cache.insert(key, methods);
        }
        let receiver_type = self.show(ty);
        for (name, signature, trait_name) in &self.method_cache[&key] {
            self.trait_methods.push(MethodFact {
                receiver: span,
                receiver_type: receiver_type.clone(),
                trait_name: trait_name.clone(),
                name: name.clone(),
                signature: signature.clone(),
            });
        }
    }

    /// Use the resolved function/binding identity, never a method's spelling.
    pub(super) fn call_def(&self, name: &str) -> Option<(Span, String)> {
        if let Some(id) = self.lookup(name) {
            let binding = &self.bindings[id as usize];
            return Some((binding.def, self.show(binding.ty)));
        }
        let func = self
            .ast_fns
            .iter()
            .find(|func| func.name == name)
            .or_else(|| {
                matches!(name, "print" | "println")
                    .then(|| {
                        self.ast_fns
                            .iter()
                            .find(|func| func.name == format!("$std${name}"))
                    })
                    .flatten()
            });
        if let Some(func) = func {
            return Some((func.name_span, parsed_signature(func, false)));
        }
        // Constructors and extern functions have no RFL function body.
        self.facts
            .iter()
            .find(|fact| {
                fact.name == name && (fact.variant || fact.ty.starts_with("extern \"C\" fn("))
            })
            .map(|fact| (fact.def, fact.ty.clone()))
    }

    pub(super) fn note_inherent_target(&mut self, span: usize, name: &str, method: &str) {
        if !self.editor {
            return;
        }
        if let Some(func) = self.ast_fns.iter().find(|f| f.name == name) {
            self.method_targets.insert(
                (span, self.current_span.end, method.into()),
                (func.name_span, parsed_signature(func, true)),
            );
        }
    }

    // Associated-call ASTs retain the full expression span. Tokenize just the
    // call so whitespace/comments and module rewriting cannot skew names.
    pub(super) fn note_assoc_use(&mut self, span: Span, ty_name: &str, method_name: &str) {
        if !self.editor {
            return;
        }
        let Some(text) = self.source.get(span.start..span.end) else {
            return;
        };
        let Ok(tokens) = crate::lexer::tokenize(text) else {
            return;
        };
        let mut names = tokens
            .iter()
            .filter(|t| matches!(t.kind, crate::lexer::TokenKind::Ident(_)));
        let Some(owner) = names.next() else { return };
        let Some(method) = names.next() else { return };
        let owner_span = Span::new(span.start + owner.span.start, span.start + owner.span.end);
        if let Some(tr) = self
            .trait_name(ty_name)
            .and_then(|name| self.traits.iter().find(|t| t.name == name))
        {
            self.uses.push(NameUse {
                span: owner_span,
                def: tr.name_span,
                def_file: None,
                ty: format!("trait {}", ty_name.rsplit('$').next().unwrap_or(ty_name)),
            });
        } else {
            self.note_type(owner_span, ty_name);
        }
        if let Some((def, ty)) = self
            .method_targets
            .get(&(span.start, span.end, method_name.into()))
            .cloned()
        {
            self.uses.push(NameUse {
                span: Span::new(span.start + method.span.start, span.start + method.span.end),
                def,
                def_file: None,
                ty,
            });
        }
    }

    pub(super) fn locate_prelude_uses(&mut self, base: Option<usize>) {
        if !self.editor {
            return;
        }
        let Some(base) = base else { return };
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("std/io.rfl");
        for name in &mut self.uses {
            if name.def.start >= base {
                name.def = Span::new(name.def.start - base, name.def.end - base);
                name.def_file = Some(path.clone());
            }
        }
    }
}
