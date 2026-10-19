//! Small no_std OS bindings for the Rust/RCC cell ABI. This is not std.
#![no_std]
extern crate alloc;
pub mod env;
pub mod fs;
pub mod io;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// The guest C library's errno number, captured at the failing call.
    Os(i16),
    InvalidInput,
    InvalidData,
    WriteZero,
    UnexpectedEof,
}
pub type Result<T> = core::result::Result<T, Error>;

fn result(value: isize) -> Result<usize> {
    if value < 0 {
        Err(Error::Os((-value) as i16))
    } else {
        Ok(value as usize)
    }
}

mod ffi {
    unsafe extern "C" {
        pub fn rustcell_argc() -> usize;
        pub fn rustcell_arg(index: usize, bytes: *mut u8, capacity: usize) -> isize;
        pub fn rustcell_open(bytes: *const u8, length: usize, options: u16) -> isize;
        pub fn rustcell_fd_read(fd: i16, bytes: *mut u8, length: usize) -> isize;
        pub fn rustcell_fd_write(fd: i16, bytes: *const u8, length: usize) -> isize;
        pub fn rustcell_seek(fd: i16, offset: i32, origin: u16) -> isize;
        pub fn rustcell_close(fd: i16) -> isize;
        pub fn rustcell_unlink(bytes: *const u8, length: usize) -> isize;
    }
}
