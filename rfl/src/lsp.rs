//! `rfl lsp` speaks Language Server Protocol on stdin/stdout.
//! Analysis uses the parser spans and the type checker.

use std::collections::{BTreeMap, HashMap};
use std::io::{self, BufRead, Write};
use std::path::PathBuf;

use serde_json::{json, Value};

mod transport;

use crate::ide::{Analysis, CompletionKind, SymbolKind};
use crate::lexer::{self, TokenKind};

struct Doc {
    source: String,
    version: Option<i64>,
    path: Option<PathBuf>,
    lines: Vec<usize>,
    analysis: Analysis,
    dependency_stamp: u64,
}

pub fn serve() {
    transport::serve();
}

fn serve_semantics(messages: impl IntoIterator<Item = Value>) {
    let mut docs: HashMap<String, Doc> = HashMap::new();
    let mut versioned_edits = false;
    let mut workspace_roots = Vec::new();
    for message in messages {
        let method = message
            .get("method")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        let id = message.get("id").cloned();
        let params = message.get("params").cloned().unwrap_or(Value::Null);
        if method == "initialize" {
            versioned_edits = params["capabilities"]["workspace"]["workspaceEdit"]
                ["documentChanges"]
                .as_bool()
                .unwrap_or(false);
            if let Some(folders) = params["workspaceFolders"].as_array() {
                workspace_roots.extend(
                    folders
                        .iter()
                        .filter_map(|folder| uri_path(folder["uri"].as_str()?)),
                );
            }
            if workspace_roots.is_empty() {
                if let Some(root) = params["rootUri"].as_str().and_then(uri_path) {
                    workspace_roots.push(root);
                }
            }
        }
        if method == "exit" {
            break;
        }
        if method == "shutdown" {
            emit(&json!({ "jsonrpc": "2.0", "id": id, "result": Value::Null }));
            continue;
        }
        if id.is_none() {
            notify(&mut docs, &method, &params);
            continue;
        }
        refresh_dependencies(&mut docs);
        let _overlay = crate::macros::source::overlay(open_sources(&docs));
        let result = if method == "textDocument/rename" {
            match rename(&docs, &params, &workspace_roots, versioned_edits) {
                Ok(result) => result,
                Err(message) => {
                    emit(&json!({ "jsonrpc": "2.0", "id": id,
                        "error": { "code": -32803, "message": message } }));
                    continue;
                }
            }
        } else {
            request(
                &mut docs,
                &method,
                &params,
                versioned_edits,
                &workspace_roots,
            )
        };
        emit(&json!({ "jsonrpc": "2.0", "id": id, "result": result }));
    }
}

fn notify(docs: &mut HashMap<String, Doc>, method: &str, params: &Value) {
    match method {
        "initialized" => {}
        "textDocument/didOpen" => {
            let uri = text_of(&params["textDocument"]["uri"]);
            let text = text_of(&params["textDocument"]["text"]);
            open_doc(docs, uri, text, params["textDocument"]["version"].as_i64());
        }
        "textDocument/didChange" => {
            let uri = text_of(&params["textDocument"]["uri"]);
            let Some(changes) = params["contentChanges"].as_array() else {
                return;
            };
            let Some(text) = changes.first().and_then(|change| change["text"].as_str()) else {
                return;
            };
            open_doc(
                docs,
                uri,
                text.to_string(),
                params["textDocument"]["version"].as_i64(),
            );
        }
        "textDocument/didClose" => {
            docs.remove(&text_of(&params["textDocument"]["uri"]));
            refresh_dependencies(docs);
        }
        _ => {}
    }
}

fn open_doc(docs: &mut HashMap<String, Doc>, uri: String, text: String, version: Option<i64>) {
    let path = uri_path(&uri);
    let mut sources = open_sources(docs);
    if let Some(path) = &path {
        sources.insert(crate::rename::canonical(path), text.clone());
    }
    let _overlay = crate::macros::source::overlay(sources);
    let analysis = Analysis::open(path.as_deref(), &text);
    let dependency_stamp = crate::macros::editor::fingerprint(path.as_deref(), &text);
    publish(&uri, &analysis, &text, version);
    docs.insert(
        uri,
        Doc {
            version,
            lines: line_starts(&text),
            source: text,
            path,
            analysis,
            dependency_stamp,
        },
    );
    refresh_dependencies(docs);
}

fn open_sources(docs: &HashMap<String, Doc>) -> BTreeMap<PathBuf, String> {
    docs.values()
        .filter_map(|d| {
            Some((
                crate::rename::canonical(d.path.as_deref()?),
                d.source.clone(),
            ))
        })
        .collect()
}
fn refresh_dependencies(docs: &mut HashMap<String, Doc>) {
    let _overlay = crate::macros::source::overlay(open_sources(docs));
    for (uri, doc) in docs {
        let stamp = crate::macros::editor::fingerprint(doc.path.as_deref(), &doc.source);
        if stamp != doc.dependency_stamp {
            doc.analysis = Analysis::open(doc.path.as_deref(), &doc.source);
            doc.dependency_stamp = stamp;
            publish(uri, &doc.analysis, &doc.source, doc.version);
        }
    }
}

