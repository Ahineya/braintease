//! `rfl lsp` speaks Language Server Protocol on stdin/stdout.
//! Analysis uses the parser spans and the type checker.

use std::collections::HashMap;
use std::io::{self, BufRead, Write};
use std::path::PathBuf;

use serde_json::{json, Value};

use crate::ide::{Analysis, CompletionKind, SymbolKind};
use crate::lexer::{self, TokenKind};

struct Doc {
    source: String,
    path: Option<PathBuf>,
    lines: Vec<usize>,
    analysis: Analysis,
}

pub fn serve() {
    let mut docs: HashMap<String, Doc> = HashMap::new();
    let stdin = io::stdin();
    let mut reader = stdin.lock();
    let mut stdout = io::stdout().lock();
    loop {
        let Some(message) = read_message(&mut reader) else {
            break;
        };
        let method = message
            .get("method")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        let id = message.get("id").cloned();
        let params = message.get("params").cloned().unwrap_or(Value::Null);
        if method == "exit" {
            break;
        }
        if method == "shutdown" {
            send(
                &mut stdout,
                &json!({ "jsonrpc": "2.0", "id": id, "result": Value::Null }),
            );
            continue;
        }
        if id.is_none() {
            notify(&mut docs, &method, &params);
            continue;
        }
        let result = request(&mut docs, &method, &params);
        send(
            &mut stdout,
            &json!({ "jsonrpc": "2.0", "id": id, "result": result }),
        );
    }
}

fn notify(docs: &mut HashMap<String, Doc>, method: &str, params: &Value) {
    match method {
        "initialized" => {}
        "textDocument/didOpen" => {
            let uri = text_of(&params["textDocument"]["uri"]);
            let text = text_of(&params["textDocument"]["text"]);
            open_doc(docs, uri, text);
        }
        "textDocument/didChange" => {
            let uri = text_of(&params["textDocument"]["uri"]);
            let Some(changes) = params["contentChanges"].as_array() else {
                return;
            };
            let Some(text) = changes.first().and_then(|change| change["text"].as_str()) else {
                return;
            };
            open_doc(docs, uri, text.to_string());
        }
        "textDocument/didClose" => {
            docs.remove(&text_of(&params["textDocument"]["uri"]));
        }
        _ => {}
    }
}

fn open_doc(docs: &mut HashMap<String, Doc>, uri: String, text: String) {
    let path = uri_path(&uri);
    let analysis = Analysis::open(path.as_deref(), &text);
    publish(&uri, &analysis, &text);
    docs.insert(
        uri,
        Doc {
            lines: line_starts(&text),
            source: text,
            path,
            analysis,
        },
    );
}

fn request(docs: &mut HashMap<String, Doc>, method: &str, params: &Value) -> Value {
    match method {
        "initialize" => initialize(),
        "textDocument/hover" => hover(docs, params),
        "textDocument/completion" => completion(docs, params),
        "textDocument/definition" => definition(docs, params),
        "textDocument/references" => references(docs, params),
        "textDocument/rename" => rename(docs, params),
        "textDocument/documentSymbol" => symbols(docs, params),
        "textDocument/signatureHelp" => signature(docs, params),
        "textDocument/inlayHint" => inlays(docs, params),
        "textDocument/semanticTokens/full" => semantic_tokens(docs, params),
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
            "renameProvider": true,
            "documentSymbolProvider": true,
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

fn hover(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return Value::Null;
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return Value::Null;
    };
    let Some(text) = doc.analysis.hover(offset) else {
        return Value::Null;
    };
    json!({ "contents": { "kind": "markdown", "value": format!("```rfl\n{text}\n```") } })
}

fn completion(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return json!([]);
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return json!([]);
    };
    let items: Vec<Value> = doc.analysis.completions(offset).into_iter().map(|item| {
        json!({
            "label": item.label,
            "kind": match item.kind {
                CompletionKind::Keyword => 14,
                CompletionKind::Function => 3,
                CompletionKind::Method => 2,
                CompletionKind::Variable => 6,
                CompletionKind::Type => 22,
                CompletionKind::Constant => 21,
                CompletionKind::Field => 5,
                CompletionKind::EnumMember => 20,
            },
            "detail": item.detail,
            "filterText": item.label,
            "textEdit": { "range": range(doc, item.replace.start, item.replace.end), "newText": item.label },
        })
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

fn rename(docs: &HashMap<String, Doc>, params: &Value) -> Value {
    let Some(doc) = document(docs, params) else {
        return Value::Null;
    };
    let Some(offset) = offset_of(doc, &params["position"]) else {
        return Value::Null;
    };
    let new_name = text_of(&params["newName"]);
    let uri = text_of(&params["textDocument"]["uri"]);
    let mut changes: HashMap<String, Vec<Value>> = HashMap::new();
    for loc in doc.analysis.references(offset) {
        let target = loc
            .path
            .as_ref()
            .map(|path| path_uri(path))
            .unwrap_or_else(|| uri.clone());
        let edit = json!({ "range": place_range(doc, loc.path.as_deref(), loc.span.start, loc.span.end), "newText": new_name });
        changes.entry(target).or_default().push(edit);
    }
    json!({ "changes": changes })
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
    let Some(close) = inside.find(')') else {
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
                "i8" | "u8"
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

fn publish(uri: &str, analysis: &Analysis, source: &str) {
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
                .map(|token| (token.span.start, token.span.end))
                .unwrap_or((offset, (offset + 1).min(source.len())));
            json!({
                "range": range_of(&lines, source, start, end.max(start)),
                "severity": 1,
                "source": "rfl",
                "message": err.detail,
            })
        })
        .collect();
    send(
        &mut io::stdout().lock(),
        &json!({
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": { "uri": uri, "diagnostics": diagnostics }
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
        .and_then(|path| std::fs::read_to_string(path).ok())
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
    let line = position["line"].as_u64()? as usize;
    let character = position["character"].as_u64()? as usize;
    let start = *doc.lines.get(line)?;
    let end = doc.lines.get(line + 1).copied().unwrap_or(doc.source.len());
    let slice = doc.source.get(start..end)?;
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
    format!("file://{}", path.display())
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

fn send(stdout: &mut impl Write, message: &Value) {
    let body = message.to_string();
    let _ = write!(stdout, "Content-Length: {}\r\n\r\n{body}", body.len());
    let _ = stdout.flush();
}
