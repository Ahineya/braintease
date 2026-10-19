//! One file is one module. Names are private until `export`. The modules are
//! flattened into a single program before type checking, so monomorphization
//! still sees the whole program. A `.c` import is compiled with the C frontend
//! and linked into the same IR module.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

use rcc_frontend::typed_ast::{TypedFunction, TypedTopLevelItem, TypedTranslationUnit};
use rcc_frontend::{Frontend, SemanticAnalyzer, StorageClass, Type, TypedCodeGenerator};
use rcc_ir::ir::{Function, Instruction, Module as IrModule, Value};

use crate::ast::*;
use crate::error::CompileError;
use crate::lexer;
use crate::parser;

struct RflUnit {
    path: PathBuf,
    source: String,
    ast: Module,
    resolved: Vec<Resolved>,
}

struct Resolved {
    import: Import,
    target: Target,
}

enum Target {
    Rfl(usize),
    C(usize),
}

pub(super) struct CMod {
    path: PathBuf,
    typed: TypedTranslationUnit,
    ir: IrModule,
}

struct Binding {
    mangled: String,
    exported: bool,
    ctors: Vec<(String, String)>,
    /// A nullary or payload constructor. Patterns spell these as bare names.
    is_ctor: bool,
    /// A function. `receiver.name()` can call it when it is in scope.
    is_fn: bool,
}

/// Typecheck `source` as the contents of `path`, resolving its imports.
/// Spans in the result are offsets into `source`, not the linked buffer.
pub(crate) fn check_root(
    path: &Path,
    source: &str,
    mut ast: Module,
) -> Result<(crate::check::ProgramInfo, Vec<CompileError>), CompileError> {
    let canon = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    let mut expanded = source.to_string();
    crate::macros::prepare(
        &canon,
        &mut expanded,
        &mut ast,
        rcc_ir::target::Target::Ripple,
    )?;
    let expansion = ast.macro_expansion.clone();
    if ast.imports.is_empty() {
        let (info, result) = crate::check::analyze(&expanded, &ast);
        let info = if let Some(data) = &expansion {
            data.editor(info)
        } else {
            info
        };
        let errors = result
            .err()
            .into_iter()
            .map(|e| {
                if let Some(data) = &expansion {
                    data.diagnostic(e)
                } else {
                    e
                }
            })
            .collect();
        return Ok((info, errors));
    }
    let mut rfls = vec![RflUnit {
        path: canon.clone(),
        source: expanded,
        ast,
        resolved: Vec::new(),
    }];
    let mut cmods = Vec::new();
    let mut stack = vec![canon];
    resolve_imports(
        0,
        &mut stack,
        &mut rfls,
        &mut cmods,
        rcc_ir::target::Target::Ripple,
    )?;
    stack.pop();
    let mut taken_c = HashSet::new();
    let mut bindings = Vec::new();
    for (index, unit) in rfls.iter().enumerate() {
        bindings.push(own_bindings(unit, index, index == 0, &mut taken_c)?);
    }
    let definition_spans: Vec<HashMap<String, Span>> = rfls
        .iter()
        .map(|unit| {
            unit.ast
                .functions
                .iter()
                .map(|i| (i.name.clone(), i.name_span))
                .chain(
                    unit.ast
                        .structs
                        .iter()
                        .map(|i| (i.name.clone(), i.name_span)),
                )
                .chain(unit.ast.adts.iter().map(|i| (i.name.clone(), i.name_span)))
                .chain(
                    unit.ast
                        .adts
                        .iter()
                        .flat_map(|i| i.ctors.iter().map(|c| (c.name.clone(), c.name_span))),
                )
                .chain(
                    unit.ast
                        .traits
                        .iter()
                        .map(|i| (i.name.clone(), i.name_span)),
                )
                .chain(
                    unit.ast
                        .consts
                        .iter()
                        .map(|i| (i.name.clone(), i.name_span)),
                )
                .collect()
        })
        .collect();
    let mut maps: Vec<HashMap<String, String>> = bindings
        .iter()
        .map(|table| {
            table
                .iter()
                .map(|(name, binding)| (name.clone(), binding.mangled.clone()))
                .collect()
        })
        .collect();
    let mut ctor_names: Vec<HashSet<String>> = bindings
        .iter()
        .map(|table| {
            table
                .iter()
                .filter(|(_, binding)| binding.is_ctor)
                .map(|(name, _)| name.clone())
                .collect()
        })
        .collect();
    let mut fn_names: Vec<HashSet<String>> = bindings
        .iter()
        .map(|table| {
            table
                .iter()
                .filter(|(_, binding)| binding.is_fn)
                .map(|(name, _)| name.clone())
                .collect()
        })
        .collect();
    for index in 0..rfls.len() {
        let resolved = rfls[index].clone_imports();
        let mut extra = Vec::new();
        for item in &resolved {
            let decls = apply_import(
                index,
                item,
                &rfls,
                &bindings,
                &mut maps,
                &mut ctor_names,
                &mut fn_names,
                &cmods,
                &extra,
            )?;
            extra.extend(decls);
        }
        rfls[index].ast.externs.extend(extra);
    }
    let mut packed = String::new();
    let mut bases = Vec::new();
    let mut lengths = Vec::new();
    for unit in &rfls {
        let header = format!("// {}\n", unit.path.display());
        bases.push(packed.len() + header.len());
        lengths.push(unit.source.len());
        packed.push_str(&header);
        packed.push_str(&unit.source);
        if !packed.ends_with('\n') {
            packed.push('\n');
        }
    }
    for (index, unit) in rfls.iter_mut().enumerate() {
        rewrite_uses(
            &mut unit.ast,
            &maps[index],
            &ctor_names[index],
            &fn_names[index],
            bases[index],
        );
        rename_defs(&mut unit.ast, &bindings[index]);
        if let Some(scope) = unit.ast.trait_scopes.last_mut() {
            scope.span.end = bases[index] + unit.source.len();
            scope.core = unit.path.parent() == Some(std_dir().as_path());
        }
    }
    // The checker sees the linked program, but editor queries must see only
    // declarations available in the root module, using their imported aliases.
    let mut imported_names = HashMap::<(usize, usize), Vec<String>>::new();
    let mut imported_values = Vec::new();
    for unit in rfls.iter().skip(1) {
        let mut declaration = |name: &str, span: Span, ty: Option<String>| {
            let mut aliases: Vec<_> = maps[0]
                .iter()
                .filter(|(alias, target)| !alias.starts_with('$') && target.as_str() == name)
                .map(|(alias, _)| alias.clone())
                .collect();
            aliases.sort();
            for alias in &aliases {
                if let Some(ty) = &ty {
                    imported_values.push(crate::check::BindingFact {
                        name: alias.clone(),
                        def: span,
                        ty: if ty.starts_with("type ") {
                            format!("type {alias}")
                        } else {
                            ty.clone()
                        },
                        scope: Span::new(0, packed.len()),
                        visible_from: 0,
                        variant: false,
                    });
                }
            }
            if !aliases.is_empty() {
                imported_names.insert((span.start, span.end), aliases);
            }
        };
        for func in &unit.ast.functions {
            declaration(&func.name, func.name_span, None);
        }
        for ext in &unit.ast.externs {
            declaration(&ext.name, ext.name_span, None);
        }
        for adt in &unit.ast.adts {
            declaration(&adt.name, adt.name_span, Some(format!("type {}", adt.name)));
            for ctor in &adt.ctors {
                declaration(&ctor.name, ctor.name_span, None);
            }
        }
        for item in &unit.ast.structs {
            declaration(
                &item.name,
                item.name_span,
                Some(format!("type {}", item.name)),
            );
        }
        for item in &unit.ast.traits {
            declaration(
                &item.name,
                item.name_span,
                Some(format!("trait {}", item.name)),
            );
        }
        for item in &unit.ast.consts {
            declaration(
                &item.name,
                item.name_span,
                Some(format!(
                    "const {}",
                    crate::check::completion_type_name(&item.ty)
                )),
            );
        }
    }
    let mut merged = Module {
        adts: Vec::new(),
        structs: Vec::new(),
        impls: Vec::new(),
        traits: Vec::new(),
        trait_scopes: Vec::new(),
        consts: Vec::new(),
        functions: Vec::new(),
        externs: Vec::new(),
        imports: Vec::new(),
        macro_imports: Vec::new(),
        macro_exports: Vec::new(),
        item_macros: Vec::new(),
        proc_macros: Vec::new(),
        macro_expansion: None,
    };
    for unit in &mut rfls {
        merged.adts.append(&mut unit.ast.adts);
        merged.structs.append(&mut unit.ast.structs);
        merged.impls.append(&mut unit.ast.impls);
        merged.traits.append(&mut unit.ast.traits);
        merged.trait_scopes.append(&mut unit.ast.trait_scopes);
        merged.consts.append(&mut unit.ast.consts);
        merged.functions.append(&mut unit.ast.functions);
        for ext in unit.ast.externs.drain(..) {
            if merged.externs.iter().any(|found| found.name == ext.name) {
                continue;
            }
            merged.externs.push(ext);
        }
    }
    let (mut info, result) = crate::check::analyze(&packed, &merged);
    // Retain unambiguous internal identities in all facts. Only presentation
    // translates them, preferring names/aliases visible from this root file.
    for table in &bindings {
        for (name, binding) in table {
            info.display_names
                .insert(binding.mangled.clone(), name.clone());
        }
    }
    let mut aliases: Vec<_> = maps[0]
        .iter()
        .filter(|(alias, _)| !alias.starts_with('$'))
        .collect();
    aliases.sort_by(|(a, _), (b, _)| a.cmp(b));
    let mut displayed = HashSet::new();
    for (alias, target) in aliases {
        let original_is_visible = info
            .display_names
            .get(target)
            .is_some_and(|original| maps[0].get(original) == Some(target));
        if !original_is_visible && displayed.insert(target) {
            info.display_names.insert(target.clone(), alias.clone());
        }
    }
    info.facts = info
        .facts
        .into_iter()
        .flat_map(|fact| {
            if shift_span(fact.def, bases[0], lengths[0]).is_some() {
                return vec![fact];
            }
            imported_names
                .get(&(fact.def.start, fact.def.end))
                .into_iter()
                .flatten()
                .map(|alias| {
                    let mut imported = fact.clone();
                    imported.name = alias.clone();
                    imported
                })
                .collect()
        })
        .collect();
    info.facts.extend(imported_values);
    info.type_members = info
        .type_members
        .into_iter()
        .flat_map(|member| {
            if !member.concrete {
                return vec![member];
            }
            maps[0]
                .iter()
                .filter(|(_, target)| **target == member.qualifier)
                .map(|(alias, _)| {
                    let mut member = member.clone();
                    member.qualifier = alias.clone();
                    member
                })
                .collect()
        })
        .collect();
    let chunks: Vec<(PathBuf, usize, usize)> = rfls
        .iter()
        .enumerate()
        .map(|(index, unit)| (unit.path.clone(), bases[index], lengths[index]))
        .collect();
    // Capture import navigation from the already-expanded, checked module graph.
    // Re-parsing a dependency on every hover would execute its macros again.
    let mut import_uses = Vec::new();
    for resolved in &rfls[0].resolved {
        if resolved.import.is_implicit() {
            continue;
        }
        let Target::Rfl(target) = resolved.target else {
            continue;
        };
        for item in &resolved.import.names {
            let Some(span) = definition_spans[target].get(&item.name).copied() else {
                continue;
            };
            let packed = Span::new(span.start + bases[target], span.end + bases[target]);
            let ty = info
                .facts
                .iter()
                .find(|f| f.def == packed)
                .map_or_else(String::new, |f| f.ty.clone());
            for span_in_root in [item.span, item.alias_span] {
                import_uses.push(crate::check::NameUse {
                    span: span_in_root,
                    def: span,
                    def_file: Some(rfls[target].path.clone()),
                    ty: ty.clone(),
                });
            }
        }
    }
    let mut info = shift_info(info, &chunks, 0);
    info.uses.extend(import_uses);
    for name in &mut info.uses {
        if let Some(path) = &name.def_file {
            if let Some(data) = rfls
                .iter()
                .find(|u| &u.path == path)
                .and_then(|u| u.ast.macro_expansion.as_ref())
            {
                name.def = data.original(name.def);
            }
        }
    }
    let info = if let Some(data) = &expansion {
        data.editor(info)
    } else {
        info
    };
    let diagnostics = result
        .err()
        .into_iter()
        .map(|err| {
            let local = shift_error(err, &rfls[0].source, chunks[0].1, chunks[0].2);
            if let Some(data) = &expansion {
                data.diagnostic(local)
            } else {
                local
            }
        })
        .collect();
    Ok((info, diagnostics))
}