fn request(
    docs: &mut HashMap<String, Doc>,
    method: &str,
    params: &Value,
    versioned_edits: bool,
    roots: &[PathBuf],
) -> Value {
    match method {
        "initialize" => initialize(),
        "textDocument/hover" => hover(docs, params),
        "textDocument/completion" => completion(docs, params),
        "textDocument/definition" => definition(docs, params),
        "textDocument/references" => references(docs, params),
        "textDocument/prepareRename" => prepare_rename(docs, params, roots),
        "textDocument/documentSymbol" => symbols(docs, params),
        "textDocument/signatureHelp" => signature(docs, params),
        "textDocument/inlayHint" => inlays(docs, params),
        "textDocument/semanticTokens/full" => semantic_tokens(docs, params),
        "textDocument/codeAction" => code_actions(docs, params, versioned_edits),
        "textDocument/formatting" => formatting(docs, params, false),
        "textDocument/rangeFormatting" => formatting(docs, params, true),
        _ => Value::Null,
    }
}

fn initialize() -> Value {
    json!({
        "capabilities": {
            "textDocumentSync": 1,
            "hoverProvider": true,
            "completionProvider": { "triggerCharacters": [".", ":"] },
            "definitionProvider": true,
            "referencesProvider": true,
            "renameProvider": { "prepareProvider": true },
            "codeActionProvider": { "codeActionKinds": ["quickfix"], "resolveProvider": false },
            "documentSymbolProvider": true,
            "documentFormattingProvider": true,
            "documentRangeFormattingProvider": true,
            "signatureHelpProvider": { "triggerCharacters": ["(", ","] },
            "inlayHintProvider": true,
            "semanticTokensProvider": {
                "legend": {
                    "tokenTypes": ["keyword", "function", "type", "parameter", "variable", "string", "number", "operator", "enumMember", "comment"],
                    "tokenModifiers": []
                },
                "full": true
            }
        },
        "serverInfo": { "name": "rfl", "version": "0.1.0" }
    })
}

fn formatting(docs: &HashMap<String, Doc>, params: &Value, selection: bool) -> Value {
    let Some(doc) = document(docs, params) else {
        return json!([]);
    };
    formatting_source(&doc.source, &doc.lines, params, selection)
}

fn formatting_source(source: &str, lines: &[usize], params: &Value, selection: bool) -> Value {
    let bounds = if selection {
        let Some(start) = offset_in(source, lines, &params["range"]["start"]) else {
            return json!([]);
        };
        let Some(end) = offset_in(source, lines, &params["range"]["end"]) else {
            return json!([]);
        };
        if start > end {
            return json!([]);
        }
        start..end
    } else {
        0..source.len()
    };
    let Ok(edits) = crate::format::format_edits(source) else {
        // Incomplete syntax is common while editing. Never rewrite a damaged file.
        return json!([]);
    };
    json!(edits
        .into_iter()
        .filter(|edit| edit.start >= bounds.start
            && edit.end <= bounds.end
            && (!selection || edit.start < bounds.end))
        .map(|edit| json!({ "range": range_of(lines, source, edit.start, edit.end), "newText": edit.new_text }))
        .collect::<Vec<_>>())
}

fn code_actions(docs: &HashMap<String, Doc>, params: &Value, versioned_edits: bool) -> Value {
    if let Some(only) = params["context"]["only"].as_array() {
        if !only
            .iter()
            .any(|kind| matches!(kind.as_str(), Some("" | "quickfix")))
        {
            return json!([]);
        }
    }
    let Some(doc) = document(docs, params) else {
        return json!([]);
    };
    let Some(start) = offset_of(doc, &params["range"]["start"]) else {
        return json!([]);
    };
    let Some(end) = offset_of(doc, &params["range"]["end"]) else {
        return json!([]);
    };
    if end < start {
        return json!([]);
    }
    let fixes = doc.analysis.import_fixes(start, end);
    let uri = text_of(&params["textDocument"]["uri"]);
    let actions: Vec<Value> = fixes.iter().map(|fix| {
        let edits = json!([{ "range": range(doc, fix.edit_span.start, fix.edit_span.end), "newText": fix.new_text }]);
        let edit = if versioned_edits {
            json!({ "documentChanges": [{ "textDocument": { "uri": uri, "version": doc.version }, "edits": edits }] })
        } else {
            json!({ "changes": { uri.clone(): edits } })
        };
        let preferred = fixes.iter().filter(|other| other.name == fix.name).count() == 1;
        json!({
            "title": format!("Import `{}` from `{}`", fix.name, fix.module),
            "kind": "quickfix",
            "isPreferred": preferred,
            "diagnostics": [{
                "range": range(doc, fix.symbol_span.start, fix.symbol_span.end),
                "severity": 1, "source": "rfl", "message": fix.diagnostic,
            }],
            "edit": edit,
        })
    }).collect();
    json!(actions)
}

fn hover(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return Value::Null;
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return Value::Null;
    };
    let Some(text) = doc.analysis.hover_markdown(offset) else {
        return Value::Null;
    };
    json!({ "contents": { "kind": "markdown", "value": text } })
}

fn completion(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return json!([]);
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return json!([]);
    };
    let items: Vec<Value> = doc.analysis.completions(offset).into_iter().map(|item| {
        let mut completion = json!({
            "label": item.label,
            "kind": match item.kind {
                CompletionKind::Keyword => 14,
                CompletionKind::Function => 3,
                CompletionKind::Method => 2,
                CompletionKind::Variable => 6,
                CompletionKind::Type => 22,
                    CompletionKind::Trait => 8,
                    CompletionKind::AssociatedType => 25,
                CompletionKind::Constant => 21,
                CompletionKind::Field => 5,
                CompletionKind::EnumMember => 20,
            },
            "detail": item.detail,
            "filterText": item.label,
            "textEdit": { "range": range(doc, item.replace.start, item.replace.end), "newText": item.label },
        });
        if let Some((span, text)) = item.import_edit {
            completion["additionalTextEdits"] = json!([{ "range": range(doc, span.start, span.end), "newText": text }]);
        }
        completion
    }).collect();
    json!(items)
}

