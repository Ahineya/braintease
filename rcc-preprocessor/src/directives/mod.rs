use crate::{Macro, Preprocessor};
use anyhow::{anyhow, Result};
use regex::Regex;
use std::fs;
use std::path::{PathBuf};

impl Preprocessor {
    /// Handle include directive implementation
    pub fn handle_include_impl(&mut self, path: String, is_system: bool) -> Result<String> {
        // Check include depth
        if self.include_depth >= crate::MAX_INCLUDE_DEPTH {
            return Err(anyhow!("Maximum include depth ({}) exceeded", crate::MAX_INCLUDE_DEPTH));
        }
        
        let file_path = self.find_include_file(&path, is_system)?;
        
        // Canonicalize the path for consistent comparison
        let canonical_path = file_path.canonicalize().unwrap_or(file_path.clone());
        
        // Check if file was marked with #pragma once
        if self.pragma_once_files.contains(&canonical_path) {
            return Ok(String::new()); // Skip this file
        }
        
        // Read and process the included file
        let content = fs::read_to_string(&file_path)
            .map_err(|e| anyhow!("Failed to read include file '{}': {}", file_path.display(), e))?;
        
        // Save current state
        let saved_file = self.current_file.clone();
        let saved_depth = self.include_depth;
        let saved_line = self.current_line;
        
        self.current_file = Some(canonical_path.clone());
        self.include_depth += 1;
        
        // Process the included file
        let result = self.process(&content, canonical_path);
        
        // Restore state
        self.current_file = saved_file;
        self.include_depth = saved_depth;
        self.current_line = saved_line;
        
        result
    }
    
    /// Find include file in search paths
    fn find_include_file(&self, path: &str, is_system: bool) -> Result<PathBuf> {
        if is_system {
            // Search in system include directories
            for dir in &self.include_dirs {
                let full_path = dir.join(path);
                if full_path.exists() {
                    return Ok(full_path);
                }
            }
            Err(anyhow!("Cannot find system include file: {}", path))
        } else {
            // First try relative to current file
            if let Some(current) = &self.current_file {
                if let Some(parent) = current.parent() {
                    let relative_path = parent.join(path);
                    if relative_path.exists() {
                        return Ok(relative_path);
                    }
                }
            }
            
            // Then search in include directories
            for dir in &self.include_dirs {
                let full_path = dir.join(path);
                if full_path.exists() {
                    return Ok(full_path);
                }
            }
            
            // Finally try as absolute or relative to CWD
            let path_buf = PathBuf::from(path);
            if path_buf.exists() {
                Ok(path_buf)
            } else {
                Err(anyhow!("Cannot find include file: {}", path))
            }
        }
    }
    
    /// Handle define directive implementation
    pub fn handle_define_impl(&mut self, name: String, params: Option<Vec<String>>, body: String, is_variadic: bool) -> Result<()> {
        self.macros.insert(
            name.clone(),
            Macro {
                name,
                params,
                body,
                is_variadic,
            },
        );
        Ok(())
    }
    
    /// Handle undef directive implementation
    pub fn handle_undef_impl(&mut self, name: String) -> Result<()> {
        self.macros.remove(&name);
        Ok(())
    }
    
    /// Evaluate conditional expression
    pub fn evaluate_condition(&self, condition: &str) -> Result<bool> {
        let replaced = replace_defined(condition, |name| {
            name == "__FILE__" || name == "__LINE__" || self.macros.contains_key(name)
        });
        let expanded = self.expand_macros_impl(&replaced)?;
        let value = crate::cond_eval::eval_expanded(&expanded)?;
        Ok(value != 0)
    }
    
