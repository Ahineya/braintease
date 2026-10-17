use crate::{Macro, Preprocessor};
use anyhow::{anyhow, Result};
use regex::Regex;
use once_cell::sync::Lazy;
use std::fs;
use std::path::{PathBuf};

static DEFINED_PAREN: Lazy<Regex> = Lazy::new(|| Regex::new(r"defined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)").unwrap());
static DEFINED_SPACE: Lazy<Regex> = Lazy::new(|| Regex::new(r"defined\s+([A-Za-z_][A-Za-z0-9_]*)").unwrap());

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
        self.insert_macro(Macro {
            name,
            params,
            body,
            is_variadic,
        });
        Ok(())
    }
    
    /// Handle undef directive implementation
    pub fn handle_undef_impl(&mut self, name: String) -> Result<()> {
        self.undefine(&name);
        Ok(())
    }
    
    /// Evaluate conditional expression
    pub fn evaluate_condition(&self, condition: &str) -> Result<bool> {
        let replaced = replace_defined(condition, |name| self.macro_is_defined(name));
        let expanded = self.expand_macros_impl(&replaced)?;
        let value = crate::cond_eval::eval_expanded(&expanded)?;
        Ok(value != 0)
    }
    
}

fn replace_defined(condition: &str, is_defined: impl Fn(&str) -> bool) -> String {
    let step = DEFINED_PAREN.replace_all(condition, |caps: &regex::Captures| {
        if is_defined(&caps[1]) { "1" } else { "0" }
    });
    DEFINED_SPACE.replace_all(&step, |caps: &regex::Captures| {
        if is_defined(&caps[1]) { "1" } else { "0" }
    }).into_owned()
}