fn definition(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return Value::Null;
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return Value::Null;
    };
    let Some((path, span)) = doc.analysis.definition(offset) else {
        return Value::Null;
    };
    json!(place(
        doc,
        &text_of(&params["textDocument"]["uri"]),
        path.as_deref(),
        span.start,
        span.end
    ))
}

fn references(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return json!([]);
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return json!([]);
    };
    let uri = text_of(&params["textDocument"]["uri"]);
    let locs: Vec<Value> = doc
        .analysis
        .references(offset)
        .into_iter()
        .map(|loc| place(doc, &uri, loc.path.as_deref(), loc.span.start, loc.span.end))
        .collect();
    json!(locs)
}

fn doc_path(uri: &str, doc: &Doc) -> PathBuf {
    crate::rename::canonical(
        doc.path
            .as_deref()
            .unwrap_or_else(|| std::path::Path::new(uri)),
    )
}

fn rename_workspace(docs: &HashMap<String, Doc>, roots: &[PathBuf]) -> crate::rename::Workspace {
    let sources = docs
        .iter()
        .map(|(uri, doc)| (doc_path(uri, doc), doc.source.clone()))
        .collect();
    crate::rename::Workspace::load(roots, sources)
}

fn rename_roots(path: &std::path::Path, roots: &[PathBuf]) -> Vec<PathBuf> {
    if roots.is_empty() {
        path.parent()
            .map(|p| vec![p.to_path_buf()])
            .unwrap_or_default()
    } else {
        roots.to_vec()
    }
}

fn prepare_rename(docs: &HashMap<String, Doc>, params: &Value, roots: &[PathBuf]) -> Value {
    let Some(doc) = document(docs, params) else {
        return Value::Null;
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return Value::Null;
    };
    let path = doc_path(&text_of(&params["textDocument"]["uri"]), doc);
    let workspace = rename_workspace(docs, &rename_roots(&path, roots));
    workspace.prepare(&path, offset).map_or(
        Value::Null,
        |(span, name)| json!({ "range": range(doc, span.start, span.end), "placeholder": name }),
    )
}

fn rename(
    docs: &HashMap<String, Doc>,
    params: &Value,
    roots: &[PathBuf],
    versioned_edits: bool,
) -> Result<Value, String> {
    let doc = document(docs, params).ok_or("The document is not open")?;
    let offset = offset_of(doc, &params["position"]).ok_or("Invalid rename position")?;
    let name = params["newName"].as_str().ok_or("Expected a new name")?;
    let path = doc_path(&text_of(&params["textDocument"]["uri"]), doc);
    let roots = rename_roots(&path, roots);
    let workspace = rename_workspace(docs, &roots);
    let edits = workspace.rename(&path, offset, name)?;
    let mut changes: BTreeMap<String, Vec<Value>> = BTreeMap::new();
    let mut versions = HashMap::new();
    for edit in edits {
        let open = docs
            .iter()
            .find(|(uri, doc)| doc_path(uri, doc) == edit.path);
        let uri = open
            .map(|(uri, _)| uri.clone())
            .unwrap_or_else(|| path_uri(&edit.path));
        versions.insert(uri.clone(), open.and_then(|(_, doc)| doc.version));
        let source = workspace
            .source(&edit.path)
            .ok_or("Cannot read rename target")?;
        let range = range_of(&line_starts(source), source, edit.span.start, edit.span.end);
        changes
            .entry(uri)
            .or_default()
            .push(json!({ "range": range, "newText": edit.text }));
    }
    if versioned_edits {
        let changes: Vec<_> = changes.into_iter().map(|(uri, edits)| {
            json!({ "textDocument": { "uri": uri, "version": versions[&uri] }, "edits": edits })
        }).collect();
        Ok(json!({ "documentChanges": changes }))
    } else {
        Ok(json!({ "changes": changes }))
    }
}

fn symbols(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return json!([]);
    };
    let items: Vec<Value> = doc
        .analysis
        .symbols
        .iter()
        .map(|symbol| {
            json!({
                "name": symbol.name,
                "kind": match symbol.kind {
                    SymbolKind::Function => 12,
                    SymbolKind::Method => 6,
                    SymbolKind::Struct => 23,
                    SymbolKind::Trait => 11,
                    SymbolKind::AssociatedType => 26,
                    SymbolKind::Enum => 10,
                    SymbolKind::Const => 14,
                    SymbolKind::Field => 8,
                },
                "detail": symbol.detail,
                "range": range(doc, symbol.span.start, symbol.span.end),
                "selectionRange": range(doc, symbol.span.start, symbol.span.end),
            })
        })
        .collect();
    json!(items)
}

fn signature(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return Value::Null;
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return Value::Null;
    };
    let Some((label, active)) = doc.analysis.signature(offset) else {
        return Value::Null;
    };
    let parameters: Vec<Value> = signature_parameters(&label)
        .into_iter()
        .map(|part| json!({ "label": part }))
        .collect();
    json!({ "signatures": [{ "label": label, "parameters": parameters }], "activeSignature": 0, "activeParameter": active })
}

