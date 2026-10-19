//! Lexical completion catalogs are independent of generic instantiation order.
use super::*;
impl Checker<'_> {
    pub(in crate::check) fn note_type_members(&mut self, module: &Module) {
        if !self.editor {
            return;
        }
        for tr in &module.traits {
            self.note_members("Self", &tr.name, tr.span, false);
        }
        for implementation in &module.impls {
            let Some((contract, _)) = &implementation.trait_name else {
                continue;
            };
            self.note_members("Self", contract, implementation.span, false);
            if let ParsedTy::Named { name, args, .. } = &implementation.ty {
                if args.is_empty() && implementation.type_params.is_empty() {
                    for scope in self.trait_scopes.clone() {
                        if scope.visible.contains(contract) {
                            self.note_members(name, contract, scope.span, true);
                        }
                    }
                }
            }
        }
        for func in &module.functions {
            let scope = Span::new(func.name_span.start, func.body.span.end);
            for param in &func.type_params {
                for (contract, _) in &param.bounds {
                    self.note_members(&param.name, contract, scope, false);
                }
            }
        }
    }

    fn note_members(&mut self, qualifier: &str, contract: &str, scope: Span, concrete: bool) {
        let Some(name) = self.trait_name(contract) else {
            return;
        };
        let tr = self.traits.iter().find(|tr| tr.name == name).unwrap();
        if tr.arity > 0 {
            for method in &tr.methods {
                self.type_members.push(TypeMemberFact {
                    operation: true,
                    qualifier: qualifier.into(),
                    contract: name.clone(),
                    name: method.name.clone(),
                    detail: trait_method_signature(method),
                    scope,
                    concrete,
                });
            }
        }
        for member in &tr.associated_types {
            let bounds: Vec<_> = member
                .bounds
                .iter()
                .map(|(name, _)| name.rsplit('$').next().unwrap_or(name))
                .collect();
            let suffix = if bounds.is_empty() {
                String::new()
            } else {
                format!(": {}", bounds.join(" + "))
            };
            self.type_members.push(TypeMemberFact {
                operation: false,
                qualifier: qualifier.into(),
                contract: name.clone(),
                name: member.name.clone(),
                detail: format!("type {}{suffix}", member.name),
                scope,
                concrete,
            });
        }
    }
}
