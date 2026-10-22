//! Explicit C16 marshalling for the portable desktop session model.
//! Caller identities and control authority are checked by the guest kernel.
use super::*;

#[path = "../../libraries/graphics/session.rs"]
pub(super) mod model;

const RECORD: usize = 338;
const CAPABILITIES: u32 = 1 | 4 | 8;

pub(super) fn error(error: model::Error) -> u16 {
    match error {
        model::Error::Invalid => 22,
        model::Error::NotFound => 2,
        model::Error::Again => 11,
        model::Error::Exhausted => 60,
        model::Error::Capacity => 12,
    }
}

fn text(rt: &Runtime, start: usize, length: usize) -> Result<String> {
    let mut bytes = Vec::new();
    bytes.try_reserve_exact(length).map_err(|_| 12u16)?;
    for at in start..start + length {
        let cell = rt.memory.read(at);
        if cell == 0 {
            return String::from_utf8(bytes).map_err(|_| 22u16);
        }
        bytes.push(u8::try_from(cell).map_err(|_| 22u16)?);
    }
    Err(22)
}

fn word(rt: &Runtime, at: usize) -> u32 {
    u32::from(rt.memory.read(at)) | (u32::from(rt.memory.read(at + 1)) << 16)
}

fn put_word(record: &mut [u16; RECORD], at: usize, value: u32) {
    record[at] = value as u16;
    record[at + 1] = (value >> 16) as u16;
}

fn put_text(record: &mut [u16; RECORD], at: usize, length: usize, value: &str) {
    let mut end = value.len().min(length - 1);
    while !value.is_char_boundary(end) {
        end -= 1;
    }
    for (index, byte) in value.bytes().take(end).enumerate() {
        record[at + index] = u16::from(byte);
    }
}

fn labels(record: &mut [u16; RECORD], registry: &model::Registry, instance: u32) {
    if let Some(instance) = registry.instance(instance) {
        put_text(record, 18, 64, &instance.app_id);
        put_text(record, 82, 64, &instance.name);
        put_text(record, 146, 128, &instance.icon);
    }
}

fn process_record(registry: &model::Registry, process: &model::Process) -> [u16; RECORD] {
    let mut record = [0; RECORD];
    put_word(&mut record, 2, process.id);
    put_word(&mut record, 4, process.instance);
    record[8] = process.pid;
    labels(&mut record, registry, process.instance);
    record
}

fn instance_record(registry: &model::Registry, instance: &model::Instance) -> [u16; RECORD] {
    let mut record = [0; RECORD];
    put_word(&mut record, 2, instance.owner);
    put_word(&mut record, 4, instance.id);
    if let Some(owner) = registry.process(instance.owner) {
        record[8] = owner.pid;
    }
    labels(&mut record, registry, instance.id);
    record
}

fn window_record(registry: &model::Registry, window: &model::Window) -> [u16; RECORD] {
    let mut record = [0; RECORD];
    put_word(&mut record, 0, window.id);
    put_word(&mut record, 2, window.process);
    put_word(&mut record, 4, window.instance);
    put_word(&mut record, 6, window.workspace);
    if let Some(process) = registry.process(window.process) {
        record[8] = process.pid;
    }
    record[9] = window.flags;
    record[10] = window.role;
    put_word(&mut record, 12, window.x as u32);
    put_word(&mut record, 14, window.y as u32);
    record[16] = window.width;
    record[17] = window.height;
    labels(&mut record, registry, window.instance);
    put_text(&mut record, 274, 64, &window.title);
    record
}

/// Synchronize policy once, without presenting or observing frame sequences.
fn synchronize(s: &mut Shared, nested: bool) -> Result<()> {
    if nested {
        let mut desktop = s.desktop.take().unwrap_or_default();
        desktop.sync(s);
        s.desktop = Some(desktop);
    }
    synchronize_windows(s)
}

