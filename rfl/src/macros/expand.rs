//! Parse -> expand -> ordinary module resolver/checker. Target code is never executed.
use super::{
    executor::{Executor, NativeExecutor},
    protocol::WireToken,
    source::ExpansionData,
    syntax::*,
    walk::{self, Role, Visitor},
};
use crate::{ast::*, CompileError, Target};
use std::{
    collections::{BTreeSet, HashMap, HashSet},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
};
static CONTEXT: AtomicU64 = AtomicU64::new(1);
thread_local! {static STACK:std::cell::RefCell<Vec<PathBuf>>=const{std::cell::RefCell::new(Vec::new())};}
struct Stack;
impl Stack {
    fn enter(path: &Path) -> Result<Self, CompileError> {
        STACK.with(|s| {
            let mut s = s.borrow_mut();
            if s.contains(&path.to_path_buf()) {
                return Err(CompileError::plain("macro bootstrap/expansion cycle"));
            }
            s.push(path.into());
            Ok(Self)
        })
    }
}
impl Drop for Stack {
    fn drop(&mut self) {
        STACK.with(|s| {
            s.borrow_mut().pop();
        });
    }
}
fn fresh() -> SyntaxContext {
    SyntaxContext(CONTEXT.fetch_add(1, Ordering::Relaxed))
}

