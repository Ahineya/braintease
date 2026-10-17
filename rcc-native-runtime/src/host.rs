//! Lazily start the window on the process main thread when native code enables
//! graphics. Console programs never initialize the window system or GPU.
use super::*;
use std::sync::mpsc::{self, Sender};

enum Event {
    DisplayRequested,
    HostedDisplayRequested,
    Finished,
}

pub(super) struct Host {
    headless: bool,
    pacing: Option<bool>,
    events: Option<Sender<Event>>,
    window_requested: bool,
    hosted_window_requested: bool,
}
impl Host {
    fn new(headless: bool, pacing: Option<bool>) -> Self {
        Self {
            headless,
            pacing,
            events: None,
            window_requested: false,
            hosted_window_requested: false,
        }
    }
    pub(super) fn from_env() -> Self {
        // Captures are headless by default, including existing test callers.
        let headless = std::env::var_os("RCC_NATIVE_HEADLESS").is_some()
            || std::env::var_os("RCC_NATIVE_FRAME").is_some();
        let pacing = if std::env::var_os("RCC_NATIVE_UNTHROTTLED").is_some() {
            Some(false)
        } else if std::env::var_os("RCC_NATIVE_PACED").is_some() {
            Some(true)
        } else {
            None
        };
        Self::new(headless, pacing)
    }
    pub(super) fn visual(&self) -> bool {
        self.window_requested
    }
    pub(super) fn paced(&self) -> bool {
        self.pacing.unwrap_or(self.window_requested)
    }
    pub(super) fn request_window(&mut self) {
        if self.headless || self.window_requested {
            return;
        }
        // Unit tests and embedding can own a runtime without a host connection.
        let Some(events) = &self.events else {
            return;
        };
        events
            .send(Event::DisplayRequested)
            .unwrap_or_else(|_| fatal("native display host disconnected"));
        self.window_requested = true;
    }
    pub(super) fn request_hosted_window(&mut self) {
        if !self.headless && !self.hosted_window_requested {
            if let Some(events) = &self.events {
                if events.send(Event::HostedDisplayRequested).is_ok() {
                    self.hosted_window_requested = true;
                }
            }
        }
    }
}

/// # Safety
/// `entry` must be the synchronous native entry emitted by rcc-native, with
/// signature `extern "C" fn(*mut Runtime) -> u64`, valid for this process.
#[no_mangle]
pub unsafe extern "C" fn rcc_native_start(entry: usize) -> i32 {
    let entry: unsafe extern "C" fn(*mut Runtime) -> u64 = unsafe { std::mem::transmute(entry) };
    let mut runtime = Runtime::new();
    let network_setting =
        std::env::var_os("RCC_NATIVE_NET").or_else(|| std::env::var_os("RIPPLE_NET"));
    runtime
        .devices
        .network
        .configure_host(network_setting.as_deref())
        .unwrap_or_else(|error| fatal(&error));
    // Interactive launchers attach before the guest starts reading stdin, so
    // vmnet's authorization prompt cannot race the console input worker.
    if std::env::var("RCC_NATIVE_NET_EAGER").as_deref() == Ok("1") {
        runtime
            .devices
            .network
            .prepare_host()
            .unwrap_or_else(|error| fatal(&error));
    }
    if let Some(path) = std::env::var_os("RCC_NATIVE_INPUT_SCRIPT") {
        let script = rvm::devices::InputScript::from_path(path).unwrap_or_else(|e| fatal(&e));
        runtime
            .devices
            .set_script(script)
            .unwrap_or_else(|e| fatal(&e));
    }
    runtime
        .devices
        .audio
        .set_output(
            !runtime.host.headless
                && std::env::var("RCC_NATIVE_AUDIO").as_deref() != Ok("off")
                && runtime.host.pacing != Some(false),
            std::env::var_os("RCC_NATIVE_AUDIO_WAV")
                .as_deref()
                .map(std::path::Path::new),
        )
        .unwrap_or_else(|e| fatal(&e));
    let state = runtime.display.get_state();
    let graphics = runtime.graphics.shared.clone();
    let finished_graphics = graphics.clone();
    let (events, receiver) = mpsc::channel();
    runtime.host.events = Some(events);
    let worker = std::thread::spawn(move || {
        let status = unsafe { entry(&mut runtime) };
        runtime.flush();
        runtime.devices.audio.finish().unwrap_or_else(|e| fatal(&e));
        if std::env::var_os("RCC_NATIVE_FRAME").is_some() {
            fatal("requested native frame was never presented");
        }
        runtime.display.shutdown();
        finished_graphics.lock().unwrap().finished = true;
        // A finished program wakes the main thread without creating an event
        // loop. On panic, dropping the runtime disconnects the same channel.
        let _ = runtime.host.events.as_ref().unwrap().send(Event::Finished);
        status
    });
    match receiver.recv() {
        Ok(Event::HostedDisplayRequested) => {
            if let Err(error) = hosted_graphics::run(graphics) {
                fatal(&format!("hosted display: {error}"));
            }
        }
        Ok(Event::DisplayRequested) => {
            // A short-lived program may already have finished after requesting UI.
            if !state.lock().unwrap().should_quit() {
                if let Err(error) = rvm::display_rgb565::run_rgb565_display(state) {
                    fatal(&format!("native display: {error}"));
                }
            }
        }
        _ => {}
    }
    worker
        .join()
        .unwrap_or_else(|_| fatal("native worker panicked")) as i32
}

