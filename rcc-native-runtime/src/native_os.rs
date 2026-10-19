//! Standalone host POSIX adapters. Checked compatibility buffers are copied;
//! host descriptors, directory iterators and pointers never enter RFL memory.
use super::{
    memory::{linear, pointer},
    *,
};
use std::{
    ffi::{CString, OsString},
    fs::{File, ReadDir},
    io,
    os::{
        fd::{AsRawFd, FromRawFd, IntoRawFd},
        unix::{
            ffi::OsStringExt,
            fs::{MetadataExt, PermissionsExt},
            process::{CommandExt, ExitStatusExt},
        },
    },
    process::Command,
};

#[derive(Default)]
pub(super) struct NativeOs {
    pub fds: BTreeMap<u16, File>,
    pub dirs: BTreeMap<u64, ReadDir>,
    pub strings: BTreeMap<Vec<u8>, u64>,
    pub windows: BTreeMap<u64, usize>,
    pub themes: BTreeMap<u64, usize>,
    pub screen: u64,
    pub scratch: u64,
    pub error: u16,
    pub identities: BTreeMap<(u64, u64), u32>,
}

pub(super) fn span(ptr: u64, count: usize) -> std::ops::Range<usize> {
    let start = linear(ptr);
    let end = start
        .checked_add(count)
        .filter(|n| *n <= CELLS && (count == 0 || start >= BANK))
        .unwrap_or_else(|| fatal("host adapter span out of bounds"));
    start..end
}
pub(super) fn octets(rt: &Runtime, ptr: u64, count: usize) -> Vec<u8> {
    rt.memory
        .cells(span(ptr, count))
        .map(|c| u8::try_from(c).unwrap_or_else(|_| fatal("host adapter needs octets")))
        .collect()
}
pub(super) fn put_bytes(rt: &mut Runtime, ptr: u64, bytes: &[u8]) {
    let range = span(ptr, bytes.len());
    for (i, &b) in bytes.iter().enumerate() {
        rt.memory.write(range.start + i, u16::from(b));
    }
}
pub(super) fn string(rt: &Runtime, ptr: u64) -> Vec<u8> {
    let start = span(ptr, 1).start;
    let mut out = Vec::new();
    for c in rt.memory.cells(start..CELLS).take(65537) {
        if c == 0 {
            return out;
        }
        out.push(u8::try_from(c).unwrap_or_else(|_| fatal("non-octet host string")));
    }
    fatal("unterminated host string")
}
pub(super) fn cached(rt: &mut Runtime, bytes: Vec<u8>) -> u64 {
    if let Some(&p) = rt.native_os.strings.get(&bytes) {
        return p;
    }
    let p = rt.allocate((bytes.len() + 1) as u64);
    if p == 0 {
        return 0;
    }
    put_bytes(rt, p, &bytes);
    rt.memory.write(linear(p) + bytes.len(), 0);
    rt.native_os.strings.insert(bytes, p);
    p
}
pub(super) fn write32(rt: &mut Runtime, ptr: u64, n: u32) {
    let r = span(ptr, 2);
    rt.memory.write(r.start, n as u16);
    rt.memory.write(r.start + 1, (n >> 16) as u16);
}
pub(super) fn read32(rt: &Runtime, ptr: u64) -> u32 {
    let r = span(ptr, 2);
    u32::from(rt.memory.read(r.start)) | (u32::from(rt.memory.read(r.start + 1)) << 16)
}
fn err(rt: &mut Runtime, error: io::Error) -> u64 {
    let code = error.raw_os_error().unwrap_or(libc::EIO) as u16;
    rt.native_os.error = code;
    if rt.errno != 0 {
        rt.memory.write(linear(rt.errno), code);
    }
    u64::MAX
}
fn result(rt: &mut Runtime, value: libc::c_int) -> u64 {
    if value < 0 {
        err(rt, io::Error::last_os_error())
    } else {
        value as u64
    }
}
fn path(rt: &Runtime, p: u64) -> CString {
    CString::new(string(rt, p)).unwrap()
}
fn fd(rt: &Runtime, value: u64) -> libc::c_int {
    let value = value as u16;
    if value <= 2 {
        value as libc::c_int
    } else {
        rt.native_os
            .fds
            .get(&value)
            .map(AsRawFd::as_raw_fd)
            .unwrap_or(-1)
    }
}
fn own_fd(rt: &mut Runtime, raw: libc::c_int) -> u64 {
    if raw < 0 {
        return err(rt, io::Error::last_os_error());
    }
    let file = unsafe { File::from_raw_fd(raw) };
    let Some(id) = (3..=32767).find(|id| !rt.native_os.fds.contains_key(id)) else {
        return err(rt, io::Error::from_raw_os_error(libc::EMFILE));
    };
    rt.native_os.fds.insert(id, file);
    id as u64
}
macro_rules! os {
    ($name:ident($rt:ident $(,$arg:ident)*) $body:block) => {
        #[no_mangle] unsafe extern "C" fn $name(ctx:*mut Runtime,$($arg:u64),*)->u64 {let $rt=unsafe{&mut *ctx};let _=&$rt;$body}
    };
}
os!(rcc_native_ext_rfl_posix_errno(rt) {u64::from(rt.native_os.error)});
os!(rcc_native_ext_rfl_posix_getpid(_rt) {u64::from(std::process::id())});
os!(rcc_native_ext_rfl_posix_name_capacity(_rt) {256});
os!(rcc_native_ext_rfl_posix_argc(_rt) {std::env::args_os().count().min(65535) as u64});
os!(rcc_native_ext_rfl_posix_argument(rt,index) {
    match std::env::args_os().nth(index as usize) {Some(s)=>cached(rt,s.into_vec()),None=>0}
});
os!(rcc_native_ext_rfl_posix_getenv(rt,name) {
    let name=OsString::from_vec(string(rt,name));
    let value=std::env::var_os(&name).or_else(|| {
        if name=="RFL_DATA_DIR" {std::env::current_exe().ok()?.parent()?.parent().map(|p|p.join("share").into_os_string())}else{None}
    });
    value.map(|s|cached(rt,s.into_vec())).unwrap_or(0)
});
os!(rcc_native_ext_rfl_posix_error_message(rt,code) {cached(rt,io::Error::from_raw_os_error(code as i16 as i32).to_string().into_bytes())});
os!(rcc_native_ext_rfl_posix_exit(rt,status) {rt.flush();unsafe{super::native_tui::end()};std::process::exit(status as u16 as i16 as i32)});
os!(rcc_native_ext_rfl_posix_open(rt,name,kind) {
    let name=path(rt,name);
    let flags=match kind {0=>libc::O_RDONLY,1=>libc::O_WRONLY|libc::O_CREAT|libc::O_TRUNC,2=>libc::O_WRONLY|libc::O_CREAT|libc::O_APPEND,3=>libc::O_WRONLY|libc::O_CREAT|libc::O_EXCL,_=>return err(rt,io::Error::from_raw_os_error(libc::EINVAL))};
    own_fd(rt,unsafe{libc::open(name.as_ptr(),flags|libc::O_CLOEXEC,0o644)})
});
os!(rcc_native_ext_rfl_posix_close(rt,value) {
    if let Some(file)=rt.native_os.fds.remove(&(value as u16)){result(rt,unsafe{libc::close(file.into_raw_fd())})}else{err(rt,io::Error::from_raw_os_error(libc::EBADF))}
});
os!(rcc_native_ext_rfl_posix_dup(rt,value) {own_fd(rt,unsafe{libc::fcntl(fd(rt,value),libc::F_DUPFD_CLOEXEC,3)})});
os!(rcc_native_ext_rfl_posix_read(rt,value,out,count) {
    if count>65536{return err(rt,io::Error::from_raw_os_error(libc::EINVAL));}
    span(out,count as usize);let mut bytes=vec![0;count as usize];
    let got=loop {let n=unsafe{libc::read(fd(rt,value),bytes.as_mut_ptr().cast(),bytes.len())};if n>=0{break n as usize;}let e=io::Error::last_os_error();if e.kind()!=io::ErrorKind::Interrupted{return err(rt,e);}};
    put_bytes(rt,out,&bytes[..got]);got as u64
});
os!(rcc_native_ext_rfl_posix_write(rt,value,data,count) {
    if count>65536{return err(rt,io::Error::from_raw_os_error(libc::EINVAL));}
    let bytes=octets(rt,data,count as usize);
    loop {let n=unsafe{libc::write(fd(rt,value),bytes.as_ptr().cast(),bytes.len())};if n>=0{return n as u64;}let e=io::Error::last_os_error();if e.kind()!=io::ErrorKind::Interrupted{return err(rt,e);}}
});
os!(rcc_native_ext_rfl_posix_seek(rt,value,offset,origin) {
    let n=unsafe{libc::lseek(fd(rt,value),offset as u32 as i32 as libc::off_t,origin as libc::c_int)};
    if n<0{err(rt,io::Error::last_os_error())}else if n>i32::MAX as libc::off_t{err(rt,io::Error::from_raw_os_error(libc::EOVERFLOW))}else{n as u64}
});
os!(rcc_native_ext_rfl_posix_unlink(rt,name) {let p=path(rt,name);result(rt,unsafe{libc::unlink(p.as_ptr())})});
os!(rcc_native_ext_rfl_posix_rename(rt,old,new) {let(a,b)=(path(rt,old),path(rt,new));result(rt,unsafe{libc::rename(a.as_ptr(),b.as_ptr())})});
os!(rcc_native_ext_rfl_posix_mkdir(rt,name) {let p=path(rt,name);result(rt,unsafe{libc::mkdir(p.as_ptr(),0o755)})});
os!(rcc_native_ext_rfl_posix_rmdir(rt,name) {let p=path(rt,name);result(rt,unsafe{libc::rmdir(p.as_ptr())})});
os!(rcc_native_ext_rfl_posix_chdir(rt,name) {let p=path(rt,name);result(rt,unsafe{libc::chdir(p.as_ptr())})});
os!(rcc_native_ext_rfl_posix_chmod(rt,name,mode) {let p=path(rt,name);result(rt,unsafe{libc::chmod(p.as_ptr(),mode as libc::mode_t)})});
os!(rcc_native_ext_rfl_posix_cwd(rt) {match std::env::current_dir(){Ok(p)=>cached(rt,p.into_os_string().into_vec()),Err(e)=>{err(rt,e);0}}});
os!(rcc_native_ext_rfl_posix_executable(rt,name) {
    let p=OsString::from_vec(string(rt,name));
    match std::fs::metadata(&p){Ok(m) if m.is_file()&&m.permissions().mode()&0o111!=0=>{let p=CString::new(p.into_vec()).unwrap();let n=unsafe{libc::access(p.as_ptr(),libc::X_OK)};if n==0{1}else if io::Error::last_os_error().raw_os_error()==Some(libc::EACCES){0}else{err(rt,io::Error::last_os_error())}},Ok(_)=>0,Err(e)=>err(rt,e)}
});
os!(rcc_native_ext_rfl_posix_metadata(rt,name,out) {
    span(out,10);
    match std::fs::symlink_metadata(OsString::from_vec(string(rt,name))) {
        Ok(m)=>{
            if m.len()>u32::MAX as u64{return err(rt,io::Error::from_raw_os_error(libc::EOVERFLOW));}
            let key=(m.dev(),m.ino());
            let next=rt.native_os.identities.len() as u32+1;
            let id=*rt.native_os.identities.entry(key).or_insert(next);
            let kind=if m.is_dir(){1}else if m.is_file(){2}else if m.file_type().is_symlink(){3}else{0};
            for (i,n) in [kind,m.len() as u32,1,id,m.mode()&0o7777].into_iter().enumerate(){write32(rt,pointer(linear(out)+i*2),n);}
            0
        },Err(e)=>err(rt,e)
    }
});
os!(rcc_native_ext_rfl_posix_opendir(rt,name) {
    match std::fs::read_dir(OsString::from_vec(string(rt,name))){Ok(dir)=>{let p=rt.allocate(1);if p!=0{rt.native_os.dirs.insert(p,dir);}p},Err(e)=>{err(rt,e);0}}
});
os!(rcc_native_ext_rfl_posix_closedir(rt,dir) {if rt.native_os.dirs.remove(&dir).is_some(){rt.release(dir);0}else{err(rt,io::Error::from_raw_os_error(libc::EBADF))}});
os!(rcc_native_ext_rfl_posix_readdir(rt,dir,out) {
    span(out,256);
    let Some(dir)=rt.native_os.dirs.get_mut(&dir) else{return err(rt,io::Error::from_raw_os_error(libc::EBADF));};
    match dir.next(){None=>0,Some(Err(e))=>err(rt,e),Some(Ok(entry))=>{let mut bytes=entry.file_name().into_vec();if bytes.len()>=256{return err(rt,io::Error::from_raw_os_error(libc::ENAMETOOLONG));}bytes.push(0);put_bytes(rt,out,&bytes);1}}
});

