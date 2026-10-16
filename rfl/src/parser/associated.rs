//! Associated type declarations and projection paths in type positions.
use super::*;
impl Parser<'_> {
    pub(super) fn parse_associated_type(
        &mut self,
        declaration: bool,
    ) -> Result<AssociatedType, CompileError> {
        self.expect(&TokenKind::Type, "type")?;
        let (name, name_span) = self.expect_ident()?;
        if name == "Self" {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                "Self is reserved for the implementing type",
            ));
        }
        if self.is(&TokenKind::Lt) {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                "generic associated types are not supported",
            ));
        }
        let mut bounds = Vec::new();
        if self.eat(&TokenKind::Colon) {
            if !declaration {
                return Err(CompileError::at(
                    self.source,
                    name_span.start,
                    "associated type bounds belong in the trait declaration",
                ));
            }
            loop {
                bounds.push(self.expect_ident()?);
                if !self.eat(&TokenKind::Plus) {
                    break;
                }
            }
        }
        let value = if declaration {
            if self.is(&TokenKind::Eq) {
                return Err(CompileError::at(
                    self.source,
                    name_span.start,
                    "associated type defaults are not supported",
                ));
            }
            None
        } else {
            self.expect(&TokenKind::Eq, "'=' in associated type definition")?;
            Some(self.parse_type()?)
        };
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(AssociatedType {
            name,
            name_span,
            bounds,
            value,
        })
    }

    pub(super) fn parse_type(&mut self) -> Result<ParsedTy, CompileError> {
        let mut ty = if self.eat_lt() {
            let span = self.prev_span();
            let base = self.parse_type()?;
            self.expect(&TokenKind::As, "`as` in qualified associated type")?;
            let trait_name = self.expect_ident()?;
            self.expect_gt()?;
            self.expect(&TokenKind::ColonColon, "'::'")?;
            let (name, name_span) = self.parse_projection_member()?;
            ParsedTy::Projection {
                span: span.to(name_span),
                base: Box::new(base),
                trait_name: Some(trait_name),
                name,
                name_span,
            }
        } else {
            self.parse_type_atom()?
        };
        while self.eat(&TokenKind::ColonColon) {
            let (name, name_span) = self.parse_projection_member()?;
            ty = ParsedTy::Projection {
                span: ty.span().to(name_span),
                base: Box::new(ty),
                trait_name: None,
                name,
                name_span,
            };
        }
        Ok(ty)
    }

    fn parse_projection_member(&mut self) -> Result<(String, Span), CompileError> {
        if self.is_ident() || !self.tolerant {
            return self.expect_ident();
        }
        let separator = self.prev_span();
        self.errors.push(CompileError::at(
            self.source,
            separator.start,
            EXPECTED_ASSOCIATED_TYPE_NAME,
        ));
        Ok((String::new(), Span::new(separator.end, separator.end)))
    }

    pub(super) fn eat_lt(&mut self) -> bool {
        if self.eat(&TokenKind::Lt) {
            return true;
        }
        if self.is(&TokenKind::Shl) {
            self.bump();
            self.pending_lt = true;
            return true;
        }
        false
    }

    pub(super) fn starts_type(&self) -> bool {
        self.is(&TokenKind::Lt)
            || matches!(
                self.current().kind,
                TokenKind::Ident(_)
                    | TokenKind::Lt
                    | TokenKind::Shl
                    | TokenKind::Amp
                    | TokenKind::Star
                    | TokenKind::LBracket
                    | TokenKind::LParen
                    | TokenKind::Fn
                    | TokenKind::Extern
            )
    }
}
