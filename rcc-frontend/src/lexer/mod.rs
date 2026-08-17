//! C99 Lexer
//! 
//! Tokenizes C99 source code into a stream of tokens.
//! Handles keywords, operators, literals, identifiers, and comments.

pub mod token;
pub mod literals;
pub mod operators;

pub use token::{IntegerSuffix, Token, TokenType};

use rcc_common::{SourceLocation, SourceSpan, CompilerError};
use std::collections::HashMap;

/// C99 Lexer
pub struct Lexer {
    pub(crate) input: Vec<char>,
    pub(crate) position: usize,
    pub(crate) line: u32,
    pub(crate) column: u32,
    keywords: HashMap<String, TokenType>,
}

impl Lexer {
    /// Create a new lexer
    pub fn new(input: &str) -> Self {
        let mut lexer = Self {
            input: input.chars().collect(),
            position: 0,
            line: 1,
            column: 1,
            keywords: HashMap::new(),
        };
        
        lexer.initialize_keywords();
        lexer
    }
    
    /// Initialize keyword map
    fn initialize_keywords(&mut self) {
        let keywords = [
            ("auto", TokenType::Auto),
            ("break", TokenType::Break),
            ("case", TokenType::Case),
            ("char", TokenType::Char),
            ("const", TokenType::Const),
            ("continue", TokenType::Continue),
            ("default", TokenType::Default),
            ("do", TokenType::Do),
            ("double", TokenType::Double),
            ("else", TokenType::Else),
            ("enum", TokenType::Enum),
            ("extern", TokenType::Extern),
            ("float", TokenType::Float),
            ("for", TokenType::For),
            ("goto", TokenType::Goto),
            ("if", TokenType::If),
            ("int", TokenType::Int),
            ("long", TokenType::Long),
            ("register", TokenType::Register),
            ("return", TokenType::Return),
            ("short", TokenType::Short),
            ("signed", TokenType::Signed),
            ("sizeof", TokenType::Sizeof),
            ("static", TokenType::Static),
            ("struct", TokenType::Struct),
            ("switch", TokenType::Switch),
            ("typedef", TokenType::Typedef),
            ("union", TokenType::Union),
            ("unsigned", TokenType::Unsigned),
            ("void", TokenType::Void),
            ("volatile", TokenType::Volatile),
            ("while", TokenType::While),
            ("_Bool", TokenType::Bool),
            ("inline", TokenType::Inline),
            ("restrict", TokenType::Restrict),
            ("asm", TokenType::Asm),
            ("__asm__", TokenType::Asm),  // GCC-style inline assembly
        ];
        
        for (keyword, token_type) in keywords {
            self.keywords.insert(keyword.to_string(), token_type);
        }
    }
    
    /// Get current character
    pub(crate) fn current_char(&self) -> Option<char> {
        self.input.get(self.position).copied()
    }
    
    /// Peek ahead n characters
    pub(crate) fn peek_char(&self, offset: usize) -> Option<char> {
        self.input.get(self.position + offset).copied()
    }
    
    /// Advance to next character
    pub(crate) fn advance(&mut self) -> Option<char> {
        if let Some(ch) = self.current_char() {
            self.position += 1;
            if ch == '\n' {
                self.line += 1;
                self.column = 1;
            } else {
                self.column += 1;
            }
            Some(ch)
        } else {
            None
        }
    }
    
    /// Get current location
    pub(crate) fn current_location(&self) -> SourceLocation {
        SourceLocation::new_simple(self.line, self.column)
    }
    
    /// Skip whitespace (except newlines)
    fn skip_whitespace(&mut self) {
        while let Some(ch) = self.current_char() {
            if ch.is_whitespace() && ch != '\n' {
                self.advance();
            } else {
                break;
            }
        }
    }
    