pub(super) fn synchronize_windows(s: &mut Shared) -> Result<()> {
    let mut windows = Vec::new();
    windows
        .try_reserve_exact(s.windows.len())
        .map_err(|_| 12u16)?;
    for (&id, surface) in &s.windows {
        // Historical callers have no trusted execution identity. Do not infer
        // one from a PID or accidentally revive an execution removed on exec.
        if surface.process == 0 || s.registry.process(surface.process).is_none() {
            continue;
        }
        let policy = s.desktop.as_ref().and_then(|desktop| desktop.wm.get(id));
        let hidden = surface.flags & 2 != 0;
        let minimized = policy.is_some_and(|window| window.minimized);
        let mut flags = 0;
        if !hidden && !minimized {
            flags |= 1;
        }
        if minimized {
            flags |= 2;
        }
        if policy.is_some_and(|window| window.maximized) {
            flags |= 4;
        }
        if !hidden && !minimized && surface.input.focused {
            flags |= 8;
        }
        if surface.flags & 1 == 0 {
            flags |= 16;
        }
        if hidden {
            flags |= 32;
        }
        if surface.closed {
            flags |= 64;
        }
        let (x, y, width, height) = policy.map_or(
            (0, 0, surface.width as u16, surface.height as u16),
            |window| {
                (
                    window.content.x,
                    window.content.y,
                    window.content.w as u16,
                    window.content.h as u16,
                )
            },
        );
        windows.push(model::Window {
            id,
            process: surface.process,
            instance: 0,
            title: surface.title.clone(),
            x,
            y,
            width,
            height,
            flags,
            role: u16::from(s.panel.as_ref().is_some_and(|panel| panel.window == id)),
            workspace: 1,
        });
    }
    s.registry.synchronize_windows(windows).map_err(error)?;
    Ok(())
}

pub(super) fn call(rt: &mut Runtime, args: &mut [u32; 12]) -> Result<u32> {
    dispatch(rt, args, desktop::enabled())
}

