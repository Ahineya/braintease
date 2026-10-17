//! The stdlib's checked Option/Result extraction leaf. No fabricated fallback
//! value is needed on its terminating branch, including for move-only outputs.
use super::*;

impl Checker<'_> {
    pub(super) fn check_expect_value(
        &mut self,
        span: usize,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if args.len() != 2 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`__expect_value` expects 2 arguments, found {}", args.len()),
            ));
        }
        let value = self.check_expr(&args[0], None)?;
        let Ty::Adt(id) = value.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                "expected Option or Result",
            ));
        };
        let adt = &self.adts[id as usize];
        let name = &self.schemas[adt.schema].name;
        let ok_tag = match name.as_str() {
            "Option" => self.ctor_schema["Some"].1,
            "Result" => self.ctor_schema["Ok"].1,
            _ => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "expected Option or Result",
                ))
            }
        };
        let payload = adt.args[0];
        let message = self.check_expr(&args[1], Some(Ty::String))?;
        if message.ty != Ty::String {
            return Err(CompileError::at(
                self.source,
                span,
                "expect message must be String",
            ));
        }
        if let Some(want) = expected {
            if want != payload {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(payload)),
                ));
            }
        }
        Ok(TExpr {
            ty: payload,
            kind: TExprKind::Expect {
                value: Box::new(value),
                message: Box::new(message),
                ok_tag,
            },
        })
    }
}
