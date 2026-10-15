//! Item configuration is evaluated after parsing each item, before checking or
//! loading its imports. The selected target, never the compiler host, supplies it.
use super::*;

impl Parser<'_> {
    pub(super) fn cfg_attributes(&mut self) -> Result<bool, CompileError> {
        let mut enabled = true;
        while self.is(&TokenKind::Hash)
            && matches!(self.tokens.get(self.index + 2).map(|t| &t.kind), Some(TokenKind::Ident(n)) if n == "cfg")
        {
            self.bump();
            self.expect(&TokenKind::LBracket, "'['")?;
            self.expect_ident()?;
            self.expect(&TokenKind::LParen, "'('")?;
            let value = self.cfg_condition(0)?;
            enabled &= value;
            self.expect(&TokenKind::RParen, "')'")?;
            self.expect(&TokenKind::RBracket, "']'")?;
        }
        Ok(enabled)
    }

    fn cfg_condition(&mut self, depth: usize) -> Result<bool, CompileError> {
        if depth >= 64 {
            return Err(CompileError::at(
                self.source,
                self.current().span.start,
                "cfg nesting exceeds 64",
            ));
        }
        let (key, span) = self.expect_ident()?;
        if self.eat(&TokenKind::LParen) {
            if !matches!(key.as_str(), "not" | "any" | "all") {
                return Err(CompileError::at(
                    self.source,
                    span.start,
                    format!("unknown cfg predicate `{key}`"),
                ));
            }
            let mut values = Vec::new();
            while !self.is(&TokenKind::RParen) {
                values.push(self.cfg_condition(depth + 1)?);
                if !self.eat(&TokenKind::Comma) {
                    break;
                }
            }
            self.expect(&TokenKind::RParen, "')'")?;
            return match key.as_str() {
                "not" if values.len() == 1 => Ok(!values[0]),
                "not" => Err(CompileError::at(
                    self.source,
                    span.start,
                    "cfg not() requires exactly one predicate",
                )),
                "any" => Ok(values.into_iter().any(|v| v)),
                _ => Ok(values.into_iter().all(|v| v)),
            };
        }
        self.expect(&TokenKind::Eq, "'='")?;
        let value = self.bump();
        let TokenKind::Str(chars) = value.kind else {
            return Err(CompileError::at(
                self.source,
                value.span.start,
                "cfg value must be a string",
            ));
        };
        let value = decode_path(&chars);
        match key.as_str() {
            "target_has" => self
                .target
                .has(&value)
                .map_err(|e| CompileError::at(self.source, span.start, e)),
            "target" => Ok(self.target.name() == value),
            "target_arch" => Ok(match self.target {
                rcc_ir::target::Target::Ripple => value == "ripple16",
                rcc_ir::target::Target::Native => value == std::env::consts::ARCH,
            }),
            _ => Err(CompileError::at(
                self.source,
                span.start,
                format!("unknown cfg key `{key}`"),
            )),
        }
    }
}
