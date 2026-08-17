pub mod lexer;
pub mod parser;
pub mod directives;
pub mod cond_eval;
pub mod tests;

use anyhow::Result;
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;

pub use lexer::Token;
pub use parser::Directive;

/// Main preprocessor struct
pub struct Preprocessor {
    /// Defined macros
    macros: HashMap<String, Macro>,
    /// Include search paths
    include_dirs: Vec<PathBuf>,
    /// Stack of conditional compilation states
    conditional_stack: Vec<ConditionalState>,
    /// Current file being processed
    current_file: Option<PathBuf>,
    /// Current include depth
    include_depth: usize,
    /// Files marked with #pragma once
    pragma_once_files: HashSet<PathBuf>,
    /// Line number mapping for #line directives
    _line_map: Vec<LineMapping>,
    /// Keep comments in output
    keep_comments: bool,
    /// Keep line directives
    keep_line_directives: bool,
    /// Approximate source line for `__LINE__` expansion
    current_line: usize,
}

/// Maximum include depth (standard is usually 200-1024)
const MAX_INCLUDE_DEPTH: usize = 200;

/// Represents a macro definition
#[derive(Debug, Clone)]
pub struct Macro {
    pub name: String,
    pub params: Option<Vec<String>>,
    pub body: String,
    pub is_variadic: bool,
}

/// State for conditional compilation
#[derive(Debug, Clone)]
struct ConditionalState {
    active: bool,
    has_else: bool,
    parent_active: bool,
}

/// Line number mapping for error reporting
#[derive(Debug, Clone)]
struct LineMapping {
    _output_line: usize,
    _source_file: PathBuf,
    _source_line: usize,
}

impl Preprocessor {
    /// Create a new preprocessor
    pub fn new() -> Self {
        let mut pp = Self {
            macros: HashMap::new(),
            include_dirs: vec![],
            conditional_stack: vec![],
            current_file: None,
            include_depth: 0,
            pragma_once_files: HashSet::new(),
            _line_map: vec![],
            keep_comments: false,
            keep_line_directives: false,
            current_line: 1,
        };
        // C99 freestanding builtins. __FILE__ / __LINE__ are expanded specially.
        pp.define("__STDC__".to_string(), Some("1".to_string()));
        pp.define("__STDC_VERSION__".to_string(), Some("199901L".to_string()));
        pp.define("__STDC_HOSTED__".to_string(), Some("0".to_string()));
        pp
    }

    /// Add an include directory
    pub fn add_include_dir(&mut self, dir: PathBuf) {
        self.include_dirs.push(dir);
    }

    /// Define a macro
    pub fn define(&mut self, name: String, value: Option<String>) {
        let body = value.unwrap_or_else(|| "1".to_string());
        self.macros.insert(
            name.clone(),
            Macro {
                name,
                params: None,
                body,
                is_variadic: false,
            },
        );
    }

    /// Undefine a macro
    pub fn undefine(&mut self, name: &str) {
        self.macros.remove(name);
    }

    /// Set whether to keep comments
    pub fn set_keep_comments(&mut self, keep: bool) {
        self.keep_comments = keep;
    }

    /// Set whether to keep line directives
    pub fn set_keep_line_directives(&mut self, keep: bool) {
        self.keep_line_directives = keep;
    }

    /// Process a source file
    pub fn process(&mut self, input: &str, source_file: PathBuf) -> Result<String> {
        self.current_file = Some(source_file);
        self.current_line = 1;
        
        // Tokenize the input
        let tokens = lexer::tokenize(input)?;
        
        // Parse directives
        let directives = parser::parse(&tokens)?;
        
        // Process directives and expand macros
        let output = self.process_directives(directives)?;
        
        Ok(output)
    }

