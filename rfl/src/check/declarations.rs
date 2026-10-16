//! Declarations helpers for RFL checking.

use super::*;

pub(super) fn prelude_result() -> Adt {
    let span = Span::empty();
    let value = ParsedTy::Named {
        span,
        name: "T".to_string(),
        args: Vec::new(),
    };
    let error = ParsedTy::Named {
        span,
        name: "E".to_string(),
        args: Vec::new(),
    };
    Adt {
        name: "Result".to_string(),
        name_span: span,
        params: vec![
            TypeParam {
                bounds: Vec::new(),
                name: "T".to_string(),
                span,
            },
            TypeParam {
                bounds: Vec::new(),
                name: "E".to_string(),
                span,
            },
        ],
        ctors: vec![
            Ctor {
                name: "Ok".to_string(),
                name_span: span,
                fields: vec![value],
            },
            Ctor {
                name: "Err".to_string(),
                name_span: span,
                fields: vec![error],
            },
        ],
        exported: false,
    }
}

pub(super) fn expand_impls(
    source: &str,
    module: &mut Module,
) -> Result<Vec<Inherent>, CompileError> {
    let impls = std::mem::take(&mut module.impls);
    let mut methods = Vec::new();
    for item in impls {
        if item.trait_name.is_some() {
            continue;
        }
        let type_name = impl_type_name(source, &item)?;
        for method in item.items {
            if methods
                .iter()
                .any(|found: &Inherent| found.type_name == type_name && found.method == method.name)
            {
                return Err(CompileError::at(
                    source,
                    method.name_span.start,
                    format!("duplicate method `{type_name}::{}`", method.name),
                ));
            }
            if type_name == "Vec"
                && matches!(
                    method.name.as_str(),
                    "len" | "push" | "is_empty" | "clear" | "pop" | "get"
                )
            {
                return Err(CompileError::at(
                    source,
                    method.name_span.start,
                    format!("`Vec` already has `{}`", method.name),
                ));
            }
            let func = format!("{type_name}${}", method.name);
            let mut type_params = item.type_params.clone();
            type_params.extend(method.type_params.iter().cloned());
            let mut params = Vec::new();
            if let Some(recv) = method.receiver {
                let ty = match recv {
                    Recv::Value => item.ty.clone(),
                    Recv::Shared => ParsedTy::Ref {
                        span: method.name_span,
                        mutable: false,
                        inner: Box::new(item.ty.clone()),
                    },
                    Recv::Mut => ParsedTy::Ref {
                        span: method.name_span,
                        mutable: true,
                        inner: Box::new(item.ty.clone()),
                    },
                };
                params.push(Param {
                    name: "self".to_string(),
                    name_span: method.name_span,
                    ty: Some(ty),
                    pat: Pat::Name {
                        name: "self".to_string(),
                        span: method.name_span,
                    },
                });
            }
            params.extend(method.params.iter().cloned());
            let ret = method.ret.or_else(|| {
                if method.body.tail.is_none() {
                    Some(ParsedTy::Named {
                        span: method.name_span,
                        name: "unit".to_string(),
                        args: Vec::new(),
                    })
                } else {
                    None
                }
            });
            module.functions.push(Function {
                name: func.clone(),
                name_span: method.name_span,
                type_params,
                params,
                ret,
                body: method.body,
                c_export: false,
                exported: false,
            });
            methods.push(Inherent {
                type_name: type_name.clone(),
                method: method.name,
                func,
                receiver: method.receiver,
            });
        }
    }
    Ok(methods)
}

pub(super) fn impl_type_name(source: &str, item: &Impl) -> Result<String, CompileError> {
    let ParsedTy::Named { span, name, args } = &item.ty else {
        return Err(CompileError::at(
            source,
            item.span.start,
            "an impl type must be a named type",
        ));
    };
    if item.type_params.is_empty() {
        return Ok(name.clone());
    }
    if args.len() != item.type_params.len() {
        return Err(CompileError::at(
            source,
            span.start,
            format!("`{name}` expects {} type arguments", item.type_params.len()),
        ));
    }
    for (arg, param) in args.iter().zip(&item.type_params) {
        let ok = matches!(arg, ParsedTy::Named { name: arg_name, args, .. } if args.is_empty() && arg_name == &param.name);
        if !ok {
            return Err(CompileError::at(
                source,
                arg.span().start,
                "a generic impl uses its type parameters in order",
            ));
        }
    }
    Ok(name.clone())
}

pub(super) fn prelude_option() -> Adt {
    let span = Span::empty();
    let param = ParsedTy::Named {
        span,
        name: "T".to_string(),
        args: Vec::new(),
    };
    Adt {
        name: "Option".to_string(),
        name_span: span,
        params: vec![TypeParam {
            bounds: Vec::new(),
            name: "T".to_string(),
            span,
        }],
        ctors: vec![
            Ctor {
                name: "None".to_string(),
                name_span: span,
                fields: Vec::new(),
            },
            Ctor {
                name: "Some".to_string(),
                name_span: span,
                fields: vec![param],
            },
        ],
        exported: false,
    }
}
