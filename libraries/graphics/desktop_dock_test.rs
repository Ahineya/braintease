//! Standalone portable dock acceptance harness. No native window or guest image.
#![allow(dead_code)]
#[path = "dock.rs"]
mod dock;
#[path = "desktop_icons.rs"]
mod icons;
#[path = "desktop.rs"]
mod policy;
#[path = "desktop_text.rs"]
mod text;
#[path = "desktop_dock.rs"]
mod view;

#[cfg(test)]
mod tests {
    use super::{
        dock::*,
        policy::{self, Rect, WindowCommand},
        view::*,
    };

    fn window(id: u32, process: u32, app: &str) -> WindowRecord<'_> {
        WindowRecord {
            id,
            process,
            instance: process,
            app_id: app,
            name: app,
            icon: "terminal",
            title: "Editor",
            role: ROLE_NORMAL,
            flags: VISIBLE,
            top_level: true,
        }
    }

    fn centre(r: Rect) -> (i32, i32) {
        assert!(!r.empty());
        (r.x + r.w / 2, r.y + r.h / 2)
    }
    fn button(view: &mut View, key: &GroupKey, which: u16) -> Interaction {
        let (x, y) = centre(view.group_rect(key));
        let out = view.button(which, true, x, y);
        assert!(view.button(which, false, x, y).consumed);
        out
    }
    fn press(view: &mut View, key: u16) -> Interaction {
        let out = view.key(key, true);
        assert!(view.key(key, false).consumed);
        out
    }
    fn menu(view: &mut View, key: &GroupKey) {
        assert!(button(view, key, 1).consumed);
        assert!(view.modal());
    }

    #[test]
    fn single_window_activates_and_keeps_its_release_even_outside_dock() {
        let mut view = View::new(640, 480);
        view.synchronize(&[window(1, 10, "rift")], 640, 480);
        let key = GroupKey::Application("rift".into());
        let (x, y) = centre(view.group_rect(&key));
        let out = view.button(0, true, x, y);
        assert!(out.consumed);
        assert_eq!(out.requests, vec![(1, WindowCommand::Activate)]);
        assert!(view.owns_button(0));
        assert!(view.button(0, false, 5, 5).consumed);
        assert!(!view.owns_button(0));
        assert!(!view.button(0, false, 5, 5).consumed);
        assert!(!view.modal());
    }

    #[test]
    fn repeated_and_chorded_dock_presses_do_not_launch_commands_or_client_capture() {
        let mut view = View::new(640, 480);
        view.synchronize(&[window(1, 10, "rift")], 640, 480);
        let (x, y) = centre(view.group_rect(&GroupKey::Application("rift".into())));
        assert_eq!(view.button(0, true, x, y).requests.len(), 1);
        let repeated = view.button(0, true, x, y);
        assert!(repeated.consumed);
        assert!(repeated.requests.is_empty());
        let chord = view.button(1, true, 20, 20);
        assert!(chord.consumed);
        assert!(chord.requests.is_empty());
        assert!(view.button(0, false, 20, 20).consumed);
        assert!(view.owns_button(1));
        assert!(view.button(1, false, 20, 20).consumed);
        assert!(!view.button(0, true, 20, 20).consumed);
    }

    #[test]
    fn primary_multiple_window_click_opens_menu_and_outside_click_dismisses() {
        let mut view = View::new(640, 480);
        view.synchronize(&[window(1, 10, "rift"), window(2, 20, "rift")], 640, 480);
        let key = GroupKey::Application("rift".into());
        assert!(button(&mut view, &key, 0).requests.is_empty());
        assert!(view.modal());
        let out = view.button(0, true, 4, 4);
        assert!(out.consumed);
        assert!(out.requests.is_empty());
        assert!(!view.modal());
        assert!(view.button(0, false, 4, 4).consumed);
        menu(&mut view, &key);
        assert!(button(&mut view, &key, 1).consumed);
        assert!(!view.modal());
    }

    #[test]
    fn dismissing_key_repeats_and_text_stay_owned_until_release() {
        let mut view = View::new(640, 480);
        view.synchronize(&[window(1, 10, "rift")], 640, 480);
        let key = GroupKey::Application("rift".into());
        menu(&mut view, &key);
        let out = view.key(40, true);
        assert_eq!(out.requests, vec![(1, WindowCommand::Activate)]);
        assert!(!view.modal());
        assert!(view.suppress_text());
        for _ in 0..3 {
            let repeat = view.key(40, true);
            assert!(repeat.consumed);
            assert!(repeat.requests.is_empty());
        }
        assert!(view.key(40, false).consumed);
        assert!(!view.suppress_text());
        assert!(!view.key(40, true).consumed);
        menu(&mut view, &key);
        assert!(
            !view.key(4, false).consumed,
            "release of prior client-owned press must reach client"
        );
        assert!(press(&mut view, 41).consumed);
        assert!(!view.modal());
    }

    #[test]
    fn disabled_bulk_actions_are_disabled_for_mouse_and_keyboard() {
        let mut a = window(1, 10, "rift");
        a.flags = MINIMIZED | CLOSE_PENDING;
        let mut b = window(2, 20, "rift");
        b.flags = MINIMIZED | CLOSE_PENDING;
        let mut view = View::new(640, 480);
        view.synchronize(&[a, b], 640, 480);
        let key = GroupKey::Application("rift".into());
        menu(&mut view, &key);
        let disabled = view
            .entries()
            .into_iter()
            .find(|e| e.command == "minimize_all")
            .unwrap();
        assert!(disabled.disabled);
        let (x, y) = centre(disabled.rect);
        assert!(view.button(0, true, x, y).requests.is_empty());
        assert!(view.button(0, false, x, y).consumed);
        assert!(view.modal());
        press(&mut view, 74);
        press(&mut view, 81);
        press(&mut view, 81);
        assert!(press(&mut view, 40).requests.is_empty());
        assert!(view.modal());
        press(&mut view, 77);
        assert!(press(&mut view, 40).requests.is_empty());
        assert!(view.modal());
    }

    #[test]
    fn show_all_uses_activate_and_preserves_maximized_windows() {
        let mut wm = policy::Desktop::new(640, 480);
        wm.add(1, 200, 140, true);
        wm.add(2, 180, 120, true);
        wm.command(1, WindowCommand::Maximize).unwrap();
        wm.command(1, WindowCommand::Minimize).unwrap();
        wm.command(2, WindowCommand::Minimize).unwrap();
        let mut a = window(1, 10, "rift");
        a.flags = MINIMIZED;
        let mut b = window(2, 20, "rift");
        b.flags = MINIMIZED;
        let mut view = View::new(640, 480);
        view.synchronize(&[a, b], 640, 480);
        menu(&mut view, &GroupKey::Application("rift".into()));
        press(&mut view, 77);
        press(&mut view, 82);
        let requests = press(&mut view, 40).requests;
        assert_eq!(
            requests,
            vec![(1, WindowCommand::Activate), (2, WindowCommand::Activate)]
        );
        for (id, command) in requests {
            wm.command(id, command).unwrap();
        }
        assert!(wm.get(1).unwrap().maximized);
        assert!(!wm.get(1).unwrap().minimized);
        assert!(!wm.get(2).unwrap().maximized);
        assert!(!wm.get(2).unwrap().minimized);
    }

    #[test]
    fn close_all_is_cooperative_and_skips_pending_windows() {
        let a = window(1, 10, "rift");
        let mut b = window(2, 20, "rift");
        b.flags |= CLOSE_PENDING;
        let mut view = View::new(640, 480);
        view.synchronize(&[a, b], 640, 480);
        menu(&mut view, &GroupKey::Application("rift".into()));
        press(&mut view, 77);
        assert_eq!(
            press(&mut view, 40).requests,
            vec![(1, WindowCommand::Close)]
        );
        assert_eq!(view.model.groups()[0].windows.len(), 2);
    }

    #[test]
    fn stale_group_cleanup_and_unchanged_snapshots_do_not_redraw() {
        let a = window(1, 10, "rift");
        let mut view = View::new(640, 480);
        assert!(view.synchronize(&[a], 640, 480));
        assert!(!view.synchronize(&[a], 640, 480));
        menu(&mut view, &GroupKey::Application("rift".into()));
        assert!(!view.synchronize(&[a], 640, 480));
        assert!(view.synchronize(&[], 640, 480));
        assert!(!view.modal());
        assert!(view.menu.is_none());
        assert!(!view.synchronize(&[], 640, 480));
    }

    #[test]
    fn mouse_selection_redraws_after_keyboard_selection_with_stationary_pointer() {
        let mut view = View::new(640, 480);
        view.synchronize(&[window(1, 10, "rift"), window(2, 20, "rift")], 640, 480);
        menu(&mut view, &GroupKey::Application("rift".into()));
        let r = view.entries()[1].rect;
        let (x, y) = centre(r);
        assert!(view.motion(x, y));
        press(&mut view, 74);
        assert!(view.motion(x, y));
        assert_eq!(
            press(&mut view, 40).requests,
            vec![(2, WindowCommand::Activate)]
        );
    }

    #[test]
    fn minimum_desktop_menu_stays_in_bounds_and_keyboard_reveals_windows() {
        let records: Vec<_> = (1..=30).map(|id| window(id, id, "rift")).collect();
        let mut view = View::new(320, 240);
        view.synchronize(&records, 320, 240);
        menu(&mut view, &GroupKey::Application("rift".into()));
        for _ in 0..29 {
            press(&mut view, 81);
        }
        let menu = view.menu_rect();
        assert!(
            menu.x >= 0 && menu.y >= 0 && menu.x + menu.w <= 320 && menu.y + menu.h <= 240 - HEIGHT
        );
        for entry in view.entries() {
            assert!(entry.rect.x >= menu.x && entry.rect.y >= menu.y);
            assert!(entry.rect.x + entry.rect.w <= menu.x + menu.w);
            assert!(entry.rect.y + entry.rect.h <= menu.y + menu.h);
        }
        assert!(view.entries().iter().any(|e| e.window == 30));
        assert_eq!(
            press(&mut view, 40).requests,
            vec![(30, WindowCommand::Activate)]
        );
    }

    #[test]
    fn wheel_only_scrolls_its_target_and_zero_delta_is_a_noop() {
        let records: Vec<_> = (1..=30).map(|id| window(id, id, "rift")).collect();
        let mut view = View::new(320, 240);
        view.synchronize(&records, 320, 240);
        assert!(!view.wheel(0, 0, -1));
        menu(&mut view, &GroupKey::Application("rift".into()));
        let (x, y) = centre(view.menu_rect());
        assert!(view.wheel(x, y, 0));
        assert_eq!(view.scroll, 0);
        assert!(view.wheel(0, 0, -1));
        assert_eq!(view.scroll, 0);
        assert!(view.wheel(x, y, -1));
        assert_eq!(view.scroll, 1);
        assert!(view.wheel(x, y, 0));
        assert_eq!(view.scroll, 1);
        assert!(view.wheel(x, y, 1));
        assert_eq!(view.scroll, 0);
    }

    #[test]
    fn dock_overflow_arrows_do_not_activate_from_blank_bar_pixels() {
        let names: Vec<_> = (1..=12).map(|id| format!("app-{id}")).collect();
        let records: Vec<_> = names
            .iter()
            .enumerate()
            .map(|(i, name)| window(i as u32 + 1, i as u32 + 1, name))
            .collect();
        let mut view = View::new(320, 240);
        view.synchronize(&records, 320, 240);
        assert!(!view.wheel(1, 200, 0));
        assert_eq!(view.offset, 0);
        assert!(!view.wheel(1, 200, -1));
        assert_eq!(view.offset, 0);
        let tray = view.footprint();
        assert!(view.wheel(tray.x + 4, 200, -1));
        assert_eq!(view.offset, 1);
        assert!(!view.button(0, true, 0, 200).consumed);
        assert!(!view.button(0, false, 0, 200).consumed);
        assert_eq!(view.offset, 1);
        let key = GroupKey::Application("app-2".into());
        let r = view.group_rect(&key);
        assert!(view.button(0, true, r.x - 16, r.y + 20).consumed);
        assert!(view.button(0, false, r.x - 16, r.y + 20).consumed);
        assert_eq!(view.offset, 0);
    }

    #[test]
    fn dock_only_paints_and_intercepts_its_contained_surface() {
        let mut view = View::new(640, 480);
        view.synchronize(&[window(1, 10, "rift")], 640, 480);
        let tray = view.footprint();
        let beside = (tray.x - 12, tray.y + tray.h / 2);
        assert!(view.bar().contains(beside.0, beside.1));
        assert!(!view.covers(beside.0, beside.1));
        assert!(!view.wheel(beside.0, beside.1, -1));
        assert!(!view.wheel(beside.0, beside.1, 0));
        assert!(!view.button(0, true, beside.0, beside.1).consumed);
        assert!(!view.button(0, false, beside.0, beside.1).consumed);
        assert!(view.covers(tray.x + 3, tray.y + tray.h / 2));

        for scale in [1, 2] {
            let width = 640 * scale;
            let height = 480 * scale;
            let underneath = 0xff123456;
            let mut out = vec![underneath; (width * height) as usize];
            view.paint(&mut out, width, height, scale, view.bar());
            for y in (480 - HEIGHT as u32) * scale..height {
                for x in 0..width {
                    let logical = ((x / scale) as i32, (y / scale) as i32);
                    if !tray.contains(logical.0, logical.1) {
                        assert_eq!(out[(y * width + x) as usize], underneath);
                    }
                }
            }
            let (x, y) = (tray.x + 3, tray.y + tray.h / 2);
            assert_ne!(
                out[(y as u32 * scale * width + x as u32 * scale) as usize],
                underneath,
                "contained dock surface still paints at density {scale}"
            );
        }
    }

    #[test]
    fn blur_clears_modal_capture_and_text_suppression() {
        let mut view = View::new(640, 480);
        view.synchronize(&[window(1, 10, "rift")], 640, 480);
        menu(&mut view, &GroupKey::Application("rift".into()));
        assert!(view.key(4, true).consumed);
        assert!(view.suppress_text());
        view.lost_focus();
        assert!(!view.modal());
        assert!(!view.suppress_text());
        assert!(!view.key(4, false).consumed);
        assert!(!view.button(0, false, 0, 0).consumed);
    }

    #[test]
    fn painting_at_both_densities_respects_damage_and_menu_bounds() {
        let mut view = View::new(320, 240);
        view.synchronize(&[window(1, 10, "rift"), window(2, 20, "rift")], 320, 240);
        menu(&mut view, &GroupKey::Application("rift".into()));
        let damage = Rect::new(20, 30, 60, 70);
        for scale in [1, 2] {
            let width = 320 * scale;
            let height = 240 * scale;
            let sentinel = 0xff123456;
            let mut out = vec![sentinel; (width * height) as usize];
            view.paint(&mut out, width, height, scale, damage);
            assert!(out.iter().any(|&pixel| pixel != sentinel));
            for y in 0..height {
                for x in 0..width {
                    if !damage.contains((x / scale) as i32, (y / scale) as i32) {
                        assert_eq!(
                            out[(y * width + x) as usize],
                            sentinel,
                            "outside damage at density {scale}"
                        );
                    }
                }
            }
        }
    }
}