fn signature_parameters(label: &str) -> Vec<String> {
    let Some(inside) = label.strip_prefix("fn(") else {
        return Vec::new();
    };
    let mut depth = 0;
    let close = inside.char_indices().find_map(|(at, ch)| match ch {
        '(' => {
            depth += 1;
            None
        }
        ')' if depth == 0 => Some(at),
        ')' => {
            depth -= 1;
            None
        }
        _ => None,
    });
    let Some(close) = close else {
        return Vec::new();
    };
    split_params(&inside[..close])
        .into_iter()
        .map(str::to_string)
        .collect()
}

fn split_params(text: &str) -> Vec<&str> {
    let mut parts = Vec::new();
    let mut depth = 0i32;
    let mut start = 0usize;
    for (index, ch) in text.char_indices() {
        match ch {
            '(' | '<' | '[' => depth += 1,
            ')' | '>' | ']' => depth -= 1,
            ',' if depth == 0 => {
                parts.push(text[start..index].trim());
                start = index + 1;
            }
            _ => {}
        }
    }
    let tail = text[start..].trim();
    if !tail.is_empty() {
        parts.push(tail);
    }
    parts
}

fn inlays(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return json!([]);
    };
    let hints: Vec<Value> = doc.analysis.inlay_hints().into_iter().map(|(offset, label)| {
        json!({ "position": position(doc, offset), "label": label, "paddingLeft": false })
    }).collect();
    json!(hints)
}

fn semantic_tokens(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return json!({ "data": [] });
    };
    let Ok(tokens) = lexer::tokenize_with_trivia(&doc.source) else {
        return json!({ "data": [] });
    };
    let mut data = Vec::new();
    let mut prev_line = 0u32;
    let mut prev_col = 0u32;
    for token in tokens {
        let Some(kind) = token_kind(&doc.analysis, &token) else {
            continue;
        };
        let (line, col) = line_col(doc, token.span.start);
        let length = utf16_len(&doc.source[token.span.start..token.span.end.min(doc.source.len())]);
        if length == 0 {
            continue;
        }
        let delta_line = line - prev_line;
        let delta_col = if delta_line == 0 {
            col.saturating_sub(prev_col)
        } else {
            col
        };
        data.extend([delta_line, delta_col, length, kind, 0]);
        prev_line = line;
        prev_col = col;
    }
    json!({ "data": data })
}

fn token_kind(analysis: &Analysis, token: &crate::lexer::Token) -> Option<u32> {
    Some(match &token.kind {
        TokenKind::Fn
        | TokenKind::Async
        | TokenKind::Await
        | TokenKind::Let
        | TokenKind::Mut
        | TokenKind::If
        | TokenKind::Else
        | TokenKind::Return
        | TokenKind::As
        | TokenKind::While
        | TokenKind::Break
        | TokenKind::Continue
        | TokenKind::Match
        | TokenKind::For
        | TokenKind::In
        | TokenKind::Type
        | TokenKind::Trait
        | TokenKind::Impl
        | TokenKind::Const
        | TokenKind::Struct
        | TokenKind::Export
        | TokenKind::Import
        | TokenKind::Extern
        | TokenKind::Unsafe
        | TokenKind::True
        | TokenKind::False => 0,
        TokenKind::LineComment | TokenKind::BlockComment => 9,
        TokenKind::Ident(name) => {
            let from_types = analysis.token_type(token.span);
            if from_types != 4 {
                from_types
            } else if matches!(
                name.as_str(),
                "Self"
                    | "i8"
                    | "u8"
                    | "i16"
                    | "u16"
                    | "i32"
                    | "u32"
                    | "i64"
                    | "u64"
                    | "f32"
                    | "f64"
                    | "bool"
                    | "char"
                    | "String"
                    | "usize"
                    | "unit"
                    | "Vec"
                    | "Slice"
                    | "Owned"
                    | "Async"
                    | "ErasedOwner"
                    | "SharedOwner"
                    | "Option"
                    | "Result"
                    | "c_int"
                    | "c_char"
                    | "c_short"
                    | "c_uint"
                    | "c_long"
                    | "c_size_t"
            ) {
                2
            } else if matches!(name.as_str(), "Some" | "None" | "Ok" | "Err") {
                8
            } else {
                4
            }
        }
        TokenKind::Str(_) | TokenKind::CStr(_) | TokenKind::Char(_) => 5,
        TokenKind::Int(_) | TokenKind::Float(_) => 6,
        TokenKind::Plus
        | TokenKind::Minus
        | TokenKind::Star
        | TokenKind::Slash
        | TokenKind::Percent
        | TokenKind::Eq
        | TokenKind::EqEq
        | TokenKind::NotEq
        | TokenKind::Lt
        | TokenKind::Le
        | TokenKind::Gt
        | TokenKind::Ge
        | TokenKind::Amp
        | TokenKind::Pipe
        | TokenKind::OrOr
        | TokenKind::Not
        | TokenKind::Arrow
        | TokenKind::FatArrow
        | TokenKind::Shl
        | TokenKind::Shr
        | TokenKind::AndAnd
        | TokenKind::Caret
        | TokenKind::Tilde
        | TokenKind::PlusEq
        | TokenKind::MinusEq
        | TokenKind::StarEq
        | TokenKind::SlashEq
        | TokenKind::PercentEq => 7,
        _ => return None,
    })
}

