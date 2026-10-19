pub fn uppercase(input: &[u8], output: &[u8]) -> rustcell::Result<()> {
    let mut bytes = rustcell::fs::read(input)?;
    for i in 0..bytes.len() {
        let byte = bytes[i];
        if byte >= b'a' && byte <= b'z' {
            bytes[i] = byte - 32;
        }
    }
    rustcell::fs::write(output, &bytes)
}