#[cfg(test)]
mod tests {
    use super::*;
    use rvm::constants::*;
    fn connected(headless: bool, pacing: Option<bool>) -> (Runtime, mpsc::Receiver<Event>) {
        let mut runtime = Runtime::new();
        runtime.host = Host::new(headless, pacing);
        let (tx, rx) = mpsc::channel();
        runtime.host.events = Some(tx);
        (runtime, rx)
    }
    #[test]
    fn console_and_mode_selection_do_not_request_a_window() {
        let (mut rt, rx) = connected(false, None);
        rt.write(HDR_DISP_MODE, 1); // TTY
        rt.write(HDR_DISP_CTL, 1);
        assert!(rx.try_recv().is_err());
        assert!(!rt.host.visual());
        assert!(!rt.host.paced());
    }
    #[test]
    fn enabling_graphics_requests_one_window_and_defaults_to_pacing() {
        for mode in [DISP_RGB565, DISP_RGB565_EXT] {
            let (mut rt, rx) = connected(false, None);
            rt.write(HDR_DISP_MODE, mode);
            assert!(rx.try_recv().is_err());
            rt.write(HDR_DISP_CTL, 1);
            assert!(matches!(rx.try_recv(), Ok(Event::DisplayRequested)));
            assert!(rt.host.visual() && rt.host.paced());
            rt.write(HDR_DISP_CTL, 1);
            rt.write(HDR_DISP_MODE, 0);
            rt.write(HDR_DISP_MODE, mode);
            assert!(rx.try_recv().is_err());
        }
    }
    #[test]
    fn graphics_can_be_selected_after_the_display_is_enabled() {
        let (mut rt, rx) = connected(false, Some(false));
        rt.write(HDR_DISP_CTL, 1);
        assert!(rx.try_recv().is_err());
        rt.write(HDR_DISP_MODE, DISP_RGB565_EXT);
        assert!(matches!(rx.try_recv(), Ok(Event::DisplayRequested)));
        assert!(rt.host.visual());
        assert!(!rt.host.paced());
    }
    #[test]
    fn headless_display_still_publishes_pixels_without_a_window() {
        for pacing in [None, Some(true)] {
            let (mut rt, rx) = connected(true, pacing);
            rt.write(HDR_DISP_RESOLUTION, 0x0101);
            rt.write(HDR_DISP_MODE, DISP_RGB565);
            rt.write(HDR_DISP_CTL, 1);
            rt.write(33, 0xf800);
            rt.write(HDR_DISP_FLUSH, 1);
            assert_eq!(rt.display.snapshot().unwrap().pixels, [0xf800]);
            assert!(rx.try_recv().is_err());
            assert!(!rt.host.visual());
            assert_eq!(rt.host.paced(), pacing.unwrap_or(false));
        }
    }
}
