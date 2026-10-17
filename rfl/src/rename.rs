//! A syntax-based binding index for refactoring. Unlike checker metadata this
//! includes assignments, unused generic bodies, constants and import aliases.
//! Keys are declaration locations, never just identifier spellings.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::{Component, Path, PathBuf};

use crate::ast::*;
use crate::lexer::{self, Token, TokenKind};
use crate::parser;
mod associated;
mod traits;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct Key {
    path: PathBuf,
    start: usize,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
enum Ty {
    Async(Box<Ty>),
    Named(Key),
    Builtin(String),
}

#[derive(Clone)]
struct Binding {
    key: Key,
    ty: Option<Ty>,
}
type Env = HashMap<String, Binding>;

// Value bindings can shadow a type's spelling without hiding that type in
// annotations, struct literals or associated calls. Keep both namespaces.
fn type_key(name: &str) -> String {
    format!("@{name}")
}

struct File {
    source: String,
    ast: Module,
    tokens: Vec<Token>,
    editable: bool,
    complete: bool,
}
struct Declaration {
    name: String,
    allowed: bool,
}
#[derive(Clone)]
enum Spelling {
    Name,
    // Renaming an external import introduces an alias instead of editing a library.
    ImportAlias(String),
    // Struct shorthand stands for two independently renameable symbols.
    Shorthand { field: bool, other: String },
}
struct Occurrence {
    path: PathBuf,
    span: Span,
    key: Option<Key>,
    spelling: Spelling,
}

#[derive(Debug)]
pub(crate) struct Edit {
    pub path: PathBuf,
    pub span: Span,
    pub text: String,
    target_offset: usize,
    other_offset: usize,
}

pub(crate) struct Workspace {
    files: BTreeMap<PathBuf, File>,
    globals: HashMap<PathBuf, Env>,
    declarations: HashMap<Key, Declaration>,
    exports: HashSet<Key>,
    returns: HashMap<Key, Ty>,
    fields: HashMap<(Ty, String), Binding>,
    methods: HashMap<(Ty, String), Vec<Binding>>,
    reachable: HashMap<PathBuf, HashSet<PathBuf>>,
    traits: HashSet<Key>,
    trait_methods: HashMap<(Key, String), Binding>,
    associated_types: HashMap<(Key, String), Binding>,
    trait_impls: Vec<traits::Implementation>,
    bounds: HashMap<Key, Vec<Key>>,
    constructors: HashSet<Key>,
    occurrences: Vec<Occurrence>,
    uncertain: Vec<(PathBuf, Span)>,
    duplicates: usize,
}

pub(crate) fn canonical(path: &Path) -> PathBuf {
    std::fs::canonicalize(path).unwrap_or_else(|_| {
        let mut result = PathBuf::new();
        for part in path.components() {
            match part {
                Component::CurDir => {}
                Component::ParentDir => {
                    result.pop();
                }
                _ => result.push(part.as_os_str()),
            }
        }
        // Canonicalize the existing parent of an unsaved/new file as well.
        // On macOS /var and /private/var must identify the same document.
        let mut parent = result.as_path();
        let mut tail = Vec::new();
        while let Some(name) = parent.file_name() {
            if let Ok(mut base) = std::fs::canonicalize(parent) {
                for part in tail.iter().rev() {
                    base.push(part);
                }
                return base;
            }
            tail.push(name.to_os_string());
            let Some(next) = parent.parent() else {
                break;
            };
            parent = next;
        }
        result
    })
}

fn scan(path: &Path, sources: &mut BTreeMap<PathBuf, String>) {
    let Ok(entries) = std::fs::read_dir(path) else {
        return;
    };
    for entry in entries.flatten() {
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        let path = entry.path();
        if kind.is_dir() {
            if !matches!(
                entry.file_name().to_str(),
                Some("target" | "build" | "node_modules" | "vendor")
            ) && !entry.file_name().to_string_lossy().starts_with('.')
            {
                scan(&path, sources);
            }
        } else if kind.is_file() && path.extension().is_some_and(|ext| ext == "rfl") {
            let path = canonical(&path);
            if let std::collections::btree_map::Entry::Vacant(slot) = sources.entry(path.clone()) {
                if let Ok(source) = std::fs::read_to_string(path) {
                    slot.insert(source);
                }
            }
        }
    }
}

fn import_path(from: &Path, spec: &str) -> Option<PathBuf> {
    if let Ok(path) = crate::modules::resolve_spec(from, spec) {
        return Some(canonical(&path));
    }
    // A newly created, unsaved module may exist only in the LSP document store.
    if spec == "std" || spec.starts_with("std/") {
        return None;
    }
    let spec = if Path::new(spec).extension().is_some() {
        spec.to_string()
    } else {
        format!("{spec}.rfl")
    };
    Some(canonical(&from.parent()?.join(spec)))
}

impl Workspace {
    pub(crate) fn load(roots: &[PathBuf], open: BTreeMap<PathBuf, String>) -> Self {
        let mut sources = open;
        for root in roots {
            scan(root, &mut sources);
        }
        Self::from_sources(sources)
    }

    fn from_sources(sources: BTreeMap<PathBuf, String>) -> Self {
        let mut files = BTreeMap::new();
        let mut pending: Vec<_> = sources.into_iter().map(|(p, s)| (p, s, true)).collect();
        let io = canonical(&PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("std/io.rfl"));
        if !pending.iter().any(|(path, _, _)| path == &io) {
            pending.push((io.clone(), include_str!("../std/io.rfl").into(), false));
        }
        while let Some((path, source, editable)) = pending.pop() {
            if files.contains_key(&path) {
                continue;
            }
            let editable = editable
                && !path.starts_with(canonical(
                    &PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("std"),
                ));
            let lexed = lexer::tokenize(&source);
            let lex_ok = lexed.is_ok();
            let tokens = lexed.unwrap_or_else(|_| {
                vec![Token {
                    kind: TokenKind::Eof,
                    span: Span::empty(),
                }]
            });
            let (ast, errors) = parser::parse_for_ide(&source, &tokens);
            let complete = lex_ok && errors.is_empty();
            for import in &ast.imports {
                if let Some(target) = import_path(&path, &import.path) {
                    if !files.contains_key(&target) && !pending.iter().any(|(p, _, _)| p == &target)
                    {
                        if let Ok(text) = std::fs::read_to_string(&target) {
                            pending.push((target, text, false));
                        }
                    }
                }
            }
            files.insert(
                path,
                File {
                    source,
                    ast,
                    tokens,
                    editable,
                    complete,
                },
            );
        }
        let mut this = Self {
            files,
            globals: HashMap::new(),
            declarations: HashMap::new(),
            exports: HashSet::new(),
            returns: HashMap::new(),
            fields: HashMap::new(),
            methods: HashMap::new(),
            reachable: HashMap::new(),
            traits: HashSet::new(),
            trait_methods: HashMap::new(),
            associated_types: HashMap::new(),
            trait_impls: Vec::new(),
            bounds: HashMap::new(),
            constructors: HashSet::new(),
            occurrences: Vec::new(),
            uncertain: Vec::new(),
            duplicates: 0,
        };
        this.index();
        this
    }

    pub(crate) fn source(&self, path: &Path) -> Option<&str> {
        self.files.get(path).map(|f| f.source.as_str())
    }

