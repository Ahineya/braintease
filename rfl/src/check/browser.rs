//! Private stdlib bridge: retain a typed callback root across host entries.
use super::*;
impl Checker<'_> {
    pub(super) fn check_browser_root(
        &mut self,
        span: usize,
        name: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let arity = usize::from(name == "__browser_root_replace");
        if args.len() != arity {
            return Err(CompileError::at(
                self.source,
                span,
                format!("{name} expects {arity} arguments"),
            ));
        }
        let ty = self.intern_func(vec![Ty::U16, Ty::U32], Ty::U32);
        if expected.is_some_and(|want| want != ty) {
            return Err(CompileError::at(
                self.source,
                span,
                "browser root has signature fn(u16, u32) -> u32",
            ));
        }
        let mut checked = Vec::new();
        for arg in args {
            let value = self.check_expr(arg, Some(ty))?;
            if value.ty != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "browser root requires fn(u16, u32) -> u32",
                ));
            }
            checked.push(value);
        }
        // Internal operations share the checked call representation. Lowering
        // intercepts these names; no managed callback is passed through C ABI.
        Ok(TExpr {
            ty,
            kind: TExprKind::Extern {
                callee: name.into(),
                args: checked,
                fixed: arity,
            },
        })
    }
}