    /// Expand macros in text implementation
    pub fn expand_macros_impl(&self, text: &str) -> Result<String> {
        let mut result = text.to_string();
        let mut expanded = true;
        let mut depth = 0;
        const MAX_DEPTH: usize = 100;
        
        while expanded && depth < MAX_DEPTH {
            expanded = false;
            depth += 1;
            
            for (name, macro_def) in &self.macros {
                if macro_def.params.is_none() {
                    // Object-like macro
                    let pattern = format!(r"\b{}\b", regex::escape(name));
                    let re = Regex::new(&pattern)?;
                    
                    if re.is_match(&result) {
                        let body = apply_object_like_body(&macro_def.body);
                        result = re.replace_all(&result, body.as_str()).to_string();
                        expanded = true;
                    }
                } else {
                    // Function-like macro
                    result = self.expand_function_macro(&result, name, macro_def)?;
                    // TODO: Track if expansion occurred
                }
            }
        }
        
        if depth >= MAX_DEPTH {
            return Err(anyhow!("Maximum macro expansion depth exceeded"));
        }

        result = self.expand_predefined(&result)?;
        
        Ok(result)
    }

    fn expand_predefined(&self, text: &str) -> Result<String> {
        let file = match &self.current_file {
            Some(path) => {
                let s = path.display().to_string().replace('\\', "\\\\").replace('"', "\\\"");
                format!("\"{s}\"")
            }
            None => "\"\"".to_string(),
        };
        let re_file = Regex::new(r"\b__FILE__\b")?;
        let with_file = re_file.replace_all(text, file.as_str()).into_owned();

        let re_line = Regex::new(r"\b__LINE__\b")?;
        if !re_line.is_match(&with_file) {
            return Ok(with_file);
        }

        let mut out = String::new();
        for (i, line) in with_file.split_inclusive('\n').enumerate() {
            let n = self.current_line + i;
            out.push_str(&re_line.replace_all(line, n.to_string()));
        }
        Ok(out)
    }
    
    /// Expand function-like macro
    fn expand_function_macro(&self, text: &str, name: &str, macro_def: &Macro) -> Result<String> {
        let params = macro_def.params.as_ref().unwrap();
        
        // Create regex pattern for function-like macro invocation
        let pattern = format!(r"\b{}\s*\(", regex::escape(name));
        let re = Regex::new(&pattern)?;
        
        let mut result = String::new();
        let mut last_end = 0;
        
        for mat in re.find_iter(text) {
            result.push_str(&text[last_end..mat.start()]);
            
            // Parse arguments
            let args_start = mat.end();
            let (args, args_end) = self.parse_macro_arguments(&text[args_start..])?;
            
            if args.len() != params.len() && !macro_def.is_variadic {
                return Err(anyhow!(
                    "Macro '{}' expects {} arguments, got {}",
                    name,
                    params.len(),
                    args.len()
                ));
            }

            let expanded = self.substitute_function_macro_body(macro_def, &args)?;
            
            result.push_str(&expanded);
            last_end = args_start + args_end + 1; // +1 for closing paren
        }
        
        result.push_str(&text[last_end..]);
        Ok(result)
    }
    
    /// Parse macro arguments from text
    fn parse_macro_arguments(&self, text: &str) -> Result<(Vec<String>, usize)> {
        let mut args = Vec::new();
        let mut current_arg = String::new();
        let mut paren_depth = 0;
        let mut in_string = false;
        let mut in_char = false;
        let mut escape = false;
        let mut pos;

        for (i, ch) in text.char_indices() {
            pos = i;
            
            if escape {
                current_arg.push(ch);
                escape = false;
                continue;
            }
            
            match ch {
                '\\' if in_string || in_char => {
                    current_arg.push(ch);
                    escape = true;
                }
                '"' if !in_char => {
                    current_arg.push(ch);
                    in_string = !in_string;
                }
                '\'' if !in_string => {
                    current_arg.push(ch);
                    in_char = !in_char;
                }
                '(' if !in_string && !in_char => {
                    paren_depth += 1;
                    current_arg.push(ch);
                }
                ')' if !in_string && !in_char => {
                    if paren_depth == 0 {
                        // End of arguments
                        if !current_arg.trim().is_empty() {
                            args.push(current_arg.trim().to_string());
                        }
                        return Ok((args, pos));
                    }
                    paren_depth -= 1;
                    current_arg.push(ch);
                }
                ',' if !in_string && !in_char && paren_depth == 0 => {
                    // Argument separator
                    args.push(current_arg.trim().to_string());
                    current_arg.clear();
                }
                _ => {
                    current_arg.push(ch);
                }
            }
        }
        
        Err(anyhow!("Unterminated macro arguments"))
    }