    fn key(path: &Path, span: Span) -> Key {
        Key {
            path: path.to_path_buf(),
            start: span.start,
        }
    }

    fn occurrence(&mut self, path: &Path, span: Span, key: Option<Key>, spelling: Spelling) {
        if self.files[path].editable && span.end > span.start {
            self.occurrences.push(Occurrence {
                path: path.to_path_buf(),
                span,
                key,
                spelling,
            });
        }
    }

    fn declare(&mut self, path: &Path, name: &str, span: Span, allowed: bool) -> Key {
        let key = Self::key(path, span);
        self.declarations.insert(
            key.clone(),
            Declaration {
                name: name.to_string(),
                allowed: allowed && self.files[path].editable && name != "self" && name != "_",
            },
        );
        self.occurrence(path, span, Some(key.clone()), Spelling::Name);
        key
    }

    fn global(
        &mut self,
        path: &Path,
        env: &mut Env,
        name: &str,
        span: Span,
        exported: bool,
        allowed: bool,
        is_type: bool,
    ) -> Key {
        let key = self.declare(path, name, span, allowed);
        if exported {
            self.exports.insert(key.clone());
        }
        if is_type {
            env.insert(
                type_key(name),
                Binding {
                    key: key.clone(),
                    ty: Some(Ty::Named(key.clone())),
                },
            );
        }
        if env
            .insert(
                name.to_string(),
                Binding {
                    key: key.clone(),
                    ty: is_type.then(|| Ty::Named(key.clone())),
                },
            )
            .is_some()
        {
            self.duplicates += 1;
        }
        key
    }