    /// Process parsed directives
    fn process_directives(&mut self, directives: Vec<Directive>) -> Result<String> {
        let mut output = String::new();
        
        for directive in directives {
            match directive {
                Directive::Include { .. } => {
                    let included = self.handle_include(directive)?;
                    output.push_str(&included);
                    self.current_line += 1;
                }
                Directive::Define { .. } => {
                    self.handle_define(directive)?;
                    self.current_line += 1;
                }
                Directive::Undef { .. } => {
                    self.handle_undef(directive)?;
                    self.current_line += 1;
                }
                Directive::If { .. } | Directive::Ifdef { .. } | Directive::Ifndef { .. } => {
                    self.handle_conditional_start(directive)?;
                    self.current_line += 1;
                }
                Directive::Elif { .. } | Directive::Else => {
                    self.handle_conditional_else(directive)?;
                    self.current_line += 1;
                }
                Directive::Endif => {
                    self.handle_conditional_end()?;
                    self.current_line += 1;
                }
                Directive::Line { number, file } => {
                    self.current_line = number;
                    if self.keep_line_directives {
                        output.push_str(&self.handle_line(Directive::Line { number, file })?);
                    }
                }
                Directive::Pragma { content } => {
                    // Handle #pragma once
                    if content.trim() == "once" {
                        if let Some(file) = &self.current_file {
                            self.pragma_once_files.insert(file.clone());
                        }
                    }
                    self.current_line += 1;
                }
                Directive::Text(text) => {
                    if self.should_output() {
                        // Filter out comments unless keep_comments is true
                        let processed_text = if !self.keep_comments {
                            self.remove_comments(&text)
                        } else {
                            text.clone()
                        };
                        output.push_str(&self.expand_macros(&processed_text)?);
                    }
                    let newlines = text.bytes().filter(|&b| b == b'\n').count();
                    self.current_line += newlines;
                }
                Directive::Error { message } => {
                    if self.should_output() {
                        let msg = if message.is_empty() {
                            "#error".to_string()
                        } else {
                            format!("#error {message}")
                        };
                        return Err(anyhow::anyhow!("{msg}"));
                    }
                    self.current_line += 1;
                }
                Directive::Warning { message } => {
                    if self.should_output() {
                        log::warn!("#warning {message}");
                        eprintln!("warning: {message}");
                    }
                    self.current_line += 1;
                }
            }
        }
        
        Ok(output)
    }

    /// Check if we should output based on conditional stack
    fn should_output(&self) -> bool {
        self.conditional_stack.iter().all(|c| c.active)
    }

    /// Expand macros in text
    fn expand_macros(&self, text: &str) -> Result<String> {
        self.expand_macros_impl(text)
    }
    
    /// Remove comments from text
    fn remove_comments(&self, text: &str) -> String {
        let mut result = String::new();
        let mut chars = text.chars().peekable();
        let mut in_string = false;
        let mut in_char = false;
        let mut escape_next = false;
        
        while let Some(ch) = chars.next() {
            // Handle escape sequences
            if escape_next {
                result.push(ch);
                escape_next = false;
                continue;
            }
            
            // Check for escape character
            if (in_string || in_char) && ch == '\\' {
                result.push(ch);
                escape_next = true;
                continue;
            }
            
            // Handle string literals
            if ch == '"' && !in_char {
                in_string = !in_string;
                result.push(ch);
                continue;
            }
            
            // Handle character literals
            if ch == '\'' && !in_string {
                in_char = !in_char;
                result.push(ch);
                continue;
            }
            
            // Only process comments if we're not inside a string or char literal
            if !in_string && !in_char && ch == '/' {
                if let Some(&next_ch) = chars.peek() {
                    if next_ch == '/' {
                        // Line comment - skip to end of line
                        chars.next(); // consume second '/'
                        for c in chars.by_ref() {
                            if c == '\n' {
                                result.push('\n');
                                break;
                            }
                        }
                    } else if next_ch == '*' {
                        // Block comment
                        chars.next(); // consume '*'
                        let mut prev = '\0';
                        for c in chars.by_ref() {
                            if prev == '*' && c == '/' {
                                result.push(' '); // Replace with space
                                break;
                            }
                            if c == '\n' {
                                result.push('\n'); // Preserve newlines
                            }
                            prev = c;
                        }
                    } else {
                        result.push(ch);
                    }
                } else {
                    result.push(ch);
                }
            } else {
                result.push(ch);
            }
        }
        
        result
    }

