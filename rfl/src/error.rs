use std::fmt;
use std::path::Path;

#[derive(Debug)]
pub struct CompileError {
    pub message: String,
}

impl CompileError {
    pub fn at(source: &str, offset: usize, message: impl Into<String>) -> Self {
        Self {
            message: render(None, source, offset, &message.into()),
        }
    }

    pub fn at_file(path: &Path, source: &str, offset: usize, message: impl Into<String>) -> Self {
        Self {
            message: render(Some(path), source, offset, &message.into()),
        }
    }

    pub fn plain(message: impl Into<String>) -> Self {
        Self {
            message: format!("error: {}", message.into()),
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

fn render(path: Option<&Path>, source: &str, offset: usize, message: &str) -> String {
    let (line_no, column) = line_col(source, offset);
    let file = path.map(|path| path.display().to_string()).unwrap_or_else(|| "<source>".to_string());
    let line = source_line(source, line_no);
    let gutter = line_no.to_string().len();
    let mut out = String::new();
    out.push_str(&format!("error: {message}\n"));
    out.push_str(&format!("{blank:>width$}--> {file}:{line_no}:{column}\n", blank = "", width = gutter));
    out.push_str(&format!("{blank:>width$} |\n", blank = "", width = gutter));
    out.push_str(&format!("{line_no:>width$} | {line}\n", width = gutter));
    let caret_at = column.saturating_sub(1) as usize;
    out.push_str(&format!(
        "{blank:>width$} | {pad}^\n",
        blank = "",
        width = gutter,
        pad = " ".repeat(caret_at)
    ));
    out
}

fn source_line(source: &str, line_no: u32) -> String {
    source
        .lines()
        .nth(line_no.saturating_sub(1) as usize)
        .unwrap_or("")
        .trim_end_matches('\r')
        .to_string()
}