    fn index(&mut self) {
        // Clone the small syntax trees to keep traversal mutations separate from storage.
        let modules: Vec<_> = self
            .files
            .iter()
            .map(|(p, f)| (p.clone(), f.ast.clone()))
            .collect();
        for (path, _) in &modules {
            let mut seen = HashSet::new();
            let mut pending = vec![
                path.clone(),
                canonical(&PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("std/io.rfl")),
            ];
            while let Some(next) = pending.pop() {
                if !seen.insert(next.clone()) {
                    continue;
                }
                if let Some(file) = self.files.get(&next) {
                    pending.extend(
                        file.ast
                            .imports
                            .iter()
                            .filter_map(|i| import_path(&next, &i.path)),
                    );
                }
            }
            self.reachable.insert(path.clone(), seen);
        }
        for (path, ast) in &modules {
            let mut env = Env::new();
            for item in &ast.functions {
                self.global(
                    path,
                    &mut env,
                    &item.name,
                    item.name_span,
                    item.exported,
                    item.name != "main" && !item.c_export,
                    false,
                );
            }
            for item in &ast.externs {
                self.global(
                    path,
                    &mut env,
                    &item.name,
                    item.name_span,
                    false,
                    false,
                    false,
                );
            }
            for item in &ast.consts {
                self.global(
                    path,
                    &mut env,
                    &item.name,
                    item.name_span,
                    item.exported,
                    true,
                    false,
                );
            }
            for item in &ast.structs {
                self.global(
                    path,
                    &mut env,
                    &item.name,
                    item.name_span,
                    item.exported,
                    true,
                    true,
                );
            }
            for item in &ast.traits {
                let key = self.global(
                    path,
                    &mut env,
                    &item.name,
                    item.name_span,
                    item.exported,
                    true,
                    true,
                );
                self.traits.insert(key);
            }
            for item in &ast.adts {
                let key = self.global(
                    path,
                    &mut env,
                    &item.name,
                    item.name_span,
                    item.exported,
                    true,
                    true,
                );
                for ctor in &item.ctors {
                    let ctor_key = self.global(
                        path,
                        &mut env,
                        &ctor.name,
                        ctor.name_span,
                        item.exported,
                        true,
                        false,
                    );
                    env.get_mut(&ctor.name).unwrap().ty = Some(Ty::Named(key.clone()));
                    self.returns
                        .insert(ctor_key.clone(), Ty::Named(key.clone()));
                    self.constructors.insert(ctor_key);
                }
            }
            self.globals.insert(path.clone(), env);
        }
        for (path, ast) in &modules {
            let mut env = self.globals[path].clone();
            for import in &ast.imports {
                let target = import_path(path, &import.path);
                for item in &import.names {
                    let origin = target
                        .as_ref()
                        .and_then(|p| self.globals.get(p))
                        .and_then(|e| e.get(&item.name))
                        .cloned()
                        .filter(|b| self.exports.contains(&b.key));
                    let external = import.path == "std"
                        || import.path.starts_with("std/")
                        || target
                            .as_ref()
                            .is_none_or(|p| !self.files.get(p).is_some_and(|f| f.editable));
                    let aliased = item.span != item.alias_span;
                    let is_type = target
                        .as_ref()
                        .and_then(|p| self.globals.get(p))
                        .is_some_and(|e| e.contains_key(&type_key(&item.name)))
                        || ((import.path == "std" || import.path.starts_with("std/"))
                            && matches!(item.name.as_str(), "Option" | "Result" | "String"));
                    let binding = if external || aliased || origin.is_none() {
                        if !external || aliased {
                            self.occurrence(
                                path,
                                item.span,
                                origin.as_ref().map(|b| b.key.clone()),
                                Spelling::Name,
                            );
                        }
                        let key = self.declare(path, &item.alias, item.alias_span, true);
                        if !aliased && self.files[path].editable {
                            self.occurrences.last_mut().unwrap().spelling =
                                Spelling::ImportAlias(item.name.clone());
                        }
                        if let Some(ty) = origin
                            .as_ref()
                            .and_then(|b| self.returns.get(&b.key))
                            .cloned()
                        {
                            self.returns.insert(key.clone(), ty);
                        }
                        Binding {
                            key,
                            ty: origin
                                .as_ref()
                                .and_then(|b| b.ty.clone())
                                .or_else(|| is_type.then(|| Ty::Builtin(item.name.clone()))),
                        }
                    } else {
                        let binding = origin.clone().unwrap();
                        self.occurrence(path, item.span, Some(binding.key.clone()), Spelling::Name);
                        binding
                    };
                    if is_type {
                        env.insert(type_key(&item.alias), binding.clone());
                    }
                    if env.insert(item.alias.clone(), binding).is_some() {
                        self.duplicates += 1;
                    }
                    // Importing an ADT also puts its constructors in scope.
                    if let Some(Ty::Named(owner)) = origin.as_ref().and_then(|b| b.ty.as_ref()) {
                        if let Some(other) = target.as_ref().and_then(|p| self.globals.get(p)) {
                            for (name, b) in other {
                                if self.constructors.contains(&b.key)
                                    && b.ty.as_ref() == Some(&Ty::Named(owner.clone()))
                                {
                                    env.insert(name.clone(), b.clone());
                                }
                            }
                        }
                    }
                }
            }
            // Printable belongs to the implicit prelude unless a local or
            // explicit imported trait shadows it. Its declarations stay read-only.
            let io = canonical(&PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("std/io.rfl"));
            if let Some(binding) = self
                .globals
                .get(&io)
                .and_then(|e| e.get("Printable"))
                .cloned()
            {
                if !env.contains_key(&type_key("Printable")) {
                    env.insert(type_key("Printable"), binding.clone());
                    env.entry("Printable".into()).or_insert(binding);
                }
            }
            self.globals.insert(path.clone(), env);
        }
        self.index_trait_methods(&modules);
        // Nominal type information sufficient for fields and inherent methods;
        // extension methods resolve through the same import/local bindings as calls.
        for (path, ast) in &modules {
            let env = self.globals[path].clone();
            for item in &ast.functions {
                if let Some(ty) = item.ret.as_ref().and_then(|t| type_of(t, &env)) {
                    self.returns.insert(Self::key(path, item.name_span), ty);
                }
            }
            for item in &ast.externs {
                if let Some(ty) = item.ret.as_ref().and_then(|t| type_of(t, &env)) {
                    self.returns.insert(Self::key(path, item.name_span), ty);
                }
            }
            for item in &ast.consts {
                if let Some(binding) = self
                    .globals
                    .get_mut(path)
                    .and_then(|e| e.get_mut(&item.name))
                {
                    binding.ty = type_of(&item.ty, &env);
                }
            }
            for item in &ast.structs {
                let ty = Ty::Named(Self::key(path, item.name_span));
                for field in &item.fields {
                    let key = self.declare(path, &field.name, field.name_span, true);
                    if self
                        .fields
                        .insert(
                            (ty.clone(), field.name.clone()),
                            Binding {
                                key,
                                ty: type_of(&field.ty, &env),
                            },
                        )
                        .is_some()
                    {
                        self.duplicates += 1;
                    }
                }
            }
            for item in &ast.impls {
                if item.trait_name.is_some() {
                    self.index_trait_impl(path, item, &env);
                    continue;
                }
                if let Some(ty) = type_of(&item.ty, &env) {
                    for method in &item.items {
                        let key = self.declare(path, &method.name, method.name_span, true);
                        if let Some(ret) = method.ret.as_ref().and_then(|t| type_of(t, &env)) {
                            self.returns.insert(key.clone(), ret);
                        }
                        self.methods
                            .entry((ty.clone(), method.name.clone()))
                            .or_default()
                            .push(Binding { key, ty: None });
                    }
                }
            }
        }
        for reachable in self.reachable.values() {
            for methods in self.methods.values() {
                self.duplicates += methods
                    .iter()
                    .filter(|b| reachable.contains(&b.key.path))
                    .count()
                    .saturating_sub(1);
            }
        }
        // Refresh imported callable results after every module's signature is known.
        for (path, ast) in &modules {
            for import in &ast.imports {
                if let Some(target) = import_path(path, &import.path) {
                    for item in &import.names {
                        let origin = self.globals.get(&target).and_then(|e| e.get(&item.name));
                        let local = self.globals[path].get(&item.alias).cloned();
                        if let (Some(origin), Some(local)) = (origin, local) {
                            let ty = self.returns.get(&origin.key).cloned();
                            if let Some(ty) = ty {
                                self.returns.insert(local.key, ty);
                            }
                        }
                    }
                }
            }
        }
        for (path, ast) in &modules {
            if !self.files[path].editable {
                continue;
            }
            let env = self.globals[path].clone();
            for item in &ast.functions {
                let mut local = env.clone();
                self.type_params(path, &item.type_params, &mut local);
                self.params(path, &item.params, &mut local);
                if let Some(ty) = &item.ret {
                    self.ty(path, ty, &local);
                }
                self.block(path, &item.body, &local);
            }
            for item in &ast.externs {
                let mut local = env.clone();
                self.params(path, &item.params, &mut local);
                if let Some(ty) = &item.ret {
                    self.ty(path, ty, &local);
                }
            }
            for item in &ast.consts {
                self.ty(path, &item.ty, &env);
                self.expr(path, &item.init, &env);
            }
            for item in &ast.structs {
                for field in &item.fields {
                    self.ty(path, &field.ty, &env);
                }
            }
            for item in &ast.adts {
                let mut local = env.clone();
                self.type_params(path, &item.params, &mut local);
                for ctor in &item.ctors {
                    for ty in &ctor.fields {
                        self.ty(path, ty, &local);
                    }
                }
            }
            for item in &ast.traits {
                let mut trait_env = env.clone();
                let owner = Self::key(path, item.name_span);
                trait_env.insert(
                    type_key("Self"),
                    Binding {
                        key: owner.clone(),
                        ty: Some(Ty::Named(owner)),
                    },
                );
                for associated in &item.associated_types {
                    for (name, span) in &associated.bounds {
                        self.type_name(path, name, *span, &trait_env);
                    }
                }
                for method in &item.methods {
                    let mut local = trait_env.clone();
                    self.params(path, &method.params, &mut local);
                    if let Some(ty) = &method.ret {
                        self.ty(path, ty, &local);
                    }
                }
            }
            for item in &ast.impls {
                if let Some((name, span)) = &item.trait_name {
                    self.type_name(path, name, *span, &env);
                }
                let mut local = env.clone();
                self.type_params(path, &item.type_params, &mut local);
                self.ty(path, &item.ty, &local);
                let receiver = type_of(&item.ty, &local);
                local.insert(
                    type_key("Self"),
                    Binding {
                        key: Self::key(path, item.ty.span()),
                        ty: receiver.clone(),
                    },
                );
                if let Some((name, _)) = &item.trait_name {
                    if let Some(binding) = env.get(&type_key(name)) {
                        local.insert("#SelfTrait".into(), binding.clone());
                    }
                }
                for associated in &item.associated_types {
                    if let Some(value) = &associated.value {
                        self.ty(path, value, &local);
                    }
                }
                for method in &item.items {
                    let mut local = local.clone();
                    // self is implicit and cannot be renamed.
                    if method.receiver.is_some() {
                        local.insert(
                            "self".into(),
                            Binding {
                                key: Self::key(path, method.name_span),
                                ty: receiver.clone(),
                            },
                        );
                    }
                    self.type_params(path, &method.type_params, &mut local);
                    self.params(path, &method.params, &mut local);
                    if let Some(ty) = &method.ret {
                        self.ty(path, ty, &local);
                    }
                    self.block(path, &method.body, &local);
                }
            }
        }
    }