    fn substitute_function_macro_body(&self, macro_def: &Macro, args: &[String]) -> Result<String> {
        let params = macro_def.params.as_ref().unwrap();
        let va_args = if macro_def.is_variadic && args.len() > params.len() {
            Some(args[params.len()..].join(", "))
        } else if macro_def.is_variadic {
            Some(String::new())
        } else {
            None
        };

        let mut expanded_args: Vec<String> = Vec::new();
        for arg in args.iter().take(params.len()) {
            expanded_args.push(self.expand_macros_impl(arg)?);
        }

        let tokens = tokenize_macro_body(&macro_def.body);
        let mut out = String::new();
        let mut i = 0;
        while i < tokens.len() {
            match &tokens[i] {
                BodyTok::Hash => {
                    let j = skip_ws(&tokens, i + 1);
                    if let Some(BodyTok::Ident(name)) = tokens.get(j) {
                        if name == "__VA_ARGS__" {
                            let raw = va_args.clone().unwrap_or_default();
                            out.push_str(&stringize_arg(&raw));
                            i = j + 1;
                            continue;
                        }
                        if let Some(idx) = params.iter().position(|p| p == name) {
                            let raw = args.get(idx).map(|s| s.as_str()).unwrap_or("");
                            out.push_str(&stringize_arg(raw));
                            i = j + 1;
                            continue;
                        }
                    }
                    out.push('#');
                    i += 1;
                }
                BodyTok::Ident(name) | BodyTok::Other(name) if looks_like_paste_left(&tokens, i) => {
                    let left = self.resolve_paste_operand(name, params, args, va_args.as_deref());
                    let after_paste = skip_ws(&tokens, skip_ws(&tokens, i + 1) + 1);
                    let right_tok = tokens.get(after_paste);
                    let right = match right_tok {
                        Some(BodyTok::Ident(s) | BodyTok::Other(s)) => {
                            self.resolve_paste_operand(s, params, args, va_args.as_deref())
                        }
                        _ => String::new(),
                    };
                    out.push_str(&left);
                    out.push_str(&right);
                    i = after_paste + 1;
                }
                BodyTok::Ident(name) => {
                    if name == "__VA_ARGS__" {
                        if let Some(va) = &va_args {
                            out.push_str(va);
                        }
                    } else if let Some(idx) = params.iter().position(|p| p == name) {
                        if let Some(expanded) = expanded_args.get(idx) {
                            out.push_str(expanded);
                        }
                    } else {
                        out.push_str(name);
                    }
                    i += 1;
                }
                BodyTok::Paste => {
                    // Leading ## is a no-op paste (GNU/C99 placemarker).
                    i += 1;
                }
                BodyTok::Other(s) => {
                    out.push_str(s);
                    i += 1;
                }
            }
        }
        Ok(out)
    }

    fn resolve_paste_operand(
        &self,
        token: &str,
        params: &[String],
        args: &[String],
        va_args: Option<&str>,
    ) -> String {
        if token == "__VA_ARGS__" {
            va_args.unwrap_or("").to_string()
        } else if let Some(idx) = params.iter().position(|p| p == token) {
            args.get(idx).cloned().unwrap_or_default()
        } else {
            token.to_string()
        }
    }
}