fn foreground(rt: &mut Runtime, mut command: Command) -> u64 {
    rt.flush();
    unsafe {
        libc::fflush(std::ptr::null_mut());
    }
    let group = unsafe { libc::getpgrp() };
    let int = unsafe { libc::signal(libc::SIGINT, libc::SIG_IGN) };
    let quit = unsafe { libc::signal(libc::SIGQUIT, libc::SIG_IGN) };
    unsafe {
        command.pre_exec(move || {
            libc::signal(libc::SIGINT, int);
            libc::signal(libc::SIGQUIT, quit);
            Ok(())
        });
    }
    let status = command.status();
    unsafe {
        let output = libc::signal(libc::SIGTTOU, libc::SIG_IGN);
        libc::tcsetpgrp(0, group);
        libc::signal(libc::SIGTTOU, output);
        libc::signal(libc::SIGINT, int);
        libc::signal(libc::SIGQUIT, quit);
    }
    match status {
        Ok(s) => s
            .code()
            .map(|n| ((n as u64) & 255) << 8)
            .unwrap_or_else(|| s.signal().unwrap_or(0) as u64 & 127),
        Err(e) => err(rt, e),
    }
}
os!(rcc_native_ext_rfl_posix_system(rt,text) {let mut c=Command::new("/bin/sh");c.arg("-c").arg(OsString::from_vec(string(rt,text)));foreground(rt,c)});
os!(rcc_native_ext_rfl_posix_program(rt,packed,count) {
    if count==0||count>255{return err(rt,io::Error::from_raw_os_error(libc::EINVAL));}
    let mut at=linear(packed);let mut args=Vec::new();let mut total=0;
    for _ in 0..count{let bytes=string(rt,pointer(at));total+=bytes.len()+1;if total>65536{return err(rt,io::Error::from_raw_os_error(libc::E2BIG));}at+=bytes.len()+1;args.push(OsString::from_vec(bytes));}
    let mut c=Command::new(&args[0]);c.args(&args[1..]);foreground(rt,c)
});

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};
    struct Temporary(std::path::PathBuf);
    impl Temporary {
        fn new() -> Self {
            let p = std::env::temp_dir().join(format!(
                "rfl-host-os-{}-{}",
                std::process::id(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ));
            std::fs::create_dir(&p).unwrap();
            Self(p)
        }
    }
    impl Drop for Temporary {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    fn name(rt: &mut Runtime, p: &std::path::Path) -> u64 {
        cached(rt, p.as_os_str().as_encoded_bytes().to_vec())
    }
    #[test]
    fn host_descriptors_copy_cross_bank_octets_and_clone_the_offset() {
        let temp = Temporary::new();
        let mut rt = Runtime::new();
        let p = name(&mut rt, &temp.0.join("data.bin"));
        let allocation = rt.allocate((BANK * 2) as u64);
        let start = (linear(allocation) / BANK + 1) * BANK - 3;
        let bytes = pointer(start);
        put_bytes(&mut rt, bytes, &[0, 255, 1, 42, 10, 128]);
        unsafe {
            let file = rcc_native_ext_rfl_posix_open(&mut rt, p, 3);
            assert!((3..32768).contains(&file));
            assert_eq!(rcc_native_ext_rfl_posix_open(&mut rt, p, 3), u64::MAX);
            assert_eq!(rt.native_os.error, libc::EEXIST as u16);
            assert_eq!(rcc_native_ext_rfl_posix_write(&mut rt, file, bytes, 6), 6);
            assert_eq!(rcc_native_ext_rfl_posix_close(&mut rt, file), 0);
            let file = rcc_native_ext_rfl_posix_open(&mut rt, p, 0);
            assert_eq!(rcc_native_ext_rfl_posix_seek(&mut rt, file, 0, 0), 0);
            let clone = rcc_native_ext_rfl_posix_dup(&mut rt, file);
            assert!(libc::fcntl(fd(&rt, clone), libc::F_GETFD) & libc::FD_CLOEXEC != 0);
            assert_eq!(rcc_native_ext_rfl_posix_read(&mut rt, file, bytes, 3), 3);
            assert_eq!(octets(&rt, bytes, 3), [0, 255, 1]);
            assert_eq!(rcc_native_ext_rfl_posix_read(&mut rt, clone, bytes, 3), 3);
            assert_eq!(octets(&rt, bytes, 3), [42, 10, 128]);
            assert_eq!(rcc_native_ext_rfl_posix_close(&mut rt, file), 0);
            assert_eq!(rcc_native_ext_rfl_posix_close(&mut rt, clone), 0);
            assert_eq!(
                rcc_native_ext_rfl_posix_read(&mut rt, file, bytes, 1),
                u64::MAX
            );
            assert_eq!(rt.native_os.error, libc::EBADF as u16);
            assert_eq!(
                rcc_native_ext_rfl_posix_read(&mut rt, 0, bytes, 65537),
                u64::MAX
            );
        }
        assert_eq!(
            std::fs::read(temp.0.join("data.bin")).unwrap(),
            [0, 255, 1, 42, 10, 128]
        );
    }
    #[test]
    fn long_utf8_names_and_host_metadata_are_not_truncated() {
        let temp = Temporary::new();
        let long = format!("{}-Привіт.txt", "long".repeat(35));
        let old = temp.0.join(&long);
        std::fs::write(&old, b"one").unwrap();
        let mut rt = Runtime::new();
        let root = name(&mut rt, &temp.0);
        let path = name(&mut rt, &old);
        let output = rt.allocate(256);
        let info = rt.allocate(10);
        unsafe {
            let dir = rcc_native_ext_rfl_posix_opendir(&mut rt, root);
            assert_ne!(dir, 0);
            assert_eq!(rcc_native_ext_rfl_posix_readdir(&mut rt, dir, output), 1);
            assert_eq!(string(&rt, output), long.as_bytes());
            assert_eq!(rcc_native_ext_rfl_posix_readdir(&mut rt, dir, output), 0);
            assert_eq!(rcc_native_ext_rfl_posix_closedir(&mut rt, dir), 0);
            assert_eq!(rcc_native_ext_rfl_posix_metadata(&mut rt, path, info), 0);
            assert_eq!(read32(&rt, info), 2);
            let id = read32(&rt, pointer(linear(info) + 6));
            let new = name(&mut rt, &temp.0.join("renamed.txt"));
            assert_eq!(rcc_native_ext_rfl_posix_rename(&mut rt, path, new), 0);
            assert_eq!(rcc_native_ext_rfl_posix_metadata(&mut rt, new, info), 0);
            assert_eq!(read32(&rt, pointer(linear(info) + 6)), id);
            let huge = temp.0.join("sparse");
            File::create(&huge)
                .unwrap()
                .set_len(u32::MAX as u64 + 1)
                .unwrap();
            let huge = name(&mut rt, &huge);
            assert_eq!(
                rcc_native_ext_rfl_posix_metadata(&mut rt, huge, info),
                u64::MAX
            );
            assert_eq!(rt.native_os.error, libc::EOVERFLOW as u16);
        }
    }
}