fn shift_span(span: crate::ast::Span, base: usize, len: usize) -> Option<crate::ast::Span> {
    if span.start >= base && span.end <= base + len {
        Some(crate::ast::Span::new(span.start - base, span.end - base))
    } else {
        None
    }
}

fn shift_info(
    info: crate::check::ProgramInfo,
    chunks: &[(PathBuf, usize, usize)],
    root: usize,
) -> crate::check::ProgramInfo {
    let (root_path, base, len) = &chunks[root];
    let root_path = root_path.clone();
    let base = *base;
    let len = *len;
    crate::check::ProgramInfo {
        display_names: info.display_names,
        facts: info
            .facts
            .into_iter()
            .filter_map(|mut fact| {
                fact.scope = shift_scope(fact.scope, base, len)?;
                if let Some(def) = shift_span(fact.def, base, len) {
                    fact.def = def;
                }
                if fact.visible_from >= base && fact.visible_from <= base + len {
                    fact.visible_from -= base;
                } else if fact.visible_from != 0 {
                    fact.visible_from = 0;
                }
                Some(fact)
            })
            .collect(),
        uses: info
            .uses
            .into_iter()
            .filter_map(|mut name| {
                name.span = shift_span(name.span, base, len)?;
                if let Some((path, def)) = name
                    .def_file
                    .is_none()
                    .then(|| locate(chunks, name.def))
                    .flatten()
                {
                    if path != root_path {
                        name.def_file = Some(path);
                    }
                    name.def = def;
                }
                Some(name)
            })
            .collect(),
        expr_types: info
            .expr_types
            .into_iter()
            .filter_map(|mut expr| {
                expr.span = shift_span(expr.span, base, len)?;
                Some(expr)
            })
            .collect(),
        type_members: info
            .type_members
            .into_iter()
            .filter_map(|mut member| {
                member.scope = shift_scope(member.scope, base, len)?;
                Some(member)
            })
            .collect(),
        trait_methods: info
            .trait_methods
            .into_iter()
            .filter_map(|mut method| {
                method.receiver = shift_span(method.receiver, base, len)?;
                Some(method)
            })
            .collect(),
        methods: info.methods,
        call_sigs: info
            .call_sigs
            .into_iter()
            .filter_map(|mut sig| {
                sig.span = shift_span(sig.span, base, len)?;
                Some(sig)
            })
            .collect(),
    }
}

fn shift_scope(span: crate::ast::Span, base: usize, len: usize) -> Option<crate::ast::Span> {
    if span.start <= base && span.end >= base + len {
        return Some(crate::ast::Span::new(0, len));
    }
    shift_span(span, base, len)
}

fn locate(
    chunks: &[(PathBuf, usize, usize)],
    span: crate::ast::Span,
) -> Option<(PathBuf, crate::ast::Span)> {
    chunks.iter().find_map(|(path, base, len)| {
        shift_span(span, *base, *len).map(|span| (path.clone(), span))
    })
}

fn shift_error(err: CompileError, root: &str, base: usize, len: usize) -> CompileError {
    let Some(offset) = err.offset else {
        return err;
    };
    if offset < base || offset > base + len {
        return err;
    }
    CompileError::at(root, offset - base, err.detail)
}

pub fn compile_path(path: &Path) -> Result<IrModule, CompileError> {
    compile_path_for_target(path, rcc_ir::target::Target::Ripple)
}

pub fn compile_path_for_target(
    path: &Path,
    target: rcc_ir::target::Target,
) -> Result<IrModule, CompileError> {
    let (typed, mut cmods) = check_path_for_target(path, target)?;
    let mut ir = crate::lower::lower("program", &typed)?;
    ir.target = target;
    for (index, cmod) in cmods.iter_mut().enumerate() {
        prefix_globals(&mut cmod.ir, &format!("c{index}_"));
        merge_ir(&mut ir, &mut cmod.ir)?;
    }
    Ok(ir)
}

pub(super) fn check_path(path: &Path) -> Result<(crate::typed::TModule, Vec<CMod>), CompileError> {
    check_path_for_target(path, rcc_ir::target::Target::Ripple)
}

pub(super) fn check_path_for_target(
    path: &Path,
    target: rcc_ir::target::Target,
) -> Result<(crate::typed::TModule, Vec<CMod>), CompileError> {
    let canon = crate::macros::source::canonical(path)
        .map_err(|err| CompileError::plain(format!("cannot read {}: {err}", path.display())))?;
    let mut source = read_text(&canon)?;
    let mut ast = parse_source_for_target(&source, target).map_err(|e| match e.offset {
        Some(at) => CompileError::at_file(&canon, &source, at, e.detail),
        None => e,
    })?;
    crate::macros::prepare(&canon, &mut source, &mut ast, target)?;
    let expansion = ast.macro_expansion.clone();
    let at_file = |err: CompileError| {
        if let Some(data) = &expansion {
            if data
                .at(Span::new(
                    err.offset.unwrap_or(usize::MAX),
                    err.offset.unwrap_or(usize::MAX),
                ))
                .is_some()
            {
                return data.diagnostic(err);
            }
        }
        match err.offset {
            Some(offset) => CompileError::at_file(&canon, &source, offset, err.detail),
            None => err,
        }
    };
    if ast.imports.is_empty() {
        let typed = crate::check::check(&source, &ast).map_err(at_file)?;
        return Ok((typed, Vec::new()));
    }
    link(canon, source, ast, target)
}