fn publish(uri: &str, analysis: &Analysis, source: &str, version: Option<i64>) {
    let lines = line_starts(source);
    let tokens = lexer::tokenize_with_trivia(source).unwrap_or_default();
    let diagnostics: Vec<Value> = analysis
        .diagnostics
        .iter()
        .map(|err| {
            let offset = err.offset.unwrap_or(0);
            let (start, end) = tokens
                .iter()
                .find(|token| token.span.contains(offset))
                .map(|token| {if matches!(token.kind,TokenKind::MacroBody(_)){let end=source[offset..].char_indices().take_while(|(_,c)|c.is_ascii_alphanumeric()||*c=='_').last().map_or(offset+source[offset..].chars().next().map_or(0,char::len_utf8),|(n,c)|offset+n+c.len_utf8());(offset,end)}else{(token.span.start, token.span.end)}})
                .unwrap_or((offset, (offset + 1).min(source.len())));
            json!({
                "range": range_of(&lines, source, start, end.max(start)),
                "severity": 1,
                "source": "rfl",
                "message": format!("{}{}",err.detail,err.message.lines().filter(|s|s.starts_with("note:")).map(|s|format!("\n{s}")).collect::<String>()),
            })
        })
        .collect();
    send(
        &mut io::stdout().lock(),
        &json!({
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": { "uri": uri, "version": version, "diagnostics": diagnostics }
        }),
    );
}

fn document<'a>(docs: &'a HashMap<String, Doc>, params: &Value) -> Option<&'a Doc> {
    docs.get(&text_of(&params["textDocument"]["uri"]))
}

fn place(
    doc: &Doc,
    current_uri: &str,
    path: Option<&std::path::Path>,
    start: usize,
    end: usize,
) -> Value {
    let uri = path
        .map(path_uri)
        .unwrap_or_else(|| current_uri.to_string());
    json!({ "uri": uri, "range": place_range(doc, path, start, end) })
}

fn place_range(doc: &Doc, path: Option<&std::path::Path>, start: usize, end: usize) -> Value {
    if path.is_none() || path == doc.path.as_deref() {
        return range(doc, start, end);
    }
    let text = path
        .and_then(|path| crate::macros::source::read(path).ok())
        .unwrap_or_default();
    let lines = line_starts(&text);
    range_of(&lines, &text, start, end)
}

fn range(doc: &Doc, start: usize, end: usize) -> Value {
    range_of(&doc.lines, &doc.source, start, end)
}

fn range_of(lines: &[usize], source: &str, start: usize, end: usize) -> Value {
    json!({ "start": position_at(lines, source, start), "end": position_at(lines, source, end) })
}

fn position(doc: &Doc, offset: usize) -> Value {
    position_at(&doc.lines, &doc.source, offset)
}

fn position_at(lines: &[usize], source: &str, offset: usize) -> Value {
    let (line, character) = line_col_at(lines, source, offset);
    json!({ "line": line, "character": character })
}

fn offset_of(doc: &Doc, position: &Value) -> Option<usize> {
    offset_in(&doc.source, &doc.lines, position)
}

fn offset_in(source: &str, lines: &[usize], position: &Value) -> Option<usize> {
    let line = position["line"].as_u64()? as usize;
    let character = position["character"].as_u64()? as usize;
    let start = *lines.get(line)?;
    let end = lines.get(line + 1).copied().unwrap_or(source.len());
    let slice = source.get(start..end)?;
    let mut utf16 = 0;
    for (index, ch) in slice.char_indices() {
        if utf16 >= character {
            return Some(start + index);
        }
        utf16 += ch.len_utf16();
    }
    Some(end)
}

fn line_col(doc: &Doc, offset: usize) -> (u32, u32) {
    line_col_at(&doc.lines, &doc.source, offset)
}

fn line_col_at(lines: &[usize], source: &str, offset: usize) -> (u32, u32) {
    let line = lines
        .partition_point(|start| *start <= offset)
        .saturating_sub(1);
    let start = lines.get(line).copied().unwrap_or(0);
    let slice = source.get(start..offset.min(source.len())).unwrap_or("");
    let character = slice.chars().map(|ch| ch.len_utf16()).sum::<usize>() as u32;
    (line as u32, character)
}

fn line_starts(source: &str) -> Vec<usize> {
    let mut starts = vec![0];
    for (index, byte) in source.bytes().enumerate() {
        if byte == b'\n' {
            starts.push(index + 1);
        }
    }
    starts
}

fn utf16_len(text: &str) -> u32 {
    text.chars().map(|ch| ch.len_utf16()).sum::<usize>() as u32
}

fn text_of(value: &Value) -> String {
    value.as_str().unwrap_or("").to_string()
}

fn uri_path(uri: &str) -> Option<PathBuf> {
    let rest = uri.strip_prefix("file://")?;
    let decoded = percent_decode(rest);
    Some(PathBuf::from(decoded))
}

fn path_uri(path: &std::path::Path) -> String {
    use std::fmt::Write as _;
    let mut uri = String::from("file://");
    for byte in path.to_string_lossy().bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'/' | b':' | b'-' | b'_' | b'.' | b'~') {
            uri.push(byte as char);
        } else {
            write!(uri, "%{byte:02X}").unwrap();
        }
    }
    uri
}

