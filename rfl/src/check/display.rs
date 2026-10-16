//! Display helpers for RFL checking.

use super::*;

pub(super) fn parsed_brief(ty: &ParsedTy) -> String {
    match ty {
        ParsedTy::Projection {
            base,
            trait_name,
            name,
            ..
        } => match trait_name {
            Some((tr, _)) => format!("<{} as {tr}>::{name}", parsed_brief(base)),
            None => format!("{}::{name}", parsed_brief(base)),
        },
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
