//! Optional manager-owned shell overlay. Kernel owns all control authority;
//! this boundary validates C16 memory and trusted execution/window identities.
use super::*;
use desktop::policy::Rect;

pub(super) const KEYBOARD: u16 = 1;
pub(super) const MODAL: u16 = 2;
pub(super) const ALPHA: u16 = 4;
const CAP_OVERLAY: u32 = 4 | 8;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct Panel {
    pub(super) window: u32,
    pub(super) process: u32,
    pub(super) regions: Vec<Rect>,
    pub(super) reserve_top: i32,
    pub(super) reserve_bottom: i32,
    pub(super) flags: u16,
}

pub(super) fn remove_process(s: &mut Shared, process: u32) {
    if s.panel.as_ref().is_some_and(|panel| panel.process == process) {
        remove(s);
    }
}

pub(super) fn remove_window(s: &mut Shared, window: u32) {
    if s.panel.as_ref().is_some_and(|panel| panel.window == window) {
        remove(s);
    }
}

fn clear(s: &mut Shared) -> bool {
    if let Some(panel) = s.panel.take() {
        if let Some(surface) = s.windows.get_mut(&panel.window) {
            surface.flags |= 2;
            surface.transition(InputTransition::Focus { focused: false });
        }
        true
    } else { false }
}

pub(super) fn remove(s: &mut Shared) {
    if clear(s) { desktop::panel_changed(s); }
}

pub(super) fn call(rt: &mut Runtime, args: &mut [u32; 12]) -> Result<u32> {
    dispatch(rt, args, desktop::enabled())
}