fn dispatch(rt: &mut Runtime, args: &mut [u32; 12], nested: bool) -> Result<u32> {
    let operation = args[0];
    let record =
        if matches!(operation, 1 | 5 | 6 | 7 | 8) || (operation == 9 && matches!(args[5], 5 | 6)) {
            Some(span(args[6], RECORD)?)
        } else {
            None
        };
    // Validate all input before a registration can change shared identities.
    let registration = if operation == 1 {
        let start = record.ok_or(14u16)?;
        Some((
            text(rt, start + 18, 64)?,
            text(rt, start + 82, 64)?,
            text(rt, start + 146, 128)?,
        ))
    } else {
        None
    };
    let shared = rt.graphics.shared.clone();
    let mut s = shared.lock().unwrap();
    match operation {
        64 => s
            .registry
            .touch_process(args[1], u16::try_from(args[2]).map_err(|_| 22u16)?)
            .map_err(error)?,
        65 => {
            panel::remove_process(&mut s, args[1]);
            s.registry.remove_process(args[1]).map_err(error)?;
        }
        _ => {}
    }
    synchronize(&mut s, nested)?;
    let revision = s.registry.revision();
    let expected = if args[4] == 0 { revision } else { args[4] };
    let mut output = None;
    let mut present = false;
    args[7] = 0;
    match operation {
        0 | 64 | 65 | 66 => {
            args[3] = s.registry.windows().count() as u32;
            args[5] = s.registry.instances().count() as u32;
            args[7] = CAPABILITIES | if nested { 2 } else { 0 };
        }
        1 => {
            let (app_id, name, icon) = registration.unwrap();
            let instance = s
                .registry
                .register(
                    args[1],
                    u16::try_from(args[2]).map_err(|_| 22u16)?,
                    &app_id,
                    &name,
                    &icon,
                )
                .map_err(error)?;
            args[3] = args[1];
            args[5] = instance;
            output = Some(process_record(
                &s.registry,
                s.registry.process(args[1]).ok_or(2u16)?,
            ));
        }
        2 => {
            args[5] = s
                .registry
                .attach_child(args[1], args[8], u16::try_from(args[9]).map_err(|_| 22u16)?)
                .map_err(error)?;
            args[3] = args[8];
        }
        5 => {
            let window = s.registry.next_window(expected, args[3]).map_err(error)?;
            args[3] = window.map_or(0, |window| window.id);
            output = Some(window.map_or([0; RECORD], |window| window_record(&s.registry, window)));
        }
        6 => {
            s.registry.check_revision(expected).map_err(error)?;
            let window = s.registry.window(args[3]).ok_or(2u16)?;
            output = Some(window_record(&s.registry, window));
        }
        7 => {
            let instance = s.registry.next_instance(expected, args[3]).map_err(error)?;
            args[3] = instance.map_or(0, |instance| instance.id);
            output = Some(instance.map_or([0; RECORD], |instance| {
                instance_record(&s.registry, instance)
            }));
        }
        8 => {
            let process = s.registry.next_process(expected, args[3]).map_err(error)?;
            args[3] = process.map_or(0, |process| process.id);
            output =
                Some(process.map_or([0; RECORD], |process| process_record(&s.registry, process)));
        }
        9 => {
            if !nested {
                return Err(44);
            }
            if s.registry.window(args[3]).is_none() {
                return Err(2);
            }
            if s.panel.as_ref().is_some_and(|panel| panel.window == args[3]) {
                return Err(44);
            }
            use desktop::policy::WindowCommand;
            let command = match args[5] {
                1 => WindowCommand::Activate,
                2 => WindowCommand::Minimize,
                3 => WindowCommand::Maximize,
                4 => WindowCommand::Restore,
                5 => {
                    let start = record.ok_or(14u16)?;
                    WindowCommand::Move {
                        x: word(rt, start + 12) as i32,
                        y: word(rt, start + 14) as i32,
                    }
                }
                6 => {
                    let start = record.ok_or(14u16)?;
                    WindowCommand::Resize {
                        width: u32::from(rt.memory.read(start + 16)),
                        height: u32::from(rt.memory.read(start + 17)),
                    }
                }
                7 => WindowCommand::Close,
                _ => return Err(22),
            };
            let mut desktop = s.desktop.take().unwrap_or_default();
            let result = desktop.command(&mut s, args[3], command);
            s.desktop = Some(desktop);
            result?;
            synchronize_windows(&mut s)?;
            present = true;
        }
        10 => {
            if args[4] > revision {
                return Err(22);
            }
            args[7] = u32::from(args[4] != revision);
        }
        _ => return Err(44),
    }
    // Registration and explicit helper attachment can change the dock without
    // a new client frame. Reconcile after the mutation, before returning the
    // accepted revision. Windowless services must not open a desktop window.
    if nested && matches!(operation, 1 | 2 | 65) && !s.windows.is_empty() {
        let mut desktop = s.desktop.take().unwrap_or_default();
        desktop.sync(&mut s);
        s.desktop = Some(desktop);
        synchronize_windows(&mut s)?;
        present = true;
    }
    args[4] = s.registry.revision();
    if let Some(output) = output {
        // The entire destination span was checked before any registration.
        rt.memory.write_from(record.ok_or(14u16)?, &output);
    }
    if present
        && std::env::var_os("RCC_NATIVE_HEADLESS").is_some()
        && std::env::var_os("FUSE_X_FRAMES").is_some()
    {
        desktop::headless_capture_synced(&mut s)?;
    }
    drop(s);
    if present {
        rt.host.request_hosted_window();
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    const PTR: u32 = 0x20000;

    fn request(operation: u32) -> [u32; 12] {
        [operation, 100, 42, 0, 0, 0, PTR, 0, 0, 0, 0, 0]
    }

    fn registration(rt: &mut Runtime, app: &str, name: &str, icon: &str) {
        let mut record = [0; RECORD];
        put_text(&mut record, 18, 64, app);
        put_text(&mut record, 82, 64, name);
        put_text(&mut record, 146, 128, icon);
        rt.memory.write_from(2 * BANK, &record);
    }

    fn create(rt: &mut Runtime, process: u32, pid: u16, flags: u32) -> u32 {
        rt.memory.write(BANK, 0);
        let mut args = [
            100,
            80,
            0x10000,
            flags,
            2,
            process,
            u32::from(pid),
            0,
            0,
            0,
            0,
            0,
        ];
        rt.graphics_call(1, &mut args).unwrap();
        args[0]
    }

    #[test]
    fn registration_strict_utf8_and_destination_validation_are_atomic() {
        let mut rt = Runtime::new();
        registration(&mut rt, "rift", "Рифт", "rift");
        let mut args = request(1);
        args[6] = (CELLS - 100) as u32;
        assert_eq!(dispatch(&mut rt, &mut args, false), Err(14));
        assert_eq!(rt.graphics.shared.lock().unwrap().registry.revision(), 1);
        args[6] = PTR;
        rt.memory.write(2 * BANK + 82, 0xff);
        assert_eq!(dispatch(&mut rt, &mut args, false), Err(22));
        assert_eq!(rt.graphics.shared.lock().unwrap().registry.revision(), 1);
        registration(&mut rt, "rift", "Рифт", "rift");
        rt.memory.write(2 * BANK + 146, 0x100);
        assert_eq!(dispatch(&mut rt, &mut args, false), Err(22));
        registration(&mut rt, "rift", "Рифт", "rift");
        assert_eq!(dispatch(&mut rt, &mut args, false), Ok(0));
        assert_eq!(args[3], 100);
        assert_eq!(args[5], 1);
        assert_eq!(word(&rt, 2 * BANK + 2), 100);
        assert_eq!(word(&rt, 2 * BANK + 4), 1);
        assert_eq!(rt.memory.read(2 * BANK + 8), 42);
        assert_eq!(text(&rt, 2 * BANK + 82, 64).unwrap(), "Рифт");
        assert_eq!(rt.memory.read(2 * BANK + 11), 0);
        let revision = args[4];
        args = request(1);
        assert_eq!(dispatch(&mut rt, &mut args, false), Ok(0));
        assert_eq!(args[4], revision);
    }

    #[test]
    fn window_snapshot_ignores_frame_updates_and_rejects_stale_cursors() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 100, 42, 0);
        let legacy = create(&mut rt, 0, 0, 0);
        let mut info = request(0);
        dispatch(&mut rt, &mut info, true).unwrap();
        assert_eq!(info[3], 1);
        assert_eq!(info[7], CAPABILITIES | 2);
        let revision = info[4];
        let mut next = request(5);
        dispatch(&mut rt, &mut next, true).unwrap();
        assert_eq!(next[3], id);
        assert_eq!(rt.memory.read(2 * BANK + 8), 42);
        assert_eq!(word(&rt, 2 * BANK + 4), 0);
        rt.graphics_call(27, &mut [id, 0, 0, 100, 80, 0x123456, 0, 0, 0, 0, 0, 0])
            .unwrap();
        rt.graphics_call(4, &mut [id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
            .unwrap();
        info = request(66);
        dispatch(&mut rt, &mut info, true).unwrap();
        assert_eq!(info[4], revision);
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            s.windows.get_mut(&id).unwrap().title = "Changed".to_owned();
        }
        next = request(5);
        next[4] = revision;
        assert_eq!(dispatch(&mut rt, &mut next, true), Err(11));
        next[4] = 0;
        dispatch(&mut rt, &mut next, true).unwrap();
        assert_eq!(text(&rt, 2 * BANK + 274, 64).unwrap(), "Changed");
        assert!(next[4] > revision);
        next[3] = legacy;
        next[0] = 6;
        assert_eq!(dispatch(&mut rt, &mut next, true), Err(2));
        let mut watch = request(10);
        watch[4] = u32::MAX;
        assert_eq!(dispatch(&mut rt, &mut watch, true), Err(22));
    }

    #[test]
    fn controls_use_shared_policy_and_deliver_close_without_destroying() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 100, 42, 0);
        let mut control = request(9);
        control[3] = id;
        control[5] = 2;
        assert_eq!(dispatch(&mut rt, &mut control, false), Err(44));
        dispatch(&mut rt, &mut control, true).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            assert!(s.desktop.as_ref().unwrap().wm.get(id).unwrap().minimized);
            assert_eq!(s.registry.window(id).unwrap().flags & 3, 2);
        }
        control[5] = 1;
        dispatch(&mut rt, &mut control, true).unwrap();
        let first = control[4];
        dispatch(&mut rt, &mut control, true).unwrap();
        assert_eq!(control[4], first);
        let mut geometry = [0; RECORD];
        put_word(&mut geometry, 12, 90);
        put_word(&mut geometry, 14, 100);
        rt.memory.write_from(2 * BANK, &geometry);
        control[5] = 5;
        dispatch(&mut rt, &mut control, true).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            assert_eq!(
                (
                    s.registry.window(id).unwrap().x,
                    s.registry.window(id).unwrap().y
                ),
                (90, 100)
            );
            assert_eq!(
                s.desktop.as_ref().unwrap().wm.get(id).unwrap().content.x,
                90
            );
        }
        control[5] = 7;
        dispatch(&mut rt, &mut control, true).unwrap();
        let s = rt.graphics.shared.lock().unwrap();
        assert!(s.windows.contains_key(&id));
        assert!(s.windows[&id].closed);
        assert_ne!(s.registry.window(id).unwrap().flags & 64, 0);
    }

    #[test]
    fn helper_association_and_exec_cleanup_do_not_reuse_identity() {
        let mut rt = Runtime::new();
        registration(&mut rt, "game", "Game", "game");
        let mut args = request(1);
        dispatch(&mut rt, &mut args, false).unwrap();
        let instance = args[5];
        args = request(2);
        args[8] = 101;
        args[9] = 43;
        dispatch(&mut rt, &mut args, false).unwrap();
        assert_eq!((args[3], args[5]), (101, instance));
        registration(&mut rt, "game", "Hijack", "game");
        args = request(1);
        args[1] = 101;
        args[2] = 43;
        assert_eq!(dispatch(&mut rt, &mut args, false), Err(22));
        args = request(65);
        dispatch(&mut rt, &mut args, false).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            assert_eq!(s.registry.instance(instance).unwrap().owner, 0);
        }
        args[1] = 101;
        dispatch(&mut rt, &mut args, false).unwrap();
        registration(&mut rt, "game", "Game", "game");
        args = request(1);
        args[1] = 102;
        dispatch(&mut rt, &mut args, false).unwrap();
        assert!(args[5] > instance);
    }

    #[test]
    fn registration_and_helper_metadata_reconcile_without_client_publication() {
        let mut rt = Runtime::new();
        let owner = create(&mut rt, 100, 42, 0);
        let owner_sequence = rt.graphics.shared.lock().unwrap().windows[&owner].sequence;
        let mut info = request(0);
        dispatch(&mut rt, &mut info, true).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            let state = s.desktop.as_ref().unwrap().metadata();
            assert_eq!(state["dock"]["groups"][0]["key"], "process:100");
            assert_eq!(s.windows[&owner].sequence, owner_sequence);
        }
        registration(&mut rt, "org.riplix.Test", "Registered app", "calculator");
        let mut args = request(1);
        dispatch(&mut rt, &mut args, true).unwrap();
        let instance = args[5];
        {
            let s = rt.graphics.shared.lock().unwrap();
            let state = s.desktop.as_ref().unwrap().metadata();
            assert_eq!(state["dock"]["groups"][0]["key"], "app:org.riplix.Test");
            assert_eq!(state["dock"]["groups"][0]["name"], "Registered app");
            assert_eq!(state["dock"]["groups"][0]["icon"], "calculator");
            assert_eq!(s.registry.window(owner).unwrap().instance, instance);
            assert_eq!(s.registry.revision(), args[4]);
            assert_eq!(s.windows[&owner].sequence, owner_sequence);
        }
        registration(&mut rt, "org.riplix.Test", "Updated name", "folder");
        args = request(1);
        dispatch(&mut rt, &mut args, true).unwrap();
        let revision = args[4];
        let state = rt.graphics.shared.lock().unwrap().desktop.as_ref().unwrap().metadata();
        assert_eq!(state["dock"]["groups"][0]["name"], "Updated name");
        assert_eq!(state["dock"]["groups"][0]["icon"], "folder");
        args = request(1);
        dispatch(&mut rt, &mut args, true).unwrap();
        assert_eq!(args[4], revision, "unchanged registration advanced the revision");
        assert_eq!(rt.graphics.shared.lock().unwrap().desktop.as_ref().unwrap().metadata(), state);

        let helper = create(&mut rt, 101, 43, 0);
        let helper_sequence = rt.graphics.shared.lock().unwrap().windows[&helper].sequence;
        info = request(0);
        dispatch(&mut rt, &mut info, true).unwrap();
        assert_eq!(rt.graphics.shared.lock().unwrap().desktop.as_ref().unwrap().metadata()["dock"]["groups"].as_array().unwrap().len(), 2);
        args = request(2);
        args[8] = 101;
        args[9] = 43;
        dispatch(&mut rt, &mut args, true).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            let state = s.desktop.as_ref().unwrap().metadata();
            let groups = state["dock"]["groups"].as_array().unwrap();
            assert_eq!(groups.len(), 1);
            assert_eq!(groups[0]["windows"], serde_json::json!([owner, helper]));
            assert_eq!(s.registry.window(helper).unwrap().instance, instance);
            assert_eq!(s.registry.revision(), args[4]);
            assert_eq!(s.windows[&owner].sequence, owner_sequence);
            assert_eq!(s.windows[&helper].sequence, helper_sequence);
        }
        rt.graphics.shared.lock().unwrap().windows.remove(&owner);
        args = request(65);
        dispatch(&mut rt, &mut args, true).unwrap();
        let s = rt.graphics.shared.lock().unwrap();
        let state = s.desktop.as_ref().unwrap().metadata();
        assert_eq!(state["dock"]["groups"][0]["windows"], serde_json::json!([helper]));
        assert_eq!(state["dock"]["groups"][0]["app_id"], "org.riplix.Test");
        assert_eq!(s.registry.instance(instance).unwrap().owner, 0);
        assert_eq!(s.registry.revision(), args[4]);
    }
}
