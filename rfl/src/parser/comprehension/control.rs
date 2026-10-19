//! Do not let syntax sugar silently introduce a return/try/await boundary.
use super::*;

pub(super) fn validate(source: &str, expr: &Expr) -> Result<(), CompileError> {
    Walk { source }.expr(expr, 0)
}
struct Walk<'a> {
    source: &'a str,
}
impl Walk<'_> {
    fn reject(&self, at: Span) -> Result<(), CompileError> {
        Err(CompileError::at(self.source, at.start,
            "control flow cannot cross a comprehension continuation; lift the operation into a binding or use an explicit callback"))
    }
    fn block(&self, block: &Block, loops: usize) -> Result<(), CompileError> {
        for stmt in &block.stmts {
            match stmt {
                Stmt::Return { span, .. } => return self.reject(*span),
                Stmt::Break { span } | Stmt::Continue { span } if loops == 0 => {
                    return self.reject(*span)
                }
                Stmt::Break { .. } | Stmt::Continue { .. } => {}
                Stmt::Let { init, .. } => self.expr(init, loops)?,
                Stmt::Assign { value, .. } | Stmt::FieldAssign { value, .. } => {
                    self.expr(value, loops)?
                }
                Stmt::IndexAssign { index, value, .. } => {
                    self.expr(index, loops)?;
                    self.expr(value, loops)?;
                }
                Stmt::DerefAssign { ptr, value } => {
                    self.expr(ptr, loops)?;
                    self.expr(value, loops)?;
                }
                Stmt::Expr(value) => self.expr(value, loops)?,
            }
        }
        if let Some(value) = &block.tail {
            self.expr(value, loops)?;
        }
        Ok(())
    }
    fn expr(&self, expr: &Expr, loops: usize) -> Result<(), CompileError> {
        match &expr.kind {
            ExprKind::Await(_) | ExprKind::Try(_) => return self.reject(expr.span),
            ExprKind::Lambda { .. } => {} // Explicit callbacks define their own boundary.
            ExprKind::Block(block) | ExprKind::Unsafe(block) => self.block(block, loops)?,
            ExprKind::Unary { expr, .. }
            | ExprKind::Borrow { expr, .. }
            | ExprKind::Deref(expr)
            | ExprKind::Cast { expr, .. } => self.expr(expr, loops)?,
            ExprKind::Binary { lhs, rhs, .. }
            | ExprKind::Range {
                start: lhs,
                end: rhs,
                ..
            } => {
                self.expr(lhs, loops)?;
                self.expr(rhs, loops)?;
            }
            ExprKind::Call { args, .. }
            | ExprKind::Assoc { args, .. }
            | ExprKind::Array(args)
            | ExprKind::Tuple(args) => {
                for arg in args {
                    self.expr(arg, loops)?;
                }
            }
            ExprKind::Method { receiver, args, .. } => {
                self.expr(receiver, loops)?;
                for arg in args {
                    self.expr(arg, loops)?;
                }
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                self.expr(cond, loops)?;
                self.block(then_body, loops)?;
                if let Some(other) = else_body {
                    self.expr(other, loops)?;
                }
            }
            ExprKind::Match { scrutinee, arms } => {
                self.expr(scrutinee, loops)?;
                for arm in arms {
                    if let Some(guard) = &arm.guard {
                        self.expr(guard, loops)?;
                    }
                    self.expr(&arm.body, loops)?;
                }
            }
            ExprKind::While { cond, body } => {
                self.expr(cond, loops)?;
                self.block(body, loops + 1)?;
            }
            ExprKind::For { iter, body, .. } => {
                self.expr(iter, loops)?;
                self.block(body, loops + 1)?;
            }
            ExprKind::StructLit { fields, .. } => {
                for (_, value) in fields {
                    self.expr(value, loops)?;
                }
            }
            ExprKind::Field { base, .. } => self.expr(base, loops)?,
            ExprKind::Index { base, index } => {
                self.expr(base, loops)?;
                self.expr(index, loops)?;
            }
            ExprKind::Slice {
                base, start, end, ..
            } => {
                self.expr(base, loops)?;
                self.expr(start, loops)?;
                self.expr(end, loops)?;
            }
            // Expansion revalidates generated comprehension continuations.
            ExprKind::Macro(_) => {}
            ExprKind::Int(_)
            | ExprKind::Float(_)
            | ExprKind::Bool(_)
            | ExprKind::Str(_)
            | ExprKind::CStr(_)
            | ExprKind::Char(_)
            | ExprKind::Name(_) => {}
        }
        Ok(())
    }
}
