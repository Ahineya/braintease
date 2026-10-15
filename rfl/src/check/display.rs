//! Display helpers for RFL checking.

use super::*;

pub(super) fn signature_accepts_receiver(signature: &str, receiver: &str) -> bool {
    let Some(inside) = signature.strip_prefix("fn(") else {
        return false;
    };
    let Some(close) = close_paren(inside) else {
        return false;
    };
    let first = split_top(&inside[..close]).into_iter().next().unwrap_or("");
    first == receiver || first == format!("&{receiver}") || first == format!("&mut {receiver}")
}

pub(super) fn close_paren(text: &str) -> Option<usize> {
    let mut depth = 0i32;
    for (index, ch) in text.char_indices() {
        match ch {
            '(' | '<' | '[' => depth += 1,
            ')' if depth == 0 => return Some(index),
            ')' | '>' | ']' => depth -= 1,
            _ => {}
        }
    }
    None
}

pub(super) fn split_top(text: &str) -> Vec<&str> {
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

pub(super) fn parsed_brief(ty: &ParsedTy) -> String {
    match ty {
        ParsedTy::Named { name, args, .. } if args.is_empty() => name.clone(),
        ParsedTy::Named { name, args, .. } => {
            format!(
                "{name}<{}>",
                args.iter().map(parsed_brief).collect::<Vec<_>>().join(", ")
            )
        }
        ParsedTy::Ref { mutable, inner, .. } => format!(
            "{}{}",
            if *mutable { "&mut " } else { "&" },
            parsed_brief(inner)
        ),
        ParsedTy::Ptr { mutable, inner, .. } => format!(
            "{}{}",
            if *mutable { "*mut " } else { "*const " },
            parsed_brief(inner)
        ),
        ParsedTy::Func { params, ret, .. } => {
            format!(
                "fn({}) -> {}",
                params
                    .iter()
                    .map(parsed_brief)
                    .collect::<Vec<_>>()
                    .join(", "),
                parsed_brief(ret)
            )
        }
        ParsedTy::CFn { params, ret, .. } => {
            format!(
                "extern \"C\" fn({}) -> {}",
                params
                    .iter()
                    .map(parsed_brief)
                    .collect::<Vec<_>>()
                    .join(", "),
                parsed_brief(ret)
            )
        }
        ParsedTy::Tuple { fields, .. } => format!(
            "({})",
            fields
                .iter()
                .map(parsed_brief)
                .collect::<Vec<_>>()
                .join(", ")
        ),
        ParsedTy::Array { elem, .. } => format!("[{}; _]", parsed_brief(elem)),
    }
}

pub(super) fn parsed_signature(func: &Function, skip_first: bool) -> String {
    let mut params: Vec<String> = func
        .params
        .iter()
        .map(|param| {
            param
                .ty
                .as_ref()
                .map(parsed_brief)
                .unwrap_or_else(|| "_".to_string())
        })
        .collect();
    if skip_first && !params.is_empty() {
        params.remove(0);
    }
    let ret = func
        .ret
        .as_ref()
        .map(parsed_brief)
        .unwrap_or_else(|| "()".to_string());
    format!("fn({}) -> {ret}", params.join(", "))
}

pub(super) fn applied_name(name: &str, args: &[Ty], checker: &Checker) -> String {
    if args.is_empty() {
        return name.to_string();
    }
    let parts: Vec<String> = args.iter().map(|ty| checker.show(*ty)).collect();
    format!("{name}<{}>", parts.join(", "))
}
