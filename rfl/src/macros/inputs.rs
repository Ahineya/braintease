//! Declared UTF-8 build inputs. Macros never open files themselves.
use crate::CompileError;
use std::path::Path;
#[derive(Clone, Debug)]
pub struct Dependency {
    pub path: String,
    pub text: String,
    pub hash: String,
}
pub fn declared(package: &Path) -> Result<(String, Vec<Dependency>), CompileError> {
    let manifest = package.with_extension("rfl.inputs");
    let declaration = match super::source::read(&manifest) {
        Ok(s) => s,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return Ok((String::new(), Vec::new()))
        }
        Err(e) => {
            return Err(CompileError::plain(format!(
                "cannot read {}: {e}",
                manifest.display()
            )))
        }
    };
    if declaration.len() > 4096 {
        return Err(CompileError::plain(
            "macro input manifest exceeds 4096 octets",
        ));
    }
    let mut result = Vec::<Dependency>::new();
    let mut total = 0;
    for name in declaration
        .lines()
        .map(str::trim)
        .filter(|n| !n.is_empty() && !n.starts_with('#'))
    {
        if result.len() >= 32 {
            return Err(CompileError::plain("macro declares more than 32 inputs"));
        }
        if result.iter().any(|d| d.path == name) {
            return Err(CompileError::plain("duplicate declared macro input"));
        }
        let path = package.parent().unwrap().join(name);
        let text = super::source::read(&path).map_err(|e| {
            CompileError::plain(format!(
                "cannot read declared macro input {}: {e}",
                path.display()
            ))
        })?;
        total += text.len();
        if total > 20000 {
            return Err(CompileError::plain(
                "declared macro inputs exceed 20000 UTF-8 octets",
            ));
        }
        // Versioned stable content fingerprint for invalidation, not an
        // authentication/security digest. Cache keys also include full content.
        let mut hash = 0xcbf29ce484222325u64;
        for byte in text.bytes() {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
        result.push(Dependency {
            path: name.into(),
            text,
            hash: format!("fnv1a64:{hash:016x}"),
        });
    }
    Ok((declaration, result))
}
