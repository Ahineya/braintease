//! C streams use checked logical objects. Host File pointers never enter IR.
use super::memory::linear;
use super::*;
use std::{
    fs::OpenOptions,
    io::{BufReader, ErrorKind, Read, Seek, SeekFrom, Write},
};

pub(super) struct Stream {
    file: BufReader<std::fs::File>,
    eof: bool,
    error: bool,
}
impl Runtime {
    fn file_error(&mut self, code: u16) -> u64 {
        if self.errno != 0 {
            self.memory[linear(self.errno)] = code;
        }
        u64::MAX
    }
    fn io_error(&mut self, error: std::io::Error) -> u64 {
        self.file_error(error.raw_os_error().unwrap_or(5) as u16)
    }
    fn c_string(&self, ptr: u64) -> Vec<u8> {
        let mut out = Vec::new();
        for &c in &self.memory[linear(ptr)..] {
            if c == 0 {
                return out;
            }
            if c > 255 {
                fatal("native file string is not an octet string");
            }
            out.push(c as u8);
        }
        fatal("unterminated native file string")
    }
    fn file_span(&self, ptr: u64, count: u64) -> std::ops::Range<usize> {
        let start = linear(ptr);
        let end = start
            .checked_add(usize::try_from(count).unwrap_or_else(|_| fatal("file span overflow")))
            .filter(|e| *e <= CELLS)
            .unwrap_or_else(|| fatal("file buffer out of bounds"));
        start..end
    }
    fn write_bytes(&mut self, stream: u64, bytes: &[u8]) -> usize {
        if let Some(file) = self.files.get_mut(&stream) {
            // Discard the read buffer at its logical cursor before writing.
            let result = file
                .file
                .seek(SeekFrom::Current(0))
                .and_then(|_| file.file.get_mut().write_all(bytes));
            match result {
                Ok(()) => bytes.len(),
                Err(error) => {
                    file.error = true;
                    self.io_error(error);
                    0
                }
            }
        } else if stream != 0 && matches!(self.memory[linear(stream)], 1 | 2) {
            for &byte in bytes {
                self.putc(u64::from(byte));
            }
            bytes.len()
        } else {
            self.file_error(9);
            0
        }
    }
}
// Generated code exclusively borrows the live runtime for each synchronous call.
macro_rules! file_service {
    ($name:ident($rt:ident $(,$arg:ident)*) $body:block) => {
        #[no_mangle]
        unsafe extern "C" fn $name(ctx: *mut Runtime, $($arg: u64),*) -> u64 {
            let $rt = unsafe { &mut *ctx };
            $body
        }
    };
}
fn host_path(bytes: Vec<u8>) -> std::ffi::OsString {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStringExt;
        std::ffi::OsString::from_vec(bytes)
    }
    #[cfg(not(unix))]
    {
        std::ffi::OsString::from(String::from_utf8_lossy(&bytes).into_owned())
    }
}
file_service!(rcc_native_ext_remove(rt, path) {
    let path = host_path(rt.c_string(path));
    let result = if std::fs::symlink_metadata(&path).is_ok_and(|m| m.is_dir()) {
        std::fs::remove_dir(path)
    } else {
        std::fs::remove_file(path)
    };
    match result { Ok(()) => 0, Err(error) => rt.io_error(error) }
});
file_service!(rcc_native_ext_rename(rt, old, new) {
    let result = std::fs::rename(host_path(rt.c_string(old)), host_path(rt.c_string(new)));
    match result { Ok(()) => 0, Err(error) => rt.io_error(error) }
});
file_service!(rcc_native_ext_fopen(rt, path, mode) {
    let path = rt.c_string(path);
    let mode = rt.c_string(mode);
    let mut options = OpenOptions::new();
    let update = mode.contains(&b'+');
    match mode.first() {
        Some(b'r') => { options.read(true).write(update); }
        Some(b'w') => { options.write(true).read(update).create(true).truncate(true); }
        Some(b'a') => { options.append(true).read(update).create(true); }
        _ => { rt.file_error(22); return 0; }
    }
    match options.open(host_path(path)) {
        Ok(file) => {
            let ptr = rt.allocate(1);
            if ptr == 0 { rt.file_error(12); return 0; }
            rt.memory[linear(ptr)] = 3;
            rt.files.insert(ptr, Stream { file: BufReader::new(file), eof: false, error: false });
            ptr
        }
        Err(error) => { rt.io_error(error); 0 }
    }
});
file_service!(rcc_native_ext_fclose(rt, stream) {
    if rt.files.remove(&stream).is_none() { return rt.file_error(9); }
    rt.release(stream);
    0
});
file_service!(rcc_native_ext_fread(rt, buffer, size, count, stream) {
    let Some(total) = size.checked_mul(count) else { rt.file_error(75); return 0; };
    if total == 0 { return 0; }
    let range = rt.file_span(buffer, total);
    let Some(file) = rt.files.get_mut(&stream) else { rt.file_error(9); return 0; };
    let mut bytes = vec![0; range.len()];
    let mut got = 0;
    let mut error = None;
    while got < bytes.len() {
        match file.file.read(&mut bytes[got..]) {
            Ok(0) => { file.eof = true; break; }
            Ok(n) => got += n,
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) => { file.error = true; error = Some(e); break; }
        }
    }
    for (dest, byte) in rt.memory[range.start..range.start + got].iter_mut().zip(bytes) {
        *dest = u16::from(byte);
    }
    if let Some(error) = error { rt.io_error(error); }
    got as u64 / size
});
file_service!(rcc_native_ext_fgetc(rt, stream) {
    let Some(file) = rt.files.get_mut(&stream) else { return rt.file_error(9); };
    let mut byte = [0];
    loop {
        match file.file.read(&mut byte) {
            Ok(1) => return u64::from(byte[0]),
            Ok(_) => { file.eof = true; return 65535; }
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) => { file.error = true; return rt.io_error(e); }
        }
    }
});
file_service!(rcc_native_ext_getc(rt, stream) {
    unsafe { rcc_native_ext_fgetc(rt, stream) }
});
file_service!(rcc_native_ext_fseek(rt, stream, offset, origin) {
    let offset = offset as u32 as i32 as i64;
    let position = match origin {
        0 if offset >= 0 => SeekFrom::Start(offset as u64),
        1 => SeekFrom::Current(offset),
        2 => SeekFrom::End(offset),
        _ => return rt.file_error(22),
    };
    let Some(file) = rt.files.get_mut(&stream) else { return rt.file_error(9); };
    match file.file.seek(position) {
        Ok(_) => { file.eof = false; 0 }
        Err(error) => { file.error = true; rt.io_error(error) }
    }
});
file_service!(rcc_native_ext_ftell(rt, stream) {
    let Some(file) = rt.files.get_mut(&stream) else { return rt.file_error(9); };
    match file.file.stream_position() {
        Ok(n) if n <= i32::MAX as u64 => n,
        Ok(_) => rt.file_error(75),
        Err(error) => rt.io_error(error),
    }
});
file_service!(rcc_native_ext_feof(rt, stream) {
    if let Some(f) = rt.files.get(&stream) { u64::from(f.eof) }
    else { rt.file_error(9); 1 }
});
file_service!(rcc_native_ext_ferror(rt, stream) {
    if let Some(f) = rt.files.get(&stream) { u64::from(f.error) }
    else { rt.file_error(9); 1 }
});
file_service!(rcc_native_ext_fflush(rt, stream) {
    if stream == 0 { rt.flush(); return 0; }
    if let Some(file) = rt.files.get_mut(&stream) {
        match file.file.get_mut().flush() {
            Ok(()) => 0,
            Err(error) => { file.error = true; rt.io_error(error) }
        }
    } else if matches!(rt.memory[linear(stream)], 1 | 2) {
        rt.flush(); 0
    } else { rt.file_error(9) }
});
file_service!(rcc_native_ext_fwrite(rt, buffer, size, count, stream) {
    let Some(total) = size.checked_mul(count) else { rt.file_error(75); return 0; };
    if total == 0 { return 0; }
    let range = rt.file_span(buffer, total);
    let bytes: Vec<u8> = rt.memory[range].iter().map(|v| *v as u8).collect();
    rt.write_bytes(stream, &bytes) as u64 / size
});
file_service!(rcc_native_ext_fputc(rt, value, stream) {
    if rt.write_bytes(stream, &[value as u8]) == 1 { value & 255 } else { 65535 }
});
file_service!(rcc_native_ext_putc(rt, value, stream) {
    unsafe { rcc_native_ext_fputc(rt, value, stream) }
});
file_service!(rcc_native_ext_fputs(rt, text, stream) {
    let bytes = rt.c_string(text);
    if rt.write_bytes(stream, &bytes) == bytes.len() { 0 } else { 65535 }
});
#[no_mangle]
unsafe extern "C" fn rcc_native_errno(ctx: *mut Runtime, address: u64) -> u64 {
    unsafe {
        (*ctx).errno = address;
    }
    0
}
#[no_mangle]
unsafe extern "C" fn rcc_native_arguments(ctx: *mut Runtime) -> u64 {
    let rt = unsafe { &mut *ctx };
    let args: Vec<_> = std::env::args_os().collect();
    let table = rt.allocate(((args.len() + 1) * 2) as u64);
    if table == 0 {
        fatal("cannot allocate argv");
    }
    for (i, arg) in args.iter().enumerate() {
        let bytes = arg.as_encoded_bytes();
        let text = rt.allocate(bytes.len() as u64 + 1);
        if text == 0 {
            fatal("cannot allocate argv string");
        }
        let at = linear(text);
        for (j, b) in bytes.iter().enumerate() {
            rt.memory[at + j] = u16::from(*b);
        }
        rt.memory[at + bytes.len()] = 0;
        let at = linear(table) + i * 2;
        rt.memory[at] = text as u16;
        rt.memory[at + 1] = (text >> 16) as u16;
    }
    let end = linear(table) + args.len() * 2;
    rt.memory[end..end + 2].fill(0);
    (args.len() as u64) << 32 | table
}
