//! Receiver matching for extension-function completion. This uses parsed types,
//! including generic arguments, instead of guessing from a function's name.

use crate::ast::{ExprKind, ParsedTy};
use std::collections::HashMap;

#[derive(Clone)]
pub struct ExtensionFunction {
    pub name: String,
    pub signature: String,
    pub generics: Vec<String>,
}

impl ExtensionFunction {
    pub fn method_signature(&self, receiver: &str) -> Option<String> {
        let inside = self.signature.strip_prefix("fn(")?;
        let close = closing_paren(inside)?;
        let params = split_params(&inside[..close]);
        let pattern = parse_type(params.first()?)?;
        let actual = parse_type(receiver)?;
        let mut bindings = HashMap::new();
        if !matches_type(&pattern, &actual, &self.generics, &mut bindings) {
            return None;
        }
        let signature = format!("fn({}){}", params[1..].join(", "), &inside[close + 1..]);
        Some(substitute(&signature, &bindings))
    }
}

fn parse_type(text: &str) -> Option<ParsedTy> {
    // Module-qualified internal type names contain '$', which is not source syntax.
    let source = format!(
        "fn completion_receiver(value: {}) {{}}",
        text.replace('$', "__dollar__")
    );
    let tokens = crate::lexer::tokenize(&source).ok()?;
    crate::parser::parse(&source, &tokens)
        .ok()?
        .functions
        .into_iter()
        .next()?
        .params
        .into_iter()
        .next()?
        .ty
}

fn matches_type(
    pattern: &ParsedTy,
    actual: &ParsedTy,
    generics: &[String],
    bindings: &mut HashMap<String, ParsedTy>,
) -> bool {
    match (pattern, actual) {
        (ParsedTy::Named { name, args, .. }, _) if args.is_empty() && generics.contains(name) => {
            if let Some(bound) = bindings.get(name) {
                return matches_type(bound, actual, &[], &mut HashMap::new());
            }
            bindings.insert(name.clone(), actual.clone());
            true
        }
        (
            ParsedTy::Named {
                name: left,
                args: lhs,
                ..
            },
            ParsedTy::Named {
                name: right,
                args: rhs,
                ..
            },
        ) => left == right && match_all(lhs, rhs, generics, bindings),
        (
            ParsedTy::Ref {
                mutable: left,
                inner: lhs,
                ..
            },
            ParsedTy::Ref {
                mutable: right,
                inner: rhs,
                ..
            },
        )
        | (
            ParsedTy::Ptr {
                mutable: left,
                inner: lhs,
                ..
            },
            ParsedTy::Ptr {
                mutable: right,
                inner: rhs,
                ..
            },
        ) => (!left || *right) && matches_type(lhs, rhs, generics, bindings),
        (
            ParsedTy::Func {
                params: lhs,
                ret: left,
                ..
            },
            ParsedTy::Func {
                params: rhs,
                ret: right,
                ..
            },
        )
        | (
            ParsedTy::CFn {
                params: lhs,
                ret: left,
                ..
            },
            ParsedTy::CFn {
                params: rhs,
                ret: right,
                ..
            },
        ) => {
            match_all(lhs, rhs, generics, bindings) && matches_type(left, right, generics, bindings)
        }
        (ParsedTy::Tuple { fields: lhs, .. }, ParsedTy::Tuple { fields: rhs, .. }) => {
            match_all(lhs, rhs, generics, bindings)
        }
        (
            ParsedTy::Array {
                elem: lhs,
                len: left,
                ..
            },
            ParsedTy::Array {
                elem: rhs,
                len: right,
                ..
            },
        ) => {
            let length_matches = match (&left.kind, &right.kind) {
                (ExprKind::Int(lhs), ExprKind::Int(rhs)) => lhs == rhs,
                (ExprKind::Name(name), _) if name == "_" => true,
                _ => false,
            };
            length_matches && matches_type(lhs, rhs, generics, bindings)
        }
        _ => false,
    }
}

