use std::fmt;

#[derive(Debug)]
pub struct CompileError {
    pub message: String,
}

impl CompileError {
    pub fn at(source: &str, offset: usize, message: impl Into<String>) -> Self {
        let (line, column) = line_col(source, offset);
        Self {
            message: format!("{line}:{column}: {}", message.into()),
        }
    }

    pub fn plain(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for CompileError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for CompileError {}

pub fn line_col(source: &str, offset: usize) -> (u32, u32) {
    let offset = offset.min(source.len());
    let mut line = 1u32;
    let mut column = 1u32;
    for (index, ch) in source.char_indices() {
        if index >= offset {
            break;
        }
        if ch == '\n' {
            line += 1;
            column = 1;
        } else {
            column += 1;
        }
    }
    (line, column)
}
