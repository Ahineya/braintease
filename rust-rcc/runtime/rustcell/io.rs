use crate::{Error, Result, ffi, result};

pub(crate) fn read(fd: i16, buffer: &mut [u8]) -> Result<usize> {
    if buffer.is_empty() {
        return Ok(0);
    }
    let count = result(unsafe { ffi::rustcell_fd_read(fd, buffer.as_mut_ptr(), buffer.len()) })?;
    if count > buffer.len() {
        Err(Error::InvalidData)
    } else {
        Ok(count)
    }
}

pub(crate) fn write(fd: i16, bytes: &[u8]) -> Result<usize> {
    if bytes.is_empty() {
        return Ok(0);
    }
    let count = result(unsafe { ffi::rustcell_fd_write(fd, bytes.as_ptr(), bytes.len()) })?;
    if count > bytes.len() {
        Err(Error::InvalidData)
    } else {
        Ok(count)
    }
}

pub(crate) fn write_all(fd: i16, mut bytes: &[u8]) -> Result<()> {
    while !bytes.is_empty() {
        let count = write(fd, bytes)?;
        if count == 0 {
            return Err(Error::WriteZero);
        }
        bytes = &bytes[count..];
    }
    Ok(())
}

pub fn read_stdin(buffer: &mut [u8]) -> Result<usize> {
    read(0, buffer)
}
pub fn write_stdout(bytes: &[u8]) -> Result<()> {
    write_all(1, bytes)
}
pub fn write_stderr(bytes: &[u8]) -> Result<()> {
    write_all(2, bytes)
}