    /// Tokenize an identifier or keyword
    fn tokenize_identifier(&mut self) -> Result<TokenType, CompilerError> {
        let mut identifier = String::new();
        
        // First character must be letter or underscore
        if let Some(ch) = self.current_char() {
            if ch.is_alphabetic() || ch == '_' {
                identifier.push(ch);
                self.advance();
            } else {
                return Err(CompilerError::lexer_error(
                    format!("Invalid identifier start: {ch}"),
                    self.current_location(),
                ));
            }
        }
        
        // Subsequent characters can be letters, digits, or underscores
        while let Some(ch) = self.current_char() {
            if ch.is_alphanumeric() || ch == '_' {
                identifier.push(ch);
                self.advance();
            } else {
                break;
            }
        }
        
        // Check if it's a keyword
        if let Some(keyword_token) = self.keywords.get(&identifier) {
            Ok(keyword_token.clone())
        } else {
            Ok(TokenType::Identifier(identifier))
        }
    }
    
    /// Get next token
    pub fn next_token(&mut self) -> Result<Token, CompilerError> {
        self.skip_whitespace();
        
        let start_location = self.current_location();
        
        let token_type = match self.current_char() {
            None => TokenType::EndOfFile,
            
            Some('\n') => {
                self.advance();
                TokenType::Newline
            }
            
            Some(ch) if ch.is_alphabetic() || ch == '_' => {
                self.tokenize_identifier()?
            }
            
            Some(ch) if ch.is_ascii_digit() => {
                self.tokenize_integer()?
            }
            
            Some('\'') => {
                self.tokenize_char_literal()?
            }
            
            Some('"') => {
                self.tokenize_string_literal()?
            }
            
            // Single-character tokens and operators
            Some('+') => {
                self.advance();
                if self.current_char() == Some('+') {
                    self.advance();
                    TokenType::PlusPlus
                } else if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::PlusEqual
                } else {
                    TokenType::Plus
                }
            }
            
            Some('-') => {
                self.advance();
                if self.current_char() == Some('-') {
                    self.advance();
                    TokenType::MinusMinus
                } else if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::MinusEqual
                } else if self.current_char() == Some('>') {
                    self.advance();
                    TokenType::Arrow
                } else {
                    TokenType::Minus
                }
            }
            