    /// Handle include directive
    fn handle_include(&mut self, directive: Directive) -> Result<String> {
        if let Directive::Include { path, is_system } = directive {
            self.handle_include_impl(path, is_system)
        } else {
            Ok(String::new())
        }
    }

    /// Handle define directive
    fn handle_define(&mut self, directive: Directive) -> Result<()> {
        if let Directive::Define { name, params, body, is_variadic } = directive {
            self.handle_define_impl(name, params, body, is_variadic)
        } else {
            Ok(())
        }
    }

    /// Handle undef directive
    fn handle_undef(&mut self, directive: Directive) -> Result<()> {
        if let Directive::Undef { name } = directive {
            self.handle_undef_impl(name)
        } else {
            Ok(())
        }
    }

    /// Handle conditional start directives
    fn handle_conditional_start(&mut self, directive: Directive) -> Result<()> {
        let (active, parent_active) = match directive {
            Directive::If { condition } => {
                let parent_active = self.should_output();
                let active = parent_active && self.evaluate_condition(&condition)?;
                (active, parent_active)
            }
            Directive::Ifdef { name } => {
                let parent_active = self.should_output();
                let active = parent_active && self.macros.contains_key(&name);
                (active, parent_active)
            }
            Directive::Ifndef { name } => {
                let parent_active = self.should_output();
                let active = parent_active && !self.macros.contains_key(&name);
                (active, parent_active)
            }
            _ => return Ok(()),
        };
        
        self.conditional_stack.push(ConditionalState {
            active,
            has_else: false,
            parent_active,
        });
        
        Ok(())
    }

    /// Handle else/elif directives
    fn handle_conditional_else(&mut self, directive: Directive) -> Result<()> {
        if self.conditional_stack.is_empty() {
            return Err(anyhow::anyhow!("Unexpected #else or #elif without matching #if"));
        }
        
        match directive {
            Directive::Else => {
                let state = self.conditional_stack.last_mut().unwrap();
                if state.has_else {
                    return Err(anyhow::anyhow!("Multiple #else directives"));
                }
                state.has_else = true;
                state.active = state.parent_active && !state.active;
            }
            Directive::Elif { condition } => {
                // First evaluate the condition, then update state
                let should_activate = {
                    let state = self.conditional_stack.last().unwrap();
                    if state.has_else {
                        return Err(anyhow::anyhow!("#elif after #else"));
                    }
                    !state.active && state.parent_active
                };
                
                if should_activate {
                    let active = self.evaluate_condition(&condition)?;
                    let state = self.conditional_stack.last_mut().unwrap();
                    state.active = active;
                } else {
                    let state = self.conditional_stack.last_mut().unwrap();
                    state.active = false;
                }
            }
            _ => {}
        }
        
        Ok(())
    }

    /// Handle endif directive
    fn handle_conditional_end(&mut self) -> Result<()> {
        if self.conditional_stack.is_empty() {
            return Err(anyhow::anyhow!("#endif without matching #if"));
        }
        self.conditional_stack.pop();
        Ok(())
    }

    /// Handle line directive
    fn handle_line(&self, directive: Directive) -> Result<String> {
        if let Directive::Line { number, file } = directive {
            if let Some(file) = file {
                Ok(format!("#line {number} \"{file}\"\n"))
            } else {
                Ok(format!("#line {number}\n"))
            }
        } else {
            Ok(String::new())
        }
    }
}

impl Default for Preprocessor {
    fn default() -> Self {
        Self::new()
    }
}

impl std::fmt::Display for Directive {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // TODO: Implement display for directives
        write!(f, "")
    }
}