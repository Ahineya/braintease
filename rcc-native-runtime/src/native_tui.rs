//! Host-ABI curses/theme calls, behind checked compatibility-ABI adapters.
use super::{
    memory::{linear, pointer},
    native_os::{cached, octets, put_bytes, read32, span, string, write32},
    *,
};
use std::ffi::{c_int, c_ulong, c_void, CStr, CString};
unsafe extern "C" {
    fn rfl_curses_screen() -> *mut c_void;
    fn rfl_curses_begin() -> c_int;
    fn rfl_curses_end() -> ();
    fn rfl_curses_rows() -> c_int;
    fn rfl_curses_columns() -> c_int;
    fn rfl_curses_clear() -> ();
    fn rfl_curses_move(a0: c_int, a1: c_int) -> c_int;
    fn rfl_curses_style(a0: c_ulong) -> ();
    fn rfl_curses_character(a0: c_int) -> ();
    fn rfl_curses_refresh() -> ();
    fn rfl_curses_key() -> c_int;
    fn rfl_curses_timeout(a0: c_int) -> ();
    fn rfl_curses_cursor(a0: c_int) -> ();
    fn rfl_curses_suspend() -> ();
    fn rfl_curses_resume() -> c_int;
    fn rfl_curses_colors() -> c_int;
    fn rfl_curses_pair(a0: c_int, a1: c_int, a2: c_int) -> c_int;
    fn rfl_curses_lines(a0: c_int) -> ();
    fn rfl_curses_bracketed(a0: c_int) -> ();
    fn rfl_curses_window(a0: c_int, a1: c_int, a2: c_int, a3: c_int) -> *mut c_void;
    fn rfl_curses_delete(a0: *mut c_void) -> ();
    fn rfl_curses_reposition(a0: *mut c_void, a1: c_int, a2: c_int) -> c_int;
    fn rfl_curses_height(a0: *mut c_void) -> c_int;
    fn rfl_curses_width(a0: *mut c_void) -> c_int;
    fn rfl_curses_background(a0: *mut c_void, a1: c_ulong) -> ();
    fn rfl_curses_text_begin(a0: *mut c_void, a1: c_int, a2: c_int, a3: c_ulong) -> c_int;
    fn rfl_curses_text_chunk(a0: *mut u8, a1: c_int, a2: c_int) -> c_int;
    fn rfl_curses_event(a0: *mut c_void, a1: *mut u32) -> c_int;
    fn rfl_curses_fill(
        a0: *mut c_void,
        a1: c_int,
        a2: c_int,
        a3: c_int,
        a4: c_ulong,
        a5: c_ulong,
    ) -> ();
    fn rfl_curses_border(
        a0: *mut c_void,
        a1: c_int,
        a2: c_int,
        a3: c_int,
        a4: c_int,
        a5: c_ulong,
    ) -> ();
    fn rfl_curses_stage(a0: *mut c_void) -> ();
    fn rfl_curses_present() -> ();
    fn rfl_curses_window_key(a0: *mut c_void) -> c_int;
    fn rfl_curses_window_cursor(a0: *mut c_void, a1: c_int, a2: c_int, a3: c_int) -> ();
    fn rfl_curses_scroll(a0: *mut c_void, a1: c_int) -> c_int;
    fn rfl_curses_region(a0: *mut c_void, a1: c_int, a2: c_int) -> c_int;
    fn rfl_curses_cells(
        a0: *mut c_void,
        a1: c_int,
        a2: c_int,
        a3: *const c_ulong,
        a4: c_int,
    ) -> c_int;
    fn rfl_curses_shape(a0: c_int) -> c_int;
    fn rfl_curses_mouse(mode: c_int);
    fn rfl_curses_getmouse(data: *mut u32) -> c_int;
    fn ru_append(a0: u32, a1: u32) -> u32;
    fn ripple_curses_text_run(a0: *mut c_void, a1: *mut u8, a2: u32, a3: c_int) -> u32;
    fn rfl_host_tui_signals(a0: c_int) -> ();
    fn rfl_host_tui_interrupted() -> c_int;
    fn isendwin() -> c_int;
    fn tui_theme_new(a0: *const libc::c_char, a1: c_int) -> *mut c_void;
    fn tui_theme_free(a0: *mut c_void) -> ();
    fn tui_theme_define(
        a0: *mut c_void,
        a1: *const libc::c_char,
        a2: c_int,
        a3: c_int,
        a4: u32,
    ) -> c_int;
    fn tui_theme_apply(
        a0: *mut c_void,
        a1: *const libc::c_char,
        a2: *const libc::c_char,
        a3: *const libc::c_char,
    ) -> c_int;
    fn tui_theme_named(a0: *mut c_void, a1: *const libc::c_char, a2: *const libc::c_char) -> c_int;
    fn tui_theme_file(
        a0: *mut c_void,
        a1: *const libc::c_char,
        a2: *const libc::c_char,
        a3: c_int,
    ) -> c_int;
    fn tui_theme_valid_name(a0: *const libc::c_char) -> c_int;
    fn tui_theme_error(a0: *mut c_void) -> *const libc::c_char;
    fn tui_theme_name(a0: *mut c_void) -> *const libc::c_char;
    fn tui_theme_lines(a0: *mut c_void) -> c_int;
    fn tui_theme_count(a0: *mut c_void) -> u32;
    fn tui_theme_fg(a0: *mut c_void, a1: u32) -> c_int;
    fn tui_theme_bg(a0: *mut c_void, a1: u32) -> c_int;
    fn tui_theme_flags(a0: *mut c_void, a1: u32) -> u32;
}
fn window(rt: &Runtime, p: u64) -> *mut c_void {
    if p == 0 {
        return unsafe { rfl_curses_screen() };
    }
    *rt.native_os
        .windows
        .get(&p)
        .unwrap_or_else(|| fatal("invalid host window handle")) as *mut c_void
}
fn theme(rt: &Runtime, p: u64) -> *mut c_void {
    *rt.native_os
        .themes
        .get(&p)
        .unwrap_or_else(|| fatal("invalid host theme handle")) as *mut c_void
}
fn cstring(rt: &Runtime, p: u64) -> CString {
    CString::new(string(rt, p)).unwrap()
}
fn own_window(rt: &mut Runtime, p: *mut c_void) -> u64 {
    if p.is_null() {
        return 0;
    }
    let id = rt.allocate(1);
    if id == 0 {
        unsafe { rfl_curses_delete(p) };
        return 0;
    }
    rt.native_os.windows.insert(id, p as usize);
    id
}
pub(super) unsafe fn end() {
    unsafe {
        rfl_curses_end();
        rfl_host_tui_signals(0);
    }
}
pub(super) fn check_signal() {
    let sig = unsafe { rfl_host_tui_interrupted() };
    if sig != 0 {
        unsafe { end() };
        std::process::exit(128 + sig);
    }
}
macro_rules! tui {
    ($name:ident($rt:ident $(,$arg:ident)*) $body:block) => {
        #[no_mangle] unsafe extern "C" fn $name(ctx:*mut Runtime,$($arg:u64),*)->u64 {let $rt=unsafe{&mut *ctx};let _=&$rt;$body}
    };
}