pub fn prepare(
    path: &Path,
    source: &mut String,
    ast: &mut Module,
    target: Target,
) -> Result<(), CompileError> {
    let compiler_api = |spec: &str, name: &str| {
        crate::modules::resolve_spec(path, spec)
            .ok()
            .and_then(|p| std::fs::canonicalize(p).ok())
            .is_some_and(|p| {
                p == Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("std")
                    .join(format!("{name}.rfl"))
            })
    };
    if !super::executor::in_host()
        && ast
            .imports
            .iter()
            .any(|i| compiler_api(&i.path, "macro_host"))
    {
        return Err(CompileError::plain(
            "macro_host is private executor transport",
        ));
    }
    if !super::executor::in_host()
        && ast.proc_macros.is_empty()
        && ast.imports.iter().any(|i| compiler_api(&i.path, "macro"))
    {
        return Err(CompileError::plain(
            "std/macro is a compile-time API; import procedural macros with import macro",
        ));
    }
    if ast.macro_imports.is_empty() && ast.item_macros.is_empty() && !source.contains('!') {
        validate_entries(path, ast)?;
        return Ok(());
    }
    let mut engine = Engine {
        path: path.into(),
        source,
        target,
        imports: HashMap::new(),
        executor: NativeExecutor,
        local_macros: ast.proc_macros.clone(),
        data: ExpansionData::default(),
        depth: 0,
        total: 0,
        bytes: 0,
        helpers: BTreeSet::new(),
        definitions: HashMap::new(),
        constructors: HashMap::new(),
        definition_macros: HashMap::new(),
        definition_traits: HashMap::new(),
        trait_scopes: Vec::new(),
    };
    engine.data.source(path, engine.source);
    for import in &ast.macro_imports {
        let package = super::source::canonical(&crate::modules::resolve_spec(path, &import.path)?)
            .map_err(|e| CompileError::plain(e.to_string()))?;
        if package == path {
            return Err(CompileError::at(
                engine.source,
                import.span.start,
                "same-module procedural macro bootstrap is unsupported",
            ));
        }
        for name in &import.names {
            let implementation = super::namespace::resolve(&package, &name.name)?;
            {
                let _guard = Stack::enter(&implementation.0)?;
                super::executor::validate(&implementation.0, &implementation.1).map_err(|e| {
                    let mut error = CompileError::at(
                        engine.source,
                        name.alias_span.start,
                        format!("cannot compile macro `{}`: {}", name.alias, e.detail),
                    );
                    error.message.push_str(&format!(
                        "\nnote: macro implementation diagnostic follows\n{}",
                        e.message
                    ));
                    error
                })?;
            }
            if engine
                .imports
                .insert(name.alias.clone(), implementation)
                .is_some()
            {
                return Err(CompileError::at(
                    engine.source,
                    name.alias_span.start,
                    "macro imported twice",
                ));
            }
        }
    }
    let items = std::mem::take(&mut ast.item_macros);
    for invocation in items {
        engine.item(ast, invocation)?;
    }
    walk::module(&mut engine, ast)?;
    for (path, name, alias) in &engine.helpers {
        if let Some(import) = ast
            .imports
            .iter_mut()
            .find(|i| i.is_implicit() && i.path == path.to_string_lossy())
        {
            if !import.names.iter().any(|i| i.alias == *alias) {
                import.names.push(ImportItem {
                    name: name.clone(),
                    alias: alias.clone(),
                    span: Span::empty(),
                    alias_span: Span::empty(),
                });
            }
        } else {
            ast.imports.push(Import {
                path: path.to_string_lossy().into(),
                span: Span::empty(),
                path_span: Span::empty(),
                names: vec![ImportItem {
                    name: name.clone(),
                    alias: alias.clone(),
                    span: Span::empty(),
                    alias_span: Span::empty(),
                }],
            });
        }
    }
    ast.trait_scopes.append(&mut engine.trait_scopes);
    validate_entries(&engine.path, ast)?;
    if !engine.data.mappings.is_empty() || !engine.data.calls.is_empty() {
        ast.macro_expansion = Some(Arc::new(engine.data));
    }
    Ok(())
}
struct Engine<'a> {
    path: PathBuf,
    source: &'a mut String,
    target: Target,
    imports: HashMap<String, (PathBuf, String)>,
    executor: NativeExecutor,
    local_macros: Vec<String>,
    data: ExpansionData,
    depth: usize,
    total: usize,
    bytes: usize,
    helpers: BTreeSet<(PathBuf, String, String)>,
    definitions: HashMap<SyntaxContext, HashMap<String, (PathBuf, String)>>,
    constructors: HashMap<SyntaxContext, HashSet<String>>,
    definition_macros: HashMap<SyntaxContext, HashMap<String, (PathBuf, String)>>,
    definition_traits: HashMap<SyntaxContext, Vec<String>>,
    trait_scopes: Vec<TraitScope>,
}
impl Engine<'_> {
    fn input(&self, call: &Invocation) -> Result<Vec<TokenTree>, CompileError> {
        if let Some(input) = &call.input {
            return Ok(input.clone());
        }
        let body = &call.group.text[1..call.group.text.len() - 1];
        let base = call.span.end - call.group.text.len() + 1;
        let mut result = trees(body, SourceId(0), SyntaxContext(0), base).map_err(|e| {
            let at = self.data.original(call.span).start;
            CompileError::at(&self.data.files[0].text, at, e.detail)
        })?;
        fn contextual(data: &ExpansionData, ts: &mut [TokenTree]) {
            for t in ts {
                if let Some(m) = data.at(t.span.range) {
                    t.span = m.origin;
                }
                if let TreeKind::Group { children, .. } = &mut t.kind {
                    contextual(data, children);
                }
            }
        }
        contextual(&self.data, &mut result);
        Ok(result)
    }
    fn invoke(&mut self, call: &Invocation) -> Result<(Vec<TokenTree>, ExpansionId), CompileError> {
        if self.depth >= 64 {
            return Err(self.data.diagnostic(CompileError::at(
                self.source,
                call.span.start,
                "macro expansion depth limit exceeded",
            )));
        }
        let mut parent = self.data.at(call.span).map(|m| m.expansion);
        let mut ancestry = 0;
        while let Some(id) = parent {
            ancestry += 1;
            if ancestry >= 64 {
                return Err(self.data.diagnostic(CompileError::at(
                    self.source,
                    call.span.start,
                    "macro expansion depth limit exceeded",
                )));
            }
            parent = self.data.expansions.get(&id).and_then(|e| e.parent);
        }
        let context = self.data.context(call.name_span);
        let namespace = if context.0 == 0 {
            Some(&self.imports)
        } else {
            self.definition_macros.get(&context)
        };
        let (package, entry) = namespace
            .and_then(|n| n.get(&call.name))
            .cloned()
            .ok_or_else(|| {
                self.data.diagnostic(CompileError::at(
                    self.source,
                    call.name_span.start,
                    if context.0 == 0 && self.local_macros.contains(&call.name) {
                        "same-module procedural macro bootstrap is unsupported".into()
                    } else {
                        format!("unknown macro `{}`", call.name)
                    },
                ))
            })?;
        if context.0 == 0 {
            let span = self.data.original(call.name_span);
            if self.data.files[0].text.get(span.start..span.end) == Some(call.name.as_str()) {
                self.data.calls.push((call.name.clone(), span));
            }
        }
        let _stack = Stack::enter(&package)?;
        use std::hash::{Hash, Hasher};
        let mut nonce = std::collections::hash_map::DefaultHasher::new();
        self.path.hash(&mut nonce);
        self.data.files[0].text.hash(&mut nonce);
        (call.span.start, call.span.end).hash(&mut nonce);
        call.group.text.hash(&mut nonce);
        self.target.name().hash(&mut nonce);
        let nonce = nonce.finish();
        let input = self.input(call)?;
        let mut references = HashMap::new();
        let mut wire = Vec::new();
        fn flatten(
            trees: &[TokenTree],
            refs: &mut HashMap<String, SyntaxSpan>,
            out: &mut Vec<WireToken>,
            nonce: u64,
        ) {
            for t in trees {
                let origin = format!("{nonce:016x}:t{}", refs.len());
                refs.insert(origin.clone(), t.span);
                let (kind, text) = match &t.kind {
                    TreeKind::Ident => (1, t.spelling.clone()),
                    TreeKind::Literal => (2, t.spelling.clone()),
                    TreeKind::Punct { joint } => (if *joint { 8 } else { 3 }, t.spelling.clone()),
                    TreeKind::Trivia => (4, t.spelling.clone()),
                    TreeKind::Text => (7, t.spelling.clone()),
                    TreeKind::Group { delimiter, .. } => (5, delimiter.to_string()),
                };
                out.push(WireToken {
                    kind,
                    text,
                    origin: origin.clone(),
                    syntax: origin.clone(),
                });
                if let TreeKind::Group {
                    delimiter,
                    children,
                } = &t.kind
                {
                    flatten(children, refs, out, nonce);
                    out.push(WireToken {
                        kind: 6,
                        text: match delimiter {
                            '(' => ")",
                            '[' => "]",
                            _ => "}",
                        }
                        .into(),
                        origin: origin.clone(),
                        syntax: origin,
                    });
                }
            }
        }
        flatten(&input, &mut references, &mut wire, nonce);
        let output = self
            .executor
            .execute(&package, &entry, self.target, &wire)
            .map_err(|mut e| {
                let at = self.data.original(call.name_span).start;
                let file = &self.data.files[0];
                if e.offset.is_none() {
                    e = CompileError::at_file(&file.path, &file.text, at, e.detail);
                }
                let (line, col) = crate::error::line_col(&file.text, at);
                e.message.push_str(&format!(
                    "\nnote: expanding `{}` at {}:{line}:{col}\n",
                    call.name,
                    self.path.display()
                ));
                e
            })?;
        let context = fresh();
        let definition =
            super::source::read(&package).map_err(|e| CompileError::plain(e.to_string()))?;
        let mut ast = crate::parser::parse_for_target(
            &definition,
            &crate::lexer::tokenize(&definition)?,
            self.target,
        )?;
        super::namespace::definitions(&package, &mut ast, self.target)?;
        let mut symbols: Vec<_> = ast
            .functions
            .iter()
            .filter(|f| !ast.proc_macros.contains(&f.name))
            .map(|f| f.name.clone())
            .collect();
        symbols.extend(ast.structs.iter().map(|i| i.name.clone()));
        symbols.extend(ast.adts.iter().map(|i| i.name.clone()));
        symbols.extend(
            ast.adts
                .iter()
                .flat_map(|i| i.ctors.iter().map(|c| c.name.clone())),
        );
        symbols.extend(ast.consts.iter().map(|i| i.name.clone()));
        symbols.extend(ast.traits.iter().map(|i| i.name.clone()));
        let mut constructors: HashSet<_> = ast
            .adts
            .iter()
            .flat_map(|a| a.ctors.iter().map(|c| c.name.clone()))
            .collect();
        let mut names: HashMap<_, _> = symbols
            .into_iter()
            .map(|name| (name.clone(), (package.clone(), name)))
            .collect();
        let mut traits: Vec<_> = ast.traits.iter().map(|t| t.name.clone()).collect();
        for import in &ast.imports {
            let dependency = crate::modules::resolve_spec(&package, &import.path)?;
            let macro_api = std::fs::canonicalize(&dependency)
                .ok()
                .is_some_and(|p| p == Path::new(env!("CARGO_MANIFEST_DIR")).join("std/macro.rfl"));
            if !macro_api || super::executor::in_host() {
                let path = crate::modules::resolve_spec(&package, &import.path)?;
                for n in &import.names {
                    names.insert(n.alias.clone(), (path.clone(), n.name.clone()));
                    let text = super::source::read(&path)
                        .map_err(|e| CompileError::plain(e.to_string()))?;
                    let dependency = crate::parser::parse_for_target(
                        &text,
                        &crate::lexer::tokenize(&text)?,
                        self.target,
                    )?;
                    if dependency.traits.iter().any(|t| t.name == n.name) {
                        traits.push(n.alias.clone());
                    }
                    if dependency
                        .adts
                        .iter()
                        .any(|a| a.ctors.iter().any(|c| c.name == n.name))
                    {
                        constructors.insert(n.alias.clone());
                    }
                    for adt in dependency.adts.iter().filter(|a| a.name == n.name) {
                        for ctor in &adt.ctors {
                            constructors.insert(ctor.name.clone());
                            names.insert(ctor.name.clone(), (path.clone(), ctor.name.clone()));
                        }
                    }
                }
            }
        }
        self.definitions.insert(context, names);
        self.definition_traits.insert(context, traits);
        self.constructors.insert(context, constructors);
        let mut macro_names = HashMap::new();
        for name in &ast.proc_macros {
            macro_names.insert(name.clone(), (package.clone(), name.clone()));
        }
        for import in &ast.macro_imports {
            for n in &import.names {
                macro_names.insert(
                    n.alias.clone(),
                    super::namespace::resolve(
                        &crate::modules::resolve_spec(&package, &import.path)?,
                        &n.name,
                    )?,
                );
            }
        }
        self.definition_macros.insert(context, macro_names);
        let mut origin = self.data.at(call.span).map_or(
            SyntaxSpan {
                source: SourceId(0),
                range: call.span,
                expansion: None,
                context,
            },
            |m| m.origin,
        );
        let id = ExpansionId(context.0);
        let definition_source = self.data.source(&package, &definition);
        self.data.expansions.insert(
            id,
            super::source::Expansion {
                parent: self.data.at(call.span).map(|m| m.expansion),
                name: call.name.clone(),
                call: origin,
                definition: definition_source,
            },
        );
        origin.context = context;
        origin.expansion = Some(id);
        let mut decoder = Decoder {
            wire: &output,
            at: 0,
            references: &references,
            call: origin,
            definition: context,
            fresh: HashMap::new(),
            count: &mut self.total,
            bytes: &mut self.bytes,
            source: &self.data,
            depth: 0,
        };
        decoder
            .stream(None)
            .map(|tokens| (tokens, id))
            .map_err(|mut e| {
                if e.offset.is_none() {
                    e = CompileError::at(self.source, call.span.start, e.detail);
                }
                self.data.annotate(&mut e, id);
                e
            })
    }
    fn syntax(
        &mut self,
        call: &Invocation,
        output: (Vec<TokenTree>, ExpansionId),
    ) -> Result<(String, Vec<crate::lexer::Token>, usize), CompileError> {
        let call_site = self.data.at(call.span).map_or(call.span, |m| m.call);
        let range = self
            .data
            .append(self.source, &output.0, call_site, output.1);
        let text = self.source[range.start..range.end].to_string();
        let mut tokens = crate::lexer::tokenize(&text).map_err(|e| {
            CompileError::at(
                self.source,
                call.span.start,
                format!("invalid macro output: {}", e.detail),
            )
        })?;
        for t in &mut tokens {
            t.span.start += range.start;
            t.span.end += range.start;
        }
        Ok((text, tokens, range.start))
    }
    fn normalize(&mut self, m: &mut Module) -> Result<(), CompileError> {
        struct Declared<'a> {
            data: &'a ExpansionData,
            names: HashSet<(SyntaxContext, String)>,
        }
        impl Visitor for Declared<'_> {
            fn name(
                &mut self,
                name: &mut String,
                span: Span,
                role: Role,
            ) -> Result<(), CompileError> {
                if role == Role::Item {
                    self.names.insert((self.data.context(span), name.clone()));
                }
                Ok(())
            }
        }
        let mut declared = Declared {
            data: &self.data,
            names: HashSet::new(),
        };
        walk::module(&mut declared, m)?;
        let declared = declared.names;
        let mut local_traits: HashMap<SyntaxContext, Vec<String>> = HashMap::new();
        for item in &m.traits {
            local_traits
                .entry(self.data.context(item.name_span))
                .or_default()
                .push(item.name.clone());
        }
        let mut normal = Normalize {
            declared,
            local_traits,
            data: &mut self.data,
            definitions: &self.definitions,
            definition_traits: &self.definition_traits,
            trait_scopes: &mut self.trait_scopes,
            constructors: &self.constructors,
            helpers: &mut self.helpers,
        };
        walk::module(&mut normal, m)
    }
    fn item(&mut self, ast: &mut Module, call: Invocation) -> Result<(), CompileError> {
        let output = self.invoke(&call)?;
        let (_, tokens, _) = self.syntax(&call, output)?;
        if tokens
            .iter()
            .all(|t| t.kind == crate::lexer::TokenKind::Eof)
        {
            return Ok(());
        }
        let mut items = crate::parser::parse_for_target(self.source, &tokens, self.target)
            .map_err(|e| self.data.diagnostic(e))?;
        if items.imports.iter().any(|i| !i.is_implicit())
            || !items.macro_imports.is_empty()
            || !items.proc_macros.is_empty()
        {
            return Err(CompileError::at(
                self.source,
                call.span.start,
                "item macros cannot generate imports, reexports, or procedural macro declarations",
            ));
        }
        ast.imports.extend(std::mem::take(&mut items.imports));
        self.normalize(&mut items)?;
        self.depth += 1;
        for nested in std::mem::take(&mut items.item_macros) {
            self.item(&mut items, nested)?;
        }
        self.depth -= 1;
        ast.adts.extend(items.adts);
        ast.structs.extend(items.structs);
        ast.impls.extend(items.impls);
        ast.traits.extend(items.traits);
        ast.consts.extend(items.consts);
        ast.functions.extend(items.functions);
        ast.externs.extend(items.externs);
        Ok(())
    }
}
impl Visitor for Engine<'_> {
    fn after_expression(&mut self, expr: &Expr) -> Result<(), CompileError> {
        if let ExprKind::Call { callee, args, .. } = &expr.kind {
            if callee.starts_with("$for$") {
                if let Some(source) = args.first() {
                    crate::parser::validate_continuation(self.source, source)
                        .map_err(|e| self.data.diagnostic(e))?;
                }
                if let Some(Expr {
                    kind: ExprKind::Lambda { body, .. },
                    ..
                }) = args.get(1)
                {
                    crate::parser::validate_continuation(self.source, body)
                        .map_err(|e| self.data.diagnostic(e))?;
                }
            }
        }
        Ok(())
    }

    fn expression(&mut self, expr: &mut Expr) -> Result<bool, CompileError> {
        if super::executor::in_host() && matches!(expr.kind, ExprKind::Unsafe(_)) {
            return Err(CompileError::at(
                self.source,
                expr.span.start,
                "macro deterministic profile forbids generated unsafe code",
            ));
        }
        let ExprKind::Macro(call) = &expr.kind else {
            return Ok(true);
        };
        let call = call.clone();
        let output = self.invoke(&call)?;
        let (_, tokens, _) = self.syntax(&call, output)?;
        let (mut expanded, imports) =
            crate::parser::parse_expression(self.source, &tokens, self.target)
                .map_err(|e| self.data.diagnostic(e))?;
        for import in imports {
            for n in import.names {
                self.helpers.insert((
                    crate::modules::resolve_spec(&self.path, &import.path)?,
                    n.name,
                    n.alias,
                ));
            }
        }
        let mut normal = Normalize {
            declared: HashSet::new(),
            local_traits: HashMap::new(),
            data: &mut self.data,
            definitions: &self.definitions,
            definition_traits: &self.definition_traits,
            trait_scopes: &mut self.trait_scopes,
            constructors: &self.constructors,
            helpers: &mut self.helpers,
        };
        walk::expression(&mut normal, &mut expanded)?;
        self.depth += 1;
        walk::expression(self, &mut expanded)?;
        self.depth -= 1;
        *expr = expanded;
        Ok(false)
    }
}
struct Normalize<'a> {
    declared: HashSet<(SyntaxContext, String)>,
    local_traits: HashMap<SyntaxContext, Vec<String>>,
    data: &'a mut ExpansionData,
    definitions: &'a HashMap<SyntaxContext, HashMap<String, (PathBuf, String)>>,
    definition_traits: &'a HashMap<SyntaxContext, Vec<String>>,
    trait_scopes: &'a mut Vec<TraitScope>,
    constructors: &'a HashMap<SyntaxContext, HashSet<String>>,
    helpers: &'a mut BTreeSet<(PathBuf, String, String)>,
}
impl Normalize<'_> {
    fn record_trait_scope(&mut self, span: Span) {
        let context = self.data.context(span);
        let Some(mut traits) = self.definition_traits.get(&context).cloned() else {
            return;
        };
        traits.extend(
            self.local_traits
                .get(&context)
                .into_iter()
                .flatten()
                .cloned(),
        );
        let mut visible = Vec::new();
        for name in &traits {
            let alias = format!("$macro${}${name}", context.0);
            if !self.declared.contains(&(context, name.clone())) {
                if let Some((path, symbol)) = self
                    .definitions
                    .get(&context)
                    .and_then(|names| names.get(name))
                {
                    self.helpers
                        .insert((path.clone(), symbol.clone(), alias.clone()));
                }
            }
            visible.push(alias);
        }
        if !traits.iter().any(|n| n == "Printable") {
            visible.push("$std$Printable".into());
        }
        self.trait_scopes.push(TraitScope {
            span,
            visible,
            owned: Vec::new(),
            core: false,
        });
    }
}
impl Visitor for Normalize<'_> {
    fn expression(&mut self, expr: &mut Expr) -> Result<bool, CompileError> {
        if let ExprKind::Method {
            method,
            method_span,
            resolved,
            ..
        } = &mut expr.kind
        {
            let context = self.data.context(*method_span);
            self.record_trait_scope(*method_span);
            if self
                .definitions
                .get(&context)
                .is_some_and(|names| names.contains_key(method))
            {
                self.name(resolved, *method_span, Role::Value)?;
            }
        }
        Ok(true)
    }
    fn name(&mut self, name: &mut String, span: Span, role: Role) -> Result<(), CompileError> {
        let context = self.data.context(span);
        if context.0 == 0 || name.is_empty() || name == "_" || role == Role::Member {
            return Ok(());
        }
        // Associated projections use the context of their base type token,
        // just as dot calls use their method token. Generated local traits
        // remain visible inside their own declarations and helper functions.
        if role == Role::Type {
            self.record_trait_scope(span);
        }
        let original = name.clone();
        let internal = format!("$macro${}${name}", context.0);
        let constructor = role == Role::Pattern
            && self
                .constructors
                .get(&context)
                .is_some_and(|names| names.contains(name));
        if self.declared.contains(&(context, name.clone())) {
            self.data.names.insert(internal.clone(), original);
            *name = internal;
            return Ok(());
        }
        if let Some((path, symbol)) = self
            .definitions
            .get(&context)
            .and_then(|names| names.get(name))
            .filter(|_| matches!(role, Role::Value | Role::Call | Role::Type) || constructor)
        {
            self.helpers
                .insert((path.clone(), symbol.clone(), internal.clone()));
        } else {
            if role != Role::Item
                && ((role == Role::Type
                    && (crate::ty::Ty::parse_name(name).is_some()
                        || matches!(
                            name.as_str(),
                            "Self" | "Vec" | "Slice" | "Option" | "Result" | "Owned"
                        )))
                    || name == "self"
                    || (matches!(role, Role::Value | Role::Call | Role::Pattern)
                        && matches!(name.as_str(), "Some" | "None" | "Ok" | "Err"))
                    || (role == Role::Call
                        && matches!(
                            name.as_str(),
                            "move"
                                | "drop"
                                | "owned"
                                | "replace"
                                | "from_char"
                                | "from_byte"
                                | "putc"
                        )))
            {
                return Ok(());
            }
            if name.starts_with("$for$") {
                return Ok(());
            }
            if matches!(role, Role::Value | Role::Call)
                && matches!(name.as_str(), "print" | "println")
            {
                // Let the ordinary resolver distinguish this prelude function
                // from a generated local with the same syntax identity.
                self.helpers.insert((
                    Path::new(env!("CARGO_MANIFEST_DIR")).join("std/io.rfl"),
                    name.clone(),
                    internal.clone(),
                ));
            }
        }
        self.data.names.insert(internal.clone(), original);
        *name = internal;
        Ok(())
    }
}

