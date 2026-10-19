//! The owned image wrapper embeds the same C decoder on both execution targets.
#[test]
fn owned_rgba_images_and_errors_match_on_ripple_and_native() {
    let root = std::env::temp_dir().join(format!("rfl-image-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    let source = root.join("main.rfl");
    std::fs::write(&source, r#"
import {} from "std/result";
import {} from "std/option";
import { decode, CorruptImage, InvalidImageFormat } from "lib/image";
fn main() -> i16 {
    let bytes: Vec<u8> = [137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,2,8,6,0,0,0,127,168,125,99,0,0,0,29,73,68,65,84,120,156,99,248,207,192,240,31,136,27,128,24,200,98,0,81,255,65,160,1,136,65,50,255,1,4,189,17,240,26,169,129,251,0,0,0,0,73,69,78,68,174,66,96,130];
    let red: u32 = 4278190335;
    let half: u32 = 4278190208;
    for (i in 0..32) {
        let image = decode(bytes).unwrap();
        if (image.width() != 4 || image.height() != 2 || image.opaque()) { return 1; }
        if (image.rgba(0,0).unwrap() != red || image.rgba(1,0).unwrap() != half) { return 2; }
        if (image.rgba(4,0).is_some() || image.rgba(0,2).is_some()) { return 3; }
    }
    match decode([]) { Err(CorruptImage) => {}, _ => { return 4; } }
    match decode([12,34]) { Err(InvalidImageFormat) => {}, _ => { return 5; } }
    0
}
"#).unwrap();
    for result in [rfl::run_file(&source), rfl::run_native_file(&source, false)] {
        let result = result.unwrap();
        assert_eq!(result.status, 0, "{}", result.stdout);
    }
    std::fs::remove_dir_all(root).unwrap();
}
