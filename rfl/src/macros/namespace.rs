//! Compile-time names and runtime helper dependencies have separate namespaces.
use super::walk::{self, Role, Visitor};
use crate::{ast::*, CompileError, Target};
use std::{
    collections::HashSet,
    path::{Path, PathBuf},
};
pub fn resolve(path: &Path, entry: &str) -> Result<(PathBuf, String), CompileError> {
    fn find(
        path: &Path,
        entry: &str,
        stack: &mut Vec<PathBuf>,
    ) -> Result<(PathBuf, String), CompileError> {
        let path =
            super::source::canonical(path).map_err(|e| CompileError::plain(e.to_string()))?;
        if stack.contains(&path) || stack.len() > 64 {
            return Err(CompileError::plain("macro import/reexport cycle"));
        }
        stack.push(path.clone());
        let source = super::source::read(&path).map_err(|e| CompileError::plain(e.to_string()))?;
        let ast = crate::parser::parse_for_target(
            &source,
            &crate::lexer::tokenize(&source)?,
            Target::Native,
        )?;
        if ast.proc_macros.iter().any(|n| n == entry)
            && ast.functions.iter().any(|f| f.name == entry && f.exported)
        {
            return Ok((path, entry.into()));
        }
        if ast.macro_exports.iter().any(|n| n == entry) {
            for import in &ast.macro_imports {
                for item in &import.names {
                    if item.alias == entry {
                        return find(
                            &crate::modules::resolve_spec(&path, &import.path)?,
                            &item.name,
                            stack,
                        );
                    }
                }
            }
        }
        Err(CompileError::plain(format!(
            "`{entry}` is not an exported procedural macro in {}",
            path.display()
        )))
    }
    find(path, entry, &mut Vec::new())
}
struct References(HashSet<String>);
impl Visitor for References {
    fn name(&mut self, n: &mut String, _: Span, role: Role) -> Result<(), CompileError> {
        if matches!(role, Role::Value | Role::Call | Role::Type) {
            self.0.insert(n.clone());
        }
        Ok(())
    }
}
/// Remove compiler API implementation code when loading a macro package solely
/// for its ordinary target helpers. This is phase separation, before linking.
/// Helpers that depend on compile-time functions cannot be runtime dependencies.
pub fn runtime(path: &Path, ast: &mut Module, target: crate::Target) -> Result<(), CompileError> {
    project(path, ast, target, true)
}
/// Quotation references can mention imports unused by host implementation code.
pub fn definitions(
    path: &Path,
    ast: &mut Module,
    target: crate::Target,
) -> Result<(), CompileError> {
    project(path, ast, target, false)
}
fn project(
    path: &Path,
    ast: &mut Module,
    target: crate::Target,
    prune_imports: bool,
) -> Result<(), CompileError> {
    if super::executor::in_host() || ast.proc_macros.is_empty() {
        return Ok(());
    }
    let is_api = |spec: &str| {
        crate::modules::resolve_spec(path, spec)
            .ok()
            .and_then(|p| std::fs::canonicalize(p).ok())
            .is_some_and(|p| p == Path::new(env!("CARGO_MANIFEST_DIR")).join("std/macro.rfl"))
    };
    let mut host: HashSet<_> = ast.proc_macros.iter().cloned().collect();
    for import in &ast.imports {
        if is_api(&import.path) {
            host.extend(import.names.iter().map(|n| n.alias.clone()));
        }
    }
    loop {
        let before = host.len();
        let scoped = crate::modules::macro_phase_names(ast, &host);
        for function in &scoped.functions {
            let mut unit = scoped.clone();
            unit.functions = vec![function.clone()];
            unit.adts.clear();
            unit.structs.clear();
            unit.consts.clear();
            unit.impls.clear();
            unit.traits.clear();
            unit.externs.clear();
            let mut refs = References(HashSet::new());
            walk::module(&mut refs, &mut unit)?;
            if refs.0.iter().any(|name| name.starts_with("$phase$host$")) {
                host.insert(function.name.clone());
            }
        }
        for item in &ast.structs {
            if item.fields.iter().any(|f| type_host(&f.ty, &host)) {
                host.insert(item.name.clone());
            }
        }
        for item in &ast.adts {
            if item
                .ctors
                .iter()
                .any(|c| c.fields.iter().any(|t| type_host(t, &host)))
            {
                host.insert(item.name.clone());
                host.extend(item.ctors.iter().map(|c| c.name.clone()));
            }
        }
        if host.len() == before {
            break;
        }
    }
    ast.functions.retain(|f| !host.contains(&f.name));
    ast.structs.retain(|f| !host.contains(&f.name));
    ast.adts.retain(|f| !host.contains(&f.name));
    ast.traits.retain(|t| {
        !t.methods.iter().any(|f| {
            f.params
                .iter()
                .any(|p| p.ty.as_ref().is_some_and(|t| type_host(t, &host)))
                || f.ret.as_ref().is_some_and(|t| type_host(t, &host))
        })
    });
    ast.impls.retain(|i| {
        !type_host(&i.ty, &host)
            && !i
                .associated_types
                .iter()
                .any(|a| a.value.as_ref().is_some_and(|t| type_host(t, &host)))
    });
    ast.imports.retain(|i| !is_api(&i.path));
    // A macro-only helper module must not become a runtime dependency merely
    // because its import was present beside the retained target helper.
    // Preserve type/trait/constructor imports (including implicit constructors
    // and trait scope), and remove only provably unused ordinary function imports.
    if prune_imports {
        let aliases = ast
            .imports
            .iter()
            .flat_map(|i| i.names.iter().map(|n| n.alias.clone()))
            .collect();
        let mut scoped = crate::modules::macro_phase_names(ast, &aliases);
        let mut used = References(HashSet::new());
        walk::module(&mut used, &mut scoped)?;
        for import in &mut ast.imports {
            let dependency = crate::modules::resolve_spec(path, &import.path)?;
            if dependency.extension().is_some_and(|e| e == "c") {
                continue;
            }
            let text =
                super::source::read(&dependency).map_err(|e| CompileError::plain(e.to_string()))?;
            let dependency =
                crate::parser::parse_for_target(&text, &crate::lexer::tokenize(&text)?, target)?;
            import.names.retain(|n| {
                !dependency.functions.iter().any(|f| f.name == n.name)
                    || used.0.contains(&format!("$phase$host${}", n.alias))
            });
        }
        ast.imports.retain(|i| !i.names.is_empty());
    }
    // Macro imports belong to the host graph. They are used on demand for any
    // invocations in retained runtime bodies, never by ordinary target linking.
    Ok(())
}
fn type_host(t: &ParsedTy, host: &HashSet<String>) -> bool {
    match t {
        ParsedTy::Named { name, args, .. } => {
            host.contains(name) || args.iter().any(|t| type_host(t, host))
        }
        ParsedTy::Projection { base, .. }
        | ParsedTy::Ref { inner: base, .. }
        | ParsedTy::Ptr { inner: base, .. } => type_host(base, host),
        ParsedTy::Array { elem, .. } => type_host(elem, host),
        ParsedTy::Tuple { fields, .. } => fields.iter().any(|t| type_host(t, host)),
        ParsedTy::Func { params, ret, .. } | ParsedTy::CFn { params, ret, .. } => {
            params.iter().any(|t| type_host(t, host)) || type_host(ret, host)
        }
    }
}