fn link(
    path: PathBuf,
    source: String,
    ast: Module,
    target: rcc_ir::target::Target,
) -> Result<(crate::typed::TModule, Vec<CMod>), CompileError> {
    let mut rfls = vec![RflUnit {
        path: path.clone(),
        source,
        ast,
        resolved: Vec::new(),
    }];
    let mut cmods = Vec::new();
    let mut stack = vec![path];
    resolve_imports(0, &mut stack, &mut rfls, &mut cmods, target)?;
    stack.pop();

    let mut taken_c = HashSet::new();
    let mut bindings = Vec::new();
    for (index, unit) in rfls.iter().enumerate() {
        bindings.push(own_bindings(unit, index, index == 0, &mut taken_c)?);
    }
    let mut maps: Vec<HashMap<String, String>> = bindings
        .iter()
        .map(|table| {
            table
                .iter()
                .map(|(name, binding)| (name.clone(), binding.mangled.clone()))
                .collect()
        })
        .collect();
    let mut ctor_names: Vec<HashSet<String>> = bindings
        .iter()
        .map(|table| {
            table
                .iter()
                .filter(|(_, binding)| binding.is_ctor)
                .map(|(name, _)| name.clone())
                .collect()
        })
        .collect();
    let mut fn_names: Vec<HashSet<String>> = bindings
        .iter()
        .map(|table| {
            table
                .iter()
                .filter(|(_, binding)| binding.is_fn)
                .map(|(name, _)| name.clone())
                .collect()
        })
        .collect();
    for index in 0..rfls.len() {
        let resolved = rfls[index].clone_imports();
        let mut extra = Vec::new();
        for item in &resolved {
            let decls = apply_import(
                index,
                item,
                &rfls,
                &bindings,
                &mut maps,
                &mut ctor_names,
                &mut fn_names,
                &cmods,
                &extra,
            )?;
            extra.extend(decls);
        }
        rfls[index].ast.externs.extend(extra);
    }

    let mut packed = String::new();
    let mut bases = Vec::new();
    for unit in &rfls {
        let header = format!("// {}\n", unit.path.display());
        bases.push(packed.len() + header.len());
        packed.push_str(&header);
        packed.push_str(&unit.source);
        if !packed.ends_with('\n') {
            packed.push('\n');
        }
    }
    for (index, unit) in rfls.iter_mut().enumerate() {
        rewrite_uses(
            &mut unit.ast,
            &maps[index],
            &ctor_names[index],
            &fn_names[index],
            bases[index],
        );
        rename_defs(&mut unit.ast, &bindings[index]);
        if let Some(scope) = unit.ast.trait_scopes.last_mut() {
            scope.span.end = bases[index] + unit.source.len();
            scope.core = unit.path.parent() == Some(std_dir().as_path());
        }
    }

    let mut merged = Module {
        adts: Vec::new(),
        structs: Vec::new(),
        impls: Vec::new(),
        traits: Vec::new(),
        trait_scopes: Vec::new(),
        consts: Vec::new(),
        functions: Vec::new(),
        externs: Vec::new(),
        imports: Vec::new(),
        macro_imports: Vec::new(),
        macro_exports: Vec::new(),
        item_macros: Vec::new(),
        proc_macros: Vec::new(),
        macro_expansion: None,
    };
    for unit in &mut rfls {
        merged.adts.append(&mut unit.ast.adts);
        merged.structs.append(&mut unit.ast.structs);
        merged.impls.append(&mut unit.ast.impls);
        merged.traits.append(&mut unit.ast.traits);
        merged.trait_scopes.append(&mut unit.ast.trait_scopes);
        merged.consts.append(&mut unit.ast.consts);
        merged.functions.append(&mut unit.ast.functions);
        for ext in unit.ast.externs.drain(..) {
            if merged.externs.iter().any(|found| found.name == ext.name) {
                continue;
            }
            merged.externs.push(ext);
        }
    }
    let typed = crate::check::check(&packed, &merged).map_err(|err| {
        let Some(offset) = err.offset else {
            return err;
        };
        for (unit, base) in rfls.iter().zip(&bases) {
            if (*base..=base + unit.source.len()).contains(&offset) {
                let error =
                    CompileError::at_file(&unit.path, &unit.source, offset - base, err.detail);
                return unit
                    .ast
                    .macro_expansion
                    .as_ref()
                    .map_or(error.clone(), |data| data.diagnostic(error));
            }
        }
        err
    })?;
    Ok((typed, cmods))
}

/// Written imports need a path; compiler-only std dependencies also work for
/// string inputs. Their resolver never needs to read the virtual root file.
pub(super) fn source_dependencies(
    source: &str,
    ast: Module,
    target: rcc_ir::target::Target,
) -> Result<crate::typed::TModule, CompileError> {
    let (typed, cmods) = link(PathBuf::from("<source>"), source.into(), ast, target)?;
    if !cmods.is_empty() {
        return Err(CompileError::plain(
            "compiler-only source dependencies cannot contain C imports",
        ));
    }
    Ok(typed)
}

impl RflUnit {
    fn clone_imports(&self) -> Vec<Resolved> {
        self.resolved
            .iter()
            .map(|item| Resolved {
                import: item.import.clone(),
                target: item.target.clone_index(),
            })
            .collect()
    }
}

impl Target {
    fn clone_index(&self) -> Target {
        match self {
            Target::Rfl(index) => Target::Rfl(*index),
            Target::C(index) => Target::C(*index),
        }
    }
}

fn resolve_imports(
    index: usize,
    stack: &mut Vec<PathBuf>,
    rfls: &mut Vec<RflUnit>,
    cmods: &mut Vec<CMod>,
    target: rcc_ir::target::Target,
) -> Result<(), CompileError> {
    let imports = rfls[index].ast.imports.clone();
    let from = rfls[index].path.clone();
    let mut resolved = Vec::new();
    for import in imports {
        let target_path = resolve_spec(&from, &import.path)?;
        let target = if target_path.extension().is_some_and(|ext| ext == "c") {
            Target::C(load_c(&target_path, cmods)?)
        } else {
            Target::Rfl(load_rfl(&target_path, stack, rfls, cmods, target)?)
        };
        resolved.push(Resolved { import, target });
    }
    rfls[index].resolved = resolved;
    Ok(())
}

fn load_rfl(
    path: &Path,
    stack: &mut Vec<PathBuf>,
    rfls: &mut Vec<RflUnit>,
    cmods: &mut Vec<CMod>,
    target: rcc_ir::target::Target,
) -> Result<usize, CompileError> {
    let canon = crate::macros::source::canonical(path)
        .map_err(|err| CompileError::plain(format!("cannot read {}: {err}", path.display())))?;
    if let Some(start) = stack.iter().position(|found| found == &canon) {
        let mut chain: Vec<String> = stack[start..].iter().map(|path| file_label(path)).collect();
        chain.push(file_label(&canon));
        return Err(CompileError::plain(format!(
            "import cycle: {}",
            chain.join(" -> ")
        )));
    }
    if let Some(index) = rfls.iter().position(|unit| unit.path == canon) {
        return Ok(index);
    }
    let mut source = read_text(&canon)?;
    let mut ast = parse_source_for_target(&source, target)?;
    crate::macros::namespace::runtime(&canon, &mut ast, target)?;
    crate::macros::prepare(&canon, &mut source, &mut ast, target)?;
    let index = rfls.len();
    rfls.push(RflUnit {
        path: canon.clone(),
        source,
        ast,
        resolved: Vec::new(),
    });
    stack.push(canon);
    resolve_imports(index, stack, rfls, cmods, target)?;
    stack.pop();
    Ok(index)
}