            Some('*') => {
                self.advance();
                if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::StarEqual
                } else {
                    TokenType::Star
                }
            }
            
            Some('/') => {
                if self.peek_char(1) == Some('/') {
                    self.tokenize_line_comment()
                } else if self.peek_char(1) == Some('*') {
                    self.tokenize_block_comment()?
                } else {
                    self.advance();
                    if self.current_char() == Some('=') {
                        self.advance();
                        TokenType::SlashEqual
                    } else {
                        TokenType::Slash
                    }
                }
            }
            
            Some('%') => {
                self.advance();
                if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::PercentEqual
                } else {
                    TokenType::Percent
                }
            }
            
            Some('&') => {
                self.advance();
                if self.current_char() == Some('&') {
                    self.advance();
                    TokenType::AmpersandAmpersand
                } else if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::AmpersandEqual
                } else {
                    TokenType::Ampersand
                }
            }
            
            Some('|') => {
                self.advance();
                if self.current_char() == Some('|') {
                    self.advance();
                    TokenType::PipePipe
                } else if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::PipeEqual
                } else {
                    TokenType::Pipe
                }
            }
            
            Some('^') => {
                self.advance();
                if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::CaretEqual
                } else {
                    TokenType::Caret
                }
            }
            
            Some('~') => { self.advance(); TokenType::Tilde }
            Some('!') => {
                self.advance();
                if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::BangEqual
                } else {
                    TokenType::Bang
                }
            }
            
            Some('=') => {
                self.advance();
                if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::EqualEqual
                } else {
                    TokenType::Equal
                }
            }
            
            Some('<') => {
                self.advance();
                if self.current_char() == Some('<') {
                    self.advance();
                    if self.current_char() == Some('=') {
                        self.advance();
                        TokenType::LeftShiftEqual
                    } else {
                        TokenType::LeftShift
                    }
                } else if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::LessEqual
                } else {
                    TokenType::Less
                }
            }
            
            Some('>') => {
                self.advance();
                if self.current_char() == Some('>') {
                    self.advance();
                    if self.current_char() == Some('=') {
                        self.advance();
                        TokenType::RightShiftEqual
                    } else {
                        TokenType::RightShift
                    }
                } else if self.current_char() == Some('=') {
                    self.advance();
                    TokenType::GreaterEqual
                } else {
                    TokenType::Greater
                }
            }
            
            Some('?') => { self.advance(); TokenType::Question }
            Some(':') => { self.advance(); TokenType::Colon }
            Some('(') => { self.advance(); TokenType::LeftParen }
            Some(')') => { self.advance(); TokenType::RightParen }
            Some('{') => { self.advance(); TokenType::LeftBrace }
            Some('}') => { self.advance(); TokenType::RightBrace }
            Some('[') => { self.advance(); TokenType::LeftBracket }
            Some(']') => { self.advance(); TokenType::RightBracket }
            Some(';') => { self.advance(); TokenType::Semicolon }
            Some(',') => { self.advance(); TokenType::Comma }
            Some('.') => {
                if self.peek_char(1) == Some('.') && self.peek_char(2) == Some('.') {
                    self.advance();
                    self.advance();
                    self.advance();
                    TokenType::Ellipsis
                } else {
                    self.advance();
                    TokenType::Dot
                }
            }
            
            Some(ch) => {
                return Err(CompilerError::lexer_error(
                    format!("Unexpected character: {ch}"),
                    self.current_location(),
                ));
            }
        };
        
        let end_location = self.current_location();
        let span = SourceSpan::new(start_location, end_location);
        
        Ok(Token::new(token_type, span))
    }
    
    /// Tokenize entire input into a vector of tokens
    pub fn tokenize(&mut self) -> Result<Vec<Token>, CompilerError> {
        let mut tokens = Vec::new();
        
        loop {
            let token = self.next_token()?;
            let is_eof = matches!(token.token_type, TokenType::EndOfFile);
            tokens.push(token);
            
            if is_eof {
                break;
            }
        }
        
        Ok(tokens)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_keywords() {
        let mut lexer = Lexer::new("int main void return if else");
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens.len(), 7); // 6 keywords + EOF
        assert!(matches!(tokens[0].token_type, TokenType::Int));
        assert!(matches!(tokens[1].token_type, TokenType::Identifier(_)));
        assert!(matches!(tokens[2].token_type, TokenType::Void));
        assert!(matches!(tokens[3].token_type, TokenType::Return));
        assert!(matches!(tokens[4].token_type, TokenType::If));
        assert!(matches!(tokens[5].token_type, TokenType::Else));
    }

    #[test]
    fn test_operators() {
        let mut lexer = Lexer::new("+ - * / % == != <= >= && || ++ -- ...");
        let tokens = lexer.tokenize().unwrap();
        
        let expected = vec![
            TokenType::Plus, TokenType::Minus, TokenType::Star, TokenType::Slash,
            TokenType::Percent, TokenType::EqualEqual, TokenType::BangEqual,
            TokenType::LessEqual, TokenType::GreaterEqual, TokenType::AmpersandAmpersand,
            TokenType::PipePipe, TokenType::PlusPlus, TokenType::MinusMinus,
            TokenType::Ellipsis,
            TokenType::EndOfFile,
        ];
        
        for (i, expected_type) in expected.iter().enumerate() {
            assert_eq!(tokens[i].token_type, *expected_type);
        }
    }

    #[test]
    fn test_literals() {
        let mut lexer = Lexer::new("42 'a' \"hello\" 0xff");
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens.len(), 5); // 4 literals + EOF
        assert_eq!(tokens[0].token_type, TokenType::IntLiteral { value: 42, suffix: IntegerSuffix::None, hex: false });
        assert_eq!(tokens[1].token_type, TokenType::CharLiteral(b'a'));
        assert_eq!(tokens[2].token_type, TokenType::StringLiteral("hello".to_string()));
        assert_eq!(tokens[3].token_type, TokenType::IntLiteral { value: 255, suffix: IntegerSuffix::None, hex: true });
    }

    #[test]
    fn test_integer_suffixes() {
        let mut lexer = Lexer::new("64000U 63996u 0xffUL 42L 100ul");
        let tokens = lexer.tokenize().unwrap();

        assert_eq!(tokens[0].token_type, TokenType::IntLiteral { value: 64000, suffix: IntegerSuffix::Unsigned, hex: false });
        assert_eq!(tokens[1].token_type, TokenType::IntLiteral { value: 63996, suffix: IntegerSuffix::Unsigned, hex: false });
        assert_eq!(tokens[2].token_type, TokenType::IntLiteral { value: 255, suffix: IntegerSuffix::UnsignedLong, hex: true });
        assert_eq!(tokens[3].token_type, TokenType::IntLiteral { value: 42, suffix: IntegerSuffix::Long, hex: false });
        assert_eq!(tokens[4].token_type, TokenType::IntLiteral { value: 100, suffix: IntegerSuffix::UnsignedLong, hex: false });
        assert_eq!(tokens[5].token_type, TokenType::EndOfFile);
    }

    #[test]
    fn test_long_long_suffixes() {
        let mut lexer = Lexer::new("1LL 2ll 3ULL 4ull 5LLu 6uLL");
        let tokens = lexer.tokenize().unwrap();
        assert_eq!(tokens[0].token_type, TokenType::IntLiteral { value: 1, suffix: IntegerSuffix::LongLong, hex: false });
        assert_eq!(tokens[1].token_type, TokenType::IntLiteral { value: 2, suffix: IntegerSuffix::LongLong, hex: false });
        assert_eq!(tokens[2].token_type, TokenType::IntLiteral { value: 3, suffix: IntegerSuffix::UnsignedLongLong, hex: false });
        assert_eq!(tokens[3].token_type, TokenType::IntLiteral { value: 4, suffix: IntegerSuffix::UnsignedLongLong, hex: false });
        assert_eq!(tokens[4].token_type, TokenType::IntLiteral { value: 5, suffix: IntegerSuffix::UnsignedLongLong, hex: false });
        assert_eq!(tokens[5].token_type, TokenType::IntLiteral { value: 6, suffix: IntegerSuffix::UnsignedLongLong, hex: false });
    }

    #[test]
    fn test_identifiers() {
        let mut lexer = Lexer::new("variable _private var123 __special");
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens.len(), 5); // 4 identifiers + EOF
        
        match &tokens[0].token_type {
            TokenType::Identifier(name) => assert_eq!(name, "variable"),
            _ => panic!("Expected identifier"),
        }
        
        match &tokens[1].token_type {
            TokenType::Identifier(name) => assert_eq!(name, "_private"),
            _ => panic!("Expected identifier"),
        }
    }

    #[test]
    fn test_comments() {
        let mut lexer = Lexer::new("// line comment\n/* block comment */");
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens.len(), 4); // line comment + newline + block comment + EOF
        
        match &tokens[0].token_type {
            TokenType::LineComment(comment) => assert_eq!(comment, " line comment"),
            _ => panic!("Expected line comment"),
        }
        
        assert_eq!(tokens[1].token_type, TokenType::Newline);
        
        match &tokens[2].token_type {
            TokenType::BlockComment(comment) => assert_eq!(comment, " block comment "),
            _ => panic!("Expected block comment"),
        }
    }

    #[test]
    fn test_simple_function() {
        let input = r#"
int main() {
    return 42;
}
"#;
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        
        // Should have: newline, int, main, (, ), {, newline, return, 42, ;, newline, }, newline, EOF
        let expected_count = 14;
        assert_eq!(tokens.len(), expected_count);
        
        assert_eq!(tokens[0].token_type, TokenType::Newline);
        assert_eq!(tokens[1].token_type, TokenType::Int);
        
        match &tokens[2].token_type {
            TokenType::Identifier(name) => assert_eq!(name, "main"),
            _ => panic!("Expected main identifier"),
        }
        
        assert_eq!(tokens[3].token_type, TokenType::LeftParen);
        assert_eq!(tokens[4].token_type, TokenType::RightParen);
        assert_eq!(tokens[5].token_type, TokenType::LeftBrace);
        assert_eq!(tokens[6].token_type, TokenType::Newline);
        assert_eq!(tokens[7].token_type, TokenType::Return);
        assert_eq!(tokens[8].token_type, TokenType::IntLiteral { value: 42, suffix: IntegerSuffix::None, hex: false });
        assert_eq!(tokens[9].token_type, TokenType::Semicolon);
        assert_eq!(tokens[10].token_type, TokenType::Newline);
        assert_eq!(tokens[11].token_type, TokenType::RightBrace);
        assert_eq!(tokens[12].token_type, TokenType::Newline);
        assert_eq!(tokens[13].token_type, TokenType::EndOfFile);
    }

    #[test]
    fn test_string_escapes() {
        let mut lexer = Lexer::new(r#""hello\nworld\t""#);
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens.len(), 2); // string + EOF
        assert_eq!(tokens[0].token_type, TokenType::StringLiteral("hello\nworld\t".to_string()));
    }

    #[test]
    fn test_char_escapes() {
        let mut lexer = Lexer::new(r"'\n' '\t' '\0' '\'' '\\'");
        let tokens = lexer.tokenize().unwrap();
        
        assert_eq!(tokens.len(), 6); // 5 chars + EOF
        assert_eq!(tokens[0].token_type, TokenType::CharLiteral(b'\n'));
        assert_eq!(tokens[1].token_type, TokenType::CharLiteral(b'\t'));
        assert_eq!(tokens[2].token_type, TokenType::CharLiteral(0));
        assert_eq!(tokens[3].token_type, TokenType::CharLiteral(b'\''));
        assert_eq!(tokens[4].token_type, TokenType::CharLiteral(b'\\'));
    }

    #[test]
    fn test_octal_literals_and_remaining_escapes() {
        let mut lexer = Lexer::new(r#"010 0 '\a' '\b' '\f' '\v' '\x41' '\012' "A\0B""#);
        let tokens = lexer.tokenize().unwrap();

        assert_eq!(tokens[0].token_type, TokenType::IntLiteral { value: 8, suffix: IntegerSuffix::None, hex: true });
        assert_eq!(tokens[1].token_type, TokenType::IntLiteral { value: 0, suffix: IntegerSuffix::None, hex: false });
        assert_eq!(tokens[2].token_type, TokenType::CharLiteral(7));
        assert_eq!(tokens[3].token_type, TokenType::CharLiteral(8));
        assert_eq!(tokens[4].token_type, TokenType::CharLiteral(12));
        assert_eq!(tokens[5].token_type, TokenType::CharLiteral(11));
        assert_eq!(tokens[6].token_type, TokenType::CharLiteral(b'A'));
        assert_eq!(tokens[7].token_type, TokenType::CharLiteral(10));
        assert_eq!(tokens[8].token_type, TokenType::StringLiteral("A\0B".to_string()));
        assert!(matches!(tokens[9].token_type, TokenType::EndOfFile));
    }

    #[test]
    fn test_inline_restrict_keywords() {
        let mut lexer = Lexer::new("inline restrict int");
        let tokens = lexer.tokenize().unwrap();
        assert!(matches!(tokens[0].token_type, TokenType::Inline));
        assert!(matches!(tokens[1].token_type, TokenType::Restrict));
        assert!(matches!(tokens[2].token_type, TokenType::Int));
    }
}