    fn name_span(&self, path: &Path, span: Span) -> Span {
        self.files[path]
            .tokens
            .iter()
            .find(|t| {
                t.span.start >= span.start
                    && t.span.end <= span.end
                    && matches!(t.kind, TokenKind::Ident(_))
            })
            .map(|t| t.span)
            .unwrap_or(Span::empty())
    }
    fn named(&mut self, path: &Path, name: &str, span: Span, env: &Env) -> Option<Binding> {
        let binding = env.get(name).cloned();
        if name != "self" && name != "_" {
            self.occurrence(
                path,
                span,
                binding.as_ref().map(|b| b.key.clone()),
                Spelling::Name,
            );
        }
        binding
    }
    fn type_name(&mut self, path: &Path, name: &str, span: Span, env: &Env) -> Option<Binding> {
        let binding = env.get(&type_key(name)).cloned();
        if name != "Self" {
            self.occurrence(
                path,
                span,
                binding.as_ref().map(|b| b.key.clone()),
                Spelling::Name,
            );
        }
        binding
    }
    fn type_params(&mut self, path: &Path, params: &[TypeParam], env: &mut Env) {
        for param in params {
            let key = self.declare(path, &param.name, param.span, true);
            env.insert(
                type_key(&param.name),
                Binding {
                    key: key.clone(),
                    ty: Some(Ty::Named(key.clone())),
                },
            );
            let mut bounds = Vec::new();
            for (name, span) in &param.bounds {
                if let Some(Ty::Named(bound)) =
                    self.type_name(path, name, *span, env).and_then(|b| b.ty)
                {
                    bounds.push(bound);
                }
            }
            self.bounds.insert(key.clone(), bounds);
            env.insert(
                param.name.clone(),
                Binding {
                    key: key.clone(),
                    ty: Some(Ty::Named(key)),
                },
            );
        }
    }
    fn ty(&mut self, path: &Path, ty: &ParsedTy, env: &Env) {
        match ty {
            ParsedTy::Projection {
                base,
                trait_name,
                name,
                name_span,
                ..
            } => {
                self.ty(path, base, env);
                if let Some((name, span)) = trait_name {
                    self.type_name(path, name, *span, env);
                }
                let binding = self.projected_binding(
                    path,
                    base,
                    trait_name.as_ref().map(|(name, _)| name.as_str()),
                    name,
                    *name_span,
                    env,
                );
                self.occurrence(path, *name_span, binding.map(|b| b.key), Spelling::Name);
            }
            ParsedTy::Named { span, name, args } => {
                self.type_name(path, name, self.name_span(path, *span), env);
                for arg in args {
                    self.ty(path, arg, env);
                }
            }
            ParsedTy::Func { params, ret, .. } | ParsedTy::CFn { params, ret, .. } => {
                for p in params {
                    self.ty(path, p, env);
                }
                self.ty(path, ret, env);
            }
            ParsedTy::Ref { inner, .. } | ParsedTy::Ptr { inner, .. } => self.ty(path, inner, env),
            ParsedTy::Array { elem, len, .. } => {
                self.ty(path, elem, env);
                self.expr(path, len, env);
            }
            ParsedTy::Tuple { fields, .. } => {
                for f in fields {
                    self.ty(path, f, env);
                }
            }
        }
    }
    fn params(&mut self, path: &Path, params: &[Param], env: &mut Env) {
        for param in params {
            if let Some(ty) = &param.ty {
                self.ty(path, ty, env);
            }
            self.pattern(
                path,
                &param.pat,
                param.ty.as_ref().and_then(|t| type_of(t, env)),
                env,
            );
        }
    }
    fn pattern(&mut self, path: &Path, pat: &Pat, ty: Option<Ty>, env: &mut Env) {
        match pat {
            Pat::Name { name, span } => {
                if name == "_" {
                    return;
                }
                if env
                    .get(name)
                    .is_some_and(|b| self.constructors.contains(&b.key))
                {
                    self.named(path, name, *span, env);
                    return;
                }
                let key = self.declare(path, name, *span, true);
                env.insert(name.clone(), Binding { key, ty });
            }
            Pat::Ctor { name, span, fields } => {
                self.named(path, name, self.name_span(path, *span), env);
                for f in fields {
                    self.pattern(path, f, None, env);
                }
            }
            Pat::Tuple { fields, .. } => {
                for f in fields {
                    self.pattern(path, f, None, env);
                }
            }
            Pat::Struct { name, span, fields } => {
                let owner = self
                    .type_name(path, name, self.name_span(path, *span), env)
                    .and_then(|b| b.ty);
                for (name, span, pat) in fields {
                    let field = owner
                        .as_ref()
                        .and_then(|ty| self.fields.get(&(ty.clone(), name.clone())))
                        .cloned();
                    let shorthand = matches!(pat,Pat::Name{span:s,..} if s == span);
                    self.occurrence(
                        path,
                        *span,
                        field.as_ref().map(|b| b.key.clone()),
                        if shorthand {
                            Spelling::Shorthand {
                                field: true,
                                other: name.clone(),
                            }
                        } else {
                            Spelling::Name
                        },
                    );
                    self.pattern(path, pat, field.and_then(|b| b.ty), env);
                    if shorthand && name != "_" {
                        if let Some(last) = self.occurrences.last_mut() {
                            last.spelling = Spelling::Shorthand {
                                field: false,
                                other: name.clone(),
                            };
                        }
                    }
                }
            }
            _ => {}
        }
    }
    fn block(&mut self, path: &Path, block: &Block, env: &Env) -> Option<Ty> {
        let mut local = env.clone();
        for stmt in &block.stmts {
            match stmt {
                Stmt::Let {
                    pattern, ty, init, ..
                } => {
                    let inferred = self.expr(path, init, &local);
                    if let Some(ty) = ty {
                        self.ty(path, ty, &local);
                    }
                    self.pattern(
                        path,
                        pattern,
                        ty.as_ref().and_then(|t| type_of(t, &local)).or(inferred),
                        &mut local,
                    );
                }
                Stmt::Assign {
                    name,
                    name_span,
                    value,
                } => {
                    self.named(path, name, *name_span, &local);
                    self.expr(path, value, &local);
                }
                Stmt::FieldAssign {
                    name,
                    name_span,
                    fields,
                    value,
                }
                | Stmt::IndexAssign {
                    name,
                    name_span,
                    fields,
                    value,
                    ..
                } => {
                    let mut ty = self
                        .named(path, name, *name_span, &local)
                        .and_then(|b| b.ty);
                    let tokens: Vec<_> = self.files[path]
                        .tokens
                        .iter()
                        .filter(|t| t.span.start >= name_span.end && t.span.end <= value.span.start)
                        .filter(|t| matches!(t.kind, TokenKind::Ident(_)))
                        .take(fields.len())
                        .map(|t| t.span)
                        .collect();
                    for (name, span) in fields.iter().zip(tokens) {
                        ty = self.field(path, ty, name, span, Spelling::Name);
                    }
                    if let Stmt::IndexAssign { index, .. } = stmt {
                        self.expr(path, index, &local);
                    }
                    self.expr(path, value, &local);
                }
                Stmt::DerefAssign { ptr, value } => {
                    self.expr(path, ptr, &local);
                    self.expr(path, value, &local);
                }
                Stmt::Expr(expr) => {
                    self.expr(path, expr, &local);
                }
                Stmt::Return {
                    value: Some(expr), ..
                } => {
                    self.expr(path, expr, &local);
                }
                _ => {}
            }
        }
        block.tail.as_ref().and_then(|e| self.expr(path, e, &local))
    }
    fn field(
        &mut self,
        path: &Path,
        ty: Option<Ty>,
        name: &str,
        span: Span,
        spelling: Spelling,
    ) -> Option<Ty> {
        let binding = ty
            .and_then(|ty| self.fields.get(&(ty, name.to_string())))
            .cloned();
        self.occurrence(
            path,
            span,
            binding.as_ref().map(|b| b.key.clone()),
            spelling,
        );
        binding.and_then(|b| b.ty)
    }
    fn expr(&mut self, path: &Path, expr: &Expr, env: &Env) -> Option<Ty> {
        match &expr.kind {
            ExprKind::Name(name) => self.named(path, name, expr.span, env).and_then(|b| b.ty),
            ExprKind::Call {
                callee,
                callee_span,
                type_args,
                args,
            } => {
                let binding = self.named(path, callee, *callee_span, env);
                for ty in type_args {
                    self.ty(path, ty, env);
                }
                for arg in args {
                    self.expr(path, arg, env);
                }
                binding.and_then(|b| self.returns.get(&b.key).cloned())
            }
            ExprKind::Method {
                receiver,
                method,
                method_span,
                args,
                ..
            } => {
                let ty = self.expr(path, receiver, env);
                let uncertain = ty.is_none();
                if uncertain {
                    self.uncertain.push((path.to_path_buf(), *method_span));
                }
                let intrinsic = match &ty {
                    Some(Ty::Builtin(t)) if t == "Pointer" => true,
                    Some(Ty::Builtin(t)) if t == "String" => {
                        matches!(method.as_str(), "len" | "is_empty")
                    }
                    Some(Ty::Builtin(t)) if t == "Vec" => matches!(
                        method.as_str(),
                        "len" | "is_empty" | "push" | "pop" | "clear" | "get"
                    ),
                    Some(Ty::Builtin(t)) if t == "Slice" || t == "Array" => {
                        matches!(method.as_str(), "len" | "is_empty" | "get" | "to_vec")
                    }
                    _ => false,
                };
                let binding = if intrinsic || uncertain {
                    None
                } else {
                    ty.and_then(|ty| self.resolve_method(path, ty, method, *method_span, env, true))
                };
                self.occurrence(
                    path,
                    *method_span,
                    binding.as_ref().map(|b| b.key.clone()),
                    Spelling::Name,
                );
                for arg in args {
                    self.expr(path, arg, env);
                }
                binding.and_then(|b| self.returns.get(&b.key).cloned())
            }
            ExprKind::Assoc {
                ty_name,
                method,
                args,
            } => {
                let span = self.name_span(path, expr.span);
                self.type_name(path, ty_name, span, env);
                let ty = env
                    .get(&type_key(ty_name))
                    .and_then(|b| b.ty.clone())
                    .unwrap_or_else(|| Ty::Builtin(ty_name.clone()));
                let binding = self.resolve_method(
                    path,
                    ty,
                    method,
                    self.name_span(path, Span::new(span.end, expr.span.end)),
                    env,
                    false,
                );
                let method_span = self.name_span(path, Span::new(span.end, expr.span.end));
                self.occurrence(
                    path,
                    method_span,
                    binding.as_ref().map(|b| b.key.clone()),
                    Spelling::Name,
                );
                for arg in args {
                    self.expr(path, arg, env);
                }
                binding.and_then(|b| self.returns.get(&b.key).cloned())
            }
            ExprKind::StructLit { name, fields } => {
                let span = self.name_span(path, expr.span);
                let ty = self.type_name(path, name, span, env).and_then(|b| b.ty);
                let mut end = span.end;
                for (name, value) in fields {
                    let field_span = self.name_span(path, Span::new(end, value.span.end));
                    let shorthand = field_span == value.span;
                    self.field(
                        path,
                        ty.clone(),
                        name,
                        field_span,
                        if shorthand {
                            Spelling::Shorthand {
                                field: true,
                                other: name.clone(),
                            }
                        } else {
                            Spelling::Name
                        },
                    );
                    self.expr(path, value, env);
                    if shorthand {
                        if let Some(last) = self.occurrences.last_mut() {
                            last.spelling = Spelling::Shorthand {
                                field: false,
                                other: name.clone(),
                            };
                        }
                    }
                    end = value.span.end;
                }
                ty
            }
            ExprKind::Field {
                base,
                name,
                name_span,
            } => {
                let ty = self.expr(path, base, env);
                self.field(path, ty, name, *name_span, Spelling::Name)
            }
            ExprKind::Cast { expr, ty } => {
                self.expr(path, expr, env);
                self.ty(path, ty, env);
                type_of(ty, env)
            }
            ExprKind::Lambda { params, body } => {
                let mut local = env.clone();
                self.params(path, params, &mut local);
                self.expr(path, body, &local);
                None
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                self.expr(path, cond, env);
                let ty = self.block(path, then_body, env);
                if let Some(e) = else_body {
                    self.expr(path, e, env);
                }
                ty
            }
            ExprKind::Block(block) | ExprKind::Unsafe(block) => self.block(path, block, env),
            ExprKind::While { cond, body } => {
                self.expr(path, cond, env);
                self.block(path, body, env);
                None
            }
            ExprKind::For { pat, iter, body } => {
                self.expr(path, iter, env);
                let mut local = env.clone();
                self.pattern(path, pat, None, &mut local);
                self.block(path, body, &local);
                None
            }
            ExprKind::Match { scrutinee, arms } => {
                let ty = self.expr(path, scrutinee, env);
                let mut result = None;
                for arm in arms {
                    let mut local = env.clone();
                    self.pattern(path, &arm.pat, ty.clone(), &mut local);
                    if let Some(g) = &arm.guard {
                        self.expr(path, g, &local);
                    }
                    result = self.expr(path, &arm.body, &local);
                }
                result
            }
            ExprKind::Unary { expr, .. } | ExprKind::Borrow { expr, .. } => {
                self.expr(path, expr, env)
            }
            ExprKind::Await(expr) => match self.expr(path, expr, env) {
                Some(Ty::Async(output)) => Some(*output),
                _ => None,
            },
            ExprKind::Deref(expr) | ExprKind::Try(expr) => {
                self.expr(path, expr, env);
                None
            }
            ExprKind::Binary { op, lhs, rhs } => {
                let ty = self.expr(path, lhs, env);
                self.expr(path, rhs, env);
                if op.is_compare() || op.is_logic() {
                    Some(Ty::Builtin("bool".into()))
                } else {
                    ty
                }
            }
            ExprKind::Range {
                start: lhs,
                end: rhs,
                ..
            } => {
                self.expr(path, lhs, env);
                self.expr(path, rhs, env);
                None
            }
            ExprKind::Index { base, index } => {
                self.expr(path, base, env);
                self.expr(path, index, env);
                None
            }
            ExprKind::Slice {
                base, start, end, ..
            } => {
                self.expr(path, base, env);
                self.expr(path, start, env);
                self.expr(path, end, env);
                Some(Ty::Builtin("Slice".into()))
            }
            ExprKind::Tuple(items) | ExprKind::Array(items) => {
                for item in items {
                    self.expr(path, item, env);
                }
                matches!(expr.kind, ExprKind::Array(_)).then(|| Ty::Builtin("Vec".into()))
            }
            ExprKind::Str(_) => Some(Ty::Builtin("String".into())),
            ExprKind::Int(_) => Some(Ty::Builtin("i16".into())),
            ExprKind::Float(_) => Some(Ty::Builtin("f64".into())),
            ExprKind::Bool(_) => Some(Ty::Builtin("bool".into())),
            ExprKind::Char(_) => Some(Ty::Builtin("char".into())),
            _ => None,
        }
    }

