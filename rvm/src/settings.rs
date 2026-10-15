use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Serialize, Deserialize)]
pub struct DebuggerSettings {
    // Panel visibility
    pub show_registers: bool,
    pub show_memory: bool,
    pub show_stack: bool,
    pub show_watches: bool,
    pub show_breakpoints: bool,
    pub show_output: bool,

    // Display preferences
    pub show_ascii: bool,
    pub show_instruction_hex: bool,
}

impl Default for DebuggerSettings {
    fn default() -> Self {
        Self {
            show_registers: true,
            show_memory: true,
            show_stack: true,
            show_watches: true,
            show_breakpoints: true,
            show_output: true,
            show_ascii: true,
            show_instruction_hex: true, // Default to hex view
        }
    }
}

impl DebuggerSettings {
    /// Get the path to the settings file
    fn settings_path() -> PathBuf {
        // Try to use XDG config directory on Unix-like systems
        if let Ok(config_dir) = std::env::var("XDG_CONFIG_HOME") {
            PathBuf::from(config_dir).join("rvm").join("debugger.json")
        } else if let Ok(home) = std::env::var("HOME") {
            PathBuf::from(home)
                .join(".config")
                .join("rvm")
                .join("debugger.json")
        } else {
            // Fallback to current directory
            PathBuf::from(".rvm_debugger.json")
        }
    }

    /// Load settings from disk, or return defaults if file doesn't exist
    pub fn load() -> Self {
        let path = Self::settings_path();

        if path.exists() {
            match fs::read_to_string(&path) {
                Ok(contents) => match serde_json::from_str(&contents) {
                    Ok(settings) => return settings,
                    Err(e) => {
                        eprintln!("Failed to parse debugger settings: {e}");
                    }
                },
                Err(e) => {
                    eprintln!("Failed to read debugger settings: {e}");
                }
            }
        }

        // Return defaults if we couldn't load
        Self::default()
    }

    /// Save settings to disk
    pub fn save(&self) -> Result<(), String> {
        let path = Self::settings_path();

        // Create parent directory if it doesn't exist
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)
                .map_err(|e| format!("Failed to create config directory: {e}"))?;
        }

        let json = serde_json::to_string_pretty(self)
            .map_err(|e| format!("Failed to serialize settings: {e}"))?;

        fs::write(&path, json).map_err(|e| format!("Failed to write settings: {e}"))?;

        Ok(())
    }
}
