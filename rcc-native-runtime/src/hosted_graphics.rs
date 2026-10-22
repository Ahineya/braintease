//! Hosted GX windows. Only owned pixels/events cross the guest/UI boundary.
//! The kernel checks process ownership; this adapter checks handles and spans.
use super::*;
use rvm::input_events::{InputState, InputTransition};
use std::sync::{Arc, Mutex};

pub(super) const CLIPBOARD_MAX: usize = 32768;
const MAX_WINDOWS: usize = 16;
const MAX_WIDTH: u32 = 1920;
const MAX_HEIGHT: u32 = 1080;
// Presentation zoom changes native window points, never guest pixels or DPI.
fn presentation_size(width:u32,height:u32,percent:u32)->(u32,u32){
    ((width*percent+50)/100,(height*percent+50)/100)
}
fn guest_size(width:u32,height:u32,percent:u32)->(u32,u32){
    ((width*100+percent/2)/percent,(height*100+percent/2)/percent)
}
fn fitted_zoom(width:u32,height:u32,requested:u32,monitor:Option<(u32,u32)>)->u32{
    let Some((mw,mh))=monitor else{return requested;};
    let fit=((mw.saturating_sub(32)*100/width).min(mh.saturating_sub(80)*100/height)).max(100);
    if fit>=requested{return requested;}
    // Small steps avoid awkward scales while preserving every explicit request
    // that fits. The minimum remains 100%, even on an undersized monitor.
    (fit/5*5).max(100)
}
fn presentation_viewport(texture:(u32,u32),surface:(u32,u32))->(f32,f32,f32,f32){
    let scale=(surface.0 as f32/texture.0 as f32).min(surface.1 as f32/texture.1 as f32);
    let (width,height)=(texture.0 as f32*scale,texture.1 as f32*scale);
    ((surface.0 as f32-width)/2.0,(surface.1 as f32-height)/2.0,width,height)
}
fn presentation_pixel(position:(f32,f32),texture:(u32,u32),surface:(u32,u32))->(i32,i32){
    let (x,y,width,height)=presentation_viewport(texture,surface);
    (((position.0-x)*texture.0 as f32/width).floor() as i32,
     ((position.1-y)*texture.1 as f32/height).floor() as i32)
}
// Pixels 0.12's default renderer rounds upscaling down to an integer. Opted-in
// presentation zoom needs fractional nearest-neighbour sampling instead (e.g.
// 135% in logical points on a Retina display is a 2.7x physical enlargement).
struct PresentationRenderer{
    layout:pixels::wgpu::BindGroupLayout,
    sampler:pixels::wgpu::Sampler,
    pipeline:pixels::wgpu::RenderPipeline,
}
impl PresentationRenderer{
    fn new(device:&pixels::wgpu::Device,format:pixels::wgpu::TextureFormat)->Self{
        use pixels::wgpu as w;
        let shader=device.create_shader_module(w::ShaderModuleDescriptor{
            label:Some("GX presentation zoom"),source:w::ShaderSource::Wgsl(std::borrow::Cow::Borrowed(r#"
struct Vertex { @builtin(position) position:vec4<f32>, @location(0) uv:vec2<f32> };
@vertex fn vertex(@builtin(vertex_index) index:u32)->Vertex {
    var positions=array<vec2<f32>,3>(vec2<f32>(-1.0,-1.0),vec2<f32>(3.0,-1.0),vec2<f32>(-1.0,3.0));
    let p=positions[index]; var v:Vertex; v.position=vec4<f32>(p,0.0,1.0);
    v.uv=vec2<f32>((p.x+1.0)*0.5,(1.0-p.y)*0.5); return v;
}
@group(0) @binding(0) var image:texture_2d<f32>;
@group(0) @binding(1) var image_sampler:sampler;
@fragment fn fragment(v:Vertex)->@location(0) vec4<f32> { return textureSample(image,image_sampler,v.uv); }
"#))});
        let layout=device.create_bind_group_layout(&w::BindGroupLayoutDescriptor{
            label:None,entries:&[
                w::BindGroupLayoutEntry{binding:0,visibility:w::ShaderStages::FRAGMENT,
                    ty:w::BindingType::Texture{sample_type:w::TextureSampleType::Float{filterable:true},view_dimension:w::TextureViewDimension::D2,multisampled:false},count:None},
                w::BindGroupLayoutEntry{binding:1,visibility:w::ShaderStages::FRAGMENT,
                    ty:w::BindingType::Sampler(w::SamplerBindingType::Filtering),count:None},
            ]});
        let sampler=device.create_sampler(&w::SamplerDescriptor{mag_filter:w::FilterMode::Nearest,min_filter:w::FilterMode::Nearest,..Default::default()});
        let pipeline_layout=device.create_pipeline_layout(&w::PipelineLayoutDescriptor{label:None,bind_group_layouts:&[&layout],push_constant_ranges:&[]});
        let pipeline=device.create_render_pipeline(&w::RenderPipelineDescriptor{
            label:Some("GX presentation zoom"),layout:Some(&pipeline_layout),
            vertex:w::VertexState{module:&shader,entry_point:"vertex",buffers:&[]},
            fragment:Some(w::FragmentState{module:&shader,entry_point:"fragment",targets:&[Some(w::ColorTargetState{
                format,blend:None,write_mask:w::ColorWrites::ALL})]}),
            primitive:w::PrimitiveState::default(),depth_stencil:None,multisample:w::MultisampleState::default(),multiview:None});
        Self{layout,sampler,pipeline}
    }
    fn render(&self,encoder:&mut pixels::wgpu::CommandEncoder,target:&pixels::wgpu::TextureView,context:&pixels::PixelsContext,surface:(u32,u32)){
        use pixels::wgpu as w;
        let texture=context.texture.create_view(&w::TextureViewDescriptor::default());
        let group=context.device.create_bind_group(&w::BindGroupDescriptor{label:None,layout:&self.layout,entries:&[
            w::BindGroupEntry{binding:0,resource:w::BindingResource::TextureView(&texture)},
            w::BindGroupEntry{binding:1,resource:w::BindingResource::Sampler(&self.sampler)},
        ]});
        let (x,y,width,height)=presentation_viewport((context.texture_extent.width,context.texture_extent.height),surface);
        let mut pass=encoder.begin_render_pass(&w::RenderPassDescriptor{label:Some("GX presentation zoom"),color_attachments:&[Some(w::RenderPassColorAttachment{
            view:target,resolve_target:None,ops:w::Operations{load:w::LoadOp::Clear(w::Color::BLACK),store:true}})],depth_stencil_attachment:None});
        pass.set_pipeline(&self.pipeline);pass.set_bind_group(0,&group,&[]);
        pass.set_viewport(x,y,width,height,0.0,1.0);pass.draw(0..3,0..1);
    }
}
type Result<T> = std::result::Result<T, u16>;
#[path="hosted_raster.rs"]
mod raster;
#[path="hosted_coverage.rs"]
mod coverage;
#[path="hosted_colour.rs"]
mod colour;
#[path="hosted_render.rs"]
mod render;
#[path="hosted_desktop.rs"]
mod desktop;
#[path="hosted_session.rs"]
mod session;
#[path="hosted_panel.rs"]
mod panel;

pub(super) struct Surface {
    process: u32,
    pid: u16,
    width: u32,
    height: u32,
    title: String,
    back: Vec<u16>,
    front: Arc<Vec<u16>>,
    // RGB565 buffers are empty for a true-colour surface. Exactly one backing
    // representation is allocated; legacy clients keep their original storage.
    colour: Option<colour::TrueColour>,
    render: render::State,
    sequence: u64,
    input: InputState,
    closed: bool,
    flags: u32,
    resize: Option<(u32, u32)>,
    resize_in_flight: Option<(u32, u32)>,
    activation: u64,
    configuration: u64,
    backing_scale:u32,
    preferred_scale:u32,
    automatic_scale:bool,
    requested_zoom:u32,
    zoom_percent:u32,
    presentation_monitor:Option<(u32,u32)>,
    pointer_requested: bool,
    desktop_pointer: Option<(i32, i32)>,
    #[cfg(target_os="macos")]
    terminal: Option<super::hosted_terminal::Frame>,
}
impl Surface {
    fn rgba(&self)->Option<Arc<Vec<u32>>>{
        if let Some(frame)=self.colour.as_ref(){return Some(frame.front.clone());}
        #[cfg(target_os="macos")]
        if let Some(frame)=self.terminal.as_ref(){return Some(frame.front.clone());}
        None
    }
    fn pixel_scale(&self)->u32{
        #[cfg(target_os="macos")]
        if self.terminal.is_some(){return 2;}
        self.backing_scale
    }
    fn format(&self)->u32{if self.colour.is_some(){2}else{1}}
    fn rgb_capture(&self)->Vec<u8>{
        if let Some(frame)=self.colour.as_ref(){
            frame.front.iter().flat_map(|c|{let p=c.to_le_bytes();[p[0],p[1],p[2]]}).collect()
        }else{
            self.front.iter().flat_map(|&c|rvm::display_rgb565::rgb565_to_rgb888(c)).collect()
        }
    }
    fn raster_width(&self)->u32{self.width*self.backing_scale}
    fn raster_height(&self)->u32{self.height*self.backing_scale}
    fn presentation_size(&self)->(u32,u32){presentation_size(self.width,self.height,self.zoom_percent)}
    fn fit_presentation(&mut self,monitor:Option<(u32,u32)>){
        self.presentation_monitor=monitor;
        let zoom=fitted_zoom(self.width,self.height,self.requested_zoom,monitor);
        if self.zoom_percent!=zoom{
            self.zoom_percent=zoom;
            self.configuration=self.configuration.wrapping_add(1);
        }
    }
    fn request_presentation(&mut self,percent:u32,nested:bool){
        self.requested_zoom=percent;
        if nested {
            // The initial desktop profile presents clients at their logical
            // size. Report its actual effective zoom instead of accepting a
            // native-window enlargement which the compositor cannot display.
            if self.zoom_percent!=100{
                self.zoom_percent=100;
                self.configuration=self.configuration.wrapping_add(1);
            }
        }else{self.fit_presentation(self.presentation_monitor);}
    }
    fn prefer_scale(&mut self,scale:u32){
        self.preferred_scale=scale.clamp(1,2);
        if self.automatic_scale&&self.backing_scale!=self.preferred_scale{
            /* A fixed logical window still needs new backing pixels. */
            self.resize=Some(self.resize.or(self.resize_in_flight).unwrap_or((self.width,self.height)));
        }
    }
    // Hosted edit commands share the ordered input queue, but never manufacture
    // Ctrl+C (SIGINT) or a printable command-key letter in the guest.
    pub(super) fn action(&mut self, code: u16) {
        self.transition(InputTransition::Edit { command: code });
    }
    #[cfg(target_os="macos")]
    pub(super) fn close(&mut self) { self.transition(InputTransition::Close); }
    fn transition(&mut self, event: InputTransition) {
        if matches!(event, InputTransition::Close) {
            if self.closed {
                return;
            }
            self.closed = true;
        }
        self.input.apply(event, 0);
    }
    fn request_resize(&mut self, width: u32, height: u32) {
        let dimensions = (width.min(MAX_WIDTH), height.min(MAX_HEIGHT));
        if self.flags & 1 == 0
            && width > 0
            && height > 0
            && (dimensions != (self.width, self.height)
                || self.resize.is_some() || self.resize_in_flight.is_some()
                ||(self.automatic_scale&&self.backing_scale!=self.preferred_scale))
        {
            self.resize = Some(dimensions);
        }
    }
}
#[derive(Default)]
pub(super) struct Shared {
    pub(super) windows: BTreeMap<u32, Surface>,
    pub(super) clipboard: String,
    pub(super) clipboard_publish: Option<String>,
    next: u32,
    pub(super) finished: bool,
    pub(super) new_terminals: u16,
    desktop: Option<desktop::Session>,
    registry: session::model::Registry,
    panel: Option<panel::Panel>,
}
#[derive(Default)]
pub(super) struct Graphics {
    pub(super) shared: Arc<Mutex<Shared>>,
}
fn span(ptr: u32, count: usize) -> Result<usize> {
    let bank = (ptr >> 16) as usize;
    let offset = (ptr & 65535) as usize;
    if bank >= BANKS || offset >= BANK || ptr == 0 {
        return Err(14);
    }
    let start = bank * BANK + offset;
    if start.checked_add(count).is_none_or(|n| n > CELLS) {
        return Err(14);
    }
    Ok(start)
}
fn title(rt: &Runtime, ptr: u32) -> Result<String> {
    let start = span(ptr, 64)?;
    let mut bytes = Vec::new();
    for i in 0..64 {
        let c = rt.memory.read(start + i);
        if c == 0 {
            return Ok(String::from_utf8_lossy(&bytes).into_owned());
        }
        if c > 255 {
            return Err(22);
        }
        bytes.push(c as u8);
    }
    Err(22)
}
fn write_cells(rt: &mut Runtime, ptr: u32, cells: &[u16]) -> Result<()> {
    let start = span(ptr, cells.len())?;
    for (i, c) in cells.iter().enumerate() {
        rt.memory.write(start + i, *c);
    }
    Ok(())
}
fn pair(out: &mut Vec<u16>, n: u32) {
    out.extend([n as u16, (n >> 16) as u16]);
}
// Optional acceptance-test captures use the same immutable published frames.
// Atomic replacement lets the external test observe a coherent registry.
fn capture(shared: &mut Shared) -> Result<()> {
    let Some(directory) = std::env::var_os("FUSE_X_FRAMES") else {
        return Ok(());
    };
    let directory = std::path::Path::new(&directory);
    std::fs::create_dir_all(directory).map_err(|_| 5u16)?;
    let windows:Vec<_>=shared.windows.iter().map(|(&id,w)|serde_json::json!({
        "id":id,"process":w.process,"pid":w.pid,"title":w.title,"format":w.format(),"width":w.width,"height":w.height,"scale":w.backing_scale,"pixel_width":w.raster_width(),"pixel_height":w.raster_height(),"zoom_percent":w.zoom_percent,"presentation_width":w.presentation_size().0,"presentation_height":w.presentation_size().1,"visible":w.flags&2==0,"resizable":w.flags&1==0,"sequence":w.sequence,"configuration":w.configuration
    })).collect();
    let temporary = directory.join("windows.json.tmp");
    std::fs::write(&temporary, serde_json::to_vec(&windows).unwrap()).map_err(|_| 5u16)?;
    std::fs::rename(temporary, directory.join("windows.json")).map_err(|_| 5u16)?;
    if desktop::enabled()&&std::env::var_os("RCC_NATIVE_HEADLESS").is_some(){desktop::headless_capture(shared)?;}
    Ok(())
}
// Opt-in acceptance fixture: deliver the same owned host resize/close requests
// in a headless run. Producers atomically replace this one-command mailbox.
// No guest protocol or syscall is added, and normal sessions never read it.
fn test_event(shared: &Arc<Mutex<Shared>>) -> Result<()> {
    if std::env::var_os("RCC_NATIVE_HEADLESS").is_none() { return Ok(()); }
    let Some(path)=std::env::var_os("FUSE_X_TEST_EVENT") else { return Ok(()); };
    let bytes=match std::fs::read(&path) {
        Ok(bytes)=>bytes,
        Err(e) if e.kind()==std::io::ErrorKind::NotFound=>return Ok(()),
        Err(_)=>return Err(5),
    };
    let command:serde_json::Value=serde_json::from_slice(&bytes).map_err(|_|22u16)?;
    let number=|v:&serde_json::Value|v.as_u64().and_then(|n|u32::try_from(n).ok()).ok_or(22u16);
    let id=number(&command["window"])?;
    let resize=command.get("resize");let close=command.get("close");
    let action=command.get("action");let input=command.get("input");
    let density=command.get("density");
    if [resize,close,action,input,density].iter().filter(|v|v.is_some()).count()!=1 { return Err(22); }
    let mut state=shared.lock().unwrap();
    if let Some(text)=command.get("clipboard") {
        let text=text.as_str().filter(|s|s.len()<=CLIPBOARD_MAX&&!s.contains('\0')).ok_or(22u16)?;
        state.clipboard=text.into();
    }
    if id==0&&desktop::enabled(){desktop::headless_event(&mut state,&command)?;std::fs::remove_file(path).map_err(|_|5u16)?;return Ok(());}
    let window=state.windows.get_mut(&id).ok_or(22u16)?;
    if let Some(scale)=density{let n=number(scale)?;if !(1..=2).contains(&n){return Err(22);}window.prefer_scale(n);}
    else if let Some(size)=resize {
        let size=size.as_array().filter(|a|a.len()==2).ok_or(22u16)?;
        window.request_resize(number(&size[0])?,number(&size[1])?);
    } else if let Some(action)=action { window.action(number(action)?.try_into().map_err(|_|22u16)?); }
    else if let Some(input)=input {
        let transition:InputTransition=serde_json::from_value(input.clone()).map_err(|_|22u16)?;
        if !transition.valid() { return Err(22); } window.transition(transition);
    }
    else if close.and_then(|v|v.as_bool())==Some(true) { window.transition(InputTransition::Close); }
    else { return Err(22); }
    std::fs::remove_file(path).map_err(|_|5u16)?;
    Ok(())
}
impl Runtime {
    fn graphics_call(&mut self, op: u64, a: &mut [u32; 12]) -> Result<u32> {
        if op==31{return session::call(self,a);}
        if op==32{return panel::call(self,a);}
        if op==25{return raster::blend(self,a);}
        if op==29{return coverage::raster(self,a);}
        if op==30 && a.iter().all(|&n| n==0){return Ok(0);}
        if op==26{a[1]=2;a[2]=3;return Ok(0);}
        let shared = self.graphics.shared.clone();
        if op==21 {
            #[cfg(target_os="macos")]
            if std::env::var("FUSE_X_TERM_STYLE").as_deref()!=Ok("classic") {
                let (width,height)=super::hosted_terminal::metrics();a[1]=width;a[2]=height;return Ok(0);
            }
            return Err(42);
        }
        if matches!(op,8|10) { test_event(&shared)?; }
        if op == 11 {
            let mut s = shared.lock().unwrap();
            let ready = s.new_terminals != 0;
            s.new_terminals = s.new_terminals.saturating_sub(1);
            return Ok(u32::from(ready));
        }
        // Raster spans are process-local RAM, not window handles. No pointers
        // are retained, and page-aware operations preserve fork COW isolation.
        if op == 15 || op == 16 {
            let (width, height, dst_stride, src_stride) = (a[2], a[3], a[4], a[5]);
            if width == 0
                || height == 0
                || width > MAX_WIDTH
                || height > MAX_HEIGHT
                || dst_stride < width
                || (op == 16 && src_stride < width)
            {
                return Err(22);
            }
            let count = (height - 1)
                .checked_mul(dst_stride)
                .and_then(|n| n.checked_add(width))
                .ok_or(14u16)?;
            let dst = span(a[0], count as usize)?;
            if dst < super::BANK {
                return Err(14);
            }
            if op == 15 {
                if dst_stride == width {
                    self.memory.fill(dst..dst + count as usize, a[6] as u16);
                } else {
                    for row in 0..height {
                        let at = dst + (row * dst_stride) as usize;
                        self.memory.fill(at..at + width as usize, a[6] as u16);
                    }
                }
            } else {
                let count = (height - 1)
                    .checked_mul(src_stride)
                    .and_then(|n| n.checked_add(width))
                    .ok_or(14u16)?;
                let src = span(a[1], count as usize)?;
                if src < super::BANK {
                    return Err(14);
                }
                let dest_count = (height - 1) * dst_stride + width;
                if dst < src + count as usize && src < dst + dest_count as usize {
                    return Err(22);
                }
                if src_stride == width && dst_stride == width {
                    self.memory.copy_within(src..src + count as usize, dst);
                } else {
                    let mut line = vec![0; width as usize];
                    for row in 0..height {
                        self.memory
                            .read_into(src + (row * src_stride) as usize, &mut line);
                        self.memory
                            .write_from(dst + (row * dst_stride) as usize, &line);
                    }
                }
            }
            return Ok(0);
        }
        if op == 1 {
            let (w, h) = (a[0], a[1]);
            // a[4]=0 is the historical host call, predating format discovery.
            if w == 0 || h == 0 || w > MAX_WIDTH || h > MAX_HEIGHT || a[4]>2 {
                return Err(22);
            }
            let title = title(self, a[2])?;
            let mut s = shared.lock().unwrap();
            if s.windows.len() >= MAX_WINDOWS {
                return Err(12);
            }
            let id = s.next.checked_add(1).ok_or(12u16)?;
            let pid=u16::try_from(a[6]).map_err(|_|22u16)?;
            if a[5]!=0{s.registry.touch_process(a[5],pid).map_err(session::error)?;}
            s.next = id;
            let pixels = if a[4]==2{Vec::new()}else{vec![0; (w * h) as usize]};
            s.windows.insert(
                id,
                Surface {
                    process: a[5],
                    pid,
                    width: w,
                    height: h,
                    title,
                    front: Arc::new(pixels.clone()),
                    back: pixels,
                    colour: (a[4]==2).then(||colour::TrueColour::new((w*h) as usize)),
                    render: render::State::default(),
                    sequence: 1,
                    input: InputState::default(),
                    closed: false,
                    flags: a[3] & 2,
                    resize: None,
                    resize_in_flight: None,
                    activation: 0,
                    configuration: 0,
                    backing_scale:1,
                    preferred_scale:if cfg!(target_os="macos"){2}else{1},
                    automatic_scale:false,
                    requested_zoom:100,
                    zoom_percent:100,
                    presentation_monitor:None,
                    pointer_requested: false,
                    desktop_pointer: None,
                    #[cfg(target_os="macos")]
                    terminal: None,
                },
            );
            a[0] = id;
            capture(&mut s)?;
            drop(s);
            self.host.request_hosted_window();
            return Ok(0);
        }
        let mut s = shared.lock().unwrap();
        if matches!(op, 19 | 20) {
            if !s.windows.contains_key(&a[0]) { return Err(22); }
            let count = a[2] as usize;
            if count > CLIPBOARD_MAX { return Err(22); }
            if op == 19 {
                if s.clipboard.len() > count { a[2] = s.clipboard.len() as u32; return Err(7); }
                let cells: Vec<u16> = s.clipboard.bytes().map(u16::from).collect();
                write_cells(self, a[1], &cells)?;
                a[2] = cells.len() as u32;
            } else {
                let mut bytes = Vec::with_capacity(count);
                if count != 0 {
                    let start = span(a[1], count)?;
                    for i in 0..count {
                        let c = self.memory.read(start+i);
                        if c == 0 || c > 255 { return Err(22); }
                        bytes.push(c as u8);
                    }
                }
                let text = String::from_utf8(bytes).map_err(|_|22u16)?;
                s.clipboard.clone_from(&text);
                s.clipboard_publish = Some(text);
            }
            return Ok(0);
        }
        let panel_alpha = s.panel.as_ref().is_some_and(|p| p.window == a[0] && p.flags & panel::ALPHA != 0);
        let window = s.windows.get_mut(&a[0]).ok_or(22u16)?;
        if op==30{return render::call(self,window,a);}
        #[cfg(target_os="macos")]
        if op==22 {
            use super::hosted_terminal::{Cell,Frame};
            let (cols,rows,row,length)=(a[1],a[2],a[3],a[5] as usize);
            let (cw,ch)=super::hosted_terminal::metrics();
            if window.colour.is_some()||window.backing_scale!=1||cols==0||cols>240||rows==0||rows>100||row>=rows||length>cols as usize*67
                ||window.width!=cols*cw||window.height!=rows*ch||a[8]>1||a[9]>6
                ||(a[8]!=0&&(a[6]>=cols||a[7]>=rows)){return Err(22);}
            let start=span(a[4],length)?;let mut data=vec![0;length];self.memory.read_into(start,&mut data);
            let mut cells=Vec::with_capacity(cols as usize);let mut at=0;let mut continuation=false;
            for x in 0..cols {
                if at+3>length{return Err(22);}
                let (colour,width,n)=(data[at],data[at+1],data[at+2] as usize);at+=3;
                if colour>2047||width>2||n>64||at+n>length||(width==0)!=continuation
                    ||(width==0&&n!=0)||(width!=0&&n==0)||(width==2&&x+1==cols){return Err(22);}
                let bytes:Vec<u8>=data[at..at+n].iter().map(|&v|u8::try_from(v).map_err(|_|22u16)).collect::<Result<_>>()?;
                let text=String::from_utf8(bytes).map_err(|_|22u16)?;
                if text.chars().any(char::is_control){return Err(22);}
                cells.push(Cell{text,width,colour});at+=n;continuation=width==2;
            }
            if at!=length{return Err(22);}
            let frame=window.terminal.get_or_insert_with(||Frame::from_surface(window.width,window.height,&window.back,&window.front));
            frame.row(row,&cells,(a[8]!=0).then_some((a[6],a[7],a[9])));
            // Retain the logical RGB565 surface for screenshots and legacy GX
            // clients. Presentation uses the full-resolution RGBA frame.
            for y in row*ch..(row+1)*ch{for x in 0..window.width{
                let p=frame.back[((y*2+1)*window.width*2+x*2+1) as usize].to_le_bytes();
                window.back[(y*window.width+x) as usize]=((p[0] as u16>>3)<<11)|((p[1] as u16>>2)<<5)|(p[2] as u16>>3);
            }}
            return Ok(0);
        }
        match op {
            28=>{
                if !(100..=300).contains(&a[1]){return Err(22);}
                // The UI thread supplies monitor bounds once a native window
                // exists. Headless sessions deliberately use the requested size.
                window.request_presentation(a[1],desktop::enabled());
                a[1]=window.zoom_percent;
            }
            24=>{
                if a[1]>2{return Err(22);}
                let scale=if a[1]==0{window.preferred_scale}else{a[1]};
                window.automatic_scale=a[1]==0;
                let mut reset=scale!=window.backing_scale;
                #[cfg(target_os="macos")]
                {reset|=window.terminal.is_some();}
                if reset{
                    #[cfg(target_os="macos")]
                    {window.terminal=None;}
                    window.backing_scale=scale;
                    let count=(window.raster_width()*window.raster_height()) as usize;
                    if window.colour.is_some(){
                        let mut frame = colour::TrueColour::new(count);
                        if panel_alpha { frame.back.fill(0); frame.present(); }
                        window.colour=Some(frame);
                    }
                    else{window.back=vec![0;count];window.front=Arc::new(window.back.clone());}
                    window.sequence=window.sequence.wrapping_add(1);
                    window.configuration=window.configuration.wrapping_add(1);
                }
                a[1]=scale;a[2]=window.raster_width();a[3]=window.raster_height();
            }
            17 | 18 => {
                let (width, height, stride) = (a[3], a[4], a[5]);
                if a[1] != 0 || a[2] != 0 || width == 0 || height == 0
                    || width > MAX_WIDTH || height > MAX_HEIGHT || stride < width
                { return Err(22); }
                let count = (height - 1).checked_mul(stride)
                    .and_then(|n| n.checked_add(width)).ok_or(14u16)?;
                let start = span(a[6], count as usize)?;
                let palette = if op == 18 {
                    let start = span(a[7], 256)?;
                    Some((0..256).map(|i| self.memory.read(start+i)).collect::<Vec<_>>())
                } else { None };
                // Read compatibility RAM once per source pixel. Scaling the
                // output in a guest loop multiplies expensive C16 accesses.
                let mut source = Vec::with_capacity((width * height) as usize);
                for y in 0..height {
                    for x in 0..width {
                        let pixel = self.memory.read(start + (y * stride + x) as usize);
                        source.push(palette.as_ref().map_or(pixel, |p| p[(pixel & 255) as usize]));
                    }
                }
                let (mut vw, mut vh) = (window.raster_width(), window.raster_width() * height / width);
                if vh > window.raster_height() { vh = window.raster_height(); vw = window.raster_height() * width / height; }
                vw = vw.max(1); vh = vh.max(1);
                let (left, top) = ((window.raster_width() - vw) / 2, (window.raster_height() - vh) / 2);
                let columns: Vec<_> = (0..vw).map(|x| (x * width / vw) as usize).collect();
                #[cfg(target_os="macos")]
                {window.terminal=None;}
                if let Some(frame)=window.colour.as_mut(){frame.back.fill(colour::rgba(0));}
                else{window.back.fill(0);}
                for y in 0..vh {
                    let row = (y * height / vh * width) as usize;
                    let dest = ((top + y) * window.raster_width() + left) as usize;
                    for (x, &column) in columns.iter().enumerate() {
                        let c=source[row+column];
                        if let Some(frame)=window.colour.as_mut(){frame.back[dest+x]=colour::rgba565(c);}
                        else{window.back[dest+x]=c;}
                    }
                }
            }
            2 => {
                panel::remove_window(&mut s, a[0]);
                s.windows.remove(&a[0]);
            }
            23 => {
                let (x,y,w,h,stride)=(a[1],a[2],a[3],a[4],a[5]);
                if x>window.raster_width()||y>window.raster_height()||w>window.raster_width()-x||h>window.raster_height()-y {
                    return Err(22);
                }
                let row=w.checked_mul(3).ok_or(22u16)?;
                if stride<row{return Err(22);}
                if w==0||h==0{return Ok(0);}
                let count=(h-1).checked_mul(stride).and_then(|n|n.checked_add(row)).ok_or(14u16)?;
                let start=span(a[6],count as usize)?;
                let mut line=vec![0;row as usize];
                #[cfg(target_os="macos")]
                {window.terminal=None;}
                for dy in 0..h {
                    self.memory.read_into(start+(dy*stride) as usize,&mut line);
                    let dest=((y+dy)*window.raster_width()+x) as usize;
                    if let Some(frame)=window.colour.as_mut(){
                        for (out,bgr) in frame.back[dest..dest+w as usize].iter_mut().zip(line.chunks_exact(3)){
                            *out=u32::from_le_bytes([bgr[2] as u8,bgr[1] as u8,bgr[0] as u8,255]);
                        }
                    }else{
                        for (out,bgr) in window.back[dest..dest+w as usize].iter_mut().zip(line.chunks_exact(3)){
                            *out=((bgr[2]&248)<<8)|((bgr[1]&252)<<3)|((bgr[0]&255)>>3);
                        }
                    }
                }
            }
            3 | 5 | 6 | 27 => {
                let (x, y, w, h) = (a[1], a[2], a[3], a[4]);
                if op==27&&a[5]>0xffffff{return Err(22);}
                if x > window.raster_width()
                    || y > window.raster_height()
                    || w > window.raster_width() - x
                    || h > window.raster_height() - y
                {
                    return Err(22);
                }
                if w == 0 || h == 0 {
                    return Ok(0);
                }
                let source = if op == 3 {
                    if a[5] < w {
                        return Err(22);
                    }
                    let count = (h - 1)
                        .checked_mul(a[5])
                        .and_then(|n| n.checked_add(w))
                        .ok_or(14u16)?;
                    Some(span(a[6], count as usize)?)
                } else {
                    None
                };
                if op == 6 {
                    let (dx, dy) = (a[5], a[6]);
                    if dx > window.raster_width()
                        || dy > window.raster_height()
                        || w > window.raster_width() - dx
                        || h > window.raster_height() - dy
                    {
                        return Err(22);
                    }
                    #[cfg(target_os="macos")]
                    {window.terminal=None;}
                    let stride=window.raster_width();
                    if let Some(frame)=window.colour.as_mut(){colour::copy_rect(&mut frame.back,stride,x,y,w,h,dx,dy);}
                    else{colour::copy_rect(&mut window.back,stride,x,y,w,h,dx,dy);}
                } else {
                    #[cfg(target_os="macos")]
                    {window.terminal=None;}
                    let mut line=vec![0;w as usize];
                    for row in 0..h {
                        let dest=((y+row)*window.raster_width()+x) as usize;
                        if let Some(start)=source{
                            self.memory.read_into(start+(row*a[5]) as usize,&mut line);
                            if let Some(frame)=window.colour.as_mut(){
                                for (out,&c) in frame.back[dest..dest+w as usize].iter_mut().zip(&line){*out=colour::rgba565(c);}
                            }else{window.back[dest..dest+w as usize].copy_from_slice(&line);}
                        }else if let Some(frame)=window.colour.as_mut(){
                            let c=if op==27{colour::rgba(a[5])}else{colour::rgba565(a[5] as u16)};
                            frame.back[dest..dest+w as usize].fill(c);
                        }else{
                            let c=if op==27{colour::rgb565(a[5])}else{a[5] as u16};
                            window.back[dest..dest+w as usize].fill(c);
                        }
                    }
                }
            }
            4 => {
                #[cfg(target_os="macos")]
                if let Some(frame)=window.terminal.as_mut(){frame.front=Arc::new(frame.back.clone());}
                window.front = Arc::new(window.back.clone());
                if let Some(frame)=window.colour.as_mut(){frame.present();}
                window.sequence = window.sequence.wrapping_add(1);
                if let Some(path) = std::env::var_os("FUSE_X_FRAME") {
                    let mut bytes =
                        format!("P6\n{} {}\n255\n", window.raster_width(), window.raster_height()).into_bytes();
                    bytes.extend(window.rgb_capture());
                    std::fs::write(path, bytes).map_err(|_| 5u16)?;
                }
                if let Some(directory) = std::env::var_os("FUSE_X_FRAMES") {
                    let directory = std::path::Path::new(&directory);
                    std::fs::create_dir_all(directory).map_err(|_| 5u16)?;
                    let mut bytes =
                        format!("P6\n{} {}\n255\n", window.raster_width(), window.raster_height()).into_bytes();
                    bytes.extend(window.rgb_capture());
                    let temporary = directory.join(format!("{}.ppm.tmp", a[0]));
                    std::fs::write(&temporary, bytes).map_err(|_| 5u16)?;
                    std::fs::rename(temporary, directory.join(format!("{}.ppm", a[0])))
                        .map_err(|_| 5u16)?;
                    if panel_alpha {
                        if let Some(frame) = &window.colour {
                            let mut alpha = format!("P5\n{} {}\n255\n",window.raster_width(),window.raster_height()).into_bytes();
                            alpha.extend(frame.front.iter().map(|pixel|(pixel >> 24) as u8));
                            let temporary = directory.join(format!("{}.alpha.pgm.tmp",a[0]));
                            std::fs::write(&temporary,alpha).map_err(|_|5u16)?;
                            std::fs::rename(temporary,directory.join(format!("{}.alpha.pgm",a[0]))).map_err(|_|5u16)?;
                        }
                    }
                }
            }
            7 => {
                window.title = title(self, a[1])?;
            }
            8 => {
                // Validate before dequeueing: a bad destination cannot lose input.
                span(a[1], 21)?;
                let mut out = vec![1];
                pair(&mut out, a[0]);
                if window.input.overflow {
                    window.input.overflow = false;
                    // Overflow recovery may discard stale motion/keys, but a
                    // window-close request must still reach its owner.
                    window.input.events.retain(|e| e.kind == 8);
                    pair(&mut out, window.input.sequence);
                    out.extend([9, 0, 0, window.input.buttons]);
                    out.extend([0; 12]);
                } else if let Some((width, height)) = window.resize.take() {
                    window.resize_in_flight = Some((width, height));
                    pair(&mut out, window.input.sequence);
                    out.extend([10, 0, 0, 0]);
                    pair(&mut out, width);
                    pair(&mut out, height);
                    out.extend([0; 8]);
                } else if let Some(e) = window.input.events.pop_front() {
                    if e.kind == 8 {
                        window.closed = false;
                    }
                    pair(&mut out, e.sequence);
                    out.extend([e.kind, e.flags, e.code, e.buttons]);
                    for n in [e.x, e.y, e.dx, e.dy, e.wheel_x, e.wheel_y] {
                        pair(&mut out, n as u32);
                    }
                } else {
                    out.extend([0; 18]);
                }
                write_cells(self, a[1], &out)?;
            }
            9 => {
                let mut out = vec![1];
                pair(&mut out, a[0]);
                out.extend([window.input.flags(), window.input.buttons]);
                out.extend(window.input.held);
                pair(&mut out, window.input.x as u32);
                pair(&mut out, window.input.y as u32);
                write_cells(self, a[1], &out)?;
            }
            10 => {
                return Ok(u32::from(
                    window.input.overflow
                        || window.resize.is_some()
                        || !window.input.events.is_empty(),
                ))
            }
            12 => {
                let (width, height, flags) = (a[1], a[2], a[3]);
                if width == 0
                    || height == 0
                    || width > MAX_WIDTH
                    || height > MAX_HEIGHT
                    || flags & !3 != 0
                {
                    return Err(22);
                }
                if (width, height) != (window.width, window.height) {
                    #[cfg(target_os="macos")]
                    {window.terminal=None;}
                    let rw=width*window.backing_scale;
                    let rh=height*window.backing_scale;
                    let old_width=window.raster_width();
                    let old_height=window.raster_height();
                    if let Some(frame)=window.colour.as_mut(){
                        frame.back=colour::resized(&frame.back,old_width,old_height,rw,rh,if panel_alpha {0}else{colour::rgba(0)});
                        frame.present();
                    }else{
                        window.back=colour::resized(&window.back,old_width,old_height,rw,rh,0);
                        window.front=Arc::new(window.back.clone());
                    }
                    window.width = width;
                    window.height = height;
                    window.sequence = window.sequence.wrapping_add(1);
                }
                if window.resize == Some((width, height)) {
                    window.resize = None;
                }
                window.resize_in_flight = None;
                if window.flags & 2 == 0 && flags & 2 != 0 {
                    window.transition(InputTransition::Focus { focused: false });
                }
                window.flags = flags;
                if flags & 1 != 0 { window.resize=None;window.resize_in_flight=None; }
                window.configuration = window.configuration.wrapping_add(1);
            }
            13 => {
                window.activation = window.activation.wrapping_add(1);
            }
            14 => {
                window.pointer_requested = true;
                let (x, y) = window.desktop_pointer.ok_or(11u16)?;
                a[1] = x as u32;
                a[2] = y as u32;
            }
            _ => return Err(42),
        }
        if matches!(op, 2 | 4 | 7 | 12 | 24 | 28) {
            capture(&mut s)?;
        }
        Ok(0)
    }
}
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_gx(ctx: *mut Runtime, op: u64, request: u64) -> u64 {
    let rt = unsafe { &mut *ctx };
    let result = (|| {
        let at = span(request as u32, 24)?;
        let mut a = [0; 12];
        for (i, n) in a.iter_mut().enumerate() {
            *n =
                rt.memory.read(at + 2 * i) as u32 | ((rt.memory.read(at + 2 * i + 1) as u32) << 16);
        }
        let result = rt.graphics_call(op, &mut a);
        for (i, n) in a.iter().enumerate() {
            rt.memory.write(at + 2 * i, *n as u16);
            rt.memory.write(at + 2 * i + 1, (*n >> 16) as u16);
        }
        result
    })();
    match result {
        Ok(n) => n as u64,
        Err(e) => (-(e as i32)) as u32 as u64,
    }
}

// All winit/Pixels objects stay on the Mac main thread. An empty registry is a
// live session: closing the last window never terminates the kernel/event loop.
pub(super) fn run(
    shared: Arc<Mutex<Shared>>,
) -> std::result::Result<(), Box<dyn std::error::Error>> {
    if desktop::enabled(){return desktop::run(shared);}
    use pixels::{Pixels, SurfaceTexture};
    use winit::{
        dpi::LogicalSize,
        event::{ElementState, Event, MouseButton, MouseScrollDelta, WindowEvent},
        event_loop::{ControlFlow, EventLoopBuilder},
        platform::run_return::EventLoopExtRunReturn,
        window::WindowBuilder,
    };
    struct View {
        pixels: Pixels,
        window: winit::window::Window,
        sequence: u64,
        title: String,
        visible: bool,
        dimensions: (u32, u32),
        resizable: bool,
        activation: u64,
        configuration: u64,
        pixel_scale:u32,
        zoom_percent:u32,
        presentation_renderer:Option<PresentationRenderer>,
    }
    impl View{
        fn window_pixel(&self,position:(f32,f32))->(i32,i32){
            if self.zoom_percent==100{
                return match self.pixels.window_pos_to_pixel(position){
                    Ok((x,y))=>(x as i32,y as i32),Err((x,y))=>(x as i32,y as i32),
                };
            }
            let size=self.window.inner_size();
            presentation_pixel(position,(self.dimensions.0*self.pixel_scale,self.dimensions.1*self.pixel_scale),(size.width,size.height))
        }
    }
    fn monitor_size(window:&winit::window::Window)->Option<(u32,u32)>{
        window.current_monitor().map(|m|{
            let size=m.size().to_logical::<u32>(m.scale_factor());
            (size.width,size.height)
        })
    }
    let mut event_loop = EventLoopBuilder::new().build();
    #[cfg(target_os = "macos")]
    super::hosted_graphics_macos::independent_windows();
    let mut views: BTreeMap<u32, View> = BTreeMap::new();
    let mut failure = None;
    event_loop.run_return(|event, target, flow| {
        *flow = ControlFlow::WaitUntil(
            std::time::Instant::now() + std::time::Duration::from_millis(16),
        );
        if shared.lock().unwrap().finished {
            *flow = ControlFlow::Exit;
            return;
        }
        let result: std::result::Result<(), Box<dyn std::error::Error>> = (|| {
            match event {
                Event::MainEventsCleared => {
                    {
                        let mut s=shared.lock().unwrap();
                        let mut changed=false;
                        for (&id,view) in &views{
                            if let Some(w)=s.windows.get_mut(&id){
                                let previous=w.zoom_percent;
                                w.fit_presentation(monitor_size(&view.window));
                                changed|=previous!=w.zoom_percent;
                            }
                        }
                        if changed{capture(&mut s).map_err(|e|format!("GX capture failed: {e}"))?;}
                    }
                    let descriptions: Vec<_> = {
                        let s = shared.lock().unwrap();
                        s.windows
                            .iter()
                            .filter(|(_, w)| w.flags & 2 == 0)
                            .map(|(&id, w)| {
                                (
                                    id,
                                    w.width,
                                    w.height,
                                    w.title.clone(),
                                    w.sequence,
                                    w.flags & 1 == 0,
                                    w.activation,
                                    w.configuration,
                                    w.resize.is_some() || w.resize_in_flight.is_some(),
                                    w.pixel_scale(),
                                    w.zoom_percent,
                                )
                            })
                            .collect()
                    };
                    views.retain(|id, _| descriptions.iter().any(|d| d.0 == *id));
                    for (
                        id,
                        width,
                        height,
                        title,
                        sequence,
                        resizable,
                        activation,
                        mut configuration,
                        resize_pending,
                        pixel_scale,
                        mut zoom_percent,
                    ) in descriptions
                    {
                        if !views.contains_key(&id) {
                            let monitor=target.primary_monitor().or_else(||target.available_monitors().next()).map(|m|{
                                let size=m.size().to_logical::<u32>(m.scale_factor());(size.width,size.height)
                            });
                            {
                                let mut s=shared.lock().unwrap();
                                if let Some(w)=s.windows.get_mut(&id){
                                    w.fit_presentation(monitor);zoom_percent=w.zoom_percent;configuration=w.configuration;
                                }
                            }
                            let (pw,ph)=presentation_size(width,height,zoom_percent);
                            let (minw,minh)=presentation_size(8,16,zoom_percent);
                            let (maxw,maxh)=presentation_size(MAX_WIDTH,MAX_HEIGHT,zoom_percent);
                            let window = WindowBuilder::new()
                                .with_title(&title)
                                .with_inner_size(LogicalSize::new(pw,ph))
                                .with_min_inner_size(LogicalSize::new(minw,minh))
                                .with_max_inner_size(LogicalSize::new(maxw,maxh))
                                .with_resizable(resizable)
                                .build(target)?;
                            window.set_ime_allowed(true);
                            {
                                let mut s=shared.lock().unwrap();
                                if let Some(w)=s.windows.get_mut(&id){
                                    w.prefer_scale(if window.scale_factor()>=1.5{2}else{1});
                                    w.fit_presentation(monitor_size(&window));
                                    zoom_percent=w.zoom_percent;
                                    configuration=w.configuration;
                                }
                                capture(&mut s).map_err(|e|format!("GX capture failed: {e}"))?;
                            }
                            let (pw,ph)=presentation_size(width,height,zoom_percent);
                            let current=window.inner_size().to_logical::<u32>(window.scale_factor());
                            if (current.width,current.height)!=(pw,ph){window.set_inner_size(LogicalSize::new(pw,ph));}
                            let (minw,minh)=presentation_size(8,16,zoom_percent);
                            let (maxw,maxh)=presentation_size(MAX_WIDTH,MAX_HEIGHT,zoom_percent);
                            window.set_min_inner_size(Some(LogicalSize::new(minw,minh)));
                            window.set_max_inner_size(Some(LogicalSize::new(maxw,maxh)));
                            let size = window.inner_size();
                            let pixels = Pixels::new(
                                width*pixel_scale,
                                height*pixel_scale,
                                SurfaceTexture::new(size.width.max(1), size.height.max(1), &window),
                            )?;
                            views.insert(
                                id,
                                View {
                                    window,
                                    pixels,
                                    sequence: 0,
                                    title: title.clone(),
                                    visible: true,
                                    dimensions: (width, height),
                                    resizable,
                                    activation: 0,
                                    configuration,
                                    pixel_scale,
                                    zoom_percent,
                                    presentation_renderer:None,
                                },
                            );
                        }
                        let view = views.get_mut(&id).unwrap();
                        if zoom_percent!=100&&view.presentation_renderer.is_none(){
                            view.presentation_renderer=Some(PresentationRenderer::new(view.pixels.device(),view.pixels.surface_texture_format()));
                        }
                        if view.zoom_percent!=zoom_percent{
                            let (minw,minh)=presentation_size(8,16,zoom_percent);
                            let (maxw,maxh)=presentation_size(MAX_WIDTH,MAX_HEIGHT,zoom_percent);
                            view.window.set_min_inner_size(Some(LogicalSize::new(minw,minh)));
                            view.window.set_max_inner_size(Some(LogicalSize::new(maxw,maxh)));
                            view.zoom_percent=zoom_percent;
                            view.window.request_redraw();
                        }
                        if view.activation != activation {
                            view.window.set_minimized(false);
                            view.window.focus_window();
                            view.activation = activation;
                        }
                        if view.dimensions != (width, height)||view.pixel_scale!=pixel_scale {
                            view.pixels.resize_buffer(width*pixel_scale, height*pixel_scale)?;
                            view.dimensions = (width, height);
                            view.pixel_scale=pixel_scale;
                            view.sequence = 0;
                        }
                        if view.configuration != configuration {
                            // A refused guest resize acknowledges the old size too.
                            // Restore native geometry even when pixels did not change.
                            let current = view.window.inner_size()
                                .to_logical::<u32>(view.window.scale_factor());
                            let (pw,ph)=presentation_size(width,height,zoom_percent);
                            if !resize_pending && (current.width, current.height) != (pw,ph) {
                                view.window.set_inner_size(LogicalSize::new(pw,ph));
                            }
                            view.configuration = configuration;
                        }
                        if view.resizable != resizable {
                            view.window.set_resizable(resizable);
                            view.resizable = resizable;
                        }
                        if title != view.title {
                            view.window.set_title(&title);
                            view.title = title;
                        }
                        if sequence != view.sequence {
                            view.window.request_redraw();
                        }
                    }
                    #[cfg(target_os = "macos")]
                    {
                        use winit::platform::macos::WindowExtMacOS;
                        for (&id, view) in &views {
                            let requested = shared
                                .lock()
                                .unwrap()
                                .windows
                                .get(&id)
                                .is_some_and(|w| w.pointer_requested);
                            if requested {
                                let p = super::hosted_graphics_macos::pointer(&view.window);
                                let position = (
                                    p.0 * view.window.scale_factor() as f32,
                                    p.1 * view.window.scale_factor() as f32,
                                );
                                let (x,y)=view.window_pixel(position);
                                if let Some(w) = shared.lock().unwrap().windows.get_mut(&id) {
                                    w.desktop_pointer = Some((x/view.pixel_scale as i32, y/view.pixel_scale as i32));
                                }
                            }
                        }
                        super::hosted_graphics_macos::update(
                            shared.clone(),
                            views
                                .iter()
                                .map(|(&id, v)| (id, v.title.clone(), v.window.ns_window()))
                                .collect(),
                        );
                    }
                }
                Event::RedrawRequested(window_id) => {
                    if let Some((&id, view)) =
                        views.iter_mut().find(|(_, v)| v.window.id() == window_id)
                    {
                        if !view.visible {
                            return Ok(());
                        }
                        let frame = {
                            let s = shared.lock().unwrap();
                            s.windows.get(&id).map(|w| (w.front.clone(),w.rgba(), w.sequence, (w.width,w.height),w.pixel_scale()))
                        };
                        if let Some((frame,rgba, sequence, dimensions,pixel_scale)) = frame {
                            // The guest can accept another resize between the
                            // geometry pass and this redraw. Never reinterpret
                            // its new frame with the previous texture stride.
                            if dimensions != view.dimensions||pixel_scale!=view.pixel_scale {
                                view.window.request_redraw();
                                return Ok(());
                            }
                            if sequence != view.sequence {
                                if let Some(rgba)=rgba {
                                    for (pixel,c) in view.pixels.frame_mut().chunks_exact_mut(4).zip(rgba.iter()){
                                        pixel.copy_from_slice(&c.to_le_bytes());
                                    }
                                }else{for (pixel, c) in view
                                    .pixels
                                    .frame_mut()
                                    .chunks_exact_mut(4)
                                    .zip(frame.iter())
                                {
                                    let [r, g, b] = rvm::display_rgb565::rgb565_to_rgb888(*c);
                                    pixel.copy_from_slice(&[r, g, b, 255]);
                                }}
                                view.sequence = sequence;
                            }
                            if view.zoom_percent!=100{
                                let size=view.window.inner_size();
                                if let Some(renderer)=view.presentation_renderer.as_ref(){
                                    view.pixels.render_with(|encoder,target,context|{
                                        renderer.render(encoder,target,context,(size.width,size.height));Ok(())
                                    })?;
                                }
                            }else{view.pixels.render()?;}
                        }
                    }
                }
                Event::WindowEvent { window_id, event } => {
                    if let Some((&id, view)) =
                        views.iter_mut().find(|(_, v)| v.window.id() == window_id)
                    {
                        let mut s = shared.lock().unwrap();
                        let Some(w) = s.windows.get_mut(&id) else {
                            return Ok(());
                        };
                        match event {
                            WindowEvent::CloseRequested => w.transition(InputTransition::Close),
                            WindowEvent::Focused(focused) => {
                                w.transition(InputTransition::Focus { focused })
                            }
                            WindowEvent::CursorEntered { .. } => {
                                w.transition(InputTransition::Enter { inside: true })
                            }
                            WindowEvent::CursorLeft { .. } => {
                                w.transition(InputTransition::Enter { inside: false })
                            }
                            WindowEvent::KeyboardInput { input, .. } => {
                                #[cfg(target_os = "macos")]
                                {
                                    #[allow(deprecated)]
                                    let menu_shortcut = input.modifiers.logo()
                                        && matches!(
                                            input.virtual_keycode,
                                            Some(
                                                winit::event::VirtualKeyCode::N
                                                    | winit::event::VirtualKeyCode::Q
                                                    | winit::event::VirtualKeyCode::H
                                                    | winit::event::VirtualKeyCode::W
                                                    | winit::event::VirtualKeyCode::C
                                                    | winit::event::VirtualKeyCode::V
                                                    | winit::event::VirtualKeyCode::X
                                                    | winit::event::VirtualKeyCode::A
                                            )
                                        );
                                    if menu_shortcut {
                                        return Ok(());
                                    }
                                }
                                if let Some(key) = input
                                    .virtual_keycode
                                    .and_then(rvm::display_rgb565::key_usage)
                                {
                                    #[allow(deprecated)]
                                    let text = rvm::display_rgb565::text_usage(key)
                                        && !input.modifiers.ctrl()
                                        && !input.modifiers.logo();
                                    let down = input.state == ElementState::Pressed;
                                    w.transition(if text {
                                        InputTransition::TextKey { key, down }
                                    } else {
                                        InputTransition::Key { key, down }
                                    });
                                }
                            }
                            WindowEvent::Ime(winit::event::Ime::Commit(text)) => {
                                if w.input.held(224) || w.input.held(228) || w.input.held(227) || w.input.held(231) { return Ok(()); }
                                for ch in text.chars().filter(|ch| !ch.is_control()) {
                                    w.transition(InputTransition::Text { scalar: ch as u32 });
                                }
                            }
                            WindowEvent::ReceivedCharacter(ch) => {
                                if !ch.is_control()
                                    && !w.input.held(224)
                                    && !w.input.held(228)
                                    && !w.input.held(227)
                                    && !w.input.held(231)
                                {
                                    w.transition(InputTransition::Text { scalar: ch as u32 });
                                }
                            }
                            WindowEvent::ModifiersChanged(mods) => {
                                for (left, right, pressed) in [
                                    (224, 228, mods.ctrl()),
                                    (225, 229, mods.shift()),
                                    (226, 230, mods.alt()),
                                    (227, 231, mods.logo()),
                                ] {
                                    if !pressed {
                                        w.transition(InputTransition::Key {
                                            key: left,
                                            down: false,
                                        });
                                        w.transition(InputTransition::Key {
                                            key: right,
                                            down: false,
                                        });
                                    } else if !w.input.held(left) && !w.input.held(right) {
                                        w.transition(InputTransition::Key {
                                            key: left,
                                            down: true,
                                        });
                                    }
                                }
                            }
                            WindowEvent::CursorMoved { position, .. } => {
                                let (x,y)=view.window_pixel((position.x as f32,position.y as f32));
                                w.transition(InputTransition::Motion { x:x/view.pixel_scale as i32, y:y/view.pixel_scale as i32 });
                            }
                            WindowEvent::MouseInput { state, button, .. } => {
                                let button = match button {
                                    MouseButton::Left => 0,
                                    MouseButton::Right => 1,
                                    MouseButton::Middle => 2,
                                    MouseButton::Other(n) => n.saturating_add(3),
                                };
                                w.transition(InputTransition::Button {
                                    button,
                                    down: state == ElementState::Pressed,
                                    x: w.input.x,
                                    y: w.input.y,
                                });
                            }
                            WindowEvent::MouseWheel { delta, .. } => {
                                let (x, y) = match delta {
                                    MouseScrollDelta::LineDelta(x, y) => {
                                        (f64::from(x), f64::from(y))
                                    }
                                    MouseScrollDelta::PixelDelta(p) => (
                                        p.x / view.window.scale_factor() / 40.0,
                                        p.y / view.window.scale_factor() / 40.0,
                                    ),
                                };
                                w.transition(InputTransition::Wheel {
                                    x: (x * 65536.0).round() as i32,
                                    y: (y * 65536.0).round() as i32,
                                });
                            }
                            WindowEvent::Resized(size) => {
                                let logical = size.to_logical::<u32>(view.window.scale_factor());
                                let (width,height)=guest_size(logical.width,logical.height,view.zoom_percent);
                                w.request_resize(width,height);
                                drop(s);
                                view.visible = size.width > 0 && size.height > 0;
                                if view.visible {
                                    view.pixels.resize_surface(size.width, size.height)?;
                                    view.window.request_redraw();
                                }
                            }
                            WindowEvent::ScaleFactorChanged { scale_factor,new_inner_size } => {
                                w.prefer_scale(if scale_factor>=1.5{2}else{1});
                                let previous=w.zoom_percent;
                                w.fit_presentation(monitor_size(&view.window));
                                let changed=previous!=w.zoom_percent;
                                if changed{capture(&mut s).map_err(|e|format!("GX capture failed: {e}"))?;}
                                drop(s);
                                view.visible =
                                    new_inner_size.width > 0 && new_inner_size.height > 0;
                                if view.visible {
                                    view.pixels.resize_surface(
                                        new_inner_size.width,
                                        new_inner_size.height,
                                    )?;
                                    view.window.request_redraw();
                                }
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
            Ok(())
        })();
        if let Err(e) = result {
            failure = Some(e);
            *flow = ControlFlow::Exit;
        }
    });
    if let Some(e) = failure {
        Err(e)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn create(rt: &mut Runtime, w: u32, h: u32) -> u32 {
        let title = 0x10000;
        rt.memory.write(BANK, 0);
        let mut a = [0; 12];
        a[0] = w;
        a[1] = h;
        a[2] = title;
        rt.graphics_call(1, &mut a).unwrap();
        a[0]
    }
    #[test]
    fn nested_presentation_returns_actual_one_hundred_without_resizing_pixels_or_input(){
        let mut rt=Runtime::new();let id=create(&mut rt,640,400);
        let mut shared=rt.graphics.shared.lock().unwrap();let w=shared.windows.get_mut(&id).unwrap();
        let front=w.front.clone();let back=w.back.clone();
        w.transition(InputTransition::Motion{x:17,y:-3});
        w.request_presentation(200,false);
        assert_eq!((w.requested_zoom,w.zoom_percent,w.presentation_size()),(200,200,(1280,800)));
        w.request_presentation(300,true);
        assert_eq!((w.requested_zoom,w.zoom_percent,w.presentation_size()),(300,100,(640,400)));
        assert_eq!((w.width,w.height,w.backing_scale,w.input.x,w.input.y),(640,400,1,17,-3));
        assert_eq!(w.resize,None);assert_eq!(w.resize_in_flight,None);assert_eq!(w.back,back);
        assert!(Arc::ptr_eq(&front,&w.front));
        let configuration=w.configuration;w.request_presentation(150,true);
        assert_eq!((w.requested_zoom,w.zoom_percent,w.configuration),(150,100,configuration));
        w.request_presentation(150,false);
        assert_eq!((w.requested_zoom,w.zoom_percent),(150,150));
    }
    #[test]
    fn presentation_zoom_fits_monitor_points_without_changing_the_request(){
        assert_eq!(fitted_zoom(800,600,150,None),150);
        assert_eq!(presentation_size(800,600,150),(1200,900));
        assert_eq!(fitted_zoom(800,600,150,Some((1440,900))),135);
        assert_eq!(presentation_size(800,600,135),(1080,810));
        assert_eq!(fitted_zoom(800,600,150,Some((1920,1080))),150);
        assert_eq!(fitted_zoom(800,600,153,Some((1920,1080))),153);
        assert_eq!(fitted_zoom(800,600,150,Some((640,480))),100);
        assert_eq!(presentation_size(8,16,150),(12,24));
        assert_eq!(presentation_size(MAX_WIDTH,MAX_HEIGHT,150),(2880,1620));
    }
    #[test]
    fn presentation_zoom_rounding_does_not_feed_back_into_guest_resize(){
        for percent in 100..=300{
            for width in 1..=MAX_WIDTH{
                let (pw,ph)=presentation_size(width,601,percent);
                assert_eq!(guest_size(pw,ph,percent),(width,601));
            }
        }
        assert_eq!(guest_size(1200,900,150),(800,600));
        assert_eq!(guest_size(1350,1050,150),(900,700));
    }
    #[test]
    fn presentation_zoom_fractional_render_and_pointer_use_the_same_viewport(){
        assert_eq!(presentation_viewport((800,600),(2160,1620)),(0.0,0.0,2160.0,1620.0));
        assert_eq!(presentation_pixel((1080.0,810.0),(800,600),(2160,1620)),(400,300));
        assert_eq!(presentation_pixel((2159.0,1619.0),(800,600),(2160,1620)),(799,599));
        // Resizing can temporarily change the aspect ratio before the guest
        // acknowledges it. Drawing and input share the same letterbox margins.
        assert_eq!(presentation_viewport((800,600),(2160,1800)),(0.0,90.0,2160.0,1620.0));
        assert_eq!(presentation_pixel((1080.0,900.0),(800,600),(2160,1800)),(400,300));
        assert_eq!(presentation_pixel((0.0,0.0),(800,600),(2160,1800)),(0,-34));
        assert_eq!(presentation_pixel((1080.0,810.0),(1600,1200),(2160,1620)),(800,600));
    }
    #[cfg(target_os="macos")]
    #[test]
    #[ignore="requires a native Metal GPU adapter"]
    fn presentation_zoom_shader_validates_on_metal(){
        use pixels::wgpu as w;
        fn complete<F:std::future::Future>(future:F)->F::Output{
            struct Wake(std::thread::Thread);
            impl std::task::Wake for Wake{fn wake(self:Arc<Self>){self.0.unpark();}}
            let waker=std::task::Waker::from(Arc::new(Wake(std::thread::current())));
            let mut context=std::task::Context::from_waker(&waker);
            let mut future=std::pin::pin!(future);
            loop{match future.as_mut().poll(&mut context){
                std::task::Poll::Ready(result)=>return result,
                std::task::Poll::Pending=>std::thread::park(),
            }}
        }
        let instance=w::Instance::new(w::InstanceDescriptor{backends:w::Backends::METAL,..Default::default()});
        let adapter=complete(instance.request_adapter(&w::RequestAdapterOptions::default())).expect("Metal adapter");
        let (device,_queue)=complete(adapter.request_device(&w::DeviceDescriptor{label:None,features:w::Features::empty(),limits:w::Limits::downlevel_defaults()},None)).unwrap();
        device.push_error_scope(w::ErrorFilter::Validation);
        let _renderer=PresentationRenderer::new(&device,w::TextureFormat::Bgra8UnormSrgb);
        device.poll(w::Maintain::Wait);
        assert!(complete(device.pop_error_scope()).is_none());
    }
    #[test]
    fn presentation_zoom_is_per_window_and_preserves_pixels_backing_and_input(){
        let mut rt=Runtime::new();let id=create(&mut rt,4,3);let other=create(&mut rt,4,3);
        rt.graphics_call(24,&mut [id,2,0,0,0,0,0,0,0,0,0,0]).unwrap();
        rt.graphics_call(5,&mut [id,0,0,8,6,0xabcd,0,0,0,0,0,0]).unwrap();
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let shared=rt.graphics.shared.clone();
        let (front,back,sequence,configuration)={
            let mut s=shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
            w.transition(InputTransition::Motion{x:3,y:2});
            (w.front.clone(),w.back.clone(),w.sequence,w.configuration)
        };
        let mut zoom=[id,150,0,0,0,0,0,0,0,0,0,0];
        assert_eq!(rt.graphics_call(28,&mut zoom),Ok(0));assert_eq!(zoom[1],150);
        {
            let s=shared.lock().unwrap();let w=&s.windows[&id];
            assert_eq!((w.width,w.height,w.raster_width(),w.raster_height(),w.pixel_scale()),(4,3,8,6,2));
            assert_eq!((w.requested_zoom,w.zoom_percent,w.presentation_size()),(150,150,(6,5)));
            assert!(Arc::ptr_eq(&front,&w.front));assert_eq!(back,w.back);
            assert_eq!(sequence,w.sequence);assert_eq!(configuration+1,w.configuration);
            assert_eq!((w.input.x,w.input.y),(3,2));assert_eq!(w.resize,None);
            assert_eq!(s.windows[&other].zoom_percent,100);
        }
        rt.graphics_call(12,&mut [id,4,3,0,0,0,0,0,0,0,0,0]).unwrap();
        {
            let mut s=shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
            let (pw,ph)=w.presentation_size();let (gw,gh)=guest_size(pw,ph,w.zoom_percent);
            w.request_resize(gw,gh);assert_eq!(w.resize,None);
            assert!(Arc::ptr_eq(&front,&w.front));assert_eq!(back,w.back);
        }
        for percent in [0,99,301,u32::MAX]{
            assert_eq!(rt.graphics_call(28,&mut [id,percent,0,0,0,0,0,0,0,0,0,0]),Err(22));
            let s=shared.lock().unwrap();let w=&s.windows[&id];
            assert_eq!((w.requested_zoom,w.zoom_percent,w.configuration),(150,150,configuration+2));
            assert!(Arc::ptr_eq(&front,&w.front));assert_eq!(back,w.back);
        }
        assert_eq!(rt.graphics_call(28,&mut [other+1,150,0,0,0,0,0,0,0,0,0,0]),Err(22));
    }
    #[test]
    fn presentation_zoom_refits_on_monitor_change_and_preserves_true_colour(){
        let mut rt=Runtime::new();let id=create_colour(&mut rt,800,600);
        rt.graphics_call(27,&mut [id,0,0,800,600,0x123456,0,0,0,0,0,0]).unwrap();
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let shared=rt.graphics.shared.clone();
        let front=shared.lock().unwrap().windows[&id].rgba().unwrap();
        rt.graphics_call(28,&mut [id,150,0,0,0,0,0,0,0,0,0,0]).unwrap();
        {
            let mut s=shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
            w.fit_presentation(Some((1440,900)));
            assert_eq!((w.requested_zoom,w.zoom_percent,w.presentation_size()),(150,135,(1080,810)));
            assert!(Arc::ptr_eq(&front,&w.rgba().unwrap()));
        }
        let mut a=[id,150,0,0,0,0,0,0,0,0,0,0];
        rt.graphics_call(28,&mut a).unwrap();assert_eq!(a[1],135);
        {
            let mut s=shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
            w.fit_presentation(Some((1920,1080)));assert_eq!(w.zoom_percent,150);
            w.fit_presentation(None);assert_eq!(w.zoom_percent,150);
            assert_eq!((w.width,w.height,w.backing_scale,w.raster_width(),w.raster_height()),(800,600,1,800,600));
            assert!(Arc::ptr_eq(&front,&w.rgba().unwrap()));
            assert!(w.colour.as_ref().unwrap().back.iter().all(|&p|p==colour::rgba(0x123456)));
        }
    }
    #[test]
    fn backing_density_keeps_logical_input_and_resizes_physical_surfaces(){
        let mut rt=Runtime::new();let id=create(&mut rt,4,3);let shared=rt.graphics.shared.clone();
        let mut a=[id,2,0,0,0,0,0,0,0,0,0,0];
        assert_eq!(rt.graphics_call(24,&mut a),Ok(0));assert_eq!(&a[1..4],&[2,8,6]);
        rt.memory.write(2*BANK,0xffff);
        assert_eq!(rt.graphics_call(3,&mut [id,7,5,1,1,1,0x20000,0,0,0,0,0]),Ok(0));
        assert_eq!(rt.graphics_call(3,&mut [id,8,5,1,1,1,0x20000,0,0,0,0,0]),Err(22));
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let old=shared.lock().unwrap().windows[&id].front.clone();assert_eq!(old.len(),48);assert_eq!(old[47],0xffff);
        rt.graphics_call(12,&mut [id,6,4,0,0,0,0,0,0,0,0,0]).unwrap();
        {let mut s=shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
            assert_eq!((w.width,w.height,w.back.len()),(6,4,96));assert_eq!(w.back[5*12+7],0xffff);
            w.transition(InputTransition::Motion{x:3,y:2});assert_eq!((w.input.x,w.input.y),(3,2));
            w.prefer_scale(2);
        }
        a[1]=0;rt.graphics_call(24,&mut a).unwrap();assert_eq!(&a[1..4],&[2,12,8]);
        shared.lock().unwrap().windows.get_mut(&id).unwrap().prefer_scale(1);
        assert_eq!(shared.lock().unwrap().windows[&id].resize,Some((6,4)));
        a[1]=0;rt.graphics_call(24,&mut a).unwrap();assert_eq!(&a[1..4],&[1,6,4]);
        a[1]=3;assert_eq!(rt.graphics_call(24,&mut a),Err(22));
        {let mut s=shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
            w.flags=1;w.resize=Some((9,7));w.prefer_scale(2);
            assert_eq!(w.resize,Some((9,7)),"density must preserve a pending logical resize, even for fixed windows");
        }
        assert_eq!(old[47],0xffff);
    }
    #[cfg(target_os="macos")]
    #[test]
    fn explicit_backing_switch_clears_terminal_texture_even_at_one(){
        let mut rt=Runtime::new();let id=create(&mut rt,4,3);let shared=rt.graphics.shared.clone();
        {let mut s=shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
            w.terminal=Some(super::super::hosted_terminal::Frame::from_surface(w.width,w.height,&w.back,&w.front));
            assert_eq!(w.pixel_scale(),2);
        }
        let mut a=[id,1,0,0,0,0,0,0,0,0,0,0];rt.graphics_call(24,&mut a).unwrap();
        let s=shared.lock().unwrap();assert_eq!(s.windows[&id].pixel_scale(),1);assert!(s.windows[&id].rgba().is_none());
    }
    #[test]
    fn bgr_raster_matches_mask_opacity_math_and_preserves_padding(){
        let mut rt=Runtime::new();let start=3*BANK-2;let dst=super::super::memory::pointer(start) as u32;
        let mask_start=4*BANK-1;let mask=super::super::memory::pointer(mask_start) as u32;
        let mut a=[dst,mask,256,1,770,256,30,60,90,255,0,0];
        let values:Vec<u16>=(0..256).collect();rt.memory.write_from(mask_start,&values);
        for opacity in 0..256{
            let mut background=vec![0x55;770];for p in background[..768].chunks_exact_mut(3){p.copy_from_slice(&[100,150,200]);}
            rt.memory.write_from(start,&background);a[9]=opacity;
            assert_eq!(rt.graphics_call(25,&mut a),Ok(0));
            let mut actual=vec![0;770];rt.memory.read_into(start,&mut actual);
            for (i,p) in actual[..768].chunks_exact(3).enumerate(){
                let mix=if opacity>=253{i as u32}else{(i as u32*opacity)>>8};
                for c in 0..3{let bg=[100,150,200][c];let fg=[30,60,90][c];
                    let expected=if mix>=253{fg}else if mix==0{bg}else{(fg*mix+bg*(255-mix))>>8};
                    assert_eq!(p[c],expected as u16,"opacity {opacity}, mask {i}, channel {c}");
                }
            }
            assert_eq!(&actual[768..],&[0x55,0x55]);
        }
        let before:Vec<_>=rt.memory.cells(start..start+770).collect();
        for(index,value,error)in [(0,0,14),(1,dst,22),(4,767,22),(8,256,22)]{
            let old=a[index];a[index]=value;assert_eq!(rt.graphics_call(25,&mut a),Err(error));a[index]=old;
            assert_eq!(rt.memory.cells(start..start+770).collect::<Vec<_>>(),before);
        }
    }
    #[cfg(target_os="macos")]
    #[test]
    fn native_terminal_validates_complete_rows_and_publishes_atomically(){
        let mut rt=Runtime::new();let (cw,ch)=super::super::hosted_terminal::metrics();
        let id=create(&mut rt,4*cw,2*ch);let start=3*BANK-2;
        let data=[7,1,1,b'A' as u16,0x14,2,4,0xf0,0x9f,0x99,0x82,7,0,0,7,1,1,b'B' as u16];
        rt.memory.write_from(start,&data);
        let mut a=[id,4,2,0,super::super::memory::pointer(start) as u32,data.len() as u32,0,0,0,0,0,0];
        let shared=rt.graphics.shared.clone();
        assert_eq!(rt.graphics_call(22,&mut a),Ok(0));
        let front=shared.lock().unwrap().windows[&id].rgba().unwrap();
        assert!(front.iter().all(|&p|p==0xff000000));
        a[0]=id+1;assert_eq!(rt.graphics_call(22,&mut a),Err(22));a[0]=id;
        // A bad final octet must not partially replace the earlier cells.
        let before=shared.lock().unwrap().windows[&id].terminal.as_ref().unwrap().back.clone();
        rt.memory.write(start+data.len()-1,0x100);
        assert_eq!(rt.graphics_call(22,&mut a),Err(22));
        assert_eq!(shared.lock().unwrap().windows[&id].terminal.as_ref().unwrap().back,before);
        rt.memory.write(start+data.len()-1,b'B' as u16);
        a[8]=1;a[6]=4;assert_eq!(rt.graphics_call(22,&mut a),Err(22));a[8]=0;
        a[5]-=1;assert_eq!(rt.graphics_call(22,&mut a),Err(22));a[5]+=1;
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let after=shared.lock().unwrap().windows[&id].rgba().unwrap();
        assert!(after.iter().any(|&p|p!=0xff000000));assert!(!Arc::ptr_eq(&front,&after));
        let now=std::time::Instant::now();
        for _ in 0..100 {rt.graphics_call(22,&mut a).unwrap();}
        eprintln!("native terminal row (4 cells): {:?} average",now.elapsed()/100);
        rt.graphics_call(5,&mut [id,0,0,4*cw,2*ch,0xf800,0,0,0,0,0,0]).unwrap();
        assert!(shared.lock().unwrap().windows[&id].rgba().is_none());
        assert!(shared.lock().unwrap().windows[&id].back.iter().all(|&p|p==0xf800));
    }
    #[test]
    fn clipboard_is_bounded_utf8_atomic_and_validated_before_guest_writes() {
        let mut rt=Runtime::new();let id=create(&mut rt,3,2);
        let bytes="URL Ж🙂".as_bytes();let start=3*BANK-2;
        for (i,b) in bytes.iter().enumerate(){rt.memory.write(start+i,u16::from(*b));}
        let mut a=[id,super::super::memory::pointer(start) as u32,bytes.len() as u32,0,0,0,0,0,0,0,0,0];
        rt.graphics_call(20,&mut a).unwrap();
        assert_eq!(rt.graphics.shared.lock().unwrap().clipboard,"URL Ж🙂");
        a[0]=id+1;assert_eq!(rt.graphics_call(20,&mut a),Err(22));a[0]=id;
        rt.memory.write(start,0x100);assert_eq!(rt.graphics_call(20,&mut a),Err(22));
        rt.memory.write(start,0xff);assert_eq!(rt.graphics_call(20,&mut a),Err(22));
        rt.memory.write(start,0);assert_eq!(rt.graphics_call(20,&mut a),Err(22));
        a[2]=CLIPBOARD_MAX as u32+1;assert_eq!(rt.graphics_call(20,&mut a),Err(22));
        assert_eq!(rt.graphics.shared.lock().unwrap().clipboard,"URL Ж🙂");
        a[1]=0x40000;a[2]=2;rt.memory.write(4*BANK,0xbeef);
        assert_eq!(rt.graphics_call(19,&mut a),Err(7));assert_eq!(rt.memory.read(4*BANK),0xbeef);
        a[2]=CLIPBOARD_MAX as u32;rt.graphics_call(19,&mut a).unwrap();assert_eq!(a[2],bytes.len() as u32);
        for (i,b) in bytes.iter().enumerate(){assert_eq!(rt.memory.read(4*BANK+i),u16::from(*b));}
        a[1]=0;a[2]=0;rt.graphics_call(20,&mut a).unwrap();
        assert_eq!(rt.graphics.shared.lock().unwrap().clipboard_publish,Some(String::new()));
    }
    #[test]
    fn edit_actions_preserve_input_order_and_never_manufacture_control_keys() {
        let mut rt=Runtime::new();let id=create(&mut rt,3,2);
        {let mut s=rt.graphics.shared.lock().unwrap();let w=s.windows.get_mut(&id).unwrap();
         w.transition(InputTransition::Text{scalar:'a' as u32});w.action(2);
         w.transition(InputTransition::Text{scalar:'b' as u32});
         assert_eq!(w.input.events.iter().map(|e|e.kind).collect::<Vec<_>>(),[11,12,11]);
         assert!(w.input.held.iter().all(|x|*x==0)&&w.input.validate(0));}
        let mut a=[id,0x30000,0,0,0,0,0,0,0,0,0,0];
        rt.graphics_call(8,&mut a).unwrap();rt.graphics_call(8,&mut a).unwrap();
        assert_eq!(rt.memory.read(3*BANK+5),12);assert_eq!(rt.memory.read(3*BANK+7),2);
    }
    #[test]
    fn dragging_back_to_the_original_size_supersedes_an_in_flight_resize() {
        let mut rt=Runtime::new();
        let id=create(&mut rt,600,420);
        rt.graphics.shared.lock().unwrap().windows.get_mut(&id).unwrap().request_resize(1280,800);
        let mut event=[id,0x30000,0,0,0,0,0,0,0,0,0,0];
        rt.graphics_call(8,&mut event).unwrap();
        rt.graphics.shared.lock().unwrap().windows.get_mut(&id).unwrap().request_resize(600,420);
        let mut ack=[id,1280,800,0,0,0,0,0,0,0,0,0];
        rt.graphics_call(12,&mut ack).unwrap();
        assert_eq!(rt.graphics.shared.lock().unwrap().windows[&id].resize,Some((600,420)));
        rt.graphics_call(8,&mut event).unwrap();
        assert_eq!(rt.memory.read(3*BANK+9),600);
        assert_eq!(rt.memory.read(3*BANK+11),420);
        ack[1]=600;ack[2]=420;rt.graphics_call(12,&mut ack).unwrap();
        assert_eq!(rt.graphics_call(10,&mut event),Ok(0));
    }
    #[test]
    fn full_hd_resize_preserves_overlap_and_rejects_invalid_geometry_atomically() {
        let mut rt=Runtime::new();let id=create(&mut rt,3,2);
        let mut fill=[id,0,0,3,2,0xabcd,0,0,0,0,0,0];rt.graphics_call(5,&mut fill).unwrap();
        let mut ack=[id,MAX_WIDTH,MAX_HEIGHT,0,0,0,0,0,0,0,0,0];
        rt.graphics_call(12,&mut ack).unwrap();
        {let s=rt.graphics.shared.lock().unwrap();let w=&s.windows[&id];
         assert_eq!(w.back.len(),(MAX_WIDTH*MAX_HEIGHT)as usize);
         assert_eq!(w.back[MAX_WIDTH as usize+2],0xabcd);assert_eq!(w.back[MAX_WIDTH as usize+3],0);}
        ack[1]=MAX_WIDTH+1;assert_eq!(rt.graphics_call(12,&mut ack),Err(22));
        assert_eq!(rt.graphics.shared.lock().unwrap().windows[&id].width,MAX_WIDTH);
        ack[1]=2;ack[2]=1;rt.graphics_call(12,&mut ack).unwrap();
        assert_eq!(rt.graphics.shared.lock().unwrap().windows[&id].back,vec![0xabcd;2]);
        fill[3]=3;assert_eq!(rt.graphics_call(5,&mut fill),Err(22));
    }
    #[test]
    fn scaled_upload_fits_letterboxes_and_validates_before_modifying_pixels() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 8, 8);
        let start = 3 * BANK - 2;
        for (i, value) in [1, 2, 3, 4, 5, 6, 7, 8].iter().enumerate() {
            rt.memory.write(start + i, *value);
        }
        let mut a = [id, 0, 0, 4, 2, 4, super::super::memory::pointer(start) as u32, 0, 0, 0, 0, 0];
        rt.graphics_call(17, &mut a).unwrap();
        let before = rt.graphics.shared.lock().unwrap().windows[&id].back.clone();
        assert_eq!(&before[..16], &[0;16]);
        assert_eq!(&before[16..24], &[1,1,2,2,3,3,4,4]);
        assert_eq!(&before[32..40], &[5,5,6,6,7,7,8,8]);
        assert_eq!(&before[48..], &[0;16]);
        a[5] = 3; assert_eq!(rt.graphics_call(17, &mut a), Err(22));
        a[5] = 4; a[6] = 0; assert_eq!(rt.graphics_call(17, &mut a), Err(14));
        assert_eq!(rt.graphics.shared.lock().unwrap().windows[&id].back, before);
        a[6] = super::super::memory::pointer(start) as u32;
        a[7] = 0;
        assert_eq!(rt.graphics_call(18, &mut a), Err(14));
        assert_eq!(rt.graphics.shared.lock().unwrap().windows[&id].back, before);
        for i in 0..256 { rt.memory.write(4 * BANK + i, i as u16 * 16); }
        a[7] = 0x40000;
        rt.graphics_call(18, &mut a).unwrap();
        assert_eq!(&rt.graphics.shared.lock().unwrap().windows[&id].back[16..24], &[16,16,32,32,48,48,64,64]);
    }
    #[test]
    fn unicode_text_survives_the_gx_cell_transport() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        rt.graphics.shared.lock().unwrap().windows.get_mut(&id).unwrap()
            .transition(InputTransition::Text { scalar: 0x1f642 });
        let mut request = [0; 12]; request[0] = id; request[1] = 0x30000;
        rt.graphics_call(8, &mut request).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 11);
        assert_eq!(u32::from(rt.memory.read(3 * BANK + 13)) | (u32::from(rt.memory.read(3 * BANK + 14)) << 16), 0x1f642);
    }
    #[test]
    fn surfaces_copy_overlap_publish_snapshots_and_reject_stale_handles() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        let second = create(&mut rt, 2, 2);
        let ptr = 0x20000;
        for i in 0..6 {
            rt.memory.write(2 * BANK + i, (i + 1) as u16);
        }
        let mut a = [id, 0, 0, 3, 2, 3, ptr, 0, 0, 0, 0, 0];
        rt.graphics_call(3, &mut a).unwrap();
        a = [id, 0, 0, 2, 2, 1, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(6, &mut a).unwrap();
        a = [0; 12];
        a[0] = id;
        rt.graphics_call(4, &mut a).unwrap();
        let snapshot = rt.graphics.shared.lock().unwrap().windows[&id]
            .front
            .clone();
        assert_eq!(&**snapshot, &[1, 1, 2, 4, 4, 5]);
        a = [id, 0, 0, 3, 2, 9, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(5, &mut a).unwrap();
        assert_eq!(&**snapshot, &[1, 1, 2, 4, 4, 5]);
        assert_eq!(
            rt.graphics.shared.lock().unwrap().windows[&second].back,
            vec![0; 4]
        );
        a = [id, 3, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(rt.graphics_call(5, &mut a), Err(22));
        a = [0; 12];
        a[0] = id;
        rt.graphics_call(2, &mut a).unwrap();
        assert_eq!(rt.graphics_call(4, &mut a), Err(22));
        assert!(create(&mut rt, 1, 1) > second);
    }
    #[test]
    fn bgr888_upload_preserves_padding_snapshots_and_validates_spans() {
        let mut rt=Runtime::new();let id=create(&mut rt,4,3);
        let start=3*BANK-2;let ptr=super::super::memory::pointer(start) as u32;
        rt.memory.write_from(start,&[0,0,255,0,255,0,999,999,255,0,0,255,255,255]);
        let mut a=[id,1,1,2,2,8,ptr,0,0,0,0,0];
        let shared=rt.graphics.shared.clone();
        let before=shared.lock().unwrap().windows[&id].front.clone();
        assert_eq!(rt.graphics_call(23,&mut a),Ok(0));
        assert_eq!(shared.lock().unwrap().windows[&id].back,
            vec![0,0,0,0,0,0xf800,0x07e0,0,0,0x001f,0xffff,0]);
        assert!(before.iter().all(|&p|p==0));
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let committed=shared.lock().unwrap().windows[&id].front.clone();
        for (index,value,error) in [(0,id+1,22),(1,4,22),(5,5,22),(6,0,14),(5,u32::MAX,14)] {
            let old=a[index];a[index]=value;assert_eq!(rt.graphics_call(23,&mut a),Err(error));a[index]=old;
            assert_eq!(shared.lock().unwrap().windows[&id].back,**committed);
        }
        a[3]=0;a[5]=0;a[6]=0;assert_eq!(rt.graphics_call(23,&mut a),Ok(0));
        assert!(before.iter().all(|&p|p==0));
    }
    fn create_colour(rt:&mut Runtime,w:u32,h:u32)->u32{
        rt.memory.write(BANK,0);
        let mut a=[w,h,0x10000,0,2,0,0,0,0,0,0,0];
        rt.graphics_call(1,&mut a).unwrap();a[0]
    }
    #[test]
    fn true_colour_discovery_upload_and_publication_preserve_all_channel_bits(){
        let mut rt=Runtime::new();let mut formats=[0;12];
        assert_eq!(rt.graphics_call(26,&mut formats),Ok(0));assert_eq!(&formats[1..3],&[2,3]);
        assert_eq!(rt.graphics_call(1,&mut [1,1,0x10000,0,3,0,0,0,0,0,0,0]),Err(22));
        let id=create_colour(&mut rt,256,2);let shared=rt.graphics.shared.clone();
        let start=3*BANK-2;let ptr=super::super::memory::pointer(start) as u32;
        let mut cells=Vec::new();let mut expected=Vec::new();
        for row in 0..2{
            for i in 0..256u16{
                let (r,g,b)=(i as u8,(255-i) as u8,(i.wrapping_mul(17)+row) as u8);
                cells.extend([b as u16,g as u16,r as u16]);expected.extend([r,g,b]);
            }
            if row==0{cells.extend([999,999]);}
        }
        rt.memory.write_from(start,&cells);
        let mut upload=[id,0,0,256,2,770,ptr,0,0,0,0,0];
        rt.graphics_call(23,&mut upload).unwrap();
        {
            let s=shared.lock().unwrap();let w=&s.windows[&id];
            assert!(w.back.is_empty()&&w.front.is_empty());assert_eq!(w.format(),2);
            assert_eq!(w.pixel_scale(),1);assert_eq!(w.rgb_capture(),vec![0;256*2*3]);
        }
        let back=shared.lock().unwrap().windows[&id].colour.as_ref().unwrap().back.clone();
        for (index,value,error) in [(0,id+1,22),(1,256,22),(5,767,22),(6,0,14),(5,u32::MAX,14)]{
            let old=upload[index];upload[index]=value;
            assert_eq!(rt.graphics_call(23,&mut upload),Err(error));upload[index]=old;
            assert_eq!(shared.lock().unwrap().windows[&id].colour.as_ref().unwrap().back,back);
        }
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let snapshot=shared.lock().unwrap().windows[&id].rgba().unwrap();
        assert_eq!(shared.lock().unwrap().windows[&id].rgb_capture(),expected);
        assert!(snapshot.iter().all(|c|c.to_le_bytes()[3]==255));
        assert_eq!(rt.graphics_call(27,&mut [id,0,0,1,1,0x1000000,0,0,0,0,0,0]),Err(22));
        rt.graphics_call(27,&mut [id,0,0,256,2,0x123456,0,0,0,0,0,0]).unwrap();
        assert_eq!(shared.lock().unwrap().windows[&id].rgba().unwrap(),snapshot);
        assert_ne!(shared.lock().unwrap().windows[&id].colour.as_ref().unwrap().back,*snapshot);
        assert_eq!(snapshot[0].to_le_bytes(),[0,255,0,255]);
    }
    #[test]
    fn true_colour_copy_resize_and_density_keep_precision_and_legacy_sources(){
        let mut rt=Runtime::new();let id=create_colour(&mut rt,3,2);let shared=rt.graphics.shared.clone();
        rt.memory.write_from(2*BANK,&[3,2,1,6,5,4,9,8,7,12,11,10,15,14,13,18,17,16]);
        rt.graphics_call(23,&mut [id,0,0,3,2,9,0x20000,0,0,0,0,0]).unwrap();
        rt.graphics_call(6,&mut [id,0,0,2,2,1,0,0,0,0,0,0]).unwrap();
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let snapshot=shared.lock().unwrap().windows[&id].rgba().unwrap();
        assert_eq!(*snapshot,[0x010203,0x010203,0x040506,0x0a0b0c,0x0a0b0c,0x0d0e0f].map(colour::rgba));
        rt.graphics_call(12,&mut [id,4,3,0,0,0,0,0,0,0,0,0]).unwrap();
        let rgb=shared.lock().unwrap().windows[&id].rgb_capture();
        assert_eq!(&rgb[..12],&[1,2,3,1,2,3,4,5,6,0,0,0]);
        assert_eq!(&rgb[24..],&[0;12]);
        assert_eq!(snapshot.len(),6);
        rt.memory.write(2*BANK,0xabcd);
        rt.graphics_call(3,&mut [id,0,0,1,1,1,0x20000,0,0,0,0,0]).unwrap();
        assert_eq!(shared.lock().unwrap().windows[&id].colour.as_ref().unwrap().back[0],colour::rgba565(0xabcd));
        rt.graphics_call(5,&mut [id,1,0,1,1,0x1357,0,0,0,0,0,0]).unwrap();
        assert_eq!(shared.lock().unwrap().windows[&id].colour.as_ref().unwrap().back[1],colour::rgba565(0x1357));
        rt.graphics_call(17,&mut [id,0,0,1,1,1,0x20000,0,0,0,0,0]).unwrap();
        assert!(shared.lock().unwrap().windows[&id].colour.as_ref().unwrap().back.iter()
            .all(|&p|p==colour::rgba(0)||p==colour::rgba565(0xabcd)));
        let mut density=[id,2,0,0,0,0,0,0,0,0,0,0];
        rt.graphics_call(24,&mut density).unwrap();assert_eq!(&density[1..4],&[2,8,6]);
        rt.graphics_call(27,&mut [id,7,5,1,1,0x222324,0,0,0,0,0,0]).unwrap();
        assert_eq!(rt.graphics_call(27,&mut [id,8,5,1,1,0x222324,0,0,0,0,0,0]),Err(22));
        rt.graphics_call(4,&mut [id,0,0,0,0,0,0,0,0,0,0,0]).unwrap();
        let s=shared.lock().unwrap();let w=&s.windows[&id];
        assert_eq!(w.pixel_scale(),2);assert_eq!(w.rgba().unwrap().len(),48);
        assert_eq!(&w.rgb_capture()[47*3..],&[0x22,0x23,0x24]);
    }
    #[test]
    fn full_colour_fill_quantizes_only_when_explicitly_using_rgb565(){
        let mut rt=Runtime::new();let legacy=create(&mut rt,1,1);let wide=create_colour(&mut rt,1,1);
        for id in [legacy,wide]{rt.graphics_call(27,&mut [id,0,0,1,1,0x222324,0,0,0,0,0,0]).unwrap();}
        let s=rt.graphics.shared.lock().unwrap();
        assert_eq!(s.windows[&legacy].back,[colour::rgb565(0x222324)]);
        assert_eq!(s.windows[&wide].colour.as_ref().unwrap().back,[colour::rgba(0x222324)]);
    }
    #[test]
    fn input_is_per_window_focus_releases_keys_and_close_keeps_session_alive() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        let other = create(&mut rt, 3, 2);
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            let w = s.windows.get_mut(&id).unwrap();
            w.transition(InputTransition::Focus { focused: true });
            w.transition(InputTransition::Key { key: 4, down: true });
            w.transition(InputTransition::Close);
            assert!(!w.input.held(4));
            assert!(!s.finished);
        }
        let mut a = [0; 12];
        a[0] = id;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(1));
        a[0] = other;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(0));
        a[0] = id;
        a[1] = 0;
        assert_eq!(rt.graphics_call(8, &mut a), Err(14));
        let mut closed = false;
        a[1] = 0x30000;
        while rt.graphics_call(10, &mut a) == Ok(1) {
            rt.graphics_call(8, &mut a).unwrap();
            closed |= rt.memory.read(3 * BANK + 5) == 8;
        }
        assert!(closed);
        rt.graphics_call(9, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 3) & 8, 0);
    }
    #[test]
    fn resource_and_pointer_limits_are_bounded() {
        let mut rt = Runtime::new();
        let mut a = [MAX_WIDTH+1, 1, 0x10000, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(rt.graphics_call(1, &mut a), Err(22));
        for _ in 0..MAX_WINDOWS {
            create(&mut rt, 1, 1);
        }
        a[0] = 1;
        assert_eq!(rt.graphics_call(1, &mut a), Err(12));
        assert_eq!(span(0x1fa00, 1), Err(14));
        assert_eq!(span(0xfffffa00, 1), Err(14));
    }
    #[test]
    fn overflow_preserves_close_and_an_application_can_decline_then_close_again() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            let w = s.windows.get_mut(&id).unwrap();
            for x in 0..300 {
                w.transition(InputTransition::Motion { x, y: 0 });
            }
            w.transition(InputTransition::Close);
        }
        let mut a = [0; 12];
        a[0] = id;
        a[1] = 0x30000;
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 9);
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 8);
        rt.graphics
            .shared
            .lock()
            .unwrap()
            .windows
            .get_mut(&id)
            .unwrap()
            .transition(InputTransition::Close);
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 8);
    }
    #[test]
    fn desktop_pointer_is_signed_window_relative_and_does_not_inject_input() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 20, 10);
        let other = create(&mut rt, 20, 10);
        let mut a = [0; 12];
        a[0] = id;
        assert_eq!(rt.graphics_call(14, &mut a), Err(11));
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            assert!(s.windows[&id].pointer_requested);
            assert!(!s.windows[&other].pointer_requested);
            s.windows.get_mut(&id).unwrap().desktop_pointer = Some((-600, 1200));
        }
        rt.graphics_call(14, &mut a).unwrap();
        assert_eq!((a[1] as i32, a[2] as i32), (-600, 1200));
        assert_eq!(rt.graphics_call(10, &mut a), Ok(0));
        let s = rt.graphics.shared.lock().unwrap();
        assert_eq!((s.windows[&id].input.x, s.windows[&id].input.y), (0, 0));
        assert_eq!(s.windows[&id].input.buttons, 0);
    }
    #[test]
    fn resize_requests_coalesce_per_window_and_owner_acknowledges_geometry() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        let other = create(&mut rt, 2, 2);
        let mut a = [id, 0, 0, 3, 2, 0x1234, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(5, &mut a).unwrap();
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            let w = s.windows.get_mut(&id).unwrap();
            w.request_resize(7, 5);
            w.request_resize(12, 9);
        }
        a = [0; 12];
        a[0] = other;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(0));
        a[0] = id;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(1));
        a[1] = 0x30000;
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 10);
        assert_eq!(rt.memory.read(3 * BANK + 9), 12);
        assert_eq!(rt.memory.read(3 * BANK + 11), 9);
        a = [id, 10, 8, 2, 0, 0, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(12, &mut a).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            let w = &s.windows[&id];
            assert_eq!((w.width, w.height, w.flags), (10, 8, 2));
            assert_eq!(&w.front[..4], &[0x1234, 0x1234, 0x1234, 0]);
            assert_eq!(s.windows[&other].width, 2);
            assert_eq!(w.configuration, 1);
        }
        // A resize refusal acknowledges the previous size, without new pixels.
        let sequence = rt.graphics.shared.lock().unwrap().windows[&id].sequence;
        rt.graphics_call(12, &mut a).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            assert_eq!(s.windows[&id].configuration, 2);
            assert_eq!(s.windows[&id].sequence, sequence);
        }
        let mut activate = [0; 12];
        activate[0] = other;
        rt.graphics_call(13, &mut activate).unwrap();
        assert_eq!(
            rt.graphics.shared.lock().unwrap().windows[&other].activation,
            1
        );
        a[3] = 4;
        assert_eq!(rt.graphics_call(12, &mut a), Err(22));
        a[3] = 1;
        rt.graphics_call(12, &mut a).unwrap();
        rt.graphics
            .shared
            .lock()
            .unwrap()
            .windows
            .get_mut(&id)
            .unwrap()
            .request_resize(20, 20);
        assert_eq!(rt.graphics_call(10, &mut a), Ok(0));
    }
}
