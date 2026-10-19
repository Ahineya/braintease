//! Keep source-only requests responsive while the serialized semantic worker
//! expands macros and checks imports. Each side owns its document snapshots;
//! only JSON messages cross the thread boundary (Analysis contains local state).
use super::*;
use std::sync::mpsc;

pub(super) fn serve() {
    let (send, receive) = mpsc::channel();
    let worker = std::thread::Builder::new()
        .name("rfl-analysis".into())
        .stack_size(8 * 1024 * 1024)
        .spawn(move || serve_semantics(receive.into_iter()))
        .expect("start RFL semantic worker");
    let mut sources: HashMap<String, (String, Vec<usize>)> = HashMap::new();
    let stdin = io::stdin();
    let mut reader = stdin.lock();
    while let Some(message) = read_message(&mut reader) {
        let method = message["method"].as_str().unwrap_or("");
        let params = &message["params"];
        let uri = text_of(&params["textDocument"]["uri"]);
        match method {
            "textDocument/didOpen" | "textDocument/didChange" => {
                let text = if method == "textDocument/didOpen" {
                    params["textDocument"]["text"].as_str()
                } else {
                    params["contentChanges"]
                        .as_array()
                        .and_then(|a| a.first())
                        .and_then(|c| c["text"].as_str())
                };
                if let Some(text) = text {
                    sources.insert(uri, (text.into(), line_starts(text)));
                }
            }
            "textDocument/didClose" => {
                sources.remove(&uri);
            }
            "textDocument/formatting" | "textDocument/rangeFormatting"
                if message.get("id").is_some() =>
            {
                let result = sources.get(&uri).map_or_else(
                    || json!([]),
                    |(text, lines)| {
                        formatting_source(
                            text,
                            lines,
                            params,
                            method == "textDocument/rangeFormatting",
                        )
                    },
                );
                emit(&json!({"jsonrpc":"2.0", "id":message["id"], "result":result}));
                continue;
            }
            _ => {}
        }
        let exiting = method == "exit";
        if send.send(message).is_err() || exiting {
            break;
        }
    }
    drop(send);
    worker.join().expect("RFL semantic worker failed");
}