fn load_c(path: &Path, cmods: &mut Vec<CMod>) -> Result<usize, CompileError> {
    let canon = crate::macros::source::canonical(path)
        .map_err(|err| CompileError::plain(format!("cannot read {}: {err}", path.display())))?;
    if let Some(index) = cmods.iter().position(|unit| unit.path == canon) {
        return Ok(index);
    }
    let source = read_text(&canon)?;
    let mut preprocessor = rcc_preprocessor::Preprocessor::new();
    // Imported C uses the same compatibility ABI and guest runtime headers as
    // rcc. In particular, stdlib device bridges must not use host SDK headers.
    preprocessor
        .add_include_dir(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../runtime/include"));
    let processed = preprocessor
        .process(&source, canon.clone())
        .map_err(|err| CompileError::plain(err.to_string()))?;
    let mut ast =
        Frontend::parse_source(&processed).map_err(|err| CompileError::plain(err.to_string()))?;
    let mut analyzer = SemanticAnalyzer::new();
    analyzer
        .analyze(&mut ast)
        .map_err(|err| CompileError::plain(err.to_string()))?;
    let types = analyzer.into_type_info();
    let typed = rcc_frontend::type_translation_unit(&ast, types)
        .map_err(|err| CompileError::plain(err.to_string()))?;
    let module = TypedCodeGenerator::new(canon.display().to_string())
        .generate(&typed)
        .map_err(|err| CompileError::plain(err.to_string()))?;
    if module
        .functions
        .iter()
        .any(|func| func.name == "main" && !func.is_external)
    {
        return Err(CompileError::plain("C `main` cannot be linked"));
    }
    cmods.push(CMod {
        path: canon,
        typed,
        ir: module,
    });
    Ok(cmods.len() - 1)
}

fn own_bindings(
    unit: &RflUnit,
    index: usize,
    entry: bool,
    taken_c: &mut HashSet<String>,
) -> Result<HashMap<String, Binding>, CompileError> {
    let mut table = HashMap::new();
    let mangle = |name: &str| {
        if unit.path == std_dir().join("io.rfl")
            && matches!(name, "Printable" | "print" | "println")
        {
            format!("$std${name}")
        } else {
            format!("m{index}${name}")
        }
    };
    for func in &unit.ast.functions {
        if func.name == "main" {
            if !entry {
                return Err(CompileError::plain(format!(
                    "{}: another file cannot define `main`",
                    file_label(&unit.path)
                )));
            }
            table.insert(
                func.name.clone(),
                Binding {
                    mangled: "main".to_string(),
                    exported: false,
                    ctors: Vec::new(),
                    is_ctor: false,
                    is_fn: true,
                },
            );
            continue;
        }
        if func.c_export {
            if !taken_c.insert(func.name.clone()) {
                return Err(CompileError::plain(format!(
                    "duplicate C export `{}`",
                    func.name
                )));
            }
            table.insert(
                func.name.clone(),
                Binding {
                    mangled: func.name.clone(),
                    exported: true,
                    ctors: Vec::new(),
                    is_ctor: false,
                    is_fn: true,
                },
            );
            continue;
        }
        table.insert(
            func.name.clone(),
            Binding {
                mangled: mangle(&func.name),
                exported: func.exported,
                ctors: Vec::new(),
                is_ctor: false,
                is_fn: true,
            },
        );
    }
    for adt in &unit.ast.adts {
        let mangled = mangle(&adt.name);
        let mut ctors = Vec::new();
        for ctor in &adt.ctors {
            let ctor_mangled = mangle(&ctor.name);
            ctors.push((ctor.name.clone(), ctor_mangled.clone()));
            table.insert(
                ctor.name.clone(),
                Binding {
                    mangled: ctor_mangled,
                    exported: adt.exported,
                    ctors: Vec::new(),
                    is_ctor: true,
                    is_fn: false,
                },
            );
        }
        table.insert(
            adt.name.clone(),
            Binding {
                mangled,
                exported: adt.exported,
                ctors,
                is_ctor: false,
                is_fn: false,
            },
        );
    }
    for item in &unit.ast.structs {
        table.insert(
            item.name.clone(),
            Binding {
                mangled: mangle(&item.name),
                exported: item.exported,
                ctors: Vec::new(),
                is_ctor: false,
                is_fn: false,
            },
        );
    }
    for item in &unit.ast.traits {
        if table.contains_key(&item.name) {
            return Err(CompileError::plain(format!(
                "duplicate declaration `{}`",
                item.name
            )));
        }
        table.insert(
            item.name.clone(),
            Binding {
                mangled: mangle(&item.name),
                exported: item.exported,
                ctors: Vec::new(),
                is_ctor: false,
                is_fn: false,
            },
        );
    }
    for item in &unit.ast.consts {
        table.insert(
            item.name.clone(),
            Binding {
                mangled: mangle(&item.name),
                exported: item.exported,
                ctors: Vec::new(),
                is_ctor: false,
                is_fn: false,
            },
        );
    }
    for item in std_facade(&unit.path) {
        if table.contains_key(item.name) {
            return Err(CompileError::plain(format!(
                "`{}` conflicts with a standard name",
                item.name
            )));
        }
        let ctors = item
            .ctors
            .iter()
            .map(|(name, mangled)| (name.to_string(), mangled.to_string()))
            .collect();
        table.insert(
            item.name.to_string(),
            Binding {
                mangled: item.mangled.to_string(),
                exported: true,
                ctors,
                is_ctor: item.is_ctor,
                is_fn: item.is_fn,
            },
        );
    }
    Ok(table)
}

struct Facade {
    name: &'static str,
    mangled: &'static str,
    ctors: &'static [(&'static str, &'static str)],
    is_ctor: bool,
    is_fn: bool,
}

fn std_facade(path: &Path) -> &'static [Facade] {
    let in_std = path
        .parent()
        .and_then(|parent| parent.file_name())
        .is_some_and(|name| name == "std");
    if !in_std {
        return &[];
    }
    match path.file_stem().and_then(|name| name.to_str()) {
        Some("option") => OPTION_FACADE,
        Some("result") => RESULT_FACADE,
        Some("io") => IO_FACADE,
        Some("string") => STRING_FACADE,
        _ => &[],
    }
}

const OPTION_FACADE: &[Facade] = &[
    Facade {
        name: "Option",
        mangled: "Option",
        ctors: &[("Some", "Some"), ("None", "None")],
        is_ctor: false,
        is_fn: false,
    },
    Facade {
        name: "Some",
        mangled: "Some",
        ctors: &[],
        is_ctor: true,
        is_fn: false,
    },
    Facade {
        name: "None",
        mangled: "None",
        ctors: &[],
        is_ctor: true,
        is_fn: false,
    },
];

const RESULT_FACADE: &[Facade] = &[
    Facade {
        name: "Result",
        mangled: "Result",
        ctors: &[("Ok", "Ok"), ("Err", "Err")],
        is_ctor: false,
        is_fn: false,
    },
    Facade {
        name: "Ok",
        mangled: "Ok",
        ctors: &[],
        is_ctor: true,
        is_fn: false,
    },
    Facade {
        name: "Err",
        mangled: "Err",
        ctors: &[],
        is_ctor: true,
        is_fn: false,
    },
];

const STRING_FACADE: &[Facade] = &[
    Facade {
        name: "from_char",
        mangled: "from_char",
        ctors: &[],
        is_ctor: false,
        is_fn: true,
    },
    Facade {
        name: "from_byte",
        mangled: "from_byte",
        ctors: &[],
        is_ctor: false,
        is_fn: true,
    },
];

const IO_FACADE: &[Facade] = &[Facade {
    name: "putc",
    mangled: "putc",
    ctors: &[],
    is_ctor: false,
    is_fn: true,
}];

/// The same std/io definitions supply implicit printing for standalone sources.
/// Imports and the prelude share canonical symbols, so there is only one impl.
pub(crate) fn printing_prelude(base: usize) -> Result<Module, CompileError> {
    let source = include_str!("../std/io.rfl");
    let mut ast = parse_source(source)?;
    ast.functions
        .retain(|f| matches!(f.name.as_str(), "print" | "println"));
    ast.externs.clear();
    let unit = RflUnit {
        path: std_dir().join("io.rfl"),
        source: source.into(),
        ast: ast.clone(),
        resolved: Vec::new(),
    };
    let bindings = own_bindings(&unit, 0, false, &mut HashSet::new())?;
    let map = bindings
        .iter()
        .map(|(n, b)| (n.clone(), b.mangled.clone()))
        .collect();
    rewrite_uses(&mut ast, &map, &HashSet::new(), &HashSet::new(), base);
    rename_defs(&mut ast, &bindings);
    ast.trait_scopes[0].core = true;
    ast.trait_scopes[0].span.end = base + source.len();
    Ok(ast)
}

fn apply_import(
    index: usize,
    resolved: &Resolved,
    rfls: &[RflUnit],
    bindings: &[HashMap<String, Binding>],
    maps: &mut [HashMap<String, String>],
    ctor_names: &mut [HashSet<String>],
    fn_names: &mut [HashSet<String>],
    cmods: &[CMod],
    already: &[ExternDecl],
) -> Result<Vec<ExternDecl>, CompileError> {
    let mut decls = Vec::new();
    match resolved.target {
        Target::Rfl(target) => {
            for item in &resolved.import.names {
                if !crate::macros::executor_host()
                    && rfls[target].ast.proc_macros.contains(&item.name)
                {
                    return Err(CompileError::plain(format!(
                        "`{}` is a compile-time macro; use import macro",
                        item.name
                    )));
                }
                let Some(binding) = bindings[target].get(&item.name) else {
                    return Err(CompileError::plain(format!(
                        "`{}` is not exported",
                        item.name
                    )));
                };
                if !binding.exported
                    && !(resolved.import.is_implicit() && item.alias.starts_with("$macro$"))
                {
                    return Err(CompileError::plain(format!(
                        "`{}` is not exported",
                        item.name
                    )));
                }
                bind_name(&mut maps[index], &item.alias, &binding.mangled, false)?;
                if binding.is_ctor {
                    ctor_names[index].insert(item.alias.clone());
                }
                if binding.is_fn {
                    fn_names[index].insert(item.alias.clone());
                }
                for (ctor, mangled) in binding.ctors.iter().filter(|_| {
                    !(resolved.import.is_implicit() && item.alias.starts_with("$macro$"))
                }) {
                    bind_name(&mut maps[index], ctor, mangled, true)?;
                    ctor_names[index].insert(ctor.clone());
                }
            }
        }
        Target::C(target) => {
            for item in &resolved.import.names {
                let func = find_c_fn(&cmods[target], &item.name)?;
                bind_name(&mut maps[index], &item.alias, &func.name, false)?;
                fn_names[index].insert(item.alias.clone());
                let present = rfls[index]
                    .ast
                    .externs
                    .iter()
                    .chain(already.iter())
                    .chain(decls.iter())
                    .any(|decl| decl.name == func.name);
                if present {
                    continue;
                }
                decls.push(extern_from_c(func, item.span)?);
            }
        }
    }
    Ok(decls)
}