fn percent_decode(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = Vec::new();
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' && index + 2 < bytes.len() {
            if let Ok(byte) = u8::from_str_radix(&text[index + 1..index + 3], 16) {
                out.push(byte);
                index += 3;
                continue;
            }
        }
        out.push(bytes[index]);
        index += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn read_message(reader: &mut impl BufRead) -> Option<Value> {
    let mut length = 0usize;
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line).ok()? == 0 {
            return None;
        }
        let line = line.trim();
        if line.is_empty() {
            break;
        }
        if let Some(value) = line.strip_prefix("Content-Length:") {
            length = value.trim().parse().ok()?;
        }
    }
    let mut body = vec![0u8; length];
    reader.read_exact(&mut body).ok()?;
    serde_json::from_slice(&body).ok()
}

fn emit(message: &Value) {
    send(&mut io::stdout().lock(), message);
}

fn send(stdout: &mut impl Write, message: &Value) {
    let body = message.to_string();
    let _ = write!(stdout, "Content-Length: {}\r\n\r\n{body}", body.len());
    let _ = stdout.flush();
}

#[cfg(test)]
mod tests {
    use super::*;

    fn format_doc(source: &str) -> Doc {
        Doc {
            source: source.to_string(),
            version: Some(1),
            path: None,
            lines: line_starts(source),
            dependency_stamp: 0,
            analysis: Analysis::open(None, source),
        }
    }

    fn apply_formatting(doc: &Doc, edits: &Value) -> String {
        let mut text = doc.source.clone();
        for edit in edits.as_array().unwrap().iter().rev() {
            let start = offset_of(doc, &edit["range"]["start"]).unwrap();
            let end = offset_of(doc, &edit["range"]["end"]).unwrap();
            text.replace_range(start..end, edit["newText"].as_str().unwrap());
        }
        text
    }

    #[test]
    fn formatting_uses_unsaved_text_and_utf16_ranges() {
        assert_eq!(
            initialize()["capabilities"]["documentFormattingProvider"],
            true
        );
        assert_eq!(
            initialize()["capabilities"]["documentRangeFormattingProvider"],
            true
        );
        let uri = "file:///format.rfl";
        let source = "fn main(){/* 🙂 */let x=1;println(x);}";
        let docs = HashMap::from([(uri.to_string(), format_doc(source))]);
        let params =
            json!({"textDocument":{"uri":uri},"options":{"tabSize":8,"insertSpaces":false}});
        let edits = formatting(&docs, &params, false);
        assert_eq!(
            apply_formatting(&docs[uri], &edits),
            crate::format_source(source).unwrap()
        );
        assert!(!edits.as_array().unwrap().is_empty());
        // No server-side mutation: the client owns application of TextEdits.
        assert_eq!(docs[uri].source, source);
    }

    #[test]
    fn selection_formatting_never_changes_outside_the_selection() {
        let uri = "file:///selection.rfl";
        let source = "fn main(){let x=1;let y=2;}";
        let doc = format_doc(source);
        let start = source.find("let y").unwrap();
        let end = source.find("2;").unwrap() + 2;
        let params = json!({"textDocument":{"uri":uri},"range":range(&doc,start,end)});
        let docs = HashMap::from([(uri.to_string(), doc)]);
        let edits = formatting(&docs, &params, true);
        assert_eq!(
            apply_formatting(&docs[uri], &edits),
            "fn main(){let x=1;\n    let y = 2;}"
        );
        assert_eq!(
            formatting(
                &docs,
                &json!({"textDocument":{"uri":"file:///missing.rfl"}}),
                false
            ),
            json!([])
        );
    }

    #[test]
    fn formatting_noops_for_invalid_and_already_formatted_buffers() {
        let uri = "file:///format.rfl";
        for source in [
            "fn main() {\n    println(1);\n}\n",
            "fn main() { let x = ; }",
        ] {
            let docs = HashMap::from([(uri.to_string(), format_doc(source))]);
            assert_eq!(
                formatting(&docs, &json!({"textDocument":{"uri":uri}}), false),
                json!([])
            );
        }
    }

    fn doc(source: &str) -> Doc {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("examples/auto-import-lsp.rfl");
        Doc {
            source: source.to_string(),
            version: Some(7),
            path: Some(path.clone()),
            lines: line_starts(source),
            dependency_stamp: 0,
            analysis: Analysis::open(Some(&path), source),
        }
    }