fn dispatch(rt: &mut Runtime, args: &mut [u32; 12], nested: bool) -> Result<u32> {
    let operation = args[0];
    // Revocation is internal, used after a successful kernel authority check.
    // It is available even when INFO/control capabilities are absent.
    if operation == 64 {
        let mut s = rt.graphics.shared.lock().unwrap();
        remove_process(&mut s, args[1]);
        session::synchronize_windows(&mut s)?;
        drop(s);
        rt.host.request_hosted_window();
        return Ok(0);
    }
    if !nested {
        return Err(44);
    }
    if operation == 3 { return upload(rt, args); }
    if operation > 2 {
        return Err(44);
    }
    let configuration = if operation == 1 {
        let flags = u16::try_from(args[3]).map_err(|_| 22u16)?;
        if flags & !(KEYBOARD | MODAL | ALPHA) != 0 || flags & MODAL != 0 && flags & KEYBOARD == 0 {
            return Err(22);
        }
        let count = u16::try_from(args[4]).map_err(|_| 22u16)? as usize;
        if count == 0 && (flags & !ALPHA != 0 || args[5] != 0 || args[6] != 0 || args[7] != 0) {
            return Err(22);
        }
        let start = if count == 0 { 0 } else { span(args[5], count * 4)? };
        let mut regions = Vec::new();
        regions.try_reserve_exact(count).map_err(|_| 12u16)?;
        for index in 0..count {
            let at = start + index * 4;
            regions.push(Rect::new(
                i32::from(rt.memory.read(at)),
                i32::from(rt.memory.read(at + 1)),
                i32::from(rt.memory.read(at + 2)),
                i32::from(rt.memory.read(at + 3)),
            ));
        }
        Some((flags, regions))
    } else {
        None
    };
    let shared = rt.graphics.shared.clone();
    let mut s = shared.lock().unwrap();
    let (width, height) = desktop::output_dimensions(&mut s);
    let mut changed = false;
    match operation {
        0 => args[3] = CAP_OVERLAY,
        1 => {
            let surface = s.windows.get(&args[2]).ok_or(2u16)?;
            if args[1] == 0 || surface.process != args[1] {
                return Err(1);
            }
            if surface.width != u32::from(width) || surface.height != u32::from(height) {
                // A session watch may wake the shell before it consumes the
                // output resize event. Preserve the old valid overlay while
                // its surface catches up; malformed fixed geometry is EINVAL.
                return Err(if surface.resize.is_some() || surface.resize_in_flight.is_some() { 11 } else { 22 });
            }
            let top = u16::try_from(args[6]).map_err(|_| 22u16)?;
            let bottom = u16::try_from(args[7]).map_err(|_| 22u16)?;
            if u32::from(top) + u32::from(bottom) + 78 > u32::from(height) {
                return Err(22);
            }
            let (flags, regions) = configuration.unwrap();
            if flags & ALPHA != 0 && surface.colour.is_none() { return Err(44); }
            if regions.iter().any(|rect| rect.empty() || rect.x + rect.w > i32::from(width) || rect.y + rect.h > i32::from(height)) {
                return Err(22);
            }
            let panel = Panel {
                window: args[2], process: args[1], regions,
                reserve_top: i32::from(top), reserve_bottom: i32::from(bottom), flags,
            };
            if s.panel.as_ref() != Some(&panel) {
                let old_alpha = s.panel.as_ref().is_some_and(|old| old.window == panel.window && old.flags & ALPHA != 0);
                if old_alpha != (flags & ALPHA != 0) {
                    if let Some(surface) = s.windows.get_mut(&panel.window) {
                        if let Some(frame) = surface.colour.as_mut() {
                            if flags & ALPHA != 0 { frame.back.fill(0); }
                            else { for pixel in &mut frame.back { *pixel |= 0xff000000; } }
                            frame.present();
                            surface.sequence = surface.sequence.wrapping_add(1);
                        }
                    }
                }
                if let Some(old) = &s.panel {
                    if old.window != panel.window {
                        let old_window = old.window;
                        if let Some(surface) = s.windows.get_mut(&old_window) {
                            surface.flags |= 2;
                            surface.transition(InputTransition::Focus { focused: false });
                        }
                    }
                }
                s.panel = Some(panel);
                changed = true;
            }
        }
        2 => {
            if let Some(panel) = &s.panel {
                if panel.process != args[1] {
                    return Err(1);
                }
                if args[2] != 0 && panel.window != args[2] {
                    return Err(2);
                }
                changed = clear(&mut s);
            } else if args[2] != 0 {
                return Err(2);
            }
        }
        _ => unreachable!(),
    }
    if changed {
        desktop::panel_changed(&mut s);
        session::synchronize_windows(&mut s)?;
        if std::env::var_os("RCC_NATIVE_HEADLESS").is_some() && std::env::var_os("FUSE_X_FRAMES").is_some() {
            desktop::headless_capture_synced(&mut s)?;
        }
    }
    args[8] = u32::from(width);
    args[9] = u32::from(height);
    drop(s);
    if changed {
        rt.host.request_hosted_window();
    }
    Ok(0)
}