tui!(rcc_native_ext_rfl_curses_rows(rt) {(unsafe{rfl_curses_rows( )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_columns(rt) {(unsafe{rfl_curses_columns( )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_clear(rt) {unsafe{rfl_curses_clear( )}; 0});
tui!(rcc_native_ext_rfl_curses_move(rt,a0,a1) {(unsafe{rfl_curses_move(a0 as u16 as i16 as c_int,a1 as u16 as i16 as c_int )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_style(rt,a0) {unsafe{rfl_curses_style(a0 as c_ulong )}; 0});
tui!(rcc_native_ext_rfl_curses_character(rt,a0) {unsafe{rfl_curses_character(a0 as u16 as i16 as c_int )}; 0});
tui!(rcc_native_ext_rfl_curses_refresh(rt) {unsafe{rfl_curses_refresh( )}; 0});
tui!(rcc_native_ext_rfl_curses_key(rt) {check_signal();(unsafe{rfl_curses_key( )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_timeout(rt,a0) {unsafe{rfl_curses_timeout(a0 as u16 as i16 as c_int )}; 0});
tui!(rcc_native_ext_rfl_curses_cursor(rt,a0) {unsafe{rfl_curses_cursor(a0 as u16 as i16 as c_int )}; 0});
tui!(rcc_native_ext_rfl_curses_suspend(rt) {unsafe{rfl_curses_suspend( )}; 0});
tui!(rcc_native_ext_rfl_curses_resume(rt) {(unsafe{rfl_curses_resume( )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_colors(rt) {(unsafe{rfl_curses_colors( )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_pair(rt,a0,a1,a2) {(unsafe{rfl_curses_pair(a0 as u16 as i16 as c_int,a1 as u16 as i16 as c_int,a2 as u16 as i16 as c_int )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_lines(rt,a0) {unsafe{rfl_curses_lines(a0 as u16 as i16 as c_int )}; 0});
tui!(rcc_native_ext_rfl_curses_bracketed(rt,a0) {unsafe{rfl_curses_bracketed(a0 as u16 as i16 as c_int )}; 0});
tui!(rcc_native_ext_rfl_curses_reposition(rt,a0,a1,a2) {(unsafe{rfl_curses_reposition(window(rt,a0),a1 as u16 as i16 as c_int,a2 as u16 as i16 as c_int )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_height(rt,a0) {(unsafe{rfl_curses_height(window(rt,a0) )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_width(rt,a0) {(unsafe{rfl_curses_width(window(rt,a0) )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_background(rt,a0,a1) {unsafe{rfl_curses_background(window(rt,a0),a1 as c_ulong )}; 0});
tui!(rcc_native_ext_rfl_curses_text_begin(rt,a0,a1,a2,a3) {(unsafe{rfl_curses_text_begin(window(rt,a0),a1 as u16 as i16 as c_int,a2 as u16 as i16 as c_int,a3 as c_ulong )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_fill(rt,a0,a1,a2,a3,a4,a5) {unsafe{rfl_curses_fill(window(rt,a0),a1 as u16 as i16 as c_int,a2 as u16 as i16 as c_int,a3 as u16 as i16 as c_int,a4 as c_ulong,a5 as c_ulong )}; 0});
tui!(rcc_native_ext_rfl_curses_border(rt,a0,a1,a2,a3,a4,a5) {unsafe{rfl_curses_border(window(rt,a0),a1 as u16 as i16 as c_int,a2 as u16 as i16 as c_int,a3 as u16 as i16 as c_int,a4 as u16 as i16 as c_int,a5 as c_ulong )}; 0});
tui!(rcc_native_ext_rfl_curses_stage(rt,a0) {unsafe{rfl_curses_stage(window(rt,a0) )}; 0});
tui!(rcc_native_ext_rfl_curses_present(rt) {unsafe{rfl_curses_present( )}; 0});
tui!(rcc_native_ext_rfl_curses_window_key(rt,a0) {check_signal();(unsafe{rfl_curses_window_key(window(rt,a0) )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_window_cursor(rt,a0,a1,a2,a3) {unsafe{rfl_curses_window_cursor(window(rt,a0),a1 as u16 as i16 as c_int,a2 as u16 as i16 as c_int,a3 as u16 as i16 as c_int )}; 0});
tui!(rcc_native_ext_rfl_curses_scroll(rt,a0,a1) {(unsafe{rfl_curses_scroll(window(rt,a0),a1 as u16 as i16 as c_int )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_region(rt,a0,a1,a2) {(unsafe{rfl_curses_region(window(rt,a0),a1 as u16 as i16 as c_int,a2 as u16 as i16 as c_int )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_shape(rt,a0) {(unsafe{rfl_curses_shape(a0 as u16 as i16 as c_int )}) as i64 as u64});
tui!(rcc_native_ext_rfl_curses_mouse(rt,mode) {unsafe{rfl_curses_mouse(mode as u16 as i16 as c_int)};0});
tui!(rcc_native_ext_rfl_curses_getmouse(rt,out) {span(out,6);let mut data=[0u32;3];let status=unsafe{rfl_curses_getmouse(data.as_mut_ptr())};if status==0{for (i,value) in data.into_iter().enumerate(){write32(rt,pointer(linear(out)+i*2),value);}}status as i64 as u64});

tui!(rcc_native_ext_rfl_curses_begin(rt) {rt.flush();let n=unsafe{rfl_curses_begin()};if n==0{unsafe{rfl_host_tui_signals(1)};}n as i64 as u64});
tui!(rcc_native_ext_rfl_curses_end(_rt) {unsafe{rfl_curses_end();if isendwin()!=0{rfl_host_tui_signals(0);}}0});
tui!(rcc_native_ext_rfl_curses_screen(rt) {
    if rt.native_os.screen==0{let w=unsafe{rfl_curses_screen()};if w.is_null(){return 0;}rt.native_os.screen=own_window(rt,w);}
    rt.native_os.screen
});
tui!(rcc_native_ext_rfl_curses_window(rt,rows,cols,y,x) {let w=unsafe{rfl_curses_window(rows as u16 as i16 as c_int,cols as u16 as i16 as c_int,y as u16 as i16 as c_int,x as u16 as i16 as c_int)};own_window(rt,w)});
tui!(rcc_native_ext_rfl_curses_delete(rt,w) {if w!=rt.native_os.screen{if let Some(p)=rt.native_os.windows.remove(&w){unsafe{rfl_curses_delete(p as *mut c_void);if isendwin()!=0{rfl_host_tui_signals(0);}}rt.release(w);}}0});
tui!(rcc_native_ext_rfl_curses_scratch(rt) {if rt.native_os.scratch==0{rt.native_os.scratch=rt.allocate(512);}rt.native_os.scratch});
tui!(rcc_native_ext_rfl_curses_text_chunk(rt,bytes,count,final_) {
    if count>512{fatal("host text chunk exceeds scratch buffer");}let mut data=octets(rt,bytes,count as usize);
    (unsafe{rfl_curses_text_chunk(data.as_mut_ptr(),count as c_int,final_ as c_int)}) as i64 as u64
});
tui!(rcc_native_ext_rfl_curses_event(rt,w,out) {check_signal();span(out,2);let mut value=0;let n=unsafe{rfl_curses_event(window(rt,w),&mut value)};write32(rt,out,value);n as i64 as u64});
tui!(rcc_native_ext_rfl_host_cells(rt,w,row,col,cells,count) {
    if count>512{fatal("host cell row too large");}span(cells,count as usize*2);
    let data:Vec<c_ulong>=(0..count as usize).map(|i|{let at=linear(cells)+i*2;u64::from(rt.memory.read(at))|(u64::from(rt.memory.read(at+1))<<16)}).collect();
    (unsafe{rfl_curses_cells(window(rt,w),row as u16 as i16 as c_int,col as u16 as i16 as c_int,data.as_ptr(),count as c_int)}) as i64 as u64
});
tui!(rcc_native_ext_rfl_host_glyph(rt,scalars,count) {
    if count==0||count>16{fatal("invalid host glyph scalar count");}span(scalars,count as usize*2);
    let mut glyph=read32(rt,scalars);
    for i in 1..count as usize{let at=linear(scalars)+i*2;let cp=u32::from(rt.memory.read(at))|(u32::from(rt.memory.read(at+1))<<16);glyph=unsafe{ru_append(glyph,cp)};}
    u64::from(glyph)
});
tui!(rcc_native_ext_rfl_host_text_run(rt,w,out,limit,literal) {
    if limit>4096{fatal("host paste run exceeds bounded buffer");}span(out,limit as usize);let mut data=vec![0;limit as usize];
    let n=unsafe{ripple_curses_text_run(window(rt,w),data.as_mut_ptr(),limit as u32,literal as c_int)};
    if n as usize>data.len(){fatal("host paste run overflow");}put_bytes(rt,out,&data[..n as usize]);u64::from(n)
});
tui!(rcc_native_ext_tui_theme_new(rt,name,lines) {let name=cstring(rt,name);let p=unsafe{tui_theme_new(name.as_ptr(),lines as c_int)};if p.is_null(){return 0;}let id=rt.allocate(1);if id==0{unsafe{tui_theme_free(p)};}else{rt.native_os.themes.insert(id,p as usize);}id});
tui!(rcc_native_ext_tui_theme_free(rt,id) {if let Some(p)=rt.native_os.themes.remove(&id){unsafe{tui_theme_free(p as *mut c_void)};rt.release(id);}0});
tui!(rcc_native_ext_tui_theme_define(rt,id,role,fg,bg,flags) {let role=cstring(rt,role);(unsafe{tui_theme_define(theme(rt,id),role.as_ptr(),fg as u16 as i16 as c_int,bg as u16 as i16 as c_int,flags as u32)}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_apply(rt,id,section,key,value) {let(a,b,c)=(cstring(rt,section),cstring(rt,key),cstring(rt,value));(unsafe{tui_theme_apply(theme(rt,id),a.as_ptr(),b.as_ptr(),c.as_ptr())}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_named(rt,id,dir,name) {let(a,b)=(cstring(rt,dir),cstring(rt,name));(unsafe{tui_theme_named(theme(rt,id),a.as_ptr(),b.as_ptr())}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_file(rt,id,dir,path,optional) {let(a,b)=(cstring(rt,dir),cstring(rt,path));(unsafe{tui_theme_file(theme(rt,id),a.as_ptr(),b.as_ptr(),optional as c_int)}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_valid_name(rt,name) {let name=cstring(rt,name);(unsafe{tui_theme_valid_name(name.as_ptr())}) as i64 as u64});

tui!(rcc_native_ext_tui_theme_error(rt,id) {let p=unsafe{tui_theme_error(theme(rt,id))};if p.is_null(){0}else{cached(rt,unsafe{CStr::from_ptr(p)}.to_bytes().to_vec())}});
tui!(rcc_native_ext_tui_theme_name(rt,id) {let p=unsafe{tui_theme_name(theme(rt,id))};if p.is_null(){0}else{cached(rt,unsafe{CStr::from_ptr(p)}.to_bytes().to_vec())}});
tui!(rcc_native_ext_tui_theme_lines(rt,id) {(unsafe{tui_theme_lines(theme(rt,id))}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_count(rt,id) {(unsafe{tui_theme_count(theme(rt,id))}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_fg(rt,id,index) {(unsafe{tui_theme_fg(theme(rt,id),index as u32)}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_bg(rt,id,index) {(unsafe{tui_theme_bg(theme(rt,id),index as u32)}) as i64 as u64});
tui!(rcc_native_ext_tui_theme_flags(rt,id,index) {(unsafe{tui_theme_flags(theme(rt,id),index as u32)}) as i64 as u64});
