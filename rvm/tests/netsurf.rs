//! Opt-in NetSurf/Nano-X integration through real guest code and HID input.
use rvm::{
    display_rgb565::RGB565Display,
    input_events::InputTransition,
    vm::{VMState, VM},
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};
struct Guest {
    vm: VM,
    log: String,
    output: PathBuf,
}
impl Guest {
    fn drive(&mut self) {
        self.vm.run_batch(65536).expect("guest execution");
        let output = self.vm.get_output();
        if !output.is_empty() {
            self.log.push_str(&String::from_utf8_lossy(&output));
            std::fs::write(self.output.join("rvm.log"), &self.log).unwrap();
        }
        assert!(
            !matches!(self.vm.state, VMState::Error(_)),
            "{:?}\n{}",
            self.vm.state,
            self.log
        );
        for error in [
            "panic:",
            "GsError",
            "blit snapshot allocation failed",
            "NXR_BUFFER_FAILED",
        ] {
            assert!(!self.log.contains(error), "{}", self.log);
        }
    }
    fn until(&mut self, marker: &str, start: usize) {
        // Public navigation includes loading both RXE packages before the
        // transport's own 240-second deadline starts.
        let seconds = if std::env::var_os("NETSURF_TEST_WEB_URL").is_some() {
            420
        } else {
            150
        };
        let deadline = Instant::now() + Duration::from_secs(seconds);
        while !self.log[start..].contains(marker) {
            assert!(
                Instant::now() < deadline,
                "waiting for {marker}\n{}",
                self.log
            );
            assert!(!matches!(self.vm.state, VMState::Halted), "{}", self.log);
            self.drive();
        }
    }
    fn settle(&mut self, ms: u64) {
        let deadline = Instant::now() + Duration::from_millis(ms);
        while Instant::now() < deadline {
            self.drive();
        }
    }
    fn command(&mut self, text: &str, marker: &str) {
        let start = self.log.len();
        self.vm.push_input_string(text);
        self.until(marker, start);
    }
    fn pixels(&self) -> Vec<u16> {
        self.vm
            .rgb565_display
            .as_ref()
            .unwrap()
            .get_state()
            .lock()
            .unwrap()
            .front_buffer
            .as_ref()
            .clone()
    }
    fn wait_pixels(&mut self, predicate: impl Fn(&[u16]) -> bool, label: &str) {
        let deadline = Instant::now() + Duration::from_secs(30);
        while !predicate(&self.pixels()) {
            if Instant::now() >= deadline {
                self.snapshot("rvm-failure.rgb565");
                panic!("waiting for pixels: {label}\n{}", self.log);
            }
            self.drive();
        }
    }
    fn snapshot(&self, name: &str) {
        let bytes: Vec<u8> = self.pixels().iter().flat_map(|p| p.to_le_bytes()).collect();
        std::fs::write(self.output.join(name), bytes).unwrap();
    }
    fn key(&mut self, key: u16) {
        self.vm.devices.accept_events([
            InputTransition::Key { key, down: true },
            InputTransition::Key { key, down: false },
        ]);
        self.settle(180);
    }
    fn chord(&mut self, modifier: u16, key: u16) {
        self.vm.devices.accept_events([InputTransition::Key {
            key: modifier,
            down: true,
        }]);
        self.settle(50);
        self.key(key);
        self.vm.devices.accept_events([InputTransition::Key {
            key: modifier,
            down: false,
        }]);
        self.settle(50);
    }
    fn click(&mut self, x: i32, y: i32) {
        self.vm.devices.accept_events([
            InputTransition::Motion { x, y },
            InputTransition::Button {
                button: 0,
                down: true,
                x,
                y,
            },
        ]);
        self.settle(100);
        self.vm.devices.accept_events([InputTransition::Button {
            button: 0,
            down: false,
            x,
            y,
        }]);
        self.settle(200);
    }
    fn text(&mut self, text: &str) {
        for ch in text.chars() {
            let key = match ch {
                'a'..='z' => 4 + ch as u16 - 'a' as u16,
                '/' => 56,
                '.' => 55,
                _ => panic!("unsupported test character"),
            };
            self.key(key);
        }
    }
}
fn contains_word(pixels: &[u16], bits: &[u16], word: &str) -> bool {
    contains_word_with_tail(pixels, bits, word, &[])
}
fn contains_word_with_tail(pixels: &[u16], bits: &[u16], word: &str, tail: &[u16]) -> bool {
    (255..280).any(|y| {
        (136..260).any(|x| {
            word.bytes().enumerate().all(|(i, ch)| {
                (0..16).all(|dy| {
                    (0..8).all(|dx| {
                        let ink = bits[ch as usize * 16 + dy] & (0x8000 >> dx) != 0;
                        (pixels[(y + dy) * 640 + x + i * 8 + dx] == 0) == ink
                    })
                })
            }) && tail.iter().enumerate().all(|(dy, row)| {
                (0..8).all(|dx| {
                    (pixels[(y + dy) * 640 + x + word.len() * 8 + dx] == 0)
                        == (row & (0x8000 >> dx) != 0)
                })
            })
        })
    })
}
fn unicode_font_rows(scalar: u32) -> Vec<u16> {
    let source = std::fs::read_to_string(
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../libraries/unicode/font.inc"),
    ).unwrap();
    let (codes, rows) = source.split_once("static const uint16_t ru_font_rows[][16] = {").unwrap();
    fn hex_values(source: &str) -> Vec<u32> {
        source.split(|c: char| !(c.is_ascii_hexdigit() || c == 'x'))
            .filter_map(|s| s.strip_prefix("0x").and_then(|s| u32::from_str_radix(s, 16).ok()))
            .collect()
    }
    let index = hex_values(codes).iter().position(|&cp| cp == scalar).unwrap();
    hex_values(rows)[index*16..index*16+16].iter().map(|&v| v as u16).collect()
}
fn rgb(r: u16, g: u16, b: u16) -> u16 {
    (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
}
#[test]
#[ignore = "run through ports/netsurf/tests/gui.py with disposable disk"]
fn browser_pixels_and_input() {
    let build = PathBuf::from(std::env::var_os("NETSURF_RVM_BUILD").unwrap());
    let output = PathBuf::from(std::env::var_os("NETSURF_TEST_OUTPUT").unwrap());
    let mut vm = VM::with_options(
        64000,
        16384000,
        Some(PathBuf::from(
            std::env::var_os("NETSURF_TEST_DISK").unwrap(),
        )),
    );
    vm.host_input_enabled = false;
    vm.jit_enabled = true;
    vm.devices
        .network
        .configure_host(Some(
            std::env::var_os("NETSURF_TEST_NET")
                .as_deref()
                .unwrap_or(std::ffi::OsStr::new("off")),
        ))
        .unwrap();
    vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
        .unwrap();
    vm.rgb565_display = Some(RGB565Display::new());
    let mut g = Guest {
        vm,
        log: String::new(),
        output,
    };
    g.until("fuzix# ", 0);
    g.command("nano-X -p &\ns=$!\n", "Nano-X ready");
    if std::env::var_os("NETSURF_TEST_IMAGES").is_some() {
        g.command("netsurf --trace /images/index.html &\np=$!\n", "NETSURF_GUI_LOADED Image checks");
        assert!(!g.log.contains("NETSURF_GUI_FILE /images/alpha.png"), "default must not fetch images");
        g.click(570,50); // Options toolbar dropdown (window at 24,36).
        g.wait_pixels(|p| p[76*640+326]==rgb(65,101,136),"selected Images menu row");
        g.snapshot("rvm-image-options.rgb565");
        g.key(41); // Escape dismisses the dropdown without changing policy.
        g.chord(226,18); // Alt+O: reopen with the keyboard.
        let start=g.log.len();
        g.key(40); // Enable images using the selected menu item.
        g.until("NETSURF_GUI_IMAGES 1",start);
        g.until("NETSURF_GUI_LOADED Image checks",start);
        g.wait_pixels(|p| p[132*640+56] == rgb(255,0,0), "decoded image pixels");
        g.settle(300);
        g.snapshot("rvm-images.rgb565");
        g.click(600,110);
        let start=g.log.len();
        g.key(78);
        g.until("NETSURF_GUI_SCROLL",start);
        g.wait_pixels(|p| p[248*640+56] == rgb(255,0,0), "HTTPS image after scrolling");
        g.settle(300);
        g.snapshot("rvm-images-scroll.rgb565");
        let start=g.log.len();
        g.chord(224,15);
        g.text("/images/alpha.png");
        g.key(40);
        g.until("NETSURF_GUI_DOCUMENT file:///images/alpha.png",start);
        g.settle(300);
        g.snapshot("rvm-image-document.rgb565");
        for _ in 0..4 {
            let start=g.log.len();g.key(62); // F5: replace and release decoded content.
            g.until("NETSURF_GUI_DOCUMENT file:///images/alpha.png",start);
        }
        g.chord(224,20);
        g.command("wait $p\necho BROWSER_STATUS:$?\nkill $s\nwait $s\nshutdown\n", "disk synced; power off.");
        while !matches!(g.vm.state, VMState::Halted) {g.drive();}
        assert!(g.log.contains("BROWSER_STATUS:0"), "{}",g.log);
        println!("NetSurf RVM: image pixels, scrolling, standalone image, reload and clean exit passed");
        return;
    }
    if let Ok(url) = std::env::var("NETSURF_TEST_WEB_URL") {
        assert!(url.starts_with("https://") || url.starts_with("http://"));
        assert!(url
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b":/._-?=&%".contains(&c)));
        g.command(
            &format!("netsurf --trace '{url}' &\np=$!\n"),
            "NETSURF_GUI_DOCUMENT",
        );
        g.settle(1000);
        let title = std::env::var("NETSURF_TEST_WEB_TITLE").unwrap_or_else(|_| "Wiby".into());
        assert!(
            g.log.contains(&format!("NETSURF_GUI_LOADED {title}")),
            "{}",
            g.log
        );
        let pixels = g.pixels();
        let pixels = &pixels;
        let page: Vec<u16> = (102..400)
            .flat_map(|y| (26..600).map(move |x| pixels[y * 640 + x]))
            .collect();
        assert!(
            page.iter().filter(|&&p| p == 0).count() > 100
                && page.iter().filter(|&&p| p & 0xf79e == 0xf79e).count() > 1000,
            "public document must draw text"
        );
        g.snapshot("rvm-web.rgb565");
        // Submit Wiby's actual GET form through real keyboard/text events.
        if std::env::var_os("NETSURF_TEST_WEB_SEARCH").is_some() {
            let start = g.log.len();
            g.click(220, 280); // Wiby's live HTML search field.
            g.vm.devices.accept_events(
                "example.com"
                    .chars()
                    .map(|c| InputTransition::Text { scalar: c as u32 }),
            );
            g.key(40);
            g.until("NETSURF_GUI_DOCUMENT https://wiby.me/?q=example.com", start);
            g.settle(700);
            g.snapshot("rvm-web-search.rgb565");
        }
        g.chord(224, 20);
        g.command(
            "wait $p\necho BROWSER_STATUS:$?\nkill $s\nwait $s\nshutdown\n",
            "disk synced; power off.",
        );
        while !matches!(g.vm.state, VMState::Halted) {
            g.drive();
        }
        assert!(g.log.contains("BROWSER_STATUS:0"), "{}", g.log);
        println!("NetSurf RVM: public guest HTTP/TLS, GUI rendering, typed navigation and clean exit passed");
        return;
    }
    g.command(
        "netsurf --trace &\np=$!\n",
        "NETSURF_GUI_LOADED FuseDE Browser",
    );
    g.wait_pixels(
        |p| p[102 * 640 + 26] == rgb(238, 242, 246) && p.iter().any(|&v| v == rgb(24, 93, 153)),
        "initial CSS and link",
    );
    g.settle(300);
    g.snapshot("rvm-wide.rgb565");
    let original = g.pixels();
    assert_eq!(original[38 * 640 + 26], rgb(44, 55, 65), "toolbar");
    assert_eq!(
        original[102 * 640 + 26],
        rgb(238, 242, 246),
        "CSS page background"
    );
    g.click(600, 110); // Focus without activating a link or editable field.
    g.settle(500);
    let focused = g.pixels();
    let start = g.log.len();
    g.key(78); // PageDown, real HID -> Nano-X -> frontend.
    g.until("NETSURF_GUI_SCROLL", start);
    g.settle(200);
    g.wait_pixels(|p| p != focused, "scroll changes pixels");
    g.snapshot("rvm-scrolled.rgb565");
    g.key(74); // Home.
    g.wait_pixels(
        |p| p.iter().any(|&v| v == rgb(24, 93, 153)),
        "home restores link",
    );
    g.settle(200);
    let pixels = g.pixels();
    let index = (100..430)
        .flat_map(|y| (24..600).map(move |x| y * 640 + x))
        .find(|&p| pixels[p] == rgb(24, 93, 153))
        .expect("rendered HTML link");
    let start = g.log.len();
    g.click((index % 640) as i32, (index / 640) as i32);
    g.until("NETSURF_GUI_LOADED Local Navigation", start);
    g.wait_pixels(
        |p| p[102 * 640 + 26] == rgb(244, 237, 218),
        "next page background",
    );
    g.settle(300);
    g.snapshot("rvm-navigation.rgb565");
    let start = g.log.len();
    g.chord(226, 80); // Alt+Left: browser history.
    g.until("NETSURF_GUI_LOADED FuseDE Browser", start);
    g.wait_pixels(|p| p[102 * 640 + 26] == rgb(238, 242, 246), "history page");
    g.settle(300);
    g.click(165, 272); // The fixture's editable HTML input.
    g.vm.devices
        .accept_events([InputTransition::Motion { x: 600, y: 110 }]);
    g.settle(100);
    g.chord(224, 4); // Ctrl+A selects the input's initial value.
    g.text("world");
    let font = std::fs::read(
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../ports/nanox/upstream/src/fonts/compiled/rom8x16.c"),
    )
    .unwrap();
    let bits: Vec<u16> = String::from_utf8_lossy(&font)
        .lines()
        .filter_map(|line| {
            line.strip_prefix("0x")
                .and_then(|s| s.strip_suffix(','))
                .and_then(|s| u16::from_str_radix(s, 16).ok())
        })
        .collect();
    assert_eq!(bits.len(), 256 * 16);
    g.wait_pixels(|p| contains_word(p, &bits, "world"), "typed HTML field");
    g.snapshot("rvm-form.rgb565");
    let replacement_rows = unicode_font_rows(0xfffd);
    let start = g.log.len();
    g.vm.devices
        .accept_events([InputTransition::Text { scalar: 0x1f642 }]);
    g.until("NETSURF_GUI_KEY 128578 0", start);
    g.wait_pixels(
        |p| contains_word_with_tail(p, &bits, "world", &replacement_rows),
        "non-BMP input uses the shared Unicode fallback bitmap",
    );
    let start = g.log.len();
    g.chord(224, 15); // Ctrl+L: address field.
    g.text("/usr/share/netsurf/next.html");
    g.key(40);
    g.until("NETSURF_GUI_LOADED Local Navigation", start);
    g.wait_pixels(
        |p| p[102 * 640 + 26] == rgb(244, 237, 218),
        "typed address page",
    );
    g.settle(700);
    let frames = g.vm.presented_frames;
    g.settle(600);
    assert_eq!(
        frames, g.vm.presented_frames,
        "idle browser must not repaint"
    );
    g.chord(224, 20); // Ctrl+Q.
    g.command(
        "wait $p\necho BROWSER_STATUS:$?\nkill $s\nwait $s\nshutdown\n",
        "disk synced; power off.",
    );
    while !matches!(g.vm.state, VMState::Halted) {
        g.drive();
    }
    assert!(g.log.contains("BROWSER_STATUS:0"), "{}", g.log);
    println!("NetSurf RVM: CSS pixels, clipping, HID scroll, real link, history, typed address, idle and close passed");
}