/// Snapshot BGRA logical octets before touching a retained backing frame.
fn upload(rt: &mut Runtime, args: &mut [u32; 12]) -> Result<u32> {
    let (x,y,width,height,stride) = (args[3],args[4],args[5],args[6],args[7]);
    if width == 0 || height == 0 || width > MAX_WIDTH * 2 || height > MAX_HEIGHT * 2 { return Err(22); }
    let row = width.checked_mul(4).ok_or(22u16)?;
    if stride < row { return Err(22); }
    let cells = (height-1).checked_mul(stride).and_then(|n|n.checked_add(row)).ok_or(14u16)?;
    let start = span(args[8], cells as usize)?;
    let mut pixels = Vec::new();
    pixels.try_reserve_exact((width * height) as usize).map_err(|_|12u16)?;
    for row in 0..height {
        let at = start + (row * stride) as usize;
        for column in 0..width {
            let p = at + (column * 4) as usize;
            let b = u8::try_from(rt.memory.read(p)).map_err(|_|22u16)?;
            let g = u8::try_from(rt.memory.read(p+1)).map_err(|_|22u16)?;
            let r = u8::try_from(rt.memory.read(p+2)).map_err(|_|22u16)?;
            let a = u8::try_from(rt.memory.read(p+3)).map_err(|_|22u16)?;
            pixels.push(u32::from_le_bytes([r,g,b,a]));
        }
    }
    let mut s = rt.graphics.shared.lock().unwrap();
    let panel = s.panel.as_ref().ok_or(2u16)?;
    if panel.window != args[2] || panel.process != args[1] { return Err(1); }
    if panel.flags & ALPHA == 0 { return Err(44); }
    let surface = s.windows.get_mut(&args[2]).ok_or(2u16)?;
    if surface.process != args[1] { return Err(1); }
    let (rw,rh) = (surface.raster_width(),surface.raster_height());
    if x > rw || y > rh || width > rw-x || height > rh-y { return Err(22); }
    let frame = surface.colour.as_mut().ok_or(44u16)?;
    for row in 0..height {
        let at = ((y+row)*rw+x) as usize;
        let from = (row*width) as usize;
        frame.back[at..at+width as usize].copy_from_slice(&pixels[from..from+width as usize]);
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    const PTR: u32 = 0x20000;

    fn window(rt: &mut Runtime) -> u32 {
        rt.memory.write(BANK, 0);
        let mut a = [1280,800,0x10000,0,2,100,42,0,0,0,0,0];
        rt.graphics_call(1, &mut a).unwrap();
        a[0]
    }
    fn configuration(rt: &mut Runtime, id: u32) -> [u32;12] {
        rt.memory.write_from(2 * BANK, &[8,8,100,32, 4,700,200,96]);
        [1,100,id,0,2,PTR,40,104,0,0,0,0]
    }

    #[test]
    fn panel_validation_is_atomic_and_native_mode_unsupported() {
        let mut rt = Runtime::new();
        let id = window(&mut rt);
        let mut args = configuration(&mut rt, id);
        assert_eq!(dispatch(&mut rt, &mut args, false), Err(44));
        args[5] = 0;
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(14));
        assert!(rt.graphics.shared.lock().unwrap().panel.is_none());
        args = configuration(&mut rt, id);
        args[1] = 101;
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(1));
        args[1] = 100;
        args[3] = u32::from(MODAL);
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(22));
        args[3] = 0;
        args[7] = 760;
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(22));
        args[7] = 104;
        rt.memory.write(2 * BANK + 6, 1280);
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(22));
        assert!(rt.graphics.shared.lock().unwrap().panel.is_none());
        args = configuration(&mut rt, id);
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            s.windows.get_mut(&id).unwrap().width = 1000;
        }
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(22));
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            s.windows.get_mut(&id).unwrap().request_resize(1280, 800);
        }
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(11));
        assert!(rt.graphics.shared.lock().unwrap().panel.is_none());
    }

    #[test]
    fn configure_is_idempotent_role_is_visible_and_cleanup_revokes() {
        let mut rt = Runtime::new();
        let id = window(&mut rt);
        let mut args = configuration(&mut rt, id);
        dispatch(&mut rt, &mut args, true).unwrap();
        let revision = {
            let s = rt.graphics.shared.lock().unwrap();
            assert_eq!(s.registry.window(id).unwrap().role, 1);
            assert!(s.desktop.as_ref().unwrap().wm.get(id).is_none());
            s.registry.revision()
        };
        dispatch(&mut rt, &mut args, true).unwrap();
        assert_eq!(rt.graphics.shared.lock().unwrap().registry.revision(), revision);
        let mut release = [2,101,id,0,0,0,0,0,0,0,0,0];
        assert_eq!(dispatch(&mut rt, &mut release, true), Err(1));
        let mut revoke = [64,101,0,0,0,0,0,0,0,0,0,0];
        dispatch(&mut rt, &mut revoke, false).unwrap();
        assert!(rt.graphics.shared.lock().unwrap().panel.is_some());
        revoke[1] = 100;
        dispatch(&mut rt, &mut revoke, false).unwrap();
        let s = rt.graphics.shared.lock().unwrap();
        assert!(s.panel.is_none());
        assert_eq!(s.registry.window(id).unwrap().role, 0);
        assert!(s.registry.revision() > revision);
    }
    #[test]
    fn inactive_prepare_has_no_rectangles_and_cannot_grab_keyboard() {
        let mut rt = Runtime::new();
        let id = window(&mut rt);
        let mut args = [1,100,id,0,0,0,0,0,0,0,0,0];
        dispatch(&mut rt, &mut args, true).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            assert!(s.panel.as_ref().unwrap().regions.is_empty());
            assert!(s.desktop.as_ref().unwrap().wm.get(id).is_none());
            assert!(s.desktop.as_ref().unwrap().metadata()["panel"].is_null());
            assert_eq!(s.registry.window(id).unwrap().role, 1);
        }
        args[3] = u32::from(KEYBOARD);
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(22));
        args[3] = 0;
        args[6] = 40;
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(22));
        args[6] = 0;
        args[5] = PTR;
        assert_eq!(dispatch(&mut rt, &mut args, true), Err(22));
        let mut release = [2,100,id,0,0,0,0,0,0,0,0,0];
        dispatch(&mut rt, &mut release, true).unwrap();
        let s = rt.graphics.shared.lock().unwrap();
        assert!(s.panel.is_none());
        assert_ne!(s.windows[&id].flags & 2, 0, "release must not expose a fullscreen ordinary surface");
    }

    #[test]
    fn alpha_upload_is_atomic_and_publishes_color_and_mask_together() {
        let mut rt = Runtime::new();
        let id = window(&mut rt);
        let mut args = configuration(&mut rt,id);
        args[3] = u32::from(ALPHA);
        dispatch(&mut rt,&mut args,true).unwrap();
        let at = 2*BANK+128;
        rt.memory.write_from(at,&[30,20,10,0,90,80,70,128]);
        let mut upload = [3,100,id,8,8,2,1,8,PTR+128,0,0,0];
        rt.memory.write(at+7,256);
        assert_eq!(dispatch(&mut rt,&mut upload,true),Err(22));
        {
            let s=rt.graphics.shared.lock().unwrap();
            let frame=s.windows[&id].colour.as_ref().unwrap();
            assert!(frame.back.iter().all(|&p|p==0));
            assert!(frame.front.iter().all(|&p|p==0));
        }
        rt.memory.write(at+7,128);
        upload[7]=7;
        assert_eq!(dispatch(&mut rt,&mut upload,true),Err(22));
        upload[7]=8;
        upload[1]=101;
        assert_eq!(dispatch(&mut rt,&mut upload,true),Err(1));
        upload[1]=100;
        dispatch(&mut rt,&mut upload,true).unwrap();
        let index=8*1280+8;
        {
            let s=rt.graphics.shared.lock().unwrap();
            let frame=s.windows[&id].colour.as_ref().unwrap();
            assert_eq!(frame.back[index],u32::from_le_bytes([10,20,30,0]));
            assert_eq!(frame.back[index+1],u32::from_le_bytes([70,80,90,128]));
            assert_eq!(frame.front[index+1],0,"upload cannot publish a mask early");
        }
        rt.graphics_call(4,&mut[id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        {
            let s=rt.graphics.shared.lock().unwrap();
            assert_eq!(s.windows[&id].colour.as_ref().unwrap().front[index+1],u32::from_le_bytes([70,80,90,128]));
        }
        rt.graphics_call(24,&mut[id,2,0,0,0,0,0,0,0,0,0,0]).unwrap();
        {
            let s=rt.graphics.shared.lock().unwrap();
            let frame=s.windows[&id].colour.as_ref().unwrap();
            assert!(frame.front.iter().all(|&p|p==0));
        }
        rt.graphics_call(12,&mut[id,1400,800,0,0,0,0,0,0,0,0,0]).unwrap();
        let s=rt.graphics.shared.lock().unwrap();
        let frame=s.windows[&id].colour.as_ref().unwrap();
        assert!(frame.front.iter().all(|&p|p==0),"resize must not introduce opaque black");
    }

}
