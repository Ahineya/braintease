//! Argument bytes are owned copies, with no trailing NUL and no UTF-8 coercion.
use crate::{Error, Result, ffi, result};
use alloc::vec::Vec;

pub fn args() -> Result<Vec<Vec<u8>>> {
    let count = unsafe { ffi::rustcell_argc() };
    let mut arguments = Vec::new();
    for index in 0..count {
        let length = result(unsafe { ffi::rustcell_arg(index, core::ptr::null_mut(), 0) })?;
        let mut bytes = Vec::new();
        bytes.resize(length, 0);
        if length != 0 {
            let copied = result(unsafe { ffi::rustcell_arg(index, bytes.as_mut_ptr(), length) })?;
            if copied != length {
                return Err(Error::InvalidData);
            }
        }
        arguments.push(bytes);
    }
    Ok(arguments)
}