    #[test]
    fn method_completion_returns_import_edit_and_preserves_existing_call() {
        let source = "// 🦀\r\nimport { replace } from \"std/string\"; // keep\r\nfn main() { let a = \"something\"; let b = a.replace(\"some\", \"oth\"); println(b); b.tr(); }\r\n";
        let document = doc(source);
        let at = source.find("b.tr").unwrap() + 4;
        let params = json!({ "textDocument": { "uri": "file:///source.rfl" }, "position": position(&document, at) });
        let docs = HashMap::from([("file:///source.rfl".to_string(), document)]);
        let response = completion(&docs, &params);
        let items = response.as_array().unwrap();
        let item = items.iter().find(|item| item["label"] == "trim").unwrap();
        assert_eq!(item["kind"], 2);
        let mut edits = vec![item["textEdit"].clone()];
        edits.extend(
            item["additionalTextEdits"]
                .as_array()
                .unwrap()
                .iter()
                .cloned(),
        );
        let document = &docs["file:///source.rfl"];
        let mut edits: Vec<_> = edits
            .iter()
            .map(|edit| {
                (
                    offset_of(document, &edit["range"]["start"]).unwrap(),
                    offset_of(document, &edit["range"]["end"]).unwrap(),
                    edit["newText"].as_str().unwrap(),
                )
            })
            .collect();
        edits.sort_by_key(|(start, _, _)| std::cmp::Reverse(*start));
        let mut edited = source.to_string();
        for (start, end, text) in edits {
            edited.replace_range(start..end, text);
        }
        assert!(edited.contains("{ trim, replace }"));
        assert!(edited.contains("b.trim()"));
        let after = Analysis::open(document.path.as_deref(), &edited);
        assert!(
            after.diagnostics.is_empty(),
            "{edited}\n{:?}",
            after.diagnostics
        );
        let imported = items
            .iter()
            .find(|item| item["label"] == "replace")
            .unwrap();
        assert!(imported.get("additionalTextEdits").is_none());
        assert!(!items
            .iter()
            .any(|item| item["label"] == "region_eq" || item["label"] == "push"));
    }

    #[test]
    fn import_action_uses_utf16_positions_and_versioned_edits() {
        let source = "// header 🦀\r\nimport { cos as cosine } from \"std/math\";\r\nfn value() -> f64 { sin(0.0) }\r\n";
        let document = doc(source);
        let at = source.find("sin(").unwrap();
        let params = json!({
            "textDocument": { "uri": "file:///tmp/source%20file.rfl" },
            "range": range(&document, at, at), "context": { "diagnostics": [] },
        });
        let docs = HashMap::from([("file:///tmp/source%20file.rfl".to_string(), document)]);
        let result = code_actions(&docs, &params, true);
        let actions = result.as_array().unwrap();
        assert_eq!(actions.len(), 1);
        assert_eq!(actions[0]["title"], "Import `sin` from `std/math`");
        assert_eq!(actions[0]["isPreferred"], true);
        assert_eq!(actions[0]["kind"], "quickfix");
        let change = &actions[0]["edit"]["documentChanges"][0];
        assert_eq!(change["textDocument"]["version"], 7);
        assert_eq!(
            change["textDocument"]["uri"],
            "file:///tmp/source%20file.rfl"
        );
        let edit = &change["edits"][0];
        let document = &docs["file:///tmp/source%20file.rfl"];
        let start = offset_of(document, &edit["range"]["start"]).unwrap();
        let end = offset_of(document, &edit["range"]["end"]).unwrap();
        let mut edited = source.to_string();
        edited.replace_range(start..end, edit["newText"].as_str().unwrap());
        assert!(edited.contains("{ sin, cos as cosine }"));
        assert!(Analysis::open(document.path.as_deref(), &edited)
            .diagnostics
            .is_empty());
        // Clients without documentChanges support receive plain WorkspaceEdit.changes.
        let legacy = code_actions(&docs, &params, false);
        assert_eq!(
            legacy[0]["edit"]["changes"]["file:///tmp/source%20file.rfl"][0],
            *edit
        );
        let mut filtered = params.clone();
        filtered["context"]["only"] = json!(["source.organizeImports"]);
        assert_eq!(code_actions(&docs, &filtered, true), json!([]));
    }

    #[test]
    fn ambiguous_import_actions_are_not_preferred() {
        let source = "fn value() -> bool { is_ascii('a') }";
        let document = doc(source);
        let at = source.find("is_ascii").unwrap();
        let params = json!({ "textDocument": { "uri": "file:///source.rfl" },
            "range": range(&document, at, at + "is_ascii".len()),
            "context": { "only": ["quickfix"], "diagnostics": [] } });
        let docs = HashMap::from([("file:///source.rfl".to_string(), document)]);
        let actions = code_actions(&docs, &params, true);
        assert_eq!(actions.as_array().unwrap().len(), 2);
        for action in actions.as_array().unwrap() {
            assert_eq!(action["isPreferred"], false);
        }
        assert!(
            initialize()["capabilities"]["codeActionProvider"]["codeActionKinds"]
                .as_array()
                .unwrap()
                .contains(&json!("quickfix"))
        );
    }
}

#[cfg(test)]
mod rename_tests {
    use super::*;

    fn document(path: &std::path::Path, text: &str, version: i64) -> Doc {
        Doc {
            source: text.into(),
            version: Some(version),
            path: Some(path.to_path_buf()),
            lines: line_starts(text),
            dependency_stamp: 0,
            analysis: Analysis::open(Some(path), text),
        }
    }