fn find_c_fn<'a>(module: &'a CMod, name: &str) -> Result<&'a TypedFunction, CompileError> {
    for item in &module.typed.items {
        match item {
            TypedTopLevelItem::Function(func) if func.name == name => {
                if func.storage_class == StorageClass::Static {
                    return Err(CompileError::plain(format!("`{name}` is static")));
                }
                return Ok(func);
            }
            TypedTopLevelItem::GlobalVariable {
                name: found,
                storage_class,
                ..
            } if found == name => {
                if *storage_class == StorageClass::Static {
                    return Err(CompileError::plain(format!("`{name}` is static")));
                }
                return Err(CompileError::plain(format!("`{name}` is not a function")));
            }
            _ => {}
        }
    }
    Err(CompileError::plain(format!(
        "`{name}` is not defined in {}",
        file_label(&module.path)
    )))
}

fn extern_from_c(func: &TypedFunction, span: Span) -> Result<ExternDecl, CompileError> {
    let mut params = Vec::new();
    for (name, ty) in &func.parameters {
        params.push(Param {
            name: name.clone(),
            name_span: span,
            ty: Some(parsed_c_type(ty, span)?),
            pat: Pat::Name {
                name: name.clone(),
                span,
            },
        });
    }
    let ret = match func.return_type.unqualified() {
        Type::Void => None,
        _ => Some(parsed_c_type(&func.return_type, span)?),
    };
    Ok(ExternDecl {
        name: func.name.clone(),
        name_span: span,
        params,
        ret,
        variadic: func.is_variadic,
    })
}

fn parsed_c_type(ty: &Type, span: Span) -> Result<ParsedTy, CompileError> {
    let named = |name: &str| ParsedTy::Named {
        span,
        name: name.to_string(),
        args: Vec::new(),
    };
    match ty.unqualified() {
        Type::Void => Ok(named("unit")),
        Type::Char | Type::SignedChar => Ok(named("c_char")),
        Type::Short => Ok(named("c_short")),
        Type::Int => Ok(named("c_int")),
        Type::UnsignedChar | Type::UnsignedShort | Type::UnsignedInt => Ok(named("c_uint")),
        Type::Long => Ok(named("c_long")),
        Type::UnsignedLong => Ok(named("u32")),
        Type::LongLong => Ok(named("i64")),
        Type::UnsignedLongLong => Ok(named("u64")),
        Type::Float => Ok(named("f32")),
        Type::Double => Ok(named("f64")),
        Type::Pointer { target, .. } if matches!(target.unqualified(), Type::Function { .. }) => {
            parsed_c_type(target, span)
        }
        Type::Pointer { target, .. } => {
            let inner = parsed_c_type(target, span)?;
            Ok(ParsedTy::Ptr {
                span,
                mutable: true,
                inner: Box::new(inner),
            })
        }
        Type::Function {
            return_type,
            parameters,
            is_variadic,
        } => {
            if *is_variadic {
                return Err(CompileError::plain(
                    "a variadic C function pointer is not supported",
                ));
            }
            let mut params = Vec::new();
            for param in parameters {
                params.push(parsed_c_type(param, span)?);
            }
            let ret = match return_type.unqualified() {
                Type::Void => named("unit"),
                _ => parsed_c_type(return_type, span)?,
            };
            Ok(ParsedTy::CFn {
                span,
                params,
                ret: Box::new(ret),
            })
        }
        Type::Struct {
            name: Some(name), ..
        } => Ok(named(name)),
        other => Err(CompileError::plain(format!("unsupported C type `{other}`"))),
    }
}

fn bind_name(
    map: &mut HashMap<String, String>,
    key: &str,
    mangled: &str,
    ctor: bool,
) -> Result<(), CompileError> {
    if let Some(prev) = map.get(key) {
        if prev == mangled {
            return Ok(());
        }
        if ctor {
            return Err(CompileError::plain(format!(
                "constructor `{key}` is imported twice"
            )));
        }
        return Err(CompileError::plain(format!("`{key}` is already defined")));
    }
    map.insert(key.to_string(), mangled.to_string());
    Ok(())
}

fn rename_defs(ast: &mut Module, bindings: &HashMap<String, Binding>) {
    for func in &mut ast.functions {
        if let Some(binding) = bindings.get(&func.name) {
            func.name = binding.mangled.clone();
        }
    }
    for adt in &mut ast.adts {
        if let Some(binding) = bindings.get(&adt.name) {
            adt.name = binding.mangled.clone();
        }
        for ctor in &mut adt.ctors {
            if let Some(binding) = bindings.get(&ctor.name) {
                ctor.name = binding.mangled.clone();
            }
        }
    }
    for item in &mut ast.structs {
        if let Some(binding) = bindings.get(&item.name) {
            item.name = binding.mangled.clone();
        }
    }
    for item in &mut ast.traits {
        if let Some(binding) = bindings.get(&item.name) {
            item.name = binding.mangled.clone();
        }
    }
    for item in &mut ast.consts {
        if let Some(binding) = bindings.get(&item.name) {
            item.name = binding.mangled.clone();
        }
    }
}

struct Rewriter<'a> {
    map: &'a HashMap<String, String>,
    ctors: &'a HashSet<String>,
    fns: &'a HashSet<String>,
    values: Vec<HashSet<String>>,
    types: Vec<HashSet<String>>,
    base: usize,
}

fn rewrite_uses(
    ast: &mut Module,
    map: &HashMap<String, String>,
    ctors: &HashSet<String>,
    fns: &HashSet<String>,
    base: usize,
) {
    let owned: Vec<_> = ast
        .traits
        .iter()
        .map(|t| t.name.clone())
        .chain(ast.structs.iter().map(|s| s.name.clone()))
        .chain(ast.adts.iter().map(|a| a.name.clone()))
        .filter_map(|name| map.get(&name).cloned())
        .collect();
    // Macro method scopes precede the enclosing module scope. Hidden helper
    // imports must not make definition-site traits visible to caller code.
    for scope in &mut ast.trait_scopes {
        scope.span.start += base;
        scope.span.end += base;
        for name in &mut scope.visible {
            if let Some(target) = map.get(name) {
                *name = target.clone();
            }
        }
    }
    let mut visible: Vec<_> = map
        .iter()
        .filter(|(alias, _)| !alias.starts_with("$macro$"))
        .map(|(_, target)| target.clone())
        .collect();
    if !map.contains_key("Printable") {
        visible.push("$std$Printable".into());
    }
    // The span covers this source module, including methods expanded later.
    ast.trait_scopes.push(TraitScope {
        span: Span::new(base, usize::MAX),
        visible,
        owned,
        core: false,
    });
    let mut walk = Rewriter {
        map,
        ctors,
        fns,
        values: Vec::new(),
        types: Vec::new(),
        base,
    };
    for adt in &mut ast.adts {
        walk.bump(&mut adt.name_span);
        walk.types
            .push(adt.params.iter().map(|param| param.name.clone()).collect());
        for param in &mut adt.params {
            walk.bump(&mut param.span);
            for (name, span) in &mut param.bounds {
                walk.rewrite_type(name);
                walk.bump(span);
            }
        }
        for ctor in &mut adt.ctors {
            walk.bump(&mut ctor.name_span);
            for field in &mut ctor.fields {
                walk.ty(field);
            }
        }
        walk.types.pop();
    }
    for item in &mut ast.structs {
        walk.bump(&mut item.name_span);
        for field in &mut item.fields {
            walk.bump(&mut field.name_span);
            walk.ty(&mut field.ty);
        }
    }
    for item in &mut ast.consts {
        walk.bump(&mut item.name_span);
        walk.ty(&mut item.ty);
        walk.expr(&mut item.init);
    }
    for tr in &mut ast.traits {
        walk.bump(&mut tr.span);
        walk.bump(&mut tr.name_span);
        for item in &mut tr.associated_types {
            walk.associated_type(item);
        }
        for method in &mut tr.methods {
            walk.function_like(
                &mut method.name_span,
                &mut method.type_params,
                &mut method.params,
                &mut method.ret,
                &mut method.body,
            );
        }
    }
    for item in &mut ast.impls {
        walk.bump(&mut item.span);
        if let Some((name, span)) = &mut item.trait_name {
            walk.rewrite_type(name);
            walk.bump(span);
        }
        walk.types.push(
            item.type_params
                .iter()
                .map(|param| param.name.clone())
                .collect(),
        );
        for param in &mut item.type_params {
            walk.bump(&mut param.span);
            for (name, span) in &mut param.bounds {
                walk.rewrite_type(name);
                walk.bump(span);
            }
        }
        walk.ty(&mut item.ty);
        for associated in &mut item.associated_types {
            walk.associated_type(associated);
        }
        for method in &mut item.items {
            walk.function_like(
                &mut method.name_span,
                &mut method.type_params,
                &mut method.params,
                &mut method.ret,
                &mut method.body,
            );
        }
        walk.types.pop();
    }
    for func in &mut ast.functions {
        walk.function_like(
            &mut func.name_span,
            &mut func.type_params,
            &mut func.params,
            &mut func.ret,
            &mut func.body,
        );
    }
    for decl in &mut ast.externs {
        walk.bump(&mut decl.name_span);
        for param in &mut decl.params {
            walk.bump(&mut param.name_span);
            walk.pat(&mut param.pat, false);
            if let Some(ty) = &mut param.ty {
                walk.ty(ty);
            }
        }
        if let Some(ret) = &mut decl.ret {
            walk.ty(ret);
        }
    }
    for import in &mut ast.imports {
        walk.bump(&mut import.span);
        walk.bump(&mut import.path_span);
        for name in &mut import.names {
            walk.bump(&mut name.span);
            walk.bump(&mut name.alias_span);
        }
    }
}

