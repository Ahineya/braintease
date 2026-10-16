//! Conservative first-order type-pattern unification; bounds do not specialize impls.
use super::*;
#[derive(Clone, PartialEq, Eq)]
enum Pattern {
    Var(String),
    Node(String, Vec<Pattern>),
}
fn pattern(ty: &ParsedTy, params: &[TypeParam], side: &str) -> Pattern {
    match ty {
        // Projection-bearing impl patterns are rejected before coherence checking.
        ParsedTy::Projection { span, .. } => {
            Pattern::Var(format!("projection:{side}:{}", span.start))
        }
        ParsedTy::Named { name, args, .. } => {
            if params.iter().any(|p| p.name == *name) {
                return Pattern::Var(format!("{side}{name}"));
            }
            Pattern::Node(
                Ty::parse_name(name)
                    .map(|t| t.to_string())
                    .unwrap_or_else(|| name.clone()),
                args.iter().map(|t| pattern(t, params, side)).collect(),
            )
        }
        ParsedTy::Ref { mutable, inner, .. } | ParsedTy::Ptr { mutable, inner, .. } => {
            Pattern::Node(
                format!(
                    "{}:{mutable}",
                    if matches!(ty, ParsedTy::Ref { .. }) {
                        "ref"
                    } else {
                        "ptr"
                    }
                ),
                vec![pattern(inner, params, side)],
            )
        }
        ParsedTy::Tuple { fields, .. } => Pattern::Node(
            "tuple".into(),
            fields.iter().map(|t| pattern(t, params, side)).collect(),
        ),
        ParsedTy::Func {
            params: args, ret, ..
        }
        | ParsedTy::CFn {
            params: args, ret, ..
        } => {
            let mut children: Vec<_> = args.iter().map(|t| pattern(t, params, side)).collect();
            children.push(pattern(ret, params, side));
            Pattern::Node(
                if matches!(ty, ParsedTy::CFn { .. }) {
                    "cfn"
                } else {
                    "fn"
                }
                .into(),
                children,
            )
        }
        ParsedTy::Array { elem, len, .. } => {
            // Constant expressions are evaluated later. Unknown lengths may
            // coincide; treating them as disjoint would admit overlapping impls.
            let length = match len.kind {
                ExprKind::Int(n) => Pattern::Node(format!("length:{n}"), Vec::new()),
                _ => Pattern::Var(format!("#length:{side}:{}", len.span.start)),
            };
            Pattern::Node("array".into(), vec![pattern(elem, params, side), length])
        }
    }
}
pub(super) fn overlaps(a: &Impl, b: &Impl) -> bool {
    fn resolve(p: Pattern, subst: &HashMap<String, Pattern>) -> Pattern {
        match p {
            Pattern::Var(ref v) if subst.contains_key(v) => resolve(subst[v].clone(), subst),
            other => other,
        }
    }
    fn occurs(v: &str, p: &Pattern, subst: &HashMap<String, Pattern>) -> bool {
        match resolve(p.clone(), subst) {
            Pattern::Var(x) => x == v,
            Pattern::Node(_, children) => children.iter().any(|c| occurs(v, c, subst)),
        }
    }
    fn unify(a: Pattern, b: Pattern, subst: &mut HashMap<String, Pattern>) -> bool {
        let a = resolve(a, subst);
        let b = resolve(b, subst);
        if a == b {
            return true;
        }
        match (a, b) {
            (Pattern::Var(v), other) | (other, Pattern::Var(v)) => {
                if occurs(&v, &other, subst) {
                    return false;
                }
                subst.insert(v, other);
                true
            }
            (Pattern::Node(a, aa), Pattern::Node(b, bb)) => {
                a == b
                    && aa.len() == bb.len()
                    && aa.into_iter().zip(bb).all(|(a, b)| unify(a, b, subst))
            }
        }
    }
    unify(
        pattern(&a.ty, &a.type_params, "a:"),
        pattern(&b.ty, &b.type_params, "b:"),
        &mut HashMap::new(),
    )
}