struct Decoder<'a> {
    wire: &'a [WireToken],
    at: usize,
    references: &'a HashMap<String, SyntaxSpan>,
    call: SyntaxSpan,
    definition: SyntaxContext,
    fresh: HashMap<String, SyntaxContext>,
    count: &'a mut usize,
    bytes: &'a mut usize,
    source: &'a ExpansionData,
    depth: usize,
}
impl Decoder<'_> {
    fn metadata(&mut self, t: &WireToken) -> Result<SyntaxSpan, CompileError> {
        let mut origin = if t.origin == "call" {
            self.call
        } else {
            *self
                .references
                .get(&t.origin)
                .ok_or_else(|| CompileError::plain("macro returned an invalid/stale source span"))?
        };
        origin.context = match t.syntax.as_str() {
            "definition" => self.definition,
            "call" => SyntaxContext(0),
            key if key.starts_with("fresh:") => *self.fresh.entry(key.into()).or_insert_with(fresh),
            key => {
                self.references
                    .get(key)
                    .ok_or_else(|| {
                        CompileError::plain("macro returned an invalid/stale syntax context")
                    })?
                    .context
            }
        };
        Ok(origin)
    }
    fn stream(&mut self, end: Option<i16>) -> Result<Vec<TokenTree>, CompileError> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return Err(CompileError::plain("macro output nesting limit exceeded"));
        }
        let result = self.stream_inner(end);
        self.depth -= 1;
        result
    }
    fn stream_inner(&mut self, end: Option<i16>) -> Result<Vec<TokenTree>, CompileError> {
        let mut result = Vec::new();
        while let Some(t) = self.wire.get(self.at).cloned() {
            if Some(t.kind) == end {
                self.metadata(&t)?;
                if t.kind == 13 && !t.text.is_empty() {
                    return Err(CompileError::plain("invalid quotation splice marker"));
                }
                self.at += 1;
                return Ok(result);
            }
            if matches!(t.kind, 6 | 13) {
                return Err(CompileError::plain("unbalanced macro output group/splice"));
            }
            self.at += 1;
            *self.count += 1;
            if *self.count > MAX_TOKENS {
                return Err(CompileError::plain("macro generated token limit exceeded"));
            }
            *self.bytes += t.text.len();
            if *self.bytes > MAX_BYTES {
                return Err(CompileError::plain("macro generated byte limit exceeded"));
            }
            let span = self.metadata(&t)?;
            if t.kind == 20 {
                let file = &self.source.files[span.source.0 as usize];
                let mut error = CompileError::at_file(
                    &file.path,
                    &file.text,
                    span.range.start,
                    format!("macro diagnostic: {}", t.text),
                );
                let mut notes = 0;
                while self.wire.get(self.at).is_some_and(|n| n.kind == 21) {
                    notes += 1;
                    if notes > 16 {
                        return Err(CompileError::plain("macro diagnostic note limit exceeded"));
                    }
                    let note = &self.wire[self.at];
                    self.metadata(note)?;
                    error.message.push_str(&format!("note: {}\n", note.text));
                    self.at += 1;
                }
                return Err(error);
            }
            if t.kind == 10 || t.kind == 11 {
                let mut values = Vec::new();
                if t.kind == 11 {
                    while self.wire.get(self.at).is_some_and(|t| t.kind == 12) {
                        let marker = self.wire[self.at].clone();
                        self.metadata(&marker)?;
                        if !marker.text.is_empty() {
                            return Err(CompileError::plain("invalid quotation splice marker"));
                        }
                        self.at += 1;
                        values.push(self.stream(Some(13))?);
                    }
                }
                let quoted = trees(&t.text, span.source, span.context, span.range.start)?;
                fn fill(
                    ts: Vec<TokenTree>,
                    values: &[Vec<TokenTree>],
                    span: SyntaxSpan,
                    count: &mut usize,
                    bytes: &mut usize,
                ) -> Result<Vec<TokenTree>, CompileError> {
                    fn account(
                        ts: &[TokenTree],
                        count: &mut usize,
                        bytes: &mut usize,
                    ) -> Result<(), CompileError> {
                        for t in ts {
                            *count += 1;
                            *bytes += t.spelling.len();
                            if *count > MAX_TOKENS || *bytes > MAX_BYTES {
                                return Err(CompileError::plain(
                                    "macro generated token/byte limit exceeded",
                                ));
                            }
                            if let TreeKind::Group { children, .. } = &t.kind {
                                account(children, count, bytes)?;
                            }
                        }
                        Ok(())
                    }
                    let mut out = Vec::new();
                    let mut iter = ts.into_iter().peekable();
                    while let Some(mut t) = iter.next() {
                        if t.spelling == "$" {
                            let index = iter
                                .next()
                                .and_then(|t| t.spelling.parse::<usize>().ok())
                                .ok_or_else(|| {
                                    CompileError::plain("quotation splice requires $0, $1, ...")
                                })?;
                            let value = values
                                .get(index)
                                .ok_or_else(|| CompileError::plain("missing quotation splice"))?;
                            account(value, count, bytes)?;
                            out.extend(value.clone());
                            continue;
                        }
                        if let TreeKind::Group { children, .. } = &mut t.kind {
                            *children = fill(std::mem::take(children), values, span, count, bytes)?;
                        }
                        *count += 1;
                        *bytes += t.spelling.len();
                        if *count > MAX_TOKENS || *bytes > MAX_BYTES {
                            return Err(CompileError::plain(
                                "macro generated token/byte limit exceeded",
                            ));
                        }
                        t.span = span;
                        out.push(t);
                    }
                    Ok(out)
                }
                result.extend(fill(quoted, &values, span, self.count, self.bytes)?);
                continue;
            }
            if matches!(t.kind, 1 | 2 | 3 | 4 | 7 | 8) {
                let scanned = trees(&t.text, span.source, span.context, span.range.start)?;
                let valid = match t.kind {
                    1 => scanned.len() == 1 && matches!(scanned[0].kind, TreeKind::Ident),
                    2 => crate::lexer::tokenize_with_trivia(&t.text).is_ok_and(|tokens| {
                        use crate::lexer::TokenKind as K;
                        tokens.len() == 2
                            && tokens[0].span == Span::new(0, t.text.len())
                            && matches!(
                                tokens[0].kind,
                                K::Int(_)
                                    | K::Float(_)
                                    | K::Str(_)
                                    | K::CStr(_)
                                    | K::Char(_)
                                    | K::True
                                    | K::False
                            )
                    }),
                    3 | 8 => {
                        !scanned.is_empty()
                            && scanned
                                .iter()
                                .all(|t| matches!(t.kind, TreeKind::Punct { .. }))
                    }
                    4 => scanned.iter().all(|t| matches!(t.kind, TreeKind::Trivia)),
                    7 => scanned.iter().all(|t| matches!(t.kind, TreeKind::Text)),
                    _ => false,
                };
                if !valid {
                    return Err(CompileError::plain(
                        "macro returned an invalid structured token spelling",
                    ));
                }
            }
            let kind = match t.kind {
                1 => TreeKind::Ident,
                2 => TreeKind::Literal,
                3 | 8 => TreeKind::Punct { joint: t.kind == 8 },
                4 => TreeKind::Trivia,
                7 => TreeKind::Text,
                5 => {
                    if t.text.len() != 1 {
                        return Err(CompileError::plain("invalid macro output delimiter"));
                    }
                    let delimiter = t
                        .text
                        .chars()
                        .next()
                        .filter(|c| matches!(c, '(' | '[' | '{'))
                        .ok_or_else(|| CompileError::plain("invalid macro output delimiter"))?;
                    let children = self.stream(Some(6))?;
                    if self.wire[self.at - 1].text
                        != match delimiter {
                            '(' => ")",
                            '[' => "]",
                            _ => "}",
                        }
                    {
                        return Err(CompileError::plain("mismatched macro output delimiter"));
                    }
                    TreeKind::Group {
                        delimiter,
                        children,
                    }
                }
                _ => {
                    return Err(CompileError::plain(
                        "unknown macro protocol token operation",
                    ))
                }
            };
            result.push(TokenTree {
                kind,
                spelling: t.text,
                span,
            });
        }
        if end.is_some() {
            return Err(CompileError::plain("unclosed macro output group/splice"));
        }
        Ok(result)
    }
}

fn validate_entries(path: &Path, ast: &Module) -> Result<(), CompileError> {
    if super::executor::in_host() {
        return Ok(());
    }
    for entry in &ast.proc_macros {
        if ast.functions.iter().any(|f| f.name == *entry) {
            let _guard = Stack::enter(path)?;
            super::executor::validate(path, entry)?;
        }
    }
    Ok(())
}