fn match_all(
    pattern: &[ParsedTy],
    actual: &[ParsedTy],
    generics: &[String],
    bindings: &mut HashMap<String, ParsedTy>,
) -> bool {
    pattern.len() == actual.len()
        && pattern
            .iter()
            .zip(actual)
            .all(|(lhs, rhs)| matches_type(lhs, rhs, generics, bindings))
}

fn type_text(ty: &ParsedTy) -> String {
    // The compiler's own signature formatter handles all supported type syntax.
    crate::check::completion_type_name(ty).replace("__dollar__", "$")
}

fn substitute(text: &str, bindings: &HashMap<String, ParsedTy>) -> String {
    let mut result = String::new();
    let mut chars = text.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch.is_ascii_alphabetic() || ch == '_' {
            let mut name = ch.to_string();
            while chars
                .peek()
                .is_some_and(|ch| ch.is_ascii_alphanumeric() || *ch == '_')
            {
                name.push(chars.next().unwrap());
            }
            result.push_str(&bindings.get(&name).map(type_text).unwrap_or(name));
        } else {
            result.push(ch);
        }
    }
    result
}

fn closing_paren(text: &str) -> Option<usize> {
    let mut depth = 0;
    for (at, ch) in text.char_indices() {
        match ch {
            '>' if at > 0 && text.as_bytes()[at - 1] == b'-' => {}
            '(' | '<' | '[' => depth += 1,
            ')' if depth == 0 => return Some(at),
            ')' | '>' | ']' => depth -= 1,
            _ => {}
        }
    }
    None
}

fn split_params(text: &str) -> Vec<&str> {
    let mut parts = Vec::new();
    let mut depth = 0;
    let mut start = 0;
    for (at, ch) in text.char_indices() {
        match ch {
            '>' if at > 0 && text.as_bytes()[at - 1] == b'-' => {}
            '(' | '<' | '[' => depth += 1,
            ')' | '>' | ']' => depth -= 1,
            ',' if depth == 0 => {
                parts.push(text[start..at].trim());
                start = at + 1;
            }
            _ => {}
        }
    }
    if !text[start..].trim().is_empty() {
        parts.push(text[start..].trim());
    }
    parts
}

#[cfg(test)]
mod tests {
    use super::ExtensionFunction;

    fn method(signature: &str, generics: &[&str], receiver: &str) -> Option<String> {
        ExtensionFunction {
            name: "test".to_string(),
            signature: signature.to_string(),
            generics: generics.iter().map(|name| name.to_string()).collect(),
        }
        .method_signature(receiver)
    }

    #[test]
    fn matches_concrete_and_nested_generic_receivers() {
        assert_eq!(
            method("fn(String, String) -> bool", &[], "String").as_deref(),
            Some("fn(String) -> bool")
        );
        assert!(method("fn(char) -> bool", &[], "String").is_none());
        assert_eq!(
            method(
                "fn(Vec<T>, fn(T) -> U) -> Vec<U>",
                &["T", "U"],
                "Vec<String>"
            )
            .as_deref(),
            Some("fn(fn(String) -> U) -> Vec<U>")
        );
        assert!(method("fn((T, T)) -> T", &["T"], "(i16, bool)").is_none());
        assert_eq!(
            method("fn((T, T)) -> T", &["T"], "(i16, i16)").as_deref(),
            Some("fn() -> i16")
        );
        assert_eq!(
            method("fn(m1$List<T>) -> T", &["T"], "m1$List<String>").as_deref(),
            Some("fn() -> String")
        );
    }

    #[test]
    fn respects_reference_compatibility_without_implicit_borrowing() {
        assert!(method("fn(&String) -> bool", &[], "String").is_none());
        assert!(method("fn(&mut String) -> bool", &[], "&String").is_none());
        assert!(method("fn(&String) -> bool", &[], "&mut String").is_some());
        assert_eq!(
            method("fn(fn(i16) -> bool, i16) -> bool", &[], "fn(i16) -> bool").as_deref(),
            Some("fn(i16) -> bool")
        );
    }
}