fn replace_defined(condition: &str, is_defined: impl Fn(&str) -> bool) -> String {
    let re_paren = Regex::new(r"defined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)").unwrap();
    let re_space = Regex::new(r"defined\s+([A-Za-z_][A-Za-z0-9_]*)").unwrap();
    let step = re_paren.replace_all(condition, |caps: &regex::Captures| {
        if is_defined(&caps[1]) { "1" } else { "0" }
    });
    re_space.replace_all(&step, |caps: &regex::Captures| {
        if is_defined(&caps[1]) { "1" } else { "0" }
    }).into_owned()
}

#[derive(Debug, Clone)]
enum BodyTok {
    Ident(String),
    Hash,
    Paste,
    Other(String),
}

fn tokenize_macro_body(body: &str) -> Vec<BodyTok> {
    let chars: Vec<char> = body.chars().collect();
    let mut i = 0;
    let mut toks = Vec::new();
    while i < chars.len() {
        if chars[i] == '#' {
            if i + 1 < chars.len() && chars[i + 1] == '#' {
                toks.push(BodyTok::Paste);
                i += 2;
            } else {
                toks.push(BodyTok::Hash);
                i += 1;
            }
        } else if chars[i].is_ascii_alphabetic() || chars[i] == '_' {
            let start = i;
            i += 1;
            while i < chars.len() && (chars[i].is_ascii_alphanumeric() || chars[i] == '_') {
                i += 1;
            }
            toks.push(BodyTok::Ident(chars[start..i].iter().collect()));
        } else if chars[i].is_ascii_digit() {
            let start = i;
            while i < chars.len() && (chars[i].is_ascii_alphanumeric() || chars[i] == '.') {
                i += 1;
            }
            toks.push(BodyTok::Other(chars[start..i].iter().collect()));
        } else if chars[i].is_whitespace() {
            let start = i;
            while i < chars.len() && chars[i].is_whitespace() {
                i += 1;
            }
            toks.push(BodyTok::Other(chars[start..i].iter().collect()));
        } else {
            toks.push(BodyTok::Other(chars[i].to_string()));
            i += 1;
        }
    }
    toks
}

fn skip_ws(tokens: &[BodyTok], mut i: usize) -> usize {
    while i < tokens.len() {
        match &tokens[i] {
            BodyTok::Other(s) if s.chars().all(char::is_whitespace) => i += 1,
            _ => break,
        }
    }
    i
}

fn looks_like_paste_left(tokens: &[BodyTok], i: usize) -> bool {
    let after = skip_ws(tokens, i + 1);
    matches!(tokens.get(after), Some(BodyTok::Paste))
}

fn stringize_arg(arg: &str) -> String {
    let collapsed = arg.split_whitespace().collect::<Vec<_>>().join(" ");
    let escaped = collapsed.replace('\\', "\\\\").replace('"', "\\\"");
    format!("\"{escaped}\"")
}

fn apply_object_like_body(body: &str) -> String {
    let tokens = tokenize_macro_body(body);
    let mut out = String::new();
    let mut i = 0;
    while i < tokens.len() {
        if looks_like_paste_left(&tokens, i) {
            let left = match &tokens[i] {
                BodyTok::Ident(s) | BodyTok::Other(s) => s.clone(),
                BodyTok::Hash => "#".to_string(),
                BodyTok::Paste => String::new(),
            };
            let after = skip_ws(&tokens, skip_ws(&tokens, i + 1) + 1);
            let right = match tokens.get(after) {
                Some(BodyTok::Ident(s) | BodyTok::Other(s)) => s.clone(),
                Some(BodyTok::Hash) => "#".to_string(),
                _ => String::new(),
            };
            out.push_str(&left);
            out.push_str(&right);
            i = after + 1;
        } else {
            match &tokens[i] {
                BodyTok::Ident(s) | BodyTok::Other(s) => out.push_str(s),
                BodyTok::Hash => out.push('#'),
                BodyTok::Paste => {}
            }
            i += 1;
        }
    }
    out
}