    fn selected(&self, path: &Path, offset: usize) -> Option<&Occurrence> {
        // Prefer the token to the right at a boundary, then allow a caret at an
        // identifier's end (the normal position after typing its name).
        let matches = |at: usize| {
            self.occurrences
                .iter()
                .filter(move |o| o.path == path && o.span.contains(at))
        };
        matches(offset)
            .filter(|o| {
                o.key
                    .as_ref()
                    .is_some_and(|k| self.declarations.get(k).is_some_and(|d| d.allowed))
            })
            .min_by_key(|o| {
                usize::from(
                    !o.key
                        .as_ref()
                        .is_some_and(|k| k.path == o.path && k.start == o.span.start),
                )
            })
            .or_else(|| {
                offset.checked_sub(1).and_then(|at| {
                    matches(at)
                        .filter(|o| {
                            o.span.end == offset
                                && o.key.as_ref().is_some_and(|k| {
                                    self.declarations.get(k).is_some_and(|d| d.allowed)
                                })
                        })
                        .min_by_key(|o| {
                            usize::from(
                                !o.key
                                    .as_ref()
                                    .is_some_and(|k| k.path == o.path && k.start == o.span.start),
                            )
                        })
                })
            })
    }
    pub(crate) fn prepare(&self, path: &Path, offset: usize) -> Option<(Span, String)> {
        let found = self.selected(path, offset)?;
        Some((
            found.span,
            self.files[path]
                .source
                .get(found.span.start..found.span.end)?
                .to_string(),
        ))
    }
    pub(crate) fn rename(
        &self,
        path: &Path,
        offset: usize,
        name: &str,
    ) -> Result<Vec<Edit>, String> {
        valid_name(name)?;
        let selected = self
            .selected(path, offset)
            .ok_or("This position has no renameable RFL symbol")?;
        let key = selected.key.as_ref().unwrap();
        if self.declarations[key].name == name {
            return Ok(Vec::new());
        }
        let old_name = &self.declarations[key].name;
        if self.uncertain.iter().any(|(path, span)| {
            self.files[path].source.get(span.start..span.end) == Some(old_name.as_str())
        }) {
            return Err(format!(
                "Cannot determine the receiver type for every `{old_name}` call"
            ));
        }
        let member = self
            .fields
            .values()
            .chain(self.methods.values().flatten())
            .chain(self.trait_methods.values())
            .chain(self.associated_types.values())
            .any(|b| &b.key == key);
        for (path, file) in &self.files {
            if !file.editable || (!member && !self.exports.contains(key) && path != &key.path) {
                continue;
            }
            // Never return a partial refactor when syntax recovery discarded a
            // possible use, or a member's receiver type cannot be established.
            let mentions = file
                .tokens
                .iter()
                .any(|t| matches!(&t.kind, TokenKind::Ident(n) if n == old_name));
            if !file.complete && (mentions || lexer::tokenize(&file.source).is_err()) {
                return Err(format!(
                    "Fix the syntax errors in {} before renaming `{old_name}`",
                    path.display()
                ));
            }
            for token in file
                .tokens
                .iter()
                .filter(|t| matches!(&t.kind, TokenKind::Ident(n) if n == old_name))
            {
                let occurrences: Vec<_> = self
                    .occurrences
                    .iter()
                    .filter(|o| &o.path == path && o.span == token.span)
                    .collect();
                if occurrences.is_empty() || (member && occurrences.iter().any(|o| o.key.is_none()))
                {
                    return Err(format!(
                        "Cannot safely resolve every use of `{old_name}` in {}",
                        path.display()
                    ));
                }
            }
        }
        let mut edits = Vec::new();
        for occurrence in &self.occurrences {
            if occurrence.key.as_ref() != Some(key) {
                continue;
            }
            let (text, target_offset, other_offset) = match &occurrence.spelling {
                Spelling::Name => (name.to_string(), 0, 0),
                Spelling::ImportAlias(original) => {
                    (format!("{original} as {name}"), original.len() + 4, 0)
                }
                Spelling::Shorthand { field: true, other } => {
                    (format!("{name}: {other}"), 0, name.len() + 2)
                }
                Spelling::Shorthand {
                    field: false,
                    other,
                } => (format!("{other}: {name}"), other.len() + 2, 0),
            };
            if !edits
                .iter()
                .any(|e: &Edit| e.path == occurrence.path && e.span == occurrence.span)
            {
                edits.push(Edit {
                    path: occurrence.path.clone(),
                    span: occurrence.span,
                    text,
                    target_offset,
                    other_offset,
                });
            }
        }
        edits.sort_by(|a, b| a.path.cmp(&b.path).then(a.span.start.cmp(&b.span.start)));
        let mut sources: BTreeMap<_, _> = self
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
        let after = Self::from_sources(sources);
        if after.duplicates > self.duplicates {
            return Err(format!(
                "`{name}` conflicts with another declaration or import"
            ));
        }
        // Re-resolve every occurrence, including uses of the *new* name. This
        // detects shadowing/capture in either direction, not just duplicates.
        for old in &self.occurrences {
            let start = mapped(
                &edits,
                &old.path,
                old.span.start,
                old.key.as_ref() == Some(key),
            );
            let expected = old.key.as_ref().map(|k| Key {
                path: k.path.clone(),
                start: mapped(&edits, &k.path, k.start, k == key),
            });
            if !after
                .occurrences
                .iter()
                .any(|o| o.path == old.path && o.span.start == start && o.key == expected)
            {
                return Err(format!("Renaming to `{name}` would change the meaning of another reference; choose a different name"));
            }
        }
        Ok(edits)
    }
}

fn mapped(edits: &[Edit], path: &Path, offset: usize, target: bool) -> usize {
    let mut delta: isize = 0;
    for edit in edits.iter().filter(|e| e.path == path) {
        if edit.span.start > offset {
            break;
        }
        if edit.span.start == offset {
            return offset.checked_add_signed(delta).unwrap()
                + if target {
                    edit.target_offset
                } else {
                    edit.other_offset
                };
        }
        delta += edit.text.len() as isize - (edit.span.end - edit.span.start) as isize;
    }
    offset.checked_add_signed(delta).unwrap()
}
fn type_of(ty: &ParsedTy, env: &Env) -> Option<Ty> {
    match ty {
        ParsedTy::Named { name, args, .. } if name == "Async" && args.len() == 1 => {
            type_of(&args[0], env).map(|ty| Ty::Async(Box::new(ty)))
        }
        ParsedTy::Named { name, .. } => env
            .get(&type_key(name))
            .and_then(|b| b.ty.clone())
            .or_else(|| {
                matches!(
                    name.as_str(),
                    "i8" | "i16"
                        | "i32"
                        | "i64"
                        | "u8"
                        | "u16"
                        | "u32"
                        | "u64"
                        | "usize"
                        | "f32"
                        | "f64"
                        | "bool"
                        | "char"
                        | "unit"
                        | "String"
                        | "Vec"
                        | "Slice"
                        | "Option"
                        | "Result"
                        | "c_int"
                        | "c_char"
                        | "c_short"
                        | "c_uint"
                        | "c_long"
                        | "c_size_t"
                )
                .then(|| Ty::Builtin(name.clone()))
            }),
        ParsedTy::Ref { inner, .. } => type_of(inner, env),
        ParsedTy::Ptr { .. } => Some(Ty::Builtin("Pointer".into())),
        ParsedTy::Array { .. } => Some(Ty::Builtin("Array".into())),
        _ => None,
    }
}
fn valid_name(name: &str) -> Result<(), String> {
    let tokens = lexer::tokenize(name).map_err(|_| "Expected an RFL identifier".to_string())?;
    if name == "_" || tokens.len() != 2 || !matches!(&tokens[0].kind,TokenKind::Ident(s) if s==name)
    {
        return Err("The new name must be an RFL identifier, not a keyword or `_`".into());
    }
    if matches!(
        name,
        "self"
            | "main"
            | "String"
            | "Vec"
            | "Option"
            | "Result"
            | "Async"
            | "Some"
            | "None"
            | "Ok"
            | "Err"
            | "bool"
            | "char"
            | "i8"
            | "i16"
            | "i32"
            | "i64"
            | "u8"
            | "u16"
            | "u32"
            | "u64"
            | "f32"
            | "f64"
            | "unit"
    ) || crate::check::BUILTIN_FUNCTIONS.iter().any(|b| b.0 == name)
    {
        return Err(format!("`{name}` is reserved by RFL"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    pub(super) fn workspace(files: &[(&str, &str)]) -> Workspace {
        Workspace::from_sources(
            files
                .iter()
                .map(|(path, source)| {
                    (
                        PathBuf::from(format!("/rename-test/{path}")),
                        source.to_string(),
                    )
                })
                .collect(),
        )
    }
    pub(super) fn apply(
        ws: &Workspace,
        file: &str,
        offset: usize,
        new: &str,
    ) -> BTreeMap<PathBuf, String> {
        let path = PathBuf::from(format!("/rename-test/{file}"));
        let edits = ws.rename(&path, offset, new).unwrap();
        let mut sources: BTreeMap<_, _> = ws
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
    pub(super) fn single(source: &str, at: usize, new: &str) -> String {
        apply(&workspace(&[("main.rfl", source)]), "main.rfl", at, new)
            .remove(Path::new("/rename-test/main.rfl"))
            .unwrap()
    }

    #[test]
    fn locals_shadowing_assignments_and_closures() {
        let source="fn main() { let mut value = 1; value = value + 1; { let value = 3; println(value); }; let f = fn(x: i16) => value + x; println(value); println(\"value\"); /* value */ }";
        let result = single(source, source.find("mut value").unwrap() + 4, "counter");
        assert_eq!(
            result,
            source
                .replacen("mut value", "mut counter", 1)
                .replacen("value = value +", "counter = counter +", 1)
                .replacen("=> value +", "=> counter +", 1)
                .replacen(
                    "println(value); println(\"",
                    "println(counter); println(\"",
                    1
                )
        );
        assert!(crate::check_source(&result).is_ok(), "{result}");
    }
    #[test]
    fn functions_parameters_constants_generics_and_types() {
        let source="const BASE: i16 = 2; fn plus<T>(value: T) -> T { value } fn main() { let f = plus; println(BASE); println(plus(1)); }";
        assert_eq!(
            single(source, source.find("plus<T>").unwrap(), "identity"),
            source.replace("plus", "identity")
        );
        assert_eq!(
            single(source, source.find("BASE:").unwrap(), "OFFSET"),
            source.replace("BASE", "OFFSET")
        );
        assert_eq!(
            single(source, source.find("value:").unwrap(), "input"),
            source.replace("value", "input")
        );
        assert_eq!(
            single(source, source.find("<T>").unwrap() + 1, "Item"),
            source
                .replace("<T>", "<Item>")
                .replace(": T", ": Item")
                .replace("-> T", "-> Item")
        );
    }
    #[test]
    fn workspace_exports_and_explicit_aliases() {
        let library = "export fn wave(value: i16) -> i16 { value }";
        let main = "import { wave } from \"./lib\"; fn main() { println(wave(1)); }";
        let aliased = "import { wave as play } from \"./lib\"; fn main() { println(play(2)); }";
        let ws = workspace(&[
            ("lib.rfl", library),
            ("main.rfl", main),
            ("alias.rfl", aliased),
        ]);
        let result = apply(&ws, "main.rfl", main.rfind("wave").unwrap(), "signal");
        assert_eq!(
            result[Path::new("/rename-test/lib.rfl")],
            library.replace("wave", "signal")
        );
        assert_eq!(
            result[Path::new("/rename-test/main.rfl")],
            main.replace("wave", "signal")
        );
        assert_eq!(
            result[Path::new("/rename-test/alias.rfl")],
            aliased.replace("wave", "signal")
        );
        let result = apply(&ws, "alias.rfl", aliased.rfind("play").unwrap(), "listen");
        assert_eq!(result[Path::new("/rename-test/lib.rfl")], library);
        assert_eq!(result[Path::new("/rename-test/main.rfl")], main);
        assert_eq!(
            result[Path::new("/rename-test/alias.rfl")],
            aliased.replace("play", "listen")
        );
    }
    #[test]
    fn external_import_rename_creates_local_alias() {
        let source = "import { sin, PI } from \"std/math\"; fn main() { println(sin(PI)); }";
        let result = single(source, source.rfind("sin").unwrap(), "sine");
        assert_eq!(
            result,
            "import { sin as sine, PI } from \"std/math\"; fn main() { println(sine(PI)); }"
        );
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("examples/rename.rfl");
        assert!(crate::ide::Analysis::open(Some(&path), &result)
            .diagnostics
            .is_empty());
    }
    #[test]
    fn invalid_names_and_capture_are_rejected() {
        let source = "fn main() { let value = 1; { let other = 2; println(value + other); }; }";
        let ws = workspace(&[("main.rfl", source)]);
        let path = Path::new("/rename-test/main.rfl");
        let at = source.find("value").unwrap();
        for name in ["", "_", "let", "a b", "3x", "é", "println", "other"] {
            assert!(ws.rename(path, at, name).is_err(), "{name}");
        }
        let source = "fn main() { let other = 1; let value = 2; println(other + value); }";
        assert!(workspace(&[("main.rfl", source)])
            .rename(path, source.find("value").unwrap(), "other")
            .is_err());
        for at in [
            source.find("fn").unwrap(),
            source.find("main").unwrap(),
            source.find("println").unwrap(),
        ] {
            assert!(ws.prepare(path, at).is_none());
        }
    }
    #[test]
    fn fields_and_methods_are_nominal_and_expand_shorthand() {
        let source="struct Point { value: i16 } struct Other { value: i16 } impl Point { fn read(&self) -> i16 { self.value } } fn main() { let value = 1; let mut p = Point { value }; let q = Other { value }; p.value = 3; println(p.read() + q.value); let Point { value } = p; println(value); }";
        let result = single(source, source.find("value:").unwrap(), "count");
        assert!(result.contains("struct Point { count: i16 }"), "{result}");
        assert!(result.contains("struct Other { value: i16 }"), "{result}");
        assert!(result.contains("self.count"), "{result}");
        assert!(result.contains("Point { count: value }"), "{result}");
        assert!(result.contains("Other { value }"), "{result}");
        assert!(result.contains("p.count = 3"), "{result}");
        assert!(crate::check_source(&result).is_ok(), "{result}");
        let result = single(source, source.find("read(&").unwrap(), "load");
        assert!(result.contains("fn load(&self)"));
        assert!(result.contains("p.load()"));
        assert!(crate::check_source(&result).is_ok(), "{result}");
        let at = source.find("let value").unwrap() + 4;
        let result = single(source, at, "input");
        assert!(result.contains("Point { value: input }"), "{result}");
        assert!(result.contains("Other { value: input }"), "{result}");
        assert!(crate::check_source(&result).is_ok(), "{result}");
    }
    #[test]
    fn loop_binding_does_not_escape_and_type_rename_updates_construction() {
        let source="struct Point { x: i16 } fn main() { let i = 9; for (i in 0..2) { println(i); } let p = Point { x: i }; println(p.x); }";
        let result = single(source, source.find("for (i").unwrap() + 5, "index");
        assert!(result.contains("for (index in 0..2) { println(index); }"));
        assert!(result.contains("x: i"));
        let result = single(source, source.find("Point").unwrap(), "Position");
        assert_eq!(result, source.replace("Point", "Position"));
        assert!(crate::check_source(&result).is_ok());
    }
}

#[cfg(test)]
mod additional_tests {
    use super::tests::{apply, single, workspace};
    use super::*;

    #[test]
    fn stdlib_extension_aliases_and_dependency_imports_do_not_affect_declarations() {
        let source="import { replace } from \"std/string\"; fn change(s: String) -> String { s.replace(\"a\", \"b\") } fn main() { let value = change(\"a\"); println(value); }";
        let result = single(source, source.find("change(s").unwrap(), "transform");
        assert_eq!(result, source.replace("change", "transform"));
        let result = single(source, source.find("replace }").unwrap(), "substitute");
        assert_eq!(
            result,
            source
                .replacen("replace }", "replace as substitute }", 1)
                .replacen(".replace(", ".substitute(", 1)
        );
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("examples/rename.rfl");
        assert!(crate::ide::Analysis::open(Some(&path), &result)
            .diagnostics
            .is_empty());
    }
    #[test]
    fn related_syntax_errors_and_unresolved_members_fail_without_partial_edits() {
        let path = Path::new("/rename-test/main.rfl");
        let source = "fn value() -> i16 { 1 } fn main() { let x = value(); x. }";
        let ws = workspace(&[("main.rfl", source)]);
        assert!(ws
            .rename(path, source.find("value").unwrap(), "answer")
            .unwrap_err()
            .contains("syntax"));
        let source = "struct Point { x: i16 } fn get(p) { p.x } fn main() {}";
        let ws = workspace(&[("main.rfl", source)]);
        assert!(ws
            .rename(path, source.find("x:").unwrap(), "value")
            .unwrap_err()
            .contains("resolve"));
        // A lexical error in another buffer cannot crash the server or silently
        // drop potential references from a workspace export rename.
        let source = "export fn value() -> i16 { 1 }";
        let ws = workspace(&[
            ("main.rfl", source),
            ("broken.rfl", "fn main() { \"unfinished"),
        ]);
        assert!(ws
            .rename(path, source.find("value").unwrap(), "answer")
            .is_err());
    }
    #[test]
    fn constructors_and_struct_patterns_keep_identity() {
        let source="type List = Nil | Cons(i16, List); fn head(xs: List) -> i16 { match (xs) { Cons(x, _) => x, Nil => 0 } } fn main() { println(head(Cons(1, Nil))); }";
        let result = single(source, source.find("Cons(").unwrap(), "Cell");
        assert_eq!(result, source.replace("Cons", "Cell"));
        assert!(crate::check_source(&result).is_ok());
        let source="struct Point { value: i16 } fn show(Point { value }: Point) -> i16 { value } fn main() { println(show(Point { value: 2 })); }";
        let result = single(source, source.find("Point { value }").unwrap() + 8, "input");
        assert!(result.contains("Point { value: input }: Point"), "{result}");
        assert!(result.contains("{ input }"), "{result}");
        assert!(crate::check_source(&result).is_ok(), "{result}");
    }
    #[test]
    fn cross_file_fields_and_independent_impls() {
        let library="export struct Point { value: i16 } impl Point { fn read(&self) -> i16 { self.value } }";
        let source="import { Point as P } from \"./lib\"; fn main() { let p = P { value: 2 }; println(p.read()); }";
        let ws = workspace(&[("lib.rfl", library), ("main.rfl", source)]);
        let result = apply(&ws, "lib.rfl", library.find("value:").unwrap(), "count");
        assert!(result[Path::new("/rename-test/main.rfl")].contains("P { count: 2 }"));
        let other = "impl i16 { fn read(&self) -> i16 { 0 } } fn main() {}";
        let ws = workspace(&[
            ("lib.rfl", library),
            ("main.rfl", source),
            ("other.rfl", other),
        ]);
        let result = apply(&ws, "lib.rfl", library.find("read(").unwrap(), "get_value");
        assert_eq!(result[Path::new("/rename-test/other.rfl")], other);
        assert!(result[Path::new("/rename-test/main.rfl")].contains("p.get_value()"));
    }
    #[test]
    fn builtin_members_keep_precedence_over_extensions() {
        let source="fn len(s: String) -> i16 { 7 } fn main() { let s = \"hi\"; println(s.len()); println(len(s)); }";
        let result = single(source, source.find("len(s").unwrap(), "length");
        assert_eq!(
            result,
            source.replacen("fn len(", "fn length(", 1).replacen(
                "println(len(",
                "println(length(",
                1
            )
        );
    }
    #[test]
    fn type_and_value_namespaces_are_independent() {
        let source="struct Point { value: i16 } fn main() { let Point = 2; let p: Point = Point { value: Point }; println(p.value); }";
        assert!(crate::check_source(source).is_ok());
        let result = single(source, source.find("Point {").unwrap(), "Position");
        assert_eq!(result,"struct Position { value: i16 } fn main() { let Point = 2; let p: Position = Position { value: Point }; println(p.value); }");
        assert!(crate::check_source(&result).is_ok());
        let result = single(source, source.find("let Point").unwrap() + 4, "amount");
        assert_eq!(result,"struct Point { value: i16 } fn main() { let amount = 2; let p: Point = Point { value: amount }; println(p.value); }");
        assert!(crate::check_source(&result).is_ok());
    }
    #[test]
    fn associated_functions_and_builtin_vector_members() {
        let source = "impl i16 { fn zero() -> i16 { 0 } } fn main() { println(i16::zero()); }";
        let result = single(source, source.find("zero(").unwrap(), "origin");
        assert_eq!(result, source.replace("zero", "origin"));
        assert!(crate::check_source(&result).is_ok());
        let source="fn push(x: i16, y: i16) -> i16 { x + y } fn main() { let mut v = [1]; v.push(2); println(push(1, 2)); }";
        let result = single(source, source.find("push(").unwrap(), "append");
        assert_eq!(
            result,
            source.replacen("fn push(", "fn append(", 1).replacen(
                "println(push(",
                "println(append(",
                1
            )
        );
        assert!(crate::check_source(&result).is_ok());
    }
}

#[cfg(test)]
mod async_tests {
    use super::tests::single;
    #[test]
    fn awaited_calls_arguments_and_result_fields_keep_their_bindings() {
        let source="struct Point { value:i16 } async fn make(n:i16)->Point {Point{value:n}} async fn work()->i16 {let n=42;let p=make(n).await;p.value} fn main(){}";
        let renamed = single(source, source.find("make(n:").unwrap(), "construct");
        assert_eq!(renamed, source.replace("make", "construct"));
        let renamed = single(source, source.find("let n=").unwrap() + 4, "answer");
        assert_eq!(
            renamed,
            source
                .replace("let n=", "let answer=")
                .replace("make(n).", "make(answer).")
        );
        let renamed = single(source, source.find("value:").unwrap(), "number");
        assert_eq!(renamed, source.replace("value", "number"));
    }
}