    #[test]
    fn rename_prepares_utf16_and_versions_all_open_buffers() {
        let root = std::env::temp_dir().join(format!("rfl-rename-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let lib = root.join("library #é.rfl");
        let main = root.join("main.rfl");
        let closed = root.join("closed.rfl");
        let original = "export fn wave(x: i16) -> i16 { x }\n";
        let unsaved = "// 🦀 unsaved\r\nexport fn wave(x: i16) -> i16 { x + 1 }\r\n";
        let source="import { wave } from \"./library #é\";\r\nfn main() { /* 🦀 */ println(wave(2)); }\r\n";
        let aliased =
            "import { wave as play } from \"./library #é\"; fn other() -> i16 { play(3) }\n";
        std::fs::write(&lib, original).unwrap();
        std::fs::write(&closed, aliased).unwrap();
        let uri = path_uri(&main);
        let lib_uri = path_uri(&lib);
        let docs = HashMap::from([
            (uri.clone(), document(&main, source, 8)),
            (lib_uri.clone(), document(&lib, unsaved, 12)),
        ]);
        let at = source.rfind("wave").unwrap();
        let params = json!({"textDocument":{"uri":uri},"position":position(&docs[&uri],at+4),"newName":"signal"});
        let prepared = prepare_rename(&docs, &params, &[root.clone()]);
        assert_eq!(prepared["placeholder"], "wave");
        assert_eq!(prepared["range"], range(&docs[&uri], at, at + 4));
        let result = rename(&docs, &params, &[root.clone()], true).unwrap();
        let changes = result["documentChanges"].as_array().unwrap();
        assert_eq!(changes.len(), 3);
        for change in changes {
            let target = change["textDocument"]["uri"].as_str().unwrap();
            let text = if target == uri {
                source
            } else if target == lib_uri {
                unsaved
            } else {
                aliased
            };
            let version = if target == uri {
                json!(8)
            } else if target == lib_uri {
                json!(12)
            } else {
                Value::Null
            };
            assert_eq!(change["textDocument"]["version"], version);
            let doc = document(&uri_path(target).unwrap(), text, 0);
            let mut edited = text.to_string();
            for edit in change["edits"].as_array().unwrap().iter().rev() {
                let start = offset_of(&doc, &edit["range"]["start"]).unwrap();
                let end = offset_of(&doc, &edit["range"]["end"]).unwrap();
                assert_eq!(&text[start..end], "wave");
                edited.replace_range(start..end, edit["newText"].as_str().unwrap());
            }
            assert_eq!(edited, text.replace("wave", "signal"));
        }
        assert_eq!(
            std::fs::read_to_string(&lib).unwrap(),
            original,
            "server only returns edits"
        );
        let legacy = rename(&docs, &params, &[root.clone()], false).unwrap();
        assert_eq!(legacy["changes"].as_object().unwrap().len(), 3);
        // prepareRename must use the same workspace as rename, so a member
        // declared in an unopened project module is offered by IDEA as well.
        std::fs::write(root.join("shape.rfl"), "export struct Point { value: i16 }").unwrap();
        let source = "import { Point } from \"./shape\"; fn main() { let p = Point { value: 2 }; println(p.value); }";
        let member_docs = HashMap::from([(uri.clone(), document(&main, source, 9))]);
        let member = json!({ "textDocument": { "uri": uri }, "position": position(&member_docs[&uri], source.rfind("value").unwrap()), "newName": "count" });
        assert_eq!(
            prepare_rename(&member_docs, &member, &[root.clone()])["placeholder"],
            "value"
        );
        assert_eq!(
            rename(&member_docs, &member, &[root.clone()], true).unwrap()["documentChanges"]
                .as_array()
                .unwrap()
                .len(),
            2
        );
        std::fs::remove_dir_all(root).unwrap();
    }
}

#[cfg(test)]
mod resources {
    use super::*;
    #[test]
    fn cleanup_signature_keeps_nested_callable_parameters() {
        assert_eq!(
            signature_parameters("fn(i16, fn(&mut i16) -> unit) -> Owned<i16>"),
            vec!["i16", "fn(&mut i16) -> unit"]
        );
        let source = "fn use_owner(owner:Owned<i16>) {}";
        let analysis = Analysis::open(None, source);
        let tokens = lexer::tokenize(source).unwrap();
        let owner = tokens
            .iter()
            .find(|t| matches!(&t.kind,TokenKind::Ident(n) if n=="Owned"))
            .unwrap();
        assert_eq!(token_kind(&analysis, owner), Some(2));
    }
}

#[cfg(test)]
mod async_tests {
    use super::*;
    #[test]
    fn async_hover_signatures_tokens_and_completion_hide_generated_helpers() {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("examples/async-editor.rfl");
        let source = r#"
            import { Future, yield_now } from "std/future";
            async fn make(n:i16)->i16 {yield_now().await;n+2}
            async fn work()->i16 {let value=make(40).await;value}
            fn main() {}
        "#;
        let analysis = Analysis::open(Some(&path), source);
        assert!(
            analysis.diagnostics.is_empty(),
            "{:?}",
            analysis.diagnostics
        );
        let call = source.find("make(40)").unwrap();
        assert_eq!(
            analysis.signature(call + 6).unwrap().0,
            "fn(i16) -> Async<i16>"
        );
        let await_at = source.find("make(40).await").unwrap() + 9;
        assert_eq!(analysis.hover(await_at).unwrap(), "i16");
        let (_, span) = analysis.definition(call).unwrap();
        assert_eq!(&source[span.start..span.end], "make");
        assert!(!analysis
            .completions(source.rfind("value}").unwrap())
            .iter()
            .any(|item| item.label.contains('$')));
        for token in lexer::tokenize(source)
            .unwrap()
            .iter()
            .filter(|token| matches!(token.kind, TokenKind::Async | TokenKind::Await))
        {
            assert_eq!(token_kind(&analysis, token), Some(0));
        }
        let incomplete = source.replace("make(40).await", "make(40).aw");
        let analysis = Analysis::open(Some(&path), &incomplete);
        let at = incomplete.find("make(40).aw").unwrap() + 11;
        assert!(analysis
            .completions(at)
            .iter()
            .any(|item| item.label == "await"));
    }
}
