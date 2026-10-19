//! A streaming binary inspector, built from the unmodified byteorder crate.
#![cfg_attr(target_os = "fuzix", no_std)]
#![cfg_attr(target_os = "fuzix", no_main)]
extern crate alloc;
use alloc::vec::Vec;
use byteorder::{ByteOrder, LittleEndian};

fn hex(value: u32, digits: u32, output: &mut Vec<u8>) {
    for index in (0..digits).rev() {
        let nibble = ((value >> (index * 4)) & 15) as u8;
        output.push(if nibble < 10 {
            b'0' + nibble
        } else {
            b'a' + nibble - 10
        });
    }
}

fn inspect(
    mut read: impl FnMut(&mut [u8]) -> Result<usize, ()>,
    mut write: impl FnMut(&[u8]) -> Result<(), ()>,
) -> Result<(), ()> {
    let mut bytes = [0u8; 16];
    let mut line = Vec::with_capacity(128);
    let mut used = 0;
    let mut offset = 0u32;
    loop {
        let count = read(&mut bytes[used..])?;
        used += count;
        if used == 16 || (count == 0 && used != 0) {
            line.clear();
            hex(offset, 8, &mut line);
            line.extend_from_slice(b"  ");
            for index in 0..16 {
                if index < used {
                    hex(bytes[index] as u32, 2, &mut line);
                } else {
                    line.extend_from_slice(b"  ");
                }
                line.push(b' ');
            }
            line.extend_from_slice(b" |");
            for byte in &bytes[..used] {
                line.push(if (32..=126).contains(byte) {
                    *byte
                } else {
                    b'.'
                });
            }
            line.extend_from_slice(b"| le32:");
            for word in bytes[..used].chunks_exact(4) {
                line.push(b' ');
                hex(LittleEndian::read_u32(word), 8, &mut line);
            }
            line.push(b'\n');
            write(&line)?;
            offset = offset.checked_add(used as u32).ok_or(())?;
            used = 0;
        }
        if count == 0 {
            return Ok(());
        }
    }
}

#[cfg(target_os = "fuzix")]
unsafe extern "C" {
    fn rustcell_panic() -> !;
}
#[cfg(target_os = "fuzix")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}

#[cfg(target_os = "fuzix")]
pub fn rust_entry() -> u16 {
    let arguments = match rustcell::env::args() {
        Ok(value) => value,
        Err(_) => return 1,
    };
    if arguments.len() != 2 {
        let _ = rustcell::io::write_stderr(b"usage: binspect FILE\n");
        return 2;
    }
    let mut file = match rustcell::fs::File::open(&arguments[1]) {
        Ok(value) => value,
        Err(_) => {
            let _ = rustcell::io::write_stderr(b"binspect: cannot open file\n");
            return 1;
        }
    };
    if inspect(
        |bytes| file.read(bytes).map_err(|_| ()),
        |bytes| rustcell::io::write_stdout(bytes).map_err(|_| ()),
    )
    .is_err()
    {
        let _ = rustcell::io::write_stderr(b"binspect: I/O error\n");
        return 1;
    }
    0
}

#[cfg(not(target_os = "fuzix"))]
fn main() -> std::process::ExitCode {
    use std::io::{Read, Write};
    let arguments: Vec<_> = std::env::args_os().collect();
    if arguments.len() != 2 {
        eprintln!("usage: binspect FILE");
        return std::process::ExitCode::from(2);
    }
    let mut file = match std::fs::File::open(&arguments[1]) {
        Ok(file) => file,
        Err(_) => {
            eprintln!("binspect: cannot open file");
            return std::process::ExitCode::from(1);
        }
    };
    // Deliberately short host reads exercise row assembly independently of the
    // guest filesystem's ordinary reads. Both must produce identical output.
    match inspect(
        |bytes| {
            let length = bytes.len().min(3);
            file.read(&mut bytes[..length]).map_err(|_| ())
        },
        |bytes| std::io::stdout().write_all(bytes).map_err(|_| ()),
    ) {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(()) => std::process::ExitCode::from(1),
    }
}
