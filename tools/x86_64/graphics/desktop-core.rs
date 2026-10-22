//! Native C adapter for the shared allocation-free desktop policy.
//! Calls are serialized by the existing kernel/runtime; callbacks only queue
//! events and never reenter this policy. No guest pointer crosses this boundary.
#[path = "../../../libraries/graphics/desktop.rs"]
#[allow(dead_code)]
mod policy;
#[path = "../../../libraries/graphics/desktop_text.rs"]
mod text;
#[path = "../../../libraries/graphics/render.rs"]
#[allow(dead_code)]
mod render;
use policy::{Action, Desktop, Rect, WindowCommand};
use core::{cell::UnsafeCell,slice,str};
type Callback = extern "C" fn(u32, u32, i32, i32, i32, i32);
// The runtime serializes calls; no IRQ handler accesses this storage. The C
// callback contract forbids reentry while an individual policy call is active.
struct Serialized(UnsafeCell<Option<Desktop>>);
unsafe impl Sync for Serialized {}
static DESKTOP: Serialized = Serialized(UnsafeCell::new(None));
static mut CALLBACK: Option<Callback> = None;
fn emit(actions: policy::Actions) {
    for action in actions.iter() {
        let Some(callback) = (unsafe { CALLBACK }) else { return };
        match action {
            Action::Focus(id, on) => callback(1,id,on as i32,0,0,0),
            Action::Resize(id,w,h) => callback(2,id,w as i32,h as i32,0,0),
            Action::Close(id) => callback(3,id,0,0,0,0),
            Action::Capture(id,on) => callback(4,id,on as i32,0,0,0),
            Action::Motion(id,x,y) => callback(5,id,x,y,0,0),
            Action::Button(id,b,on,x,y) => callback(6,id,b as i32,on as i32,x,y),
        }
    }
}
#[no_mangle]
pub unsafe extern "C" fn xd_init(w:i32,h:i32,callback:Callback) {
    CALLBACK=Some(callback);
    *DESKTOP.0.get()=Some(Desktop::with_limits(w,h,w.max(1920),h.max(1200)));
}
#[no_mangle]
pub unsafe extern "C" fn xd_add(id:u32,w:u32,h:u32,resizable:u32) {
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut() { let a=d.add(id,w,h,resizable!=0);emit(a); }
}
#[no_mangle]
pub unsafe extern "C" fn xd_remove(id:u32) {
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut() { let a=d.remove(id);emit(a); }
}
#[no_mangle]
pub unsafe extern "C" fn xd_accept(id:u32,w:u32,h:u32,resizable:u32) {
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut(){ d.accepted(id,w,h,resizable!=0); }
}
#[no_mangle]
pub unsafe extern "C" fn xd_command(id:u32,command:u32,x:i32,y:i32,w:u32,h:u32)->i32 {
    let command=match command {
        1=>WindowCommand::Activate,2=>WindowCommand::Minimize,3=>WindowCommand::Maximize,
        4=>WindowCommand::Restore,5=>WindowCommand::Move{x,y},6=>WindowCommand::Resize{width:w,height:h},
        7=>WindowCommand::Close,_=>return -22,
    };
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut(){return match d.command(id,command){
        Ok(a)=>{emit(a);0},Err(policy::CommandError::NotFound)=>-2,
        Err(policy::CommandError::Unsupported)=>-95,Err(_)=>-22,
    }}
    -2
}
#[no_mangle]
pub unsafe extern "C" fn xd_motion(x:i32,y:i32) {
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut(){let a=d.motion(x,y);emit(a);}
}
#[no_mangle]
pub unsafe extern "C" fn xd_button(button:u32,down:u32) {
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut(){let a=d.button(button as u16,down!=0);emit(a);}
}
#[no_mangle]
pub unsafe extern "C" fn xd_edges(top:i32,bottom:i32,shell:u32) {
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut(){d.enable_taskbar_input(shell==0);let a=d.reserve_edges(top,bottom);emit(a);}
}
#[no_mangle]
pub unsafe extern "C" fn xd_focus()->u32 { (&*DESKTOP.0.get()).as_ref().and_then(|d|d.focused).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn xd_hit(x:i32,y:i32)->u32 { (&*DESKTOP.0.get()).as_ref().and_then(|d|d.hit_client(x,y)).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn xd_captured()->u32 { (&*DESKTOP.0.get()).as_ref().is_some_and(|d|d.pointer_captured()) as u32 }
#[no_mangle]
pub unsafe extern "C" fn xd_window(index:u32,record:*mut i32)->u32 {
    let Some(d)=(&*DESKTOP.0.get()).as_ref() else{return 0};
    let Some(w)=d.windows().get(index as usize) else{return 0};
    let r=slice::from_raw_parts_mut(record,6);
    r.copy_from_slice(&[w.content.x,w.content.y,w.content.w,w.content.h,
        (if w.minimized{2}else{1})|(if w.maximized{4}else{0})|(if d.focused==Some(w.id){8}else{0}),
        w.resizable as i32]);
    w.id
}
#[no_mangle]
pub unsafe extern "C" fn xd_task(index:u32,record:*mut i32) {
    if let Some(d)=(&*DESKTOP.0.get()).as_ref(){let r=d.task_rect(index as usize);
        slice::from_raw_parts_mut(record,4).copy_from_slice(&[r.x,r.y,r.w,r.h]);}
}
#[no_mangle]
pub unsafe extern "C" fn xd_invalidate(x:i32,y:i32,w:i32,h:i32) {
    if let Some(d)=(&mut *DESKTOP.0.get()).as_mut(){d.invalidate(Rect::new(x,y,w,h));}
}
#[no_mangle]
pub unsafe extern "C" fn xd_damage(rectangles:*mut i32)->u32 {
    let Some(d)=(&mut *DESKTOP.0.get()).as_mut() else{return 0};let (r,n)=d.take_damage();
    let out=slice::from_raw_parts_mut(rectangles,n*4);
    for i in 0..n{out[i*4..i*4+4].copy_from_slice(&[r[i].x,r[i].y,r[i].w,r[i].h]);}n as u32
}
#[no_mangle]
pub unsafe extern "C" fn xd_chrome(pixels:*mut u32,width:u32,height:u32,id:u32,
        title:*const u8,length:u32,damage:*const i32) {
    let Some(d)=(&*DESKTOP.0.get()).as_ref() else{return};let Some(w)=d.get(id) else{return};
    if w.minimized{return} let c=w.chrome();let a=slice::from_raw_parts(damage,4);
    let damage=Rect::new(a[0],a[1],a[2],a[3]);let out=slice::from_raw_parts_mut(pixels,(width*height)as usize);
    let fill=|out:&mut [u32],r,colour|policy::fill(out,width,height,1,r,damage,colour);
    fill(out,w.outer(),0xff454951);fill(out,c.title,if d.focused==Some(id){0xff303945}else{0xff292c31});
    for (i,r) in [c.close,c.minimize,c.maximize].iter().enumerate(){
        let colour=if i==2&&!w.resizable{0xff666870}else{0xffdfe3e8};
        let x=r.x+(r.w-8)/2;let y=r.y+(r.h-8)/2;
        if i==0 {for n in 0..8 {fill(out,Rect::new(x+n,y+n,1,1),colour);fill(out,Rect::new(x+7-n,y+n,1,1),colour);}}
        else if i==1 {fill(out,Rect::new(x,y+4,8,1),colour);}
        else {fill(out,Rect::new(x,y,8,1),colour);fill(out,Rect::new(x,y+7,8,1),colour);
            fill(out,Rect::new(x,y,1,8),colour);fill(out,Rect::new(x+7,y,1,8),colour);}
    }
    let label=str::from_utf8(slice::from_raw_parts(title,length as usize)).unwrap_or("Application");
    text::paint(out,width,height,1,c.label,damage,label,0xffe2e5e9,false);
    if w.resizable&&!w.maximized{fill(out,Rect::new(c.grip.x+8,c.grip.y+8,3,3),0xff979da8);}
}
#[no_mangle]
pub unsafe extern "C" fn xd_label(pixels:*mut u32,width:u32,height:u32,x:i32,y:i32,w:i32,h:i32,
        title:*const u8,length:u32,damage:*const i32) {
    let a=slice::from_raw_parts(damage,4);let damage=Rect::new(a[0],a[1],a[2],a[3]);
    let out=slice::from_raw_parts_mut(pixels,(width*height)as usize);
    let label=str::from_utf8(slice::from_raw_parts(title,length as usize)).unwrap_or("Application");
    text::paint(out,width,height,1,Rect::new(x,y,w,h),damage,label,0xffe2e5e9,false);
}
#[repr(C)]
pub struct Primitive { x0:i32,y0:i32,x1:i32,y1:i32,dot:u16,size:u16 }
#[no_mangle]
pub unsafe extern "C" fn xd_a8(dst:*mut u8,width:usize,height:usize,stride:usize,
        primitives:*const Primitive,count:usize,clear:u32)->i32 {
    let length=match height.checked_sub(1).and_then(|h|h.checked_mul(stride)).and_then(|n|n.checked_add(width)){
        Some(n)=>n,None=>return -22};
    let out=slice::from_raw_parts_mut(dst,length);let p=if count==0{&[]}else{slice::from_raw_parts(primitives,count)};
    let primitive=|v:&Primitive|render::Primitive{x0:v.x0,y0:v.y0,x1:v.x1,y1:v.y1,dot:v.dot!=0,size:v.size};
    if p.iter().any(|v|v.dot>1||!primitive(v).valid()){return -22}
    if render::raster_a8(out,width,height,stride,&[],clear!=0).is_err(){return -22}
    for v in p{if render::raster_a8(out,width,height,stride,&[primitive(v)],false).is_err(){return -22}}
    0
}