impl Rewriter<'_> {
    fn bump(&self, span: &mut Span) {
        span.start = span.start.saturating_add(self.base);
        span.end = span.end.saturating_add(self.base);
    }

    fn hidden_value(&self, name: &str) -> bool {
        self.values.iter().any(|scope| scope.contains(name))
    }

    fn hidden_type(&self, name: &str) -> bool {
        self.types.iter().any(|scope| scope.contains(name))
    }

    fn rewrite_value(&self, name: &mut String) {
        if self.hidden_value(name) {
            return;
        }
        if let Some(next) = self.map.get(name) {
            *name = next.clone();
        }
    }

    fn rewrite_type(&self, name: &mut String) {
        if self.hidden_type(name) {
            return;
        }
        if let Some(next) = self.map.get(name) {
            *name = next.clone();
        }
    }

    fn function_like(
        &mut self,
        name_span: &mut Span,
        type_params: &mut [TypeParam],
        params: &mut [Param],
        ret: &mut Option<ParsedTy>,
        body: &mut Block,
    ) {
        self.bump(name_span);
        self.types
            .push(type_params.iter().map(|param| param.name.clone()).collect());
        for param in type_params.iter_mut() {
            self.bump(&mut param.span);
            for (name, span) in &mut param.bounds {
                self.rewrite_type(name);
                self.bump(span);
            }
        }
        for param in params.iter_mut() {
            self.bump(&mut param.name_span);
            if let Some(ty) = &mut param.ty {
                self.ty(ty);
            }
        }
        if let Some(ret) = ret {
            self.ty(ret);
        }
        self.values.push(HashSet::new());
        for param in params.iter_mut() {
            self.pat(&mut param.pat, true);
        }
        self.block(body);
        self.values.pop();
        self.types.pop();
    }

    fn block(&mut self, block: &mut Block) {
        self.bump(&mut block.span);
        self.values.push(HashSet::new());
        for stmt in &mut block.stmts {
            self.stmt(stmt);
        }
        if let Some(tail) = &mut block.tail {
            self.expr(tail);
        }
        self.values.pop();
    }

    fn stmt(&mut self, stmt: &mut Stmt) {
        match stmt {
            Stmt::Let {
                span,
                pattern,
                ty,
                init,
                ..
            } => {
                self.bump(span);
                if let Some(ty) = ty {
                    self.ty(ty);
                }
                self.expr(init);
                self.pat(pattern, true);
            }
            Stmt::Assign {
                name_span, value, ..
            } => {
                self.bump(name_span);
                self.expr(value);
            }
            Stmt::IndexAssign {
                name_span,
                index,
                value,
                ..
            } => {
                self.bump(name_span);
                self.expr(index);
                self.expr(value);
            }
            Stmt::FieldAssign {
                name_span, value, ..
            } => {
                self.bump(name_span);
                self.expr(value);
            }
            Stmt::DerefAssign { ptr, value } => {
                self.expr(ptr);
                self.expr(value);
            }
            Stmt::Expr(expr) => self.expr(expr),
            Stmt::Return { span, value } => {
                self.bump(span);
                if let Some(value) = value {
                    self.expr(value);
                }
            }
            Stmt::Break { span } | Stmt::Continue { span } => self.bump(span),
        }
    }

    fn expr(&mut self, expr: &mut Expr) {
        self.bump(&mut expr.span);
        match &mut expr.kind {
            ExprKind::Macro(_) => {}
            ExprKind::Int(_)
            | ExprKind::Float(_)
            | ExprKind::Bool(_)
            | ExprKind::Str(_)
            | ExprKind::CStr(_)
            | ExprKind::Char(_) => {}
            ExprKind::Name(name) => self.rewrite_value(name),
            ExprKind::Unary { expr, .. } | ExprKind::Await(expr) => self.expr(expr),
            ExprKind::Binary { lhs, rhs, .. } => {
                self.expr(lhs);
                self.expr(rhs);
            }
            ExprKind::Call {
                callee,
                callee_span,
                type_args,
                args,
            } => {
                self.bump(callee_span);
                self.rewrite_value(callee);
                for ty in type_args {
                    self.ty(ty);
                }
                for arg in args {
                    self.expr(arg);
                }
            }
            ExprKind::Method {
                receiver,
                method,
                resolved,
                args,
                method_span,
            } => {
                self.bump(method_span);
                self.expr(receiver);
                if self.fns.contains(method) && !self.hidden_value(method)
                    || resolved != method
                        && self.fns.contains(resolved)
                        && !self.hidden_value(resolved)
                {
                    self.rewrite_value(resolved);
                }
                for arg in args {
                    self.expr(arg);
                }
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                self.expr(cond);
                self.block(then_body);
                if let Some(else_body) = else_body {
                    self.expr(else_body);
                }
            }
            ExprKind::Block(block) | ExprKind::Unsafe(block) => self.block(block),
            ExprKind::Match { scrutinee, arms } => {
                self.expr(scrutinee);
                for arm in arms {
                    self.values.push(HashSet::new());
                    self.pat(&mut arm.pat, true);
                    if let Some(guard) = &mut arm.guard {
                        self.expr(guard);
                    }
                    self.expr(&mut arm.body);
                    self.values.pop();
                }
            }
            ExprKind::While { cond, body } => {
                self.expr(cond);
                self.block(body);
            }
            ExprKind::For { pat, iter, body } => {
                self.expr(iter);
                self.values.push(HashSet::new());
                self.pat(pat, true);
                self.block(body);
                self.values.pop();
            }
            ExprKind::Tuple(fields) | ExprKind::Array(fields) => {
                for field in fields {
                    self.expr(field);
                }
            }
            ExprKind::StructLit { name, fields } => {
                self.rewrite_type(name);
                for (_, field) in fields {
                    self.expr(field);
                }
            }
            ExprKind::Field {
                base, name_span, ..
            } => {
                self.bump(name_span);
                self.expr(base);
            }
            ExprKind::Range { start, end, .. } => {
                self.expr(start);
                self.expr(end);
            }
            ExprKind::Cast { expr, ty } => {
                self.expr(expr);
                self.ty(ty);
            }
            ExprKind::Lambda { params, body, .. } => {
                for param in params.iter_mut() {
                    self.bump(&mut param.name_span);
                    if let Some(ty) = &mut param.ty {
                        self.ty(ty);
                    }
                }
                self.values.push(HashSet::new());
                for param in params.iter_mut() {
                    self.pat(&mut param.pat, true);
                }
                self.expr(body);
                self.values.pop();
            }
            ExprKind::Index { base, index } => {
                self.expr(base);
                self.expr(index);
            }
            ExprKind::Slice {
                base, start, end, ..
            } => {
                self.expr(base);
                self.expr(start);
                self.expr(end);
            }
            ExprKind::Borrow { expr, .. } | ExprKind::Try(expr) | ExprKind::Deref(expr) => {
                self.expr(expr)
            }
            ExprKind::Assoc { ty_name, args, .. } => {
                self.rewrite_type(ty_name);
                for arg in args {
                    self.expr(arg);
                }
            }
        }
    }

    fn pat(&mut self, pat: &mut Pat, bind: bool) {
        match pat {
            Pat::Name { name, span } => {
                self.bump(span);
                if self.ctors.contains(name) && !self.hidden_value(name) {
                    self.rewrite_value(name);
                } else if bind && name != "_" {
                    if let Some(scope) = self.values.last_mut() {
                        scope.insert(name.clone());
                    }
                }
            }
            Pat::Ctor { name, span, fields } => {
                self.bump(span);
                self.rewrite_value(name);
                for field in fields {
                    self.pat(field, bind);
                }
            }
            Pat::Int { span, .. } | Pat::Bool { span, .. } | Pat::Char { span, .. } => {
                self.bump(span)
            }
            Pat::Tuple { span, fields } => {
                self.bump(span);
                for field in fields {
                    self.pat(field, bind);
                }
            }
            Pat::Struct { name, span, fields } => {
                self.bump(span);
                self.rewrite_type(name);
                for (_, field_span, field) in fields {
                    self.bump(field_span);
                    self.pat(field, bind);
                }
            }
        }
    }

    fn associated_type(&mut self, item: &mut AssociatedType) {
        self.bump(&mut item.name_span);
        for (name, span) in &mut item.bounds {
            self.rewrite_type(name);
            self.bump(span);
        }
        if let Some(value) = &mut item.value {
            self.ty(value);
        }
    }

    fn ty(&mut self, ty: &mut ParsedTy) {
        match ty {
            ParsedTy::Projection {
                span,
                base,
                trait_name,
                name_span,
                ..
            } => {
                self.bump(span);
                self.ty(base);
                if let Some((name, span)) = trait_name {
                    self.rewrite_type(name);
                    self.bump(span);
                }
                self.bump(name_span);
            }
            ParsedTy::Named { span, name, args } => {
                self.bump(span);
                self.rewrite_type(name);
                for arg in args {
                    self.ty(arg);
                }
            }
            ParsedTy::Func {
                span, params, ret, ..
            }
            | ParsedTy::CFn { span, params, ret } => {
                self.bump(span);
                for param in params {
                    self.ty(param);
                }
                self.ty(ret);
            }
            ParsedTy::Ref { span, inner, .. } | ParsedTy::Ptr { span, inner, .. } => {
                self.bump(span);
                self.ty(inner);
            }
            ParsedTy::Array { span, elem, len } => {
                self.bump(span);
                self.ty(elem);
                self.expr(len);
            }
            ParsedTy::Tuple { span, fields } => {
                self.bump(span);
                for field in fields {
                    self.ty(field);
                }
            }
        }
    }
}

