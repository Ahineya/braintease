//! Versioned, octet-framed bootstrap protocol. No compiler pointers cross it.
use super::syntax::MAX_TOKENS;
use crate::CompileError;
pub const VERSION: &str = "RFLM2\n";
pub const MAX_FRAME: usize = 60_000;
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WireToken {
    pub kind: i16,
    pub text: String,
    pub origin: String,
    pub syntax: String,
}
fn number(out: &mut Vec<u8>, n: usize) {
    out.extend_from_slice(n.to_string().as_bytes());
    out.push(b'\n');
}
fn field(out: &mut Vec<u8>, text: &str) {
    number(out, text.len());
    out.extend_from_slice(text.as_bytes());
    out.push(b'\n');
}
fn frame(body: Vec<u8>) -> Result<Vec<u8>, CompileError> {
    if body.len() > MAX_FRAME {
        return Err(CompileError::plain(
            "macro protocol frame exceeds 60000 octets",
        ));
    }
    let mut out = VERSION.as_bytes().to_vec();
    number(&mut out, body.len());
    out.extend(body);
    Ok(out)
}
pub fn request(
    target: &str,
    tokens: &[WireToken],
    dependencies: &[super::inputs::Dependency],
) -> Result<Vec<u8>, CompileError> {
    if tokens.len() > MAX_TOKENS {
        return Err(CompileError::plain("macro token limit exceeded"));
    }
    let mut body = Vec::new();
    field(&mut body, target);
    number(&mut body, dependencies.len());
    for dep in dependencies {
        field(&mut body, &dep.path);
        field(&mut body, &dep.text);
        field(&mut body, &dep.hash);
    }
    number(&mut body, tokens.len());
    for token in tokens {
        number(&mut body, token.kind as usize);
        field(&mut body, &token.text);
        field(&mut body, &token.origin);
        field(&mut body, &token.syntax);
    }
    frame(body)
}
struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}
impl Reader<'_> {
    fn fail(&self) -> CompileError {
        CompileError::plain("malformed macro protocol response")
    }
    fn number(&mut self) -> Result<usize, CompileError> {
        let mut n = 0usize;
        let start = self.at;
        loop {
            let ch = *self.bytes.get(self.at).ok_or_else(|| self.fail())?;
            self.at += 1;
            if ch == b'\n' && self.at > start + 1 {
                return Ok(n);
            }
            if !ch.is_ascii_digit() || self.at - start > 9 {
                return Err(self.fail());
            }
            n = n
                .checked_mul(10)
                .and_then(|n| n.checked_add((ch - b'0') as usize))
                .ok_or_else(|| self.fail())?;
        }
    }
    fn field(&mut self) -> Result<String, CompileError> {
        let n = self.number()?;
        let end = self
            .at
            .checked_add(n)
            .filter(|&end| end < self.bytes.len())
            .ok_or_else(|| self.fail())?;
        if n > MAX_FRAME || self.bytes[end] != b'\n' {
            return Err(self.fail());
        }
        let text = std::str::from_utf8(&self.bytes[self.at..end])
            .map_err(|_| self.fail())?
            .to_owned();
        self.at = end + 1;
        Ok(text)
    }
}
pub fn response(bytes: &[u8]) -> Result<Vec<WireToken>, CompileError> {
    if bytes.len() > MAX_FRAME + 32 || !bytes.starts_with(VERSION.as_bytes()) {
        return Err(CompileError::plain(
            "invalid macro protocol version or excess output",
        ));
    }
    let mut reader = Reader {
        bytes,
        at: VERSION.len(),
    };
    let len = reader.number()?;
    if len > MAX_FRAME || reader.at + len != bytes.len() {
        return Err(CompileError::plain("macro protocol frame length mismatch"));
    }
    let count = reader.number()?;
    if count > MAX_TOKENS {
        return Err(CompileError::plain("macro generated token limit exceeded"));
    }
    let mut tokens = Vec::new();
    for _ in 0..count {
        let kind = i16::try_from(reader.number()?).map_err(|_| reader.fail())?;
        tokens.push(WireToken {
            kind,
            text: reader.field()?,
            origin: reader.field()?,
            syntax: reader.field()?,
        });
    }
    if reader.at != bytes.len() {
        return Err(CompileError::plain("trailing macro protocol output"));
    }
    Ok(tokens)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn framing_rejects_wrong_versions_lengths_utf8_counts_and_trailing_output() {
        for value in [
            b"wrong\n0\n".as_slice(),
            b"RFLM2\n99999999999999999\n",
            b"RFLM2\n2\n0\nextra",
            b"RFLM2\n2\n1\n",
            b"RFLM2\n2\n-1",
            b"RFLM2\n5\n0\n0\n0",
        ] {
            assert!(response(value).is_err(), "{value:?}");
        }
        let invalid_utf8 = frame(b"1\n1\n1\n\xff\n0\n\n0\n\n".to_vec()).unwrap();
        assert!(response(&invalid_utf8).is_err());
        assert_eq!(response(&frame(b"0\n".to_vec()).unwrap()).unwrap(), vec![]);
    }
}
