//! Input is read by a host worker into a bounded queue. Guest execution only
//! polls it, so a terminal read cannot block other guest continuations/timers.
use super::*;
use std::io::Read;
use std::sync::mpsc::{self, Receiver, TryRecvError};

#[derive(Default)]
pub(super) struct Console {
    input: Option<Receiver<Option<u8>>>,
    eof: bool,
}
impl Console {
    fn poll(&mut self) -> u64 {
        if self.eof {
            return u64::from(u16::MAX - 1);
        }
        let input = self.input.get_or_insert_with(|| {
            let (send, receive) = mpsc::sync_channel(4096);
            std::thread::spawn(move || {
                for byte in std::io::stdin().lock().bytes() {
                    let Ok(byte) = byte else { break };
                    if send.send(Some(byte)).is_err() {
                        return;
                    }
                }
                let _ = send.send(None);
            });
            receive
        });
        match input.try_recv() {
            Ok(Some(byte)) => u64::from(byte),
            Err(TryRecvError::Empty) => u64::from(u16::MAX),
            Ok(None) | Err(TryRecvError::Disconnected) => {
                self.eof = true;
                u64::from(u16::MAX - 1)
            }
        }
    }
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_input(ctx: *mut Runtime) -> u64 {
    let rt = unsafe { &mut *ctx };
    if !rt.output.is_empty() {
        rt.flush();
    }
    rt.console.poll()
}
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_idle(_ctx: *mut Runtime, milliseconds: u64) -> u64 {
    std::thread::sleep(std::time::Duration::from_millis(milliseconds.min(10)));
    0
}
