//! AppKit menu actions stay on the main thread and enqueue guest work.
//! Preserve winit's application delegate; add only hosted-session selectors.
#![allow(unexpected_cfgs)] // objc 0.2's macros use its historical cargo-clippy cfg.
use super::hosted_graphics::Shared;
use objc::{
    class,
    declare::ClassDecl,
    msg_send,
    runtime::{Object, Sel, BOOL, NO, YES},
    sel, sel_impl,
};
use std::{
    cell::RefCell,
    ffi::{c_void, CString},
    sync::{Arc, Mutex},
};

struct Menus {
    shared: Arc<Mutex<Shared>>,
    actions: *mut Object,
    window_menu: *mut Object,
    dock: *mut Object,
    signature: Option<Vec<(u32, String)>>,
    windows: Vec<(u32, *mut Object)>,
    clipboard_count: isize,
}
thread_local! { static MENUS:RefCell<Option<Menus>>=const {RefCell::new(None)}; }
#[repr(C)]
#[derive(Clone, Copy)]
struct Point { x: f64, y: f64 }
unsafe impl objc::Encode for Point {
    fn encode() -> objc::Encoding { unsafe { objc::Encoding::from_str("{CGPoint=dd}") } }
}
/// AppKit supplies the current desktop pointer even outside this window.
/// Sample on the UI thread; convert to top-left content coordinates before
/// the caller maps physical pixels through the presentation viewport.
pub(super) fn pointer(window: &winit::window::Window) -> (f32, f32) {
    use winit::platform::macos::WindowExtMacOS;
    unsafe {
        let native = window.ns_window() as *mut Object;
        let p: Point = msg_send![native, mouseLocationOutsideOfEventStream];
        let view: *mut Object = msg_send![native, contentView];
        let p: Point = msg_send![view,convertPoint:p fromView:std::ptr::null_mut::<Object>()];
        let flipped: BOOL = msg_send![view, isFlipped];
        let height = window.inner_size().height as f64 / window.scale_factor();
        (p.x as f32, if flipped != NO { p.y } else { height - p.y } as f32)
    }
}
pub(super) fn independent_windows() {
    unsafe {
        let _: () = msg_send![class!(NSWindow),setAllowsAutomaticWindowTabbing:NO];
    }
}
unsafe extern "C" {
    fn class_addMethod(
        class: *const c_void,
        selector: Sel,
        implementation: *const c_void,
        types: *const i8,
    ) -> BOOL;
}
extern "C" fn new_terminal(_: &Object, _: Sel, _: *mut Object) {
    MENUS.with(|m| {
        if let Some(m) = m.borrow().as_ref() {
            let mut s = m.shared.lock().unwrap();
            s.new_terminals = s.new_terminals.saturating_add(1).min(16);
        }
    });
}
extern "C" fn quit(_: &Object, _: Sel, _: *mut Object) {
    unsafe {
        libc::kill(libc::getpid(), libc::SIGTERM);
    }
}
extern "C" fn should_terminate(this: &Object, _: Sel, sender: *mut Object) -> usize {
    quit(this, sel!(quitFuseX:), sender);
    0
}
extern "C" fn keep_running(_: &Object, _: Sel, _: *mut Object) -> BOOL {
    NO
}
extern "C" fn dock_menu(_: &Object, _: Sel, _: *mut Object) -> *mut Object {
    // AppKit may query while the menu/delegate is being installed.
    MENUS.with(|m| {
        m.try_borrow()
            .ok()
            .and_then(|m| m.as_ref().map(|m| m.dock))
            .unwrap_or(std::ptr::null_mut())
    })
}
extern "C" fn show_window(_: &Object, _: Sel, sender: *mut Object) {
    unsafe {
        let window: *mut Object = msg_send![sender, representedObject];
        let _: () = msg_send![window,deminiaturize:sender];
        let _: () = msg_send![window,makeKeyAndOrderFront:sender];
        let app: *mut Object = msg_send![class!(NSApplication), sharedApplication];
        let _: () = msg_send![app,activateIgnoringOtherApps:YES];
    }
}
// NSPasteboard is touched exclusively on AppKit's main thread. Only owned,
// bounded UTF-8 crosses into guest execution; no AppKit object is retained there.
unsafe fn synchronize_clipboard(m: &mut Menus) {
    use super::hosted_graphics::CLIPBOARD_MAX;
    unsafe {
        let pasteboard: *mut Object = msg_send![class!(NSPasteboard), generalPasteboard];
        let publish = m.shared.lock().unwrap().clipboard_publish.take();
        if let Some(text) = publish {
            let _: isize = msg_send![pasteboard, clearContents];
            let _: BOOL = msg_send![pasteboard, setString:string(&text) forType:string("public.utf8-plain-text")];
            m.clipboard_count = msg_send![pasteboard, changeCount];
            return;
        }
        let count: isize = msg_send![pasteboard, changeCount];
        if count == m.clipboard_count { return; }
        m.clipboard_count = count;
        let value: *mut Object = msg_send![pasteboard, stringForType:string("public.utf8-plain-text")];
        let mut text = String::new();
        if !value.is_null() {
            // Check the encoded size before allocating/copying a host paste.
            let length: usize = msg_send![value, lengthOfBytesUsingEncoding:4usize];
            if length <= CLIPBOARD_MAX {
                let bytes: *const std::ffi::c_char = msg_send![value, UTF8String];
                if !bytes.is_null() {
                    let slice = std::slice::from_raw_parts(bytes.cast::<u8>(), length);
                    if !slice.contains(&0) { text = String::from_utf8_lossy(slice).into_owned(); }
                }
            }
        }
        m.shared.lock().unwrap().clipboard = text;
    }
}
extern "C" fn edit_action(_: &Object, selector: Sel, _: *mut Object) {
    MENUS.with(|slot| {
        if let Some(m) = slot.borrow_mut().as_mut() {
            unsafe { synchronize_clipboard(m); }
            let code = if selector == sel!(pasteFuseX:) {2}
                else if selector == sel!(copyFuseX:) {1}
                else if selector == sel!(cutFuseX:) {3} else {4};
            for &(id, window) in &m.windows {
                let focused: BOOL = unsafe { msg_send![window, isKeyWindow] };
                if focused != NO {
                    if let Some(w) = m.shared.lock().unwrap().windows.get_mut(&id) { w.action(code); }
                    break;
                }
            }
        }
    });
}
unsafe fn string(text: &str) -> *mut Object {
    let bytes = CString::new(text).unwrap();
    let value: *mut Object =
        unsafe { msg_send![class!(NSString),stringWithUTF8String:bytes.as_ptr()] };
    value
}
unsafe fn menu(title: &str) -> *mut Object {
    unsafe {
        let m: *mut Object = msg_send![class!(NSMenu), alloc];
        msg_send![m,initWithTitle:string(title)]
    }
}
unsafe fn item(
    menu: *mut Object,
    title: &str,
    key: &str,
    action: Sel,
    target: *mut Object,
    window: *mut Object,
) {
    unsafe {
        let i: *mut Object = msg_send![class!(NSMenuItem), alloc];
        let i: *mut Object =
            msg_send![i,initWithTitle:string(title) action:action keyEquivalent:string(key)];
        let _: () = msg_send![i,setTarget:target];
        if !window.is_null() {
            let _: () = msg_send![i,setRepresentedObject:window];
        }
        let _: () = msg_send![menu,addItem:i];
        let _: () = msg_send![i, release];
    }
}
unsafe fn submenu(bar: *mut Object, title: &str, child: *mut Object) {
    unsafe {
        let i: *mut Object = msg_send![class!(NSMenuItem), alloc];
        let i: *mut Object = msg_send![i,initWithTitle:string(title) action:std::ptr::null::<c_void>() keyEquivalent:string("")];
        let _: () = msg_send![i,setSubmenu:child];
        let _: () = msg_send![bar,addItem:i];
        let _: () = msg_send![i, release];
    }
}
unsafe fn initialize(shared: Arc<Mutex<Shared>>) -> Menus {
    unsafe {
        let mut decl = ClassDecl::new("FuseXMenuActions", class!(NSObject)).unwrap();
        decl.add_method(
            sel!(newTerminal:),
            new_terminal as extern "C" fn(&Object, Sel, *mut Object),
        );
        decl.add_method(
            sel!(quitFuseX:),
            quit as extern "C" fn(&Object, Sel, *mut Object),
        );
        decl.add_method(
            sel!(showFuseXWindow:),
            show_window as extern "C" fn(&Object, Sel, *mut Object),
        );
        for selector in [sel!(copyFuseX:), sel!(pasteFuseX:), sel!(cutFuseX:), sel!(selectAllFuseX:)] {
            decl.add_method(selector, edit_action as extern "C" fn(&Object, Sel, *mut Object));
        }
        let actions: *mut Object = msg_send![decl.register(), new];
        let app: *mut Object = msg_send![class!(NSApplication), sharedApplication];
        let delegate: *mut Object = msg_send![app, delegate];
        let cls = (*delegate).class() as *const _ as *const c_void;
        // These selectors are absent from winit's delegate. Existing launch,
        // activation and event-loop methods continue to run unchanged.
        assert!(
            class_addMethod(
                cls,
                sel!(applicationDockMenu:),
                dock_menu as *const c_void,
                c"@@:@".as_ptr()
            ) != NO
        );
        assert!(
            class_addMethod(
                cls,
                sel!(applicationShouldTerminate:),
                should_terminate as *const c_void,
                c"Q@:@".as_ptr()
            ) != NO
        );
        assert!(
            class_addMethod(
                cls,
                sel!(applicationShouldTerminateAfterLastWindowClosed:),
                keep_running as *const c_void,
                c"c@:@".as_ptr()
            ) != NO
        );
        // AppKit caches optional delegate selectors when setting the delegate.
        // Refresh that cache after installing the hosted-only methods.
        let _: () = msg_send![app,setDelegate:std::ptr::null_mut::<Object>()];
        let _: () = msg_send![app,setDelegate:delegate];
        let bar = menu("");
        let application = menu("Fuse-X");
        let file = menu("File");
        let edit = menu("Edit");
        let windows = menu("Window");
        let dock = menu("Fuse-X");
        item(
            application,
            "Hide Fuse-X",
            "h",
            sel!(hide:),
            app,
            std::ptr::null_mut(),
        );
        item(
            application,
            "Quit Fuse-X",
            "q",
            sel!(quitFuseX:),
            actions,
            std::ptr::null_mut(),
        );
        item(
            file,
            "New Terminal",
            "n",
            sel!(newTerminal:),
            actions,
            std::ptr::null_mut(),
        );
        item(
            file,
            "Close Window",
            "w",
            sel!(performClose:),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        );
        submenu(bar, "Fuse-X", application);
        submenu(bar, "File", file);
        for (title, key, selector) in [("Cut", "x", sel!(cutFuseX:)), ("Copy", "c", sel!(copyFuseX:)),
            ("Paste", "v", sel!(pasteFuseX:)), ("Select All", "a", sel!(selectAllFuseX:))] {
            item(edit, title, key, selector, actions, std::ptr::null_mut());
        }
        submenu(bar, "Edit", edit);
        submenu(bar, "Window", windows);
        let _: () = msg_send![app,setMainMenu:bar];
        let _: () = msg_send![bar, release];
        let _: () = msg_send![application, release];
        let _: () = msg_send![file, release];
        let _: () = msg_send![edit, release];
        Menus {
            shared,
            actions,
            window_menu: windows,
            dock,
            signature: None,
            windows: Vec::new(),
            clipboard_count: -1,
        }
    }
}
pub(super) fn update(shared: Arc<Mutex<Shared>>, windows: Vec<(u32, String, *mut c_void)>) {
    MENUS.with(|slot| {
        let mut slot = slot.borrow_mut();
        let m = slot.get_or_insert_with(|| unsafe { initialize(shared) });
        m.windows = windows.iter().map(|(id, _, p)| (*id, *p as *mut Object)).collect();
        unsafe { synchronize_clipboard(m); }
        let signature: Vec<_> = windows
            .iter()
            .map(|(id, title, _)| (*id, title.clone()))
            .collect();
        if m.signature.as_ref() == Some(&signature) {
            return;
        }
        unsafe {
            for menu in [m.window_menu, m.dock] {
                let _: () = msg_send![menu, removeAllItems];
                item(
                    menu,
                    "New Terminal",
                    "",
                    sel!(newTerminal:),
                    m.actions,
                    std::ptr::null_mut(),
                );
                let separator: *mut Object = msg_send![class!(NSMenuItem), separatorItem];
                let _: () = msg_send![menu,addItem:separator];
                for (id, title, window) in &windows {
                    item(
                        menu,
                        &format!("{title} — {id}"),
                        "",
                        sel!(showFuseXWindow:),
                        m.actions,
                        *window as *mut Object,
                    );
                }
            }
        }
        m.signature = Some(signature);
    });
}