fn std_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("std")
}

/// Find importable standard-library exports using the same bindings as imports.
/// Read the sources on demand so edits to the library do not leave a stale index.
pub(crate) fn std_import_candidates(name: &str) -> Vec<String> {
    let mut candidates = Vec::new();
    for (module, unit) in std_units() {
        let Ok(bindings) = own_bindings(&unit, 0, false, &mut HashSet::new()) else {
            continue;
        };
        if bindings.get(name).is_some_and(|binding| binding.exported) {
            candidates.push(module);
        }
    }
    candidates
}

pub(crate) fn std_completion_functions() -> Vec<(String, crate::completion::ExtensionFunction)> {
    std_units()
        .into_iter()
        .flat_map(|(module, unit)| {
            unit.ast
                .functions
                .iter()
                .filter(|function| function.exported)
                .filter_map(crate::check::completion_function)
                .map(|function| (module.clone(), function))
                .collect::<Vec<_>>()
        })
        .collect()
}

fn std_units() -> Vec<(String, RflUnit)> {
    let Ok(entries) = std::fs::read_dir(std_dir()) else {
        return Vec::new();
    };
    let mut candidates = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|ext| ext.to_str()) != Some("rfl") {
            continue;
        }
        let Some(module) = path.file_stem().and_then(|stem| stem.to_str()) else {
            continue;
        };
        if matches!(module, "macro" | "macro_host") {
            continue;
        }
        let Ok(source) = read_text(&path) else {
            continue;
        };
        let Ok(ast) = parse_source(&source) else {
            continue;
        };
        let unit = RflUnit {
            path: path.clone(),
            source,
            ast,
            resolved: Vec::new(),
        };
        candidates.push((format!("std/{module}"), unit));
    }
    candidates.sort_by(|left, right| left.0.cmp(&right.0));
    candidates
}

fn std_module_name(spec: &str) -> Option<&str> {
    let rest = spec.strip_prefix("std/")?;
    let rest = rest.strip_suffix(".rfl").unwrap_or(rest);
    if rest.is_empty() || rest.contains('/') || rest.contains('.') {
        return None;
    }
    Some(rest)
}

/// Resolve import navigation without requiring the rest of the program to check.
pub(crate) fn import_definition(
    from: &Path,
    spec: &str,
    name: Option<&str>,
) -> Option<(PathBuf, Span)> {
    let from = std::fs::canonicalize(from).unwrap_or_else(|_| from.to_path_buf());
    let path = crate::macros::source::canonical(&resolve_spec(&from, spec).ok()?).ok()?;
    let Some(name) = name else {
        return Some((path, Span::empty()));
    };
    // C declarations do not carry RFL spans; their imports still open the file.
    if path.extension().is_some_and(|ext| ext == "c") {
        return Some((path, Span::empty()));
    }
    let source = read_text(&path).ok()?;
    let ast = parse_source(&source).ok()?;
    if ast.macro_exports.iter().any(|n| n == name) {
        let (definition, entry) = crate::macros::namespace::resolve(&path, name).ok()?;
        let text = read_text(&definition).ok()?;
        let ast = parse_source(&text).ok()?;
        let span = ast.functions.iter().find(|f| f.name == entry)?.name_span;
        return Some((definition, span));
    }
    let unit = RflUnit {
        path: path.clone(),
        source,
        ast,
        resolved: Vec::new(),
    };
    let bindings = own_bindings(&unit, 0, false, &mut HashSet::new()).ok()?;
    if !bindings.get(name).is_some_and(|binding| binding.exported) {
        return None;
    }
    let ast = &unit.ast;
    let span = ast
        .functions
        .iter()
        .map(|item| (&item.name, item.name_span))
        .chain(ast.consts.iter().map(|item| (&item.name, item.name_span)))
        .chain(ast.structs.iter().map(|item| (&item.name, item.name_span)))
        .chain(ast.adts.iter().map(|item| (&item.name, item.name_span)))
        .chain(ast.traits.iter().map(|item| (&item.name, item.name_span)))
        .chain(
            ast.adts
                .iter()
                .flat_map(|item| item.ctors.iter().map(|ctor| (&ctor.name, ctor.name_span))),
        )
        .find_map(|(found, span)| (found == name).then_some(span))
        // Builtin exports such as std/option's Some have no source declaration.
        .unwrap_or_else(Span::empty);
    Some((path, span))
}

pub(crate) fn resolve_spec(from: &Path, spec: &str) -> Result<PathBuf, CompileError> {
    if spec == "std" || spec.starts_with("std/") {
        let Some(name) = std_module_name(spec) else {
            return Err(CompileError::plain(
                "expected a standard library module such as `std/list`",
            ));
        };
        let path = std_dir().join(format!("{name}.rfl"));
        if !path.exists() {
            return Err(CompileError::plain(format!(
                "the standard library module `{name}` is not implemented yet"
            )));
        }
        return Ok(path);
    }
    if let Some(name) = spec.strip_prefix("lib/") {
        let name = name.strip_suffix(".rfl").unwrap_or(name);
        if name.is_empty()
            || name.split('/').any(|part| {
                part.is_empty()
                    || part == "."
                    || part == ".."
                    || part.contains('.')
                    || part.contains('\\')
            })
        {
            return Err(CompileError::plain(
                "expected a companion library module such as `lib/ini`",
            ));
        }
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("lib")
            .join(format!("{name}.rfl"));
        if !path.is_file() {
            return Err(CompileError::plain(format!(
                "the companion library module `{name}` is not implemented yet"
            )));
        }
        return Ok(path);
    }
    let spec = if spec.ends_with(".rfl") || spec.ends_with(".c") {
        spec.to_string()
    } else {
        format!("{spec}.rfl")
    };
    let base = from.parent().unwrap_or_else(|| Path::new("."));
    let path = base.join(spec);
    if !crate::macros::source::exists(&path) {
        return Err(CompileError::plain(format!(
            "cannot read {}",
            path.display()
        )));
    }
    Ok(path)
}

fn file_label(path: &Path) -> String {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.display().to_string())
}

fn read_text(path: &Path) -> Result<String, CompileError> {
    crate::macros::source::read(path)
        .map_err(|err| CompileError::plain(format!("cannot read {}: {err}", path.display())))
}

fn parse_source(source: &str) -> Result<Module, CompileError> {
    parse_source_for_target(source, rcc_ir::target::Target::Ripple)
}

