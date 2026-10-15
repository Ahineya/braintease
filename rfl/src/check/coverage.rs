//! Pattern-matrix usefulness for nested ADTs, products, booleans, and literals.
//! Guards are checked by the caller and never contribute covering rows.

use super::*;

#[derive(Clone)]
enum Pattern {
    Any,
    Ctor(u16, Vec<Pattern>),
    Product(Vec<Pattern>),
    Lit(i64),
}

impl Pattern {
    fn from_typed(pat: &TPat) -> Self {
        match pat {
            TPat::Wild | TPat::Bind(_) => Self::Any,
            TPat::Ctor { tag, fields } => Self::Ctor(
                *tag,
                fields.iter().map(|f| Self::from_typed(&f.pat)).collect(),
            ),
            TPat::Product { fields } => {
                Self::Product(fields.iter().map(|f| Self::from_typed(&f.pat)).collect())
            }
            TPat::Lit(value) => Self::Lit(*value),
        }
    }
}

impl Checker<'_> {
    pub(super) fn pattern_useful(&self, previous: &[&TPat], pat: Option<&TPat>, ty: Ty) -> bool {
        let rows = previous
            .iter()
            .map(|p| vec![Pattern::from_typed(p)])
            .collect::<Vec<_>>();
        let query = vec![pat.map(Pattern::from_typed).unwrap_or(Pattern::Any)];
        self.useful(&rows, &query, &[ty])
    }

    fn useful(&self, rows: &[Vec<Pattern>], query: &[Pattern], types: &[Ty]) -> bool {
        if query.is_empty() {
            return rows.is_empty();
        }
        if rows.is_empty() {
            return true;
        }
        if rows
            .iter()
            .any(|row| row.iter().all(|p| matches!(p, Pattern::Any)))
        {
            return false;
        }
        match &query[0] {
            Pattern::Any => {
                let heads = self.finite_heads(types[0]);
                if !heads.is_empty()
                    && heads
                        .iter()
                        .all(|(head, _)| rows.iter().any(|r| same_head(&r[0], head)))
                {
                    heads.iter().any(|(head, fields)| {
                        let mut specialized_query = vec![Pattern::Any; fields.len()];
                        specialized_query.extend_from_slice(&query[1..]);
                        let mut specialized_types = fields.clone();
                        specialized_types.extend_from_slice(&types[1..]);
                        self.useful(
                            &specialize(rows, head, fields.len()),
                            &specialized_query,
                            &specialized_types,
                        )
                    })
                } else {
                    let defaults: Vec<_> = rows
                        .iter()
                        .filter(|r| matches!(r[0], Pattern::Any))
                        .map(|r| r[1..].to_vec())
                        .collect();
                    self.useful(&defaults, &query[1..], &types[1..])
                }
            }
            head => {
                let (fields, field_types) = match head {
                    Pattern::Ctor(tag, fields) => {
                        let Ty::Adt(id) = types[0] else {
                            unreachable!("checked constructor type")
                        };
                        (
                            fields.clone(),
                            self.adts[id as usize].ctors[*tag as usize].fields.clone(),
                        )
                    }
                    Pattern::Product(fields) => (fields.clone(), self.product_types(types[0])),
                    Pattern::Lit(_) => (Vec::new(), Vec::new()),
                    Pattern::Any => unreachable!(),
                };
                let specialized = specialize(rows, head, fields.len());
                let mut next_query = fields;
                next_query.extend_from_slice(&query[1..]);
                let mut next_types = field_types;
                next_types.extend_from_slice(&types[1..]);
                self.useful(&specialized, &next_query, &next_types)
            }
        }
    }

    fn product_types(&self, ty: Ty) -> Vec<Ty> {
        match ty {
            Ty::Struct { id, .. } => self.structs[id as usize]
                .fields
                .iter()
                .map(|(_, ty)| *ty)
                .collect(),
            Ty::Tuple { id, .. } => self.tuples[id as usize].clone(),
            _ => unreachable!("checked product type"),
        }
    }

    fn finite_heads(&self, ty: Ty) -> Vec<(Pattern, Vec<Ty>)> {
        match ty {
            Ty::Adt(id) => self.adts[id as usize]
                .ctors
                .iter()
                .enumerate()
                .map(|(tag, c)| (Pattern::Ctor(tag as u16, Vec::new()), c.fields.clone()))
                .collect(),
            Ty::Bool => vec![(Pattern::Lit(0), Vec::new()), (Pattern::Lit(1), Vec::new())],
            Ty::Struct { .. } | Ty::Tuple { .. } => {
                vec![(Pattern::Product(Vec::new()), self.product_types(ty))]
            }
            _ => Vec::new(),
        }
    }
}

fn same_head(left: &Pattern, right: &Pattern) -> bool {
    match (left, right) {
        (Pattern::Ctor(a, _), Pattern::Ctor(b, _)) => a == b,
        (Pattern::Product(_), Pattern::Product(_)) => true,
        (Pattern::Lit(a), Pattern::Lit(b)) => a == b,
        _ => false,
    }
}

fn specialize(rows: &[Vec<Pattern>], head: &Pattern, arity: usize) -> Vec<Vec<Pattern>> {
    rows.iter()
        .filter_map(|row| {
            let mut fields = match &row[0] {
                Pattern::Any => vec![Pattern::Any; arity],
                p if same_head(p, head) => match p {
                    Pattern::Ctor(_, fields) | Pattern::Product(fields) => fields.clone(),
                    Pattern::Lit(_) => Vec::new(),
                    Pattern::Any => unreachable!(),
                },
                _ => return None,
            };
            fields.extend_from_slice(&row[1..]);
            Some(fields)
        })
        .collect()
}