fn parse_source_for_target(
    source: &str,
    target: rcc_ir::target::Target,
) -> Result<Module, CompileError> {
    let tokens = lexer::tokenize(source)?;
    parser::parse_for_target(source, &tokens, target)
}

fn prefix_globals(module: &mut IrModule, prefix: &str) {
    let mut names = HashMap::new();
    for global in &mut module.globals {
        if !matches!(
            global.linkage,
            rcc_ir::ir::Linkage::Internal | rcc_ir::ir::Linkage::Private
        ) {
            continue;
        }
        let next = format!("{prefix}{}", global.name);
        names.insert(global.name.clone(), next.clone());
        global.name = next;
    }
    for name in &mut module.internal_functions {
        let next = format!("{prefix}{name}");
        names.insert(name.clone(), next.clone());
        *name = next;
    }
    for func in &mut module.functions {
        if let Some(next) = names.get(&func.name) {
            func.name = next.clone();
        }
        rewrite_function(func, &names);
    }
    for global in &mut module.globals {
        if let Some(value) = &mut global.initializer {
            rewrite_value(value, &names);
        }
    }
}

fn merge_ir(dst: &mut IrModule, src: &mut IrModule) -> Result<(), CompileError> {
    dst.internal_functions.append(&mut src.internal_functions);
    let offset = max_label(dst).saturating_add(1);
    for mut func in src.functions.drain(..) {
        shift_function(&mut func, offset);
        if let Some(index) = dst
            .functions
            .iter()
            .position(|found| found.name == func.name)
        {
            let existing_external = dst.functions[index].is_external;
            if existing_external && !func.is_external {
                dst.functions[index] = func;
            } else if !existing_external && !func.is_external {
                return Err(CompileError::plain(format!(
                    "duplicate function `{}`",
                    func.name
                )));
            }
        } else {
            dst.functions.push(func);
        }
    }
    for global in src.globals.drain(..) {
        if let Some(existing) = dst.globals.iter_mut().find(|g| g.name == global.name) {
            use rcc_ir::ir::Linkage;
            if existing.linkage == Linkage::Declaration {
                *existing = global;
                continue;
            }
            if global.linkage == Linkage::Declaration {
                continue;
            }
            if existing.var_type == global.var_type
                && existing.initializer.is_none()
                && global.initializer.is_none()
            {
                continue;
            }
            return Err(CompileError::plain(format!(
                "duplicate global `{}`",
                global.name
            )));
        }
        dst.add_global(global);
    }
    Ok(())
}

fn max_label(module: &IrModule) -> u32 {
    let mut max = 0u32;
    for func in &module.functions {
        for block in &func.blocks {
            max = max.max(block.id);
            for label in block.predecessors.iter().chain(block.successors.iter()) {
                max = max.max(*label);
            }
            for insn in &block.instructions {
                visit_labels(insn, &mut |label| max = max.max(label));
            }
        }
    }
    max
}

fn shift_function(func: &mut Function, offset: u32) {
    for block in &mut func.blocks {
        block.id = block.id.saturating_add(offset);
        for label in block
            .predecessors
            .iter_mut()
            .chain(block.successors.iter_mut())
        {
            *label = label.saturating_add(offset);
        }
        for insn in &mut block.instructions {
            shift_insn(insn, offset);
        }
    }
}

fn visit_labels(insn: &Instruction, visit: &mut dyn FnMut(u32)) {
    match insn {
        Instruction::Branch(label) => visit(*label),
        Instruction::BranchCond {
            true_label,
            false_label,
            ..
        } => {
            visit(*true_label);
            visit(*false_label);
        }
        Instruction::Phi { incoming, .. } => {
            for (_, label) in incoming {
                visit(*label);
            }
        }
        _ => {}
    }
}

fn shift_insn(insn: &mut Instruction, offset: u32) {
    match insn {
        Instruction::Branch(label) => *label = label.saturating_add(offset),
        Instruction::BranchCond {
            true_label,
            false_label,
            ..
        } => {
            *true_label = true_label.saturating_add(offset);
            *false_label = false_label.saturating_add(offset);
        }
        Instruction::Phi { incoming, .. } => {
            for (_, label) in incoming {
                *label = label.saturating_add(offset);
            }
        }
        _ => {}
    }
}

fn rewrite_function(func: &mut Function, names: &HashMap<String, String>) {
    for block in &mut func.blocks {
        for insn in &mut block.instructions {
            rewrite_insn(insn, names);
        }
    }
}

fn rewrite_insn(insn: &mut Instruction, names: &HashMap<String, String>) {
    match insn {
        Instruction::Binary { lhs, rhs, .. } => {
            rewrite_value(lhs, names);
            rewrite_value(rhs, names);
        }
        Instruction::Unary { operand, .. } => rewrite_value(operand, names),
        Instruction::Load { ptr, .. } => rewrite_value(ptr, names),
        Instruction::Store { value, ptr, .. } => {
            rewrite_value(value, names);
            rewrite_value(ptr, names);
        }
        Instruction::GetElementPtr { ptr, indices, .. } => {
            rewrite_value(ptr, names);
            for index in indices {
                rewrite_value(index, names);
            }
        }
        Instruction::Alloca { count, .. } => {
            if let Some(count) = count {
                rewrite_value(count, names);
            }
        }
        Instruction::Call { function, args, .. } => {
            rewrite_value(function, names);
            for arg in args {
                rewrite_value(arg, names);
            }
        }
        Instruction::Return(value) => {
            if let Some(value) = value {
                rewrite_value(value, names);
            }
        }
        Instruction::Branch(_) | Instruction::Comment(_) | Instruction::DebugLoc { .. } => {}
        Instruction::BranchCond { condition, .. } => rewrite_value(condition, names),
        Instruction::Phi { incoming, .. } => {
            for (value, _) in incoming {
                rewrite_value(value, names);
            }
        }
        Instruction::Cast { value, .. } => rewrite_value(value, names),
        Instruction::Select {
            condition,
            true_value,
            false_value,
            ..
        } => {
            rewrite_value(condition, names);
            rewrite_value(true_value, names);
            rewrite_value(false_value, names);
        }
        Instruction::Intrinsic { args, .. } => {
            for arg in args {
                rewrite_value(arg, names);
            }
        }
        Instruction::InlineAsm {
            outputs, inputs, ..
        } => {
            for operand in outputs.iter_mut().chain(inputs.iter_mut()) {
                rewrite_value(&mut operand.value, names);
            }
        }
    }
}

fn rewrite_value(value: &mut Value, names: &HashMap<String, String>) {
    match value {
        Value::Global(name) | Value::Function(name) => {
            if let Some(next) = names.get(name) {
                *name = next.clone();
            }
        }
        Value::AddressOffset { base, .. } => rewrite_value(base, names),
        Value::FatPtr(ptr) => rewrite_value(&mut ptr.addr, names),
        Value::ConstantAggregate(values) => {
            for value in values {
                rewrite_value(value, names);
            }
        }
        Value::Temp(_) | Value::Constant(_) | Value::ConstantArray(_) | Value::Undef => {}
    }
}

/// Keep a user declaration named Option/Result distinct from core prelude types,
/// including in standalone sources that do not pass through module mangling.
pub(crate) fn isolate_prelude_types(ast: &mut Module) {
    let mut map = HashMap::new();
    let mut ctors = HashSet::new();
    for adt in &ast.adts {
        if matches!(adt.name.as_str(), "Option" | "Result") {
            map.insert(adt.name.clone(), format!("$local${}", adt.name));
            for ctor in &adt.ctors {
                ctors.insert(ctor.name.clone());
                map.insert(ctor.name.clone(), format!("$local${}", ctor.name));
            }
        }
    }
    if map.is_empty() {
        return;
    }
    let scopes = std::mem::take(&mut ast.trait_scopes);
    rewrite_uses(ast, &map, &ctors, &HashSet::new(), 0);
    ast.trait_scopes = scopes;
    for adt in &mut ast.adts {
        if let Some(name) = map.get(&adt.name) {
            adt.name = name.clone();
        }
        for ctor in &mut adt.ctors {
            if let Some(name) = map.get(&ctor.name) {
                ctor.name = name.clone();
            }
        }
    }
}

/// Reuse ordinary lexical resolution for phase classification. Local values and
/// type parameters must not be mistaken for identically named compiler imports.
pub(crate) fn macro_phase_names(ast: &Module, host: &HashSet<String>) -> Module {
    let mut result = ast.clone();
    let names = host
        .iter()
        .map(|n| (n.clone(), format!("$phase$host${n}")))
        .collect();
    let ctors = ast
        .adts
        .iter()
        .flat_map(|a| a.ctors.iter().map(|c| c.name.clone()))
        .collect();
    let functions = ast
        .functions
        .iter()
        .map(|f| f.name.clone())
        .chain(
            ast.imports
                .iter()
                .flat_map(|i| i.names.iter().map(|n| n.alias.clone())),
        )
        .collect();
    rewrite_uses(&mut result, &names, &ctors, &functions, 0);
    result
